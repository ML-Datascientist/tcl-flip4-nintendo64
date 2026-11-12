/* n64svc — root helper for the KaiOS N64 app (TCL Flip 4 / KaiOS 4).
 *
 * There is no way for a hosted web app to spawn a process, so this daemon
 * does the only privileged work the app needs:
 *
 *   GET /roms                 -> {"dir":"…","roms":["a.z64",…]}
 *   GET /start?rom=<name>     -> kill any running emulator, spawn it detached
 *   GET /stop                 -> kill the emulator
 *   GET /status               -> {"running":0|1,"rom":"…"}
 *   GET /log?m=<text>         -> append a line to /data/local/tmp/n64.log
 *   GET /health               -> "ok"
 *
 * It listens on 127.0.0.1 only, so nothing outside the phone can reach it
 * (the app runs at http://n64.localhost and talks to 127.0.0.1:8090).
 * Every reply carries Access-Control-Allow-Origin: * because the page origin
 * differs from the service origin.
 *
 * Build (see build.sh): NDK clang, armv7, dynamic bionic.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_ROMDIR "/data/media/roms"
#define SVC_BIN        "/data/local/n64/n64play_gl"
#define EMU_PORT       "8081"
#define EMU_LOG        "/data/local/tmp/n64play_gl.log"
#define APP_LOG        "/data/local/tmp/n64.log"
#define LASTRUM        "/data/local/n64/.lastrom"

static char romdir[1024] = DEFAULT_ROMDIR;
static char lastrom[256];

/* ------------------------------------------------------------------ util */
static void urldecode(char *s)
{
   char *o = s;
   for (char *p = s; *p; p++) {
      if (*p == '+') { *o++ = ' '; continue; }
      if (*p == '%' && p[1] && p[2]) {
         int hi = p[1] <= '9' ? p[1] - '0' : (p[1] | 32) - 'a' + 10;
         int lo = p[2] <= '9' ? p[2] - '0' : (p[2] | 32) - 'a' + 10;
         if (hi >= 0 && hi < 16 && lo >= 0 && lo < 16) {
            *o++ = (char)((hi << 4) | lo);
            p += 2;
            continue;
         }
      }
      *o++ = *p;
   }
   *o = 0;
}

static void reply(int fd, int code, const char *ctype, const char *body, size_t len)
{
   char h[512];
   const char *reason = code == 200 ? "OK" : code == 204 ? "No Content" :
                        code == 400 ? "Bad Request" : "Internal Error";
   int n = snprintf(h, sizeof h,
      "HTTP/1.0 %d %s\r\nServer: n64svc\r\nContent-Type: %s\r\n"
      "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: *\r\n"
      "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
      "Cache-Control: no-store\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
      code, reason, ctype, len);
   if (write(fd, h, (size_t)n) < 0) return;
   if (len) { size_t off = 0; while (off < len) {
      ssize_t k = write(fd, body + off, len - off);
      if (k <= 0) break;
      off += (size_t)k;
   } }
}

static int rom_ext_ok(const char *n)
{
   const char *dot = strrchr(n, '.');
   if (!dot) return 0;
   return !strcasecmp(dot, ".z64") || !strcasecmp(dot, ".n64") || !strcasecmp(dot, ".v64");
}

static int read_comm(pid_t pid, char *out, size_t cap)
{
   char p[64];
   snprintf(p, sizeof p, "/proc/%d/comm", (int)pid);
   int f = open(p, O_RDONLY);
   if (f < 0) return 0;
   ssize_t n = read(f, out, cap - 1);
   close(f);
   if (n <= 0) return 0;
   out[n] = 0;
   char *nl = strchr(out, '\n');
   if (nl) *nl = 0;
   return 1;
}

