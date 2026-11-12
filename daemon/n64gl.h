/* n64gl.h — EGL/GLES2 host context for GLideN64's libretro hw_render path */
#ifndef N64GL_H
#define N64GL_H
#include <stdint.h>

/* Create EGL display + pbuffer + ES2 context, allocate the hw_render
 * framebuffer (RGBA8 color + D16 depth, 640x480). 0 on failure. */
int  n64gl_init(void);
/* Re-bind the context to the calling thread. 0 on failure. */
int  n64gl_makecurrent(void);
/* FBO id handed to the core via get_current_framebuffer. */
unsigned n64gl_fbo(void);
/* Resolve "glXxx"/"eglXxx" for the core's get_proc_address callback. */
void *n64gl_get_proc(const char *name);
/* glGetString(GL_VERSION|GL_RENDERER) etc for logging; NULL if no GL. */
const char *n64gl_string(unsigned pname);
/* Read src_w x src_h RGBA from our FBO, flip to top-down, decimate to
 * 320x240 max, write RGB565 LE to dst (>= 320*240*2 bytes), set ow/oh. */
int  n64gl_capture_rgb565(uint16_t *dst, unsigned *ow, unsigned *oh,
                          unsigned src_w, unsigned src_h);

#endif
