/* n64play.c — Nintendo 64 emulator frame-streamer for the TCL Flip 4 (KaiOS)
 *
 * Static libretro frontend (mupen64plus-libretro-nx core objects linked
 * directly). Two RDP paths, same wire protocol, same phone app:
 *   GL (default): GLideN64 GLES2 on the Adreno GPU via EGL pbuffer +
 *                 RETRO_ENVIRONMENT_SET_HW_RENDER (n64gl.c). Frames arrive
 *                 as RETRO_HW_FRAME_BUFFER_VALID -> readPixels -> RGB565.
 *   --no-gl / N64GL=0 / GL init failure: angrylion software RDP into a
 *                 framebuffer (cxd4 LLE RSP + ARM new-dynarec).
 * Serves a WebSocket stream on 127.0.0.1:<port> (default 8081) for the ompterm
 * app — IDENTICAL wire protocol to mdplay (Genesis), so the phone app is
 * untouched:
 *   hello:     WS TEXT "MD1 <w> <h> <fps> <samplerate>"
 *   audio:     WS BINARY  'M','D','A',0 + interleaved stereo int16 LE
 *   frame:     WS BINARY  'M','D','1',0, w_hi,w_lo, h_hi,h_lo + RGB565 LE rows
 *   input:     client WS BINARY 2-byte LE bitmask, phone keys -> N64 pad:
 *                bit0  key '1'   -> B          bit1  key '2'  -> A
 *                bit2  key '*'   -> Select     bit3  OK       -> Start
 *                bit4..7 arrows  -> dpad AND analog stick (full deflection =
 *                                   run, matching N64-original dpad behaviour)
 *                bit8  key '3'   -> Z (alt)    bit9  SoftLeft -> (unused)
 *                bit10 SoftRight -> R          bit11 key '#'  -> Z
 *                bit12 key '7'   -> C left     bit13 key '9'  -> C right
 *                bit14 (spare)   -> C up       bit15 (spare)  -> C down
 *              (bits 12-15 feed RETRO_DEVICE_INDEX_ANALOG_RIGHT; the C
 *              buttons have no joypad ids in this core's default mapping)
 *   cmd:       TEXT "R" reset; "A0"/"A1" audio off/on
 *
 * Saves: mempak (SM64!) is written through to <romdir>/<Game Name>.mp1 by the
 * core on every mempak write — kill-safe. Cart saves additionally flushed to
 * <rom>.srm at SIGTERM and restored at boot (RetroArch-compatible blob).
 *
 * Build: nintendo64_emu/build_gl.sh (NDK clang, bionic, GLideN64 GLES2 +
 * angrylion fallback -> n64play_gl); nintendo64_emu/build.sh keeps the
 * historical angrylion-only zig/musl static binary.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <libretro.h>

#include "n64gl.h"

#if defined(__linux__) && defined(__arm__)
#include <sys/ucontext.h>
/* Static musl binaries never reach Android debuggerd — log the fault ourselves. */
static void crash_handler(int sig, siginfo_t *si, void *uc)
{
   ucontext_t *u = (ucontext_t *)uc;
   char buf[256];
   int n = snprintf(buf, sizeof(buf),
       "n64play: FATAL sig=%d addr=%p pc=%08lx lr=%08lx sp=%08lx\n",
       sig, si->si_addr, (unsigned long)u->uc_mcontext.arm_pc,
       (unsigned long)u->uc_mcontext.arm_lr, (unsigned long)u->uc_mcontext.arm_sp);
   if (n > 0) { ssize_t w = write(2, buf, (size_t)n); (void)w; }
   _exit(128 + sig);
}
static void install_crash_logging(void)
{
   struct sigaction sa;
   if (getenv("N64NOCH")) return;   /* let debuggerd capture the backtrace */
   memset(&sa, 0, sizeof(sa));
   sa.sa_sigaction = crash_handler;
   sa.sa_flags = SA_SIGINFO;
   sigaction(SIGBUS, &sa, NULL);
   sigaction(SIGSEGV, &sa, NULL);
   sigaction(SIGILL, &sa, NULL);
   sigaction(SIGFPE, &sa, NULL);
}
#else
static void install_crash_logging(void) { }
#endif

/* ------------------------------------------------------------------ logging */
static int64_t now_us(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

/* --------------------------------------------------------------------- SHA1 */
typedef struct { uint32_t h[5]; uint8_t buf[64]; uint64_t len; } sha1_ctx;
static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }
static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
   uint32_t w[80], a, b, cc, d, e, t;
   int i;
   for (i = 0; i < 16; i++)
      w[i] = (p[i*4] << 24) | (p[i*4+1] << 16) | (p[i*4+2] << 8) | p[i*4+3];
   for (; i < 80; i++)
      w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
   a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
   for (i = 0; i < 80; i++) {
      if (i < 20)      { t = (b & cc) | (~b & d);          }
      else if (i < 40) { t = b ^ cc ^ d;                   }
      else if (i < 60) { t = (b & cc) | (b & d) | (cc & d); }
      else             { t = b ^ cc ^ d;                    }
      t = rol(a, 5) + t + e + w[i] + (i < 20 ? 0x5a827999u : i < 40 ? 0x6ed9eba1u : i < 60 ? 0x8f1bbcdcu : 0xca62c1d6u);
      e = d; d = cc; cc = rol(b, 30); b = a; a = t;
   }
   c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}