/* kill every process whose comm matches (the emulator is root-owned too) */
static int kill_named(const char *name)
{
   DIR *d = opendir("/proc");
   if (!d) return 0;
   int killed = 0;
   struct dirent *de;
   while ((de = readdir(d))) {
      if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
      pid_t pid = (pid_t)atoi(de->d_name);
      if (pid <= 1 || pid == getpid()) continue;
      char comm[64];
      if (!read_comm(pid, comm, sizeof comm)) continue;
      if (!strcmp(comm, name)) { kill(pid, SIGTERM); killed++; }
   }
   closedir(d);
   return killed;
}

static int emu_running(void)
{
   DIR *d = opendir("/proc");
   if (!d) return 0;
   int found = 0;
   struct dirent *de;
   while ((de = readdir(d))) {
      if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
      char comm[64];
      if (!read_comm((pid_t)atoi(de->d_name), comm, sizeof comm)) continue;
      if (!strcmp(comm, "n64play_gl")) { found = 1; break; }
   }
   closedir(d);
   return found;
}

/* --------------------------------------------------------------- spawner */
static int spawn_emu(const char *rom)
{
   char path[1200];
   snprintf(path, sizeof path, "%s/%s", romdir, rom);

   pid_t p = fork();
   if (p < 0) return -1;
   if (p == 0) {
      setsid();
      int dn = open("/dev/null", O_RDWR);
      int lg = open(EMU_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (dn >= 0) dup2(dn, 0);
      if (lg >= 0) { dup2(lg, 1); dup2(lg, 2); } else if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); }
      /* HLE RSP: cxd4 costs 12-18 ms/frame on this SoC and pins SM64 at
       * 26-32 fps; HLE measures 60 fps and matches LLE on static screens. */
      setenv("N64RSP", "hle", 1);
      chdir("/data/local/n64");
      char *argv[] = { "n64play_gl", path, EMU_PORT, NULL };
      execv(SVC_BIN, argv);
      _exit(127);
   }
   return (int)p;
}

