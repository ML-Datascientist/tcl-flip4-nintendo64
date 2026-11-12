/* n64gl.c — EGL/GLES2 host for GLideN64 hw_render inside n64play.
 *
 * WHY function pointers everywhere: in this single binary the core's glsm
 * defines every glXxx as a DATA symbol (PFN variable). A direct call to
 * glBindFramebuffer() from here would link against glsm's variable instead
 * of libGLESv2 — instant crash. So this TU resolves every entry point via
 * dlopen/dlsym under its own names and never references a gl or egl symbol
 * from the linker. (Verified: gpu_probe, same device, EGL+PBUFFER+ES2+
 * readPixels all work from /data/local/tmp under the su domain.)
 *
 * Frame flow: GLideN64 blits the final VI-sized image into the FBO we hand
 * it through retro_hw_render_callback.get_current_framebuffer (glsm keeps
 * it bound as the "default" framebuffer for the core). capture() reads it
 * back bottom-up and writes top-down RGB565, decimating >320x240 the same
 * way the angrylion path's conv_xrgb565 does.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "n64gl.h"

#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)
#define GL_W 640   /* FBO capacity: the core may boot GLideN64 at its default
                    * 640x576 retro screen before the first VI update */
#define GL_H 576

/* ------------------------------------------------------------------ EGL */
typedef EGLDisplay (*PFN_eglGetDisplay)(EGLNativeDisplayType);
typedef EGLBoolean (*PFN_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
typedef EGLBoolean (*PFN_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLSurface (*PFN_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
typedef EGLContext (*PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLBoolean (*PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean (*PFN_eglBindAPI)(EGLenum);
typedef const char*(*PFN_eglQueryString)(EGLDisplay, EGLint);
typedef void *(*PFN_eglGetProcAddress)(const char *);
typedef EGLint (*PFN_eglGetError)(void);

static PFN_eglGetDisplay          e_GetDisplay;
static PFN_eglInitialize          e_Initialize;
static PFN_eglChooseConfig        e_ChooseConfig;
static PFN_eglCreatePbufferSurface e_CreatePbufferSurface;
static PFN_eglCreateContext       e_CreateContext;
static PFN_eglMakeCurrent         e_MakeCurrent;
static PFN_eglBindAPI             e_BindAPI;
static PFN_eglQueryString         e_QueryString;
static PFN_eglGetProcAddress      e_GetProcAddress;
static PFN_eglGetError            e_GetError;

/* -------------------------------------------------------------- GLES2 */
#define GLFN(ret, name, args) typedef ret(*PFN_gl##name) args; static PFN_gl##name p_gl##name
GLFN(void, GenFramebuffers, (GLsizei, GLuint *));
GLFN(void, BindFramebuffer, (GLenum, GLuint));
GLFN(void, GenTextures, (GLsizei, GLuint *));
GLFN(void, BindTexture, (GLenum, GLuint));
GLFN(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *));
GLFN(void, TexParameteri, (GLenum, GLenum, GLint));
GLFN(void, GenRenderbuffers, (GLsizei, GLuint *));
GLFN(void, BindRenderbuffer, (GLenum, GLuint));
GLFN(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei));
GLFN(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint));
GLFN(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint));
GLFN(GLenum, CheckFramebufferStatus, (GLenum));
GLFN(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *));
GLFN(void, Finish, (void));
GLFN(const GLubyte*, GetString, (GLenum));
GLFN(void, GetIntegerv, (GLenum, GLint *));
GLFN(void, Viewport, (GLint, GLint, GLsizei, GLsizei));
GLFN(void, PixelStorei, (GLenum, GLint));
GLFN(GLenum, GetError, (void));

static void *g_leso, *g_gleso;
static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLSurface g_sur = EGL_NO_SURFACE;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static GLuint g_fbo, g_color_tex, g_depth_rb;
static unsigned char g_rgba[GL_W * GL_H * 4];