static void sha1_init(sha1_ctx *c)
{
   c->h[0] = 0x67452301u; c->h[1] = 0xefcdab89u; c->h[2] = 0x98badcfeu;
   c->h[3] = 0x10325476u; c->h[4] = 0xc3d2e1f0u; c->len = 0;
}
static void sha1_update(sha1_ctx *c, const uint8_t *p, size_t n)
{
   size_t fill = c->len % 64, left = 64 - fill;
   c->len += n;
   if (n < left) { memcpy(c->buf + fill, p, n); return; }
   memcpy(c->buf + fill, p, left);
   sha1_block(c, c->buf); p += left; n -= left;
   while (n >= 64) { sha1_block(c, p); p += 64; n -= 64; }
   memcpy(c->buf, p, n);
}
static void sha1_final(sha1_ctx *c, uint8_t out[20])
{
   uint8_t pad[72] = {0x80};
   uint64_t bits = c->len * 8;
   int padlen = (c->len % 64) < 56 ? 56 - (int)(c->len % 64) : 120 - (int)(c->len % 64);
   int i;
   for (i = 0; i < 8; i++) pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
   sha1_update(c, pad, padlen + 8);
   for (i = 0; i < 20; i++) out[i] = (uint8_t)(c->h[i >> 2] >> (24 - 8 * (i & 3)));
}
static void b64(const uint8_t *in, size_t n, char *out)
{
   static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   size_t i = 0, o = 0;
   while (i + 2 < n) {
      uint32_t v = (in[i] << 16) | (in[i+1] << 8) | in[i+2];
      out[o++] = T[v >> 18]; out[o++] = T[(v >> 12) & 63]; out[o++] = T[(v >> 6) & 63]; out[o++] = T[v & 63];
      i += 3;
   }
   if (i < n) {
      uint32_t v = in[i] << 16;
      if (i + 1 < n) v |= in[i+1] << 8;
      out[o++] = T[v >> 18]; out[o++] = T[(v >> 12) & 63];
      out[o++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
      out[o++] = '=';
   }
   out[o] = 0;
}

/* ------------------------------------------------------------- socket utils */
static int put_all(int fd, const void *buf, size_t n)
{
   const uint8_t *p = (const uint8_t *)buf;
   while (n) {
      ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
      if (r > 0) { p += r; n -= (size_t)r; continue; }
      if (r < 0 && errno == EINTR) continue;
      return -1;
   }
   return 0;
}
static int get_line(int fd, char *line, int max)
{
   int n = 0;
   while (n < max - 1) {
      char c;
      ssize_t r = recv(fd, &c, 1, 0);
      if (r <= 0) return -1;
      if (c == '\n') break;
      if (c != '\r') line[n++] = c;
   }
   line[n] = 0;
   return n;
}

/* ------------------------------------------------------------- emulation IO */
#define FB_MAX (640 * 576 * 2)
static uint8_t  g_fb[FB_MAX];
static int      g_fb_dummy;           /* N64FB=1: 16-byte dummy payloads (probe) */
static int      g_nows;               /* N64NOWS=1: send nothing (isolate retro_run) */
static unsigned g_fb_w, g_fb_h, g_fb_pitch;
static int      g_frame_ready;
static uint16_t g_pad;                /* phone bitmask (see header comment) */
static unsigned g_av_w = 320, g_av_h = 240;
static double   g_av_fps = 50.0;
static unsigned g_sr = 44100;
static int      g_pixfmt_565;         /* 1: core already sends RGB565 */
static const char *g_savedir = NULL;
static char g_savedir_buf[512];
static char g_srm_path[560];
static int  g_srm_ready;
static unsigned g_audio_dropped;
static int      g_wsc = -1;
static int      g_audio_err;
static int      g_audio_on = 1;
static int      g_probe;              /* N64PROBE=1 */
static int      g_gl_mode = 1;        /* 1=GLideN64 GPU RDP, 0=angrylion */
static int      g_gl_frames;          /* frames received via VALID */
static int64_t  g_cap_us;             /* time spent in GL capture (per log window) */
static struct retro_hw_render_callback g_hw;

/* SIGTERM flush of the SAVE_RAM blob (cart sram/eeprom/flash; mempak is
 * already write-through on the core side). Getters/write only — safe enough
 * for a dying process. */
static void sram_flush_handler(int sig)
{
   (void)sig;
   if (g_srm_ready && g_srm_path[0]) {
      size_t sz = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
      uint8_t *p = (uint8_t *)retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
      if (sz && p) {
         int fd = open(g_srm_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
         if (fd >= 0) {
            size_t off = 0;
            while (off < sz) {
               ssize_t w = write(fd, p + off, sz - off);
               if (w <= 0) break;
               off += (size_t)w;
            }
            close(fd);
            if (off == sz) _exit(0);
         }
         {
            char b[480];
            int n = snprintf(b, sizeof(b), "n64play: SRAM FLUSH FAIL errno=%d sz=%zu path=%s\n",
                             errno, sz, g_srm_path);
            if (n > 0) { ssize_t w = write(2, b, (size_t)n); (void)w; }
         }
      }
   }
   _exit(0);
}

/* XRGB8888 (LE words) -> RGB565 LE; decimate to <=320x240.
 * SM64's VI output is a pure 2x horizontal (and during cutscenes 2x vertical)
 * duplication of its 320x240 render — dropping alternate rows/columns is
 * lossless there and halves bandwidth + phone-side pixel work. Other games at
 * <=320x240 pass through untouched.
 * NO rotation here: rotating the scattered writes over the core's VI buffer
 * garbles real frames (twin mirrored faces; repro'd on host x86 + phone,
 * while test patterns pass) — angrylion's buffer geometry vs scattered stores
 * is not worth untangling. Rotation happens in game.js's pixel loop instead
 * (sequential, verified). */
static void conv_xrgb565(const uint8_t *src, size_t spitch, unsigned w, unsigned h)
{
   unsigned y, x;
   unsigned ow = (w > 320) ? (w / 2) : w;
   unsigned oh = (h > 240) ? (h / 2) : h;
   unsigned sx = w / ow, sy = h / oh;
   for (y = 0; y < oh; y++) {
      const uint32_t *s = (const uint32_t *)(src + (size_t)(y * sy) * spitch);
      uint16_t *d = (uint16_t *)(g_fb + (size_t)y * ow * 2);
      for (x = 0; x < ow; x++) {
         uint32_t p = s[x * sx];
         d[x] = (uint16_t)((((p >> 19) & 0x1f) << 11) | (((p >> 10) & 0x3f) << 5) | ((p >> 3) & 0x1f));
      }
   }
   g_fb_w = ow; g_fb_h = oh;
}

static void cb_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
   unsigned y;
   if (data == RETRO_HW_FRAME_BUFFER_VALID) {
      /* GLideN64 blitted into our FBO (n64gl) — read + convert + decimate */
      unsigned ow = 0, oh = 0;
      int64_t c0 = now_us();
      int ok = n64gl_capture_rgb565((uint16_t *)g_fb, &ow, &oh, width, height);
      g_cap_us += now_us() - c0;
      if (ok) {
         g_fb_w = ow; g_fb_h = oh; g_fb_pitch = ow * 2;
         g_frame_ready = 1;
         g_gl_frames++;
         if (g_gl_frames == 1) LOG("n64play: GL first frame %ux%u -> %ux%u", width, height, ow, oh);
      }
      return;
   }
   if (!data) { g_frame_ready = 1; return; }          /* NULL flush */
   if ((size_t)width * 2 * height > FB_MAX) return;   /* absurd geometry */
   if (g_pixfmt_565) {
      uint32_t row = width * 2;
      for (y = 0; y < height; y++)
         memcpy(g_fb + (size_t)y * row, (const uint8_t *)data + (size_t)y * pitch, row);
      g_fb_w = width; g_fb_h = height; g_fb_pitch = row;
   } else {
      conv_xrgb565((const uint8_t *)data, pitch, width, height);
      g_fb_pitch = g_fb_w * 2;                        /* conv set decimated w/h */
   }
   g_frame_ready = 1;
}
static size_t cb_audio_batch(const int16_t *samples, size_t count)
{
   g_audio_dropped += count;
   if (g_wsc >= 0 && g_audio_on && !g_nows && count && count < 16000) {
      uint64_t payload = 4 + count * 4;
      uint8_t hdr[4] = { 0x82, 0x7e, (uint8_t)(payload >> 8), (uint8_t)payload };
      uint8_t tag[4] = { 'M','D','A',0 };
      if (payload >= 65536) return count;
      if (put_all(g_wsc, hdr, 4) || put_all(g_wsc, tag, 4) || put_all(g_wsc, samples, count * 4))
         g_audio_err = 1;
   }
   return count;
}
static void cb_audio_single(int16_t left, int16_t right) { (void)left; (void)right; }

static void cb_input_poll(void) { }

/* phone bit for each retro JOYPAD id — the -nx glue uses the SNES-style
 * layout (emulate_game_controller_via_libretro.c):
 *   N64 A = RETRO_B(0), N64 B = RETRO_Y(1), Z = L2(12), L = SELECT(2),
 *   R = R2(13), Start = 3, dpad = 4..7 (C-stick = 8..11, unused by SM64) */
static int pad_bit(unsigned id)
{
   switch (id) {
   case RETRO_DEVICE_ID_JOYPAD_Y:      return 0;    /* N64 B — key '1' */
   case RETRO_DEVICE_ID_JOYPAD_B:      return 1;    /* N64 A — key '2' */
   case RETRO_DEVICE_ID_JOYPAD_SELECT: return 2;    /* N64 L — key '*' */
   case RETRO_DEVICE_ID_JOYPAD_START:  return 3;    /* OK             */
   case RETRO_DEVICE_ID_JOYPAD_UP:     return 4;
   case RETRO_DEVICE_ID_JOYPAD_DOWN:   return 5;
   case RETRO_DEVICE_ID_JOYPAD_LEFT:   return 6;
   case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return 7;
   case RETRO_DEVICE_ID_JOYPAD_R2:     return 10;   /* N64 R — SoftRight */
   default: return -1;
   }
}
#define STICK_FULL 32000   /* < 32767 on purpose; SM64 treats full range as run */
static int16_t cb_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
   static int stick_log;   /* bounded trace: proves the stick reaches the core */
   if (port) return 0;
   if (device == RETRO_DEVICE_JOYPAD) {
      int b;
      if (id == RETRO_DEVICE_ID_JOYPAD_L2) /* N64 Z: '#' (bit11) or key '3' (bit8) */
         return (int16_t)(((g_pad >> 11) | (g_pad >> 8)) & 1);
      b = pad_bit(id);
      return (int16_t)(b < 0 ? 0 : (g_pad >> b) & 1);
   }
   if (device == RETRO_DEVICE_ANALOG && index == RETRO_DEVICE_INDEX_ANALOG_LEFT) {
      /* dpad bits -> analog stick (N64 dpad in SM64 == full-deflection stick) */
      int l = (g_pad >> 6) & 1, r = (g_pad >> 7) & 1, u = (g_pad >> 4) & 1, d = (g_pad >> 5) & 1;
      int16_t v;
      if (id == RETRO_DEVICE_ID_ANALOG_X) v = (int16_t)((r ? STICK_FULL : 0) - (l ? STICK_FULL : 0));
      else if (id == RETRO_DEVICE_ID_ANALOG_Y) v = (int16_t)((d ? STICK_FULL : 0) - (u ? STICK_FULL : 0));
      else v = 0;
      if (v && stick_log < 8) { stick_log++; LOG("n64play: astick %c=%d pad=%04x", id ? 'y' : 'x', v, (unsigned)g_pad); }
      return v;
   }
   if (device == RETRO_DEVICE_ANALOG && index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
      /* C buttons (SM64 camera rotate/zoom) come ONLY from the c-stick in the
       * default mapping: the glue ignores cbuttons_mode without '-alt-map'.
       * Sign conventions are the glue's (emulate_game_controller_via_libretro.c:375):
       * X>0 -> C-left, Y<0 -> C-up. */
      int cl = (g_pad >> 12) & 1, cr = (g_pad >> 13) & 1, cu = (g_pad >> 14) & 1, cd = (g_pad >> 15) & 1;
      int16_t v;
      if (id == RETRO_DEVICE_ID_ANALOG_X) v = (int16_t)((cl ? STICK_FULL : 0) - (cr ? STICK_FULL : 0));
      else if (id == RETRO_DEVICE_ID_ANALOG_Y) v = (int16_t)((cd ? STICK_FULL : 0) - (cu ? STICK_FULL : 0));
      else v = 0;
      if (v && stick_log < 16) { stick_log++; LOG("n64play: cstick %c=%d pad=%04x", id ? 'y' : 'x', v, (unsigned)g_pad); }
      return v;
   }
   return 0;
}