static void save_lastrom(const char *rom)
{
   int f = open(LASTRUM, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (f >= 0) { if (write(f, rom, strlen(rom)) < 0) {} close(f); }
}

static void load_lastrom(void)
{
   int f = open(LASTRUM, O_RDONLY);
   if (f < 0) return;
   ssize_t n = read(f, lastrom, sizeof lastrom - 1);
   close(f);
   if (n > 0) lastrom[n] = 0;
}

/* ------------------------------------------------------------- endpoints */
static void do_roms(int c)
{
   DIR *d = opendir(romdir);
   static char out[8192];
   size_t o = 0;
   if (!d) {
      o = (size_t)snprintf(out, sizeof out, "{\"dir\":\"%s\",\"error\":\"opendir\",\"roms\":[]}", romdir);
      reply(c, 200, "application/json", out, o);
      return;
   }
   struct dirent *de;
   int first = 1;
   o = (size_t)snprintf(out, sizeof out, "{\"dir\":\"%s\",\"roms\":[", romdir);
   while ((de = readdir(d))) {
      if (de->d_name[0] == '.') continue;
      if (!rom_ext_ok(de->d_name)) continue;
      if (o + strlen(de->d_name) * 2 + 8 >= sizeof out) break;
      o += (size_t)snprintf(out + o, sizeof out - o, "%s\"%s\"", first ? "" : ",", de->d_name);
      first = 0;
   }
   closedir(d);
   o += (size_t)snprintf(out + o, sizeof out - o, "],\"running\":%d}", emu_running());
   reply(c, 200, "application/json", out, o);
}

static void do_start(int c, char *rom)
{
   char msg[256];
   if (!rom || !*rom || strchr(rom, '/') || strstr(rom, "..") || !rom_ext_ok(rom)) {
      reply(c, 400, "text/plain", "bad rom name\n", 13);
      return;
   }
   if (kill_named("n64play_gl")) usleep(700 * 1000);
   int pid = spawn_emu(rom);
   if (pid < 0) {
      reply(c, 500, "text/plain", "spawn failed\n", 13);
      return;
   }
   snprintf(lastrom, sizeof lastrom, "%s", rom);
   save_lastrom(rom);
   snprintf(msg, sizeof msg, "started pid=%d rom=%s\n", pid, rom);
   reply(c, 200, "text/plain", msg, strlen(msg));
}

static void do_status(int c)
{
   char out[512];
   int n = snprintf(out, sizeof out, "{\"running\":%d,\"rom\":\"%s\"}", emu_running(), lastrom);
   reply(c, 200, "application/json", out, (size_t)n);
}

static void do_log(int c, char *m)
{
   int f = open(APP_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
   if (f >= 0) {
      if (write(f, m, strlen(m)) < 0) {}
      if (write(f, "\n", 1) < 0) {}
      close(f);
   }
   reply(c, 200, "text/plain", "ok", 2);
}

int main(int argc, char **argv)
{
   int port = argc > 1 ? atoi(argv[1]) : 8090;
   if (argc > 2 && argv[2][0]) snprintf(romdir, sizeof romdir, "%s", argv[2]);
   load_lastrom();
   signal(SIGPIPE, SIG_IGN);
   signal(SIGCHLD, SIG_IGN);         /* reaped by init/ignored; no zombies */

   int s = socket(AF_INET, SOCK_STREAM, 0);
   if (s < 0) return 1;
   int one = 1;
   setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
   struct sockaddr_in a;
   memset(&a, 0, sizeof a);
   a.sin_family = AF_INET;
   a.sin_port = htons((uint16_t)port);
   a.sin_addr.s_addr = htonl(0x7f000001);   /* loopback only */
   if (bind(s, (struct sockaddr *)&a, sizeof a)) { return 2; }
   if (listen(s, 16)) return 3;

   for (;;) {
      int c = accept(s, 0, 0);
      if (c < 0) { if (errno == EINTR) continue; break; }
      struct timeval tv = { 3, 0 };
      setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
      setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

      char req[8192];
      size_t rn = 0;
      while (rn < sizeof req - 1) {
         ssize_t k = read(c, req + rn, sizeof req - 1 - rn);
         if (k <= 0) break;
         rn += (size_t)k;
         req[rn] = 0;
         if (strstr(req, "\r\n\r\n")) break;
      }
      if (!rn) { close(c); continue; }
      req[rn] = 0;

      char *sp = strchr(req, ' ');
      if (!sp) { close(c); continue; }
      char *us = sp + 1;
      char *e = strchr(us, ' ');
      if (!e) { close(c); continue; }
      size_t ul = (size_t)(e - us);
      if (ul > 1023) ul = 1023;
      char url[1024];
      memcpy(url, us, ul);
      url[ul] = 0;

      if (!strcasecmp(req, "OPTIONS") || strstr(req, "OPTIONS /")) {
         reply(c, 204, "text/plain", "", 0);
      } else if (!strncmp(url, "/roms", 5)) {
         do_roms(c);
      } else if (!strncmp(url, "/start?", 7)) {
         char *q = url + 7;
         if (!strncmp(q, "rom=", 4)) q += 4;
         char *amp = strchr(q, '&');
         if (amp) *amp = 0;
         urldecode(q);
         do_start(c, q);
      } else if (!strncmp(url, "/stop", 5)) {
         int k = kill_named("n64play_gl");
         char b[32];
         int n = snprintf(b, sizeof b, "stopped %d\n", k);
         reply(c, 200, "text/plain", b, (size_t)n);
      } else if (!strncmp(url, "/status", 7)) {
         do_status(c);
      } else if (!strncmp(url, "/log?", 5)) {
         char *m = url + 5;
         char *amp = strchr(m, '&');
         if (amp) *amp = 0;
         urldecode(m);
         do_log(c, m);
      } else if (!strncmp(url, "/health", 7)) {
         reply(c, 200, "text/plain", "ok", 2);
      } else {
         reply(c, 400, "text/plain", "not found\n", 10);
      }
      close(c);
   }
   return 0;
}