static int resolve_egl(void)
{
   g_leso = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
   if (!g_leso) g_leso = dlopen("/system/lib/libEGL.so", RTLD_NOW | RTLD_GLOBAL);
   if (!g_gleso) g_gleso = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
   if (!g_gleso) g_gleso = dlopen("/system/lib/libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
   if (!g_leso || !g_gleso) { LOG("n64gl: dlopen EGL=%p GLESv2=%p", g_leso, g_gleso); return 0; }
#define E(x) do { *(void **)&e_##x = dlsym(g_leso, "egl" #x); if (!e_##x) { LOG("n64gl: missing egl" #x); return 0; } } while (0)
   E(GetDisplay); E(Initialize); E(ChooseConfig); E(CreatePbufferSurface);
   E(CreateContext); E(MakeCurrent); E(BindAPI); E(QueryString); E(GetProcAddress); E(GetError);
#undef E
#define G(x) do { *(void **)&p_gl##x = dlsym(g_gleso, "gl" #x); if (!p_gl##x) { LOG("n64gl: missing gl" #x); return 0; } } while (0)
   G(GenFramebuffers); G(BindFramebuffer); G(GenTextures); G(BindTexture);
   G(TexImage2D); G(TexParameteri); G(GenRenderbuffers); G(BindRenderbuffer);
   G(RenderbufferStorage); G(FramebufferTexture2D); G(FramebufferRenderbuffer);
   G(CheckFramebufferStatus); G(ReadPixels); G(Finish); G(GetString);
   G(GetIntegerv); G(Viewport); G(PixelStorei); G(GetError);
#undef G
   return 1;
}

void *n64gl_get_proc(const char *name)
{
   void *p;
   if (!g_leso || !name) return NULL;
   p = e_GetProcAddress(name);                       /* extensions */
   if (p && (void *)p != (void *)0x1 && (void *)p != (void *)0x2 &&
       (void *)p != (void *)0x3 && (void *)p != (void *)-1)
      return p;
   p = dlsym(g_gleso, name);                           /* core ES2 */
   if (p) return p;
   return dlsym(RTLD_DEFAULT, name);
}

int n64gl_init(void)
{
   EGLint maj = 0, min = 0, n = 0;
   EGLConfig cfg = NULL;

   if (g_ctx != EGL_NO_CONTEXT) return 1;
   if (!resolve_egl()) return 0;

   g_dpy = e_GetDisplay(EGL_DEFAULT_DISPLAY);
   if (g_dpy == EGL_NO_DISPLAY || !e_Initialize(g_dpy, &maj, &min)) {
      LOG("n64gl: eglInitialize failed err=0x%x", e_GetError());
      g_dpy = EGL_NO_DISPLAY;
      return 0;
   }
   e_BindAPI(EGL_OPENGL_ES_API);
   /* This driver exposes ES3 renderables only; walk ES3 -> ES2 -> any. */
   {
      EGLint bits[3] = { 0x30C2 /*EGL_OPENGL_ES3_BIT_KHR*/, EGL_OPENGL_ES2_BIT, 0 };
      for (int pass = 0; pass < 3 && !cfg; pass++) {
         EGLint attribs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, bits[pass],
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 16, EGL_NONE };
         if (!e_ChooseConfig(g_dpy, attribs, &cfg, 1, &n) || n < 1) cfg = NULL;
      }
   }
   if (!cfg) {
      EGLint attribs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
      if (!e_ChooseConfig(g_dpy, attribs, &cfg, 1, &n) || n < 1) {
         LOG("n64gl: no PBUFFER config err=0x%x", e_GetError());
         return 0;
      }
   }
   {
      EGLint pbuf[] = { EGL_WIDTH, 320, EGL_HEIGHT, 240, EGL_NONE };
      g_sur = e_CreatePbufferSurface(g_dpy, cfg, pbuf);
   }
   if (g_sur == EGL_NO_SURFACE) { LOG("n64gl: pbuffer failed err=0x%x", e_GetError()); return 0; }
   {
      EGLint cattribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
      g_ctx = e_CreateContext(g_dpy, cfg, EGL_NO_CONTEXT, cattribs);
      if (g_ctx == EGL_NO_CONTEXT) {
         cattribs[1] = 2;
         g_ctx = e_CreateContext(g_dpy, cfg, EGL_NO_CONTEXT, cattribs);
      }
   }
   if (g_ctx == EGL_NO_CONTEXT || !e_MakeCurrent(g_dpy, g_sur, g_sur, g_ctx)) {
      LOG("n64gl: context failed err=0x%x", e_GetError());
      g_ctx = EGL_NO_CONTEXT;
      return 0;
   }
   LOG("n64gl: EGL %d.%d ctx ok: %s | %s", maj, min,
       (const char *)p_glGetString(GL_VERSION), (const char *)p_glGetString(GL_RENDERER));

   /* hw_render framebuffer: RGBA8 color texture + D16 depth, 640x480 */
   p_glGenFramebuffers(1, &g_fbo);
   p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
   p_glGenTextures(1, &g_color_tex);
   p_glBindTexture(GL_TEXTURE_2D, g_color_tex);
   p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GL_W, GL_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color_tex, 0);
   p_glGenRenderbuffers(1, &g_depth_rb);
   p_glBindRenderbuffer(GL_RENDERBUFFER, g_depth_rb);
   p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, GL_W, GL_H);
   p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_depth_rb);
   if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      LOG("n64gl: FBO incomplete (err 0x%x)", p_glGetError());
      return 0;
   }
   p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
   LOG("n64gl: fbo %u (%dx%d RGBA8+D16) complete", g_fbo, GL_W, GL_H);
   return 1;
}

int n64gl_makecurrent(void)
{
   return g_ctx != EGL_NO_CONTEXT &&
          e_MakeCurrent(g_dpy, g_sur, g_sur, g_ctx) == EGL_TRUE;
}

unsigned n64gl_fbo(void) { return g_fbo; }

const char *n64gl_string(unsigned pname)
{
   if (g_ctx == EGL_NO_CONTEXT) return "(no gl)";
   return (const char *)p_glGetString((GLenum)pname);
}

int n64gl_capture_rgb565(uint16_t *dst, unsigned *ow, unsigned *oh,
                         unsigned src_w, unsigned src_h)
{
   unsigned ow_, oh_, sx = 1, sy = 1, y, x;
   if (g_ctx == EGL_NO_CONTEXT || !src_w || !src_h) return 0;
   if (src_w > GL_W || src_h > GL_H) return 0;
   ow_ = src_w > 320 ? src_w / 2 : src_w;
   oh_ = src_h > 240 ? src_h / 2 : src_h;
   sx = src_w / ow_; sy = src_h / oh_;

   p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
   p_glFinish();
   p_glPixelStorei(GL_PACK_ALIGNMENT, 4);
   p_glReadPixels(0, 0, (GLsizei)src_w, (GLsizei)src_h, GL_RGBA, GL_UNSIGNED_BYTE, g_rgba);

   for (y = 0; y < oh_; y++) {
      /* FBO rows are bottom-up; the WS protocol frame is top-down */
      const unsigned char *s = g_rgba + (size_t)(src_h - 1 - y * sy) * src_w * 4;
      uint16_t *d = dst + (size_t)y * ow_;
      for (x = 0; x < ow_; x++) {
         const unsigned char *p = s + (size_t)x * sx * 4;
         d[x] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
      }
   }
   *ow = ow_; *oh = oh_;
   return 1;
}