static void core_logf(enum retro_log_level level, const char *fmt, ...)
{
   (void)level;
   /* core chatter is verbose on N64; keep only the startup lines we care about
    * — full silence like mdplay would hide RDP/RSP fallback warnings. */
   va_list ap; va_start(ap, fmt);
   if (level >= RETRO_LOG_INFO) {
      char buf[512];
      vsnprintf(buf, sizeof(buf), fmt, ap);
      LOG("core[%d]: %s", (int)level, buf);
   }
   va_end(ap);
}

/* --------------------------------------------------------- hw render glue */
/* signatures per THIS tree's libretro.h: framebuffer id = uintptr_t,
 * proc address = retro_proc_address_t (opaque fn), not GLuint/void* */
static uintptr_t hw_get_fb(void) { return (uintptr_t)n64gl_fbo(); }
static retro_proc_address_t hw_get_proc(const char *name)
{
   return (retro_proc_address_t)n64gl_get_proc(name);
}

/* core options the glue asks for via GET_VARIABLE (defaults would pick
 * GLideN64 — now it IS linked in, but rdp-plugin MUST still be answered:
 * glide64 in GL mode, angrylion on the software fallback). */
static const char *corevar(const char *key)
{
   static const char *cpu = NULL;      /* N64CPU=dynarec|cached_interp|pure_interp */
   static const char *rsp = NULL;      /* N64RSP=cxd4|hle */
   static const struct { const char *k, *v; } vars[] = {
      { "mupen64plus-rsp-plugin",       "cxd4"               },
      { "mupen64plus-ThreadedRenderer", "False"              },
      { "mupen64plus-cpucore",          "dynamic_recompiler" },
      /* GLideN64 blits its final image to retro_screen size; the wire
       * protocol + phone JS rotation budget are 320x240. Answering this
       * also forces EnableNativeResFactor=1 (libretro.c:1443-1449). */
      { "mupen64plus-43screensize",     "320x240"            },
      /* MUST be answered: the global defaults to 0 in libretro.c, and
       * TextureCache::_checkCacheSize treats 0 as "always evict", calling
       * pop_back() on an EMPTY std::list. That deallocates the list's
       * embedded sentinel node = free() of an interior pointer inside the
       * TextureCache heap object, which corrupts jemalloc metadata and the
       * cache itself -> SIGSEGV in je_free at the logo screen (~1.3 s).
       * 1500 is RetroArch's default for this core. */
      { "mupen64plus-MaxTxCacheSize",   "1500"               },
      /* MUST be answered: both globals start at 0 in libretro.c:120-121 and
       * are only filled from GET_VARIABLE. emulate_game_controller_via_libretro.c:299
       * scales the stick radius by (astick_sensitivity / 100.0) — unanswered
       * means sensitivity 0, so the analogue stick is pinned to neutral while
       * every joypad bit (A/B/Z/Start) still works: Mario jumps and punches
       * but never walks. 15/100 are RetroArch's defaults for this core. */
      { "mupen64plus-astick-deadzone",    "15"  },
      { "mupen64plus-astick-sensitivity", "100" },
      { NULL, NULL }
   };
   int i;
   if (!cpu) { cpu = getenv("N64CPU"); if (!cpu || !*cpu) cpu = "-"; }
   if (!rsp) { rsp = getenv("N64RSP"); if (!rsp || !*rsp) rsp = "-"; }
   if (!strcmp(key, "mupen64plus-cpucore") && strcmp(cpu, "-")) return cpu;
   if (!strcmp(key, "mupen64plus-rsp-plugin") && strcmp(rsp, "-")) return rsp;
   if (!strcmp(key, "mupen64plus-rdp-plugin"))
      return g_gl_mode ? "gliden64" : "angrylion";
   for (i = 0; vars[i].k; i++) if (!strcmp(vars[i].k, key)) return vars[i].v;
   return NULL;
}

static bool cb_environ(unsigned cmd, void *data)
{
   switch (cmd) {
   case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
      struct retro_log_callback *lc = (struct retro_log_callback *)data;
      lc->log = core_logf;
      return 1;
   }
   case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
      enum retro_pixel_format *pf = (enum retro_pixel_format *)data;
      if (*pf == RETRO_PIXEL_FORMAT_XRGB8888) { g_pixfmt_565 = 0; LOG("n64play: core pixel format XRGB8888"); return 1; }
      if (*pf == RETRO_PIXEL_FORMAT_RGB565)   { g_pixfmt_565 = 1; LOG("n64play: core pixel format RGB565");   return 1; }
      LOG("n64play: WARNING unsupported pixel format %d", (int)*pf);
      return 0;
   }
   case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
   case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
      static const char *p;
      p = g_savedir;
      *(const char **)data = p;
      return 1;
   }
   case RETRO_ENVIRONMENT_GET_VARIABLE: {
      struct retro_variable *v = (struct retro_variable *)data;
      const char *val = v && v->key ? corevar(v->key) : NULL;
      if (!val) return 0;
      v->value = val;
      return 1;
   }
   case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *(bool *)data = false;
      return 1;
   case RETRO_ENVIRONMENT_SET_HW_RENDER: {
      /* glsm hands us its filled retro_hw_render_callback (GLES2 build =>
       * context_type OPENGLES2, depth=1 stencil=0). We own the EGL context
       * (created before retro_load_game, same thread) and one 640x480 FBO.
       * Do NOT call context_reset() here: for GLideN64 the FIRST reset is
       * what boots the core (libretro.c first_context_reset ->
       * emu_step_initialize), and the ROM data is only memcpy'd after this
       * env call. main() fires it right after retro_load_game returns. */
      struct retro_hw_render_callback *cb = (struct retro_hw_render_callback *)data;
      if (!g_gl_mode) return 0;
      LOG("n64play: SET_HW_RENDER type=%u v%u.%u depth=%d stencil=%d",
          (unsigned)cb->context_type, cb->version_major, cb->version_minor,
          (int)cb->depth, (int)cb->stencil);
      if (!n64gl_makecurrent()) { LOG("n64play: eglMakeCurrent failed"); return 0; }
      if (!cb->context_reset) { LOG("n64play: core sent no context_reset"); return 0; }
      /* fill THROUGH the pointer — glsm keeps this very struct (glsm.c
       * hw_render), not our copy */
      cb->get_current_framebuffer = hw_get_fb;
      cb->get_proc_address = hw_get_proc;
      g_hw = *cb;
      return 1;
   }
   case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
      return 0;
   case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      *(unsigned *)data = 2;
      return 1;
   case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
      return 0;
   case RETRO_ENVIRONMENT_SET_GEOMETRY:
      return 1;
   case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      return 1;
   case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
      *(bool *)data = false;
      return 1;
   case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE: {
      float *f = (float *)data; *f = (float)g_av_fps; return 1;
   }
   default:
      return 0;
   }
}

/* ------------------------------------------------------------------ WS core */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static int ws_handshake(int c)
{
   char line[512], key[128] = {0};
   int upgrade = 0;
   for (;;) {
      int n = get_line(c, line, sizeof(line));
      if (n < 0) return -1;
      if (n == 0) break;
      if (!strncasecmp(line, "GET", 3)) upgrade |= 1;
      else if (!strncasecmp(line, "Upgrade:", 8) && strcasestr(line, "websocket")) upgrade |= 2;
      else if (!strncasecmp(line, "Sec-WebSocket-Key:", 18)) {
         char *p = line + 18;
         while (*p == ' ') p++;
         snprintf(key, sizeof(key), "%s", p);
      }
   }
   if (!(upgrade & 3) || !key[0]) {
      static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nn64play\n";
      put_all(c, ok, sizeof(ok) - 1);
      return -1;
   }
   {
      char cat[256], b64out[32], resp[256];
      sha1_ctx sc; uint8_t digest[20]; int len;
      snprintf(cat, sizeof(cat), "%s%s", key, WS_GUID);
      sha1_init(&sc); sha1_update(&sc, (uint8_t *)cat, strlen(cat)); sha1_final(&sc, digest);
      b64(digest, 20, b64out);
      len = snprintf(resp, sizeof(resp),
         "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
         "Sec-WebSocket-Accept: %s\r\n\r\n", b64out);
      return put_all(c, resp, len);
   }
}

static int ws_send_text(int c, const char *s)
{
   uint8_t hdr[16]; size_t n = strlen(s); int h = 2;
   if (n < 126) { hdr[0] = 0x81; hdr[1] = (uint8_t)n; h = 2; }
   else if (n < 65536) {
      hdr[0] = 0x81; hdr[1] = 126; hdr[2] = n >> 8; hdr[3] = n & 0xff; h = 4;
   } else return -1;
   if (put_all(c, hdr, h)) return -1;
   return put_all(c, s, n);
}

static int ws_send_frame(int c)
{
   uint8_t ws[10];
   uint8_t md[8] = { 'M','D','1',0, (uint8_t)(g_fb_w >> 8), (uint8_t)g_fb_w, (uint8_t)(g_fb_h >> 8), (uint8_t)g_fb_h };
   uint64_t pixels = (uint64_t)g_fb_pitch * g_fb_h;
   uint64_t total = pixels + 8;
   int i;
   if (!g_fb_w || !g_fb_h) return 0;
   ws[0] = 0x82; ws[1] = 0x7f;
   for (i = 0; i < 8; i++) ws[2 + i] = (uint8_t)(total >> (56 - 8 * i));
   if (g_nows) return 0;
   if (g_fb_dummy) {
      uint8_t tiny[16] = { 'M','D','1',0,0,0,0,0, 0,0,0,0,0,0,0,0 };
      if (put_all(c, ws, 10)) return -1;
      return put_all(c, tiny, 16);
   }
   if (put_all(c, ws, 10)) return -1;
   if (put_all(c, md, 8)) return -1;
   return put_all(c, g_fb, (size_t)pixels);
}

static uint8_t g_acc[4096];
static int     g_acclen;

static int drain_input(int c)
{
   ssize_t r;
   if (g_acclen >= (int)sizeof(g_acc)) return -1;
   r = recv(c, g_acc + g_acclen, sizeof(g_acc) - g_acclen, MSG_DONTWAIT);
   if (r > 0) g_acclen += (int)r;
   else if (r == 0) return -1;
   else if (!(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return -1;

   for (;;) {
      unsigned op, len, hl, i;
      uint8_t *pl, mask[4], tmp[256];
      if (g_acclen < 2) break;
      op = g_acc[0] & 0x0f;
      if (!(g_acc[1] & 0x80)) return -1;
      len = g_acc[1] & 0x7f;
      hl = 2;
      if (len == 126) { if (g_acclen < 4) break; len = (g_acc[2] << 8) | g_acc[3]; hl = 4; }
      else if (len == 127) return -1;
      if (len > sizeof(tmp)) return -1;
      memcpy(mask, g_acc + hl, 4); hl += 4;
      if (g_acclen < (int)(hl + len)) break;
      pl = g_acc + hl;
      for (i = 0; i < len; i++) tmp[i] = pl[i] ^ mask[i & 3];
      if (op == 0x2 && len >= 1) {
         uint16_t np = len >= 2 ? (uint16_t)(tmp[0] | (tmp[1] << 8)) : (uint16_t)tmp[0];
         if (np != g_pad) {
            g_pad = np;
            LOG("n64play: pad %04x", (unsigned)np);   /* app-vs-core input trace */
         }
      }
      else if (op == 0x8) return -1;
      else if (op == 0x9) { uint8_t pong[2] = { 0x8a, 0x00 }; if (put_all(c, pong, 2)) return -1; }
      else if (op == 0x1 && len == 1 && tmp[0] == 'R') {
         retro_reset();
         LOG("n64play: client requested reset");
      }
      else if (op == 0x1 && len == 2 && tmp[0] == 'A') {
         g_audio_on = tmp[1] == '1';
         LOG("n64play: audio %s", g_audio_on ? "on" : "off");
      }
      g_acclen -= (int)(hl + len);
      if (g_acclen) memmove(g_acc, g_acc + hl + len, g_acclen);
   }
   return 0;
}

/* ---------------------------------------------------------------- session */
#define PROBE(tag) do { if (g_probe) { LOG("n64play: stage " tag " it=%ld", (long)it); } } while (0)

static int session(int c)
{
   char hello[64];
   int64_t t0, fps_t, frames = 0, it = 0;
   int64_t acc_run = 0, acc_send = 0, acc_cap = 0, acc_n = 0;
   struct timeval tv = { 6, 0 };
   int one = 1;

   setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
   setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

   if (ws_handshake(c)) { LOG("n64play: handshake failed/short"); return 0; }
   snprintf(hello, sizeof(hello), "MD1 %u %u %.3f %u", g_av_w, g_av_h, g_av_fps, g_sr);
   if (ws_send_text(c, hello)) { LOG("n64play: hello send failed"); return 0; }
   LOG("n64play: client attached, hello '%s'", hello);

   g_acclen = 0;
   g_pad = 0;
   g_wsc = c;
   g_audio_on = 1;
   g_audio_err = 0;
   t0 = fps_t = now_us();
   {
      int64_t period = (int64_t)(1000000.0 / g_av_fps + 0.5);
      int64_t deadline = t0 + period;
      for (;;) {
         if (drain_input(c)) { LOG("n64play: client gone at it=%ld", (long)it); g_wsc = -1; return 0; }
         {
            int64_t r0 = now_us();
            retro_run();
            acc_run += now_us() - r0;
         }
         if (g_audio_err) { LOG("n64play: audio send failed at it=%ld", (long)it); g_wsc = -1; return -1; }
         PROBE("B-run");
         if (g_frame_ready) {
            int64_t s0;
            g_frame_ready = 0;
            s0 = now_us();
            if (ws_send_frame(c)) { LOG("n64play: send failed at it=%ld", (long)it); g_wsc = -1; return -1; }
            acc_send += now_us() - s0;
            frames++;
         }
         acc_n++;
         it++;
         if (it <= 3 || it % 30 == 0) {
            int64_t nw = now_us();
            double fel = (nw - fps_t) / 1e6;
            LOG("n64play: it=%ld frames=%ld fps=%.1f w=%u h=%u el=%.2fs audio_batch=%u run=%.1fms cap=%.1fms send=%.1fms",
                (long)it, (long)frames, fel > 0 ? frames / fel : 0.0, g_fb_w, g_fb_h,
                (nw - t0) / 1e6, g_audio_dropped,
                acc_n ? acc_run / 1000.0 / acc_n : 0.0,
                acc_n ? (g_cap_us - acc_cap) / 1000.0 / acc_n : 0.0,
                acc_n ? acc_send / 1000.0 / acc_n : 0.0);
            acc_cap = g_cap_us;
            acc_run = acc_send = acc_n = 0;
            fps_t = nw; frames = 0;
         }
         {
            struct timespec w;
            int64_t nw;
            w.tv_sec = deadline / 1000000;
            w.tv_nsec = (deadline % 1000000) * 1000;
            clock_nanosleep(CLOCK_MONOTONIC, 1 /*TIMER_ABSTIME*/, &w, NULL);
            deadline += period;
            nw = now_us();
            if (deadline < nw) deadline = nw + period;
         }
      }
   }
}

/* ------------------------------------------------------------------- main */
static int tcp_listen(int port)
{
   int fd, one = 1;
   struct sockaddr_in a;
   fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) { perror("socket"); exit(1); }
   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_port = htons(port);
   a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   if (bind(fd, (struct sockaddr *)&a, sizeof(a))) { perror("bind"); exit(1); }
   if (listen(fd, 1)) { perror("listen"); exit(1); }
   return fd;
}

int main(int argc, char **argv)
{
   const char *rom;
   int port = 8081, fd, c;
   struct retro_game_info gi;
   uint8_t *romdata;
   FILE *f;
   long sz;

   signal(SIGPIPE, SIG_IGN);
   install_crash_logging();
   /* flags anywhere: strip --no-gl before positional parse */
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "--no-gl")) {
         g_gl_mode = 0;
         for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
         argc--; i--;
      }
   { const char *e = getenv("N64GL"); if (e && *e == '0') g_gl_mode = 0; }
   if (argc < 2) { fprintf(stderr, "usage: %s [--no-gl] <rom.z64> [port]\n", argv[0]); return 1; }
   rom = argv[1];
   if (argc > 2) port = atoi(argv[2]);
   {
      const char *slash = strrchr(rom, '/');
      size_t dlen = slash ? (size_t)(slash - rom) : (size_t)0;
      if (dlen >= sizeof(g_savedir_buf)) dlen = 0;
      memcpy(g_savedir_buf, rom, dlen);
      g_savedir_buf[dlen] = 0;
      g_savedir = g_savedir_buf;
      size_t rl = strlen(rom), bl = rl;
      if (slash) { const char *dot = strrchr(slash + 1, '.'); if (dot) bl = (size_t)(dot - rom); }
      if (bl + 5 >= sizeof(g_srm_path)) bl = 0;
      memcpy(g_srm_path, rom, bl);
      memcpy(g_srm_path + bl, ".srm", 5);
   }
   { const char *sd = getenv("N64PLAY_SAVE"); if (sd && *sd) g_savedir = sd; }
   { const char *e = getenv("N64FB");  if (e && *e == '1') g_fb_dummy = 1; }
   { const char *e = getenv("N64NOWS"); if (e && *e == '1') g_nows = 1; }
   { const char *e = getenv("N64PROBE"); if (e && *e == '1') g_probe = 1; }

   if (g_gl_mode) {
      if (n64gl_init()) LOG("n64play: RDP=glide64 (GLES2 on GPU)");
      else { LOG("n64play: GL init FAILED -> RDP=angrylion (software)"); g_gl_mode = 0; }
   } else LOG("n64play: RDP=angrylion (--no-gl)");

   f = fopen(rom, "rb");
   if (!f) { perror("open rom"); return 1; }
   fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > 67108864) { fprintf(stderr, "bad rom size %ld\n", sz); return 1; }
   romdata = (uint8_t *)malloc(sz);
   if (!romdata || fread(romdata, 1, sz, f) != (size_t)sz) { fprintf(stderr, "rom read failed\n"); return 1; }
   fclose(f);

   retro_set_environment(cb_environ);
   retro_set_video_refresh(cb_refresh);
   retro_set_audio_sample(cb_audio_single);
   retro_set_audio_sample_batch(cb_audio_batch);
   retro_set_input_state(cb_input_state);
   retro_set_input_poll(cb_input_poll);
   retro_init();

   memset(&gi, 0, sizeof(gi));
   gi.path = rom;
   gi.data = romdata;
   gi.size = (size_t)sz;
   if (!retro_load_game(&gi)) { fprintf(stderr, "retro_load_game failed\n"); return 1; }

   /* GL boot: first context_reset runs glsm symbol resolution + GLideN64
    * InitiateGFX + emu_step_initialize (core defers init for GLideN64,
    * libretro.c:1992). Must happen BEFORE the SRAM restore below — RAM
    * buffers only exist after init. */
   if (g_gl_mode) {
      if (!n64gl_makecurrent() || !g_hw.context_reset) {
         LOG("n64play: GL boot impossible (hw_render not registered?)");
         return 1;
      }
      g_hw.context_reset();
      LOG("n64play: GL RDP booted: %s | %s",
          n64gl_string(0x1F02 /*GL_VERSION*/), n64gl_string(0x1F01 /*GL_RENDERER*/));
   }

   retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

   {
      struct retro_system_av_info av;
      retro_get_system_av_info(&av);
      g_av_w = av.geometry.base_width;
      g_av_h = av.geometry.base_height;
      g_av_fps = av.timing.fps;
      g_sr = (unsigned)av.timing.sample_rate;
      LOG("n64play: av %ux%u fps=%.3f audio=%u rom=%ldB", g_av_w, g_av_h, g_av_fps, g_sr, sz);
   }

   /* restore cart-save blob if present (mempak lives in <romdir>/<Game>.mp1,
    * written by the core itself — nothing to do there) */
   {
      size_t msz = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
      uint8_t *m = (uint8_t *)retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
      if (msz && m) {
         int sfd = open(g_srm_path, O_RDONLY);
         if (sfd >= 0) {
            ssize_t got = 0, r;
            while ((size_t)got < msz && (r = read(sfd, m + got, msz - got)) > 0) got += r;
            close(sfd);
            LOG("n64play: srm in %zd/%zu B from %s", got, msz, g_srm_path);
         }
      }
   }
   g_srm_ready = 1;
   signal(SIGTERM, sram_flush_handler);
   signal(SIGINT, sram_flush_handler);
   LOG("n64play: srm -> %s", g_srm_path);

   fd = tcp_listen(port);
   LOG("n64play: listening on 127.0.0.1:%d", port);
   for (;;) {
      c = accept(fd, NULL, NULL);
      if (c < 0) { if (errno == EINTR) continue; perror("accept"); break; }
      session(c);
      close(c);
      g_pad = 0;
      g_acclen = 0;
      LOG("n64play: detached, idle");
   }
   retro_unload_game();
   retro_deinit();
   return 0;
}
