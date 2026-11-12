/* n64boot — boot hook installed as /data/local/service/updater/updater-daemon.
 *
 * The stock updater-daemon is exec'd from /data at boot. We stand in for it:
 *   1. start the N64 helper service (ROM list + emulator spawner),
 *   2. start the terminal daemon if that app is installed,
 *   3. hand over to the original updater-daemon (updater-daemon.real).
 *
 * Everything is fork+exec with a /dev/null stdio, errors are silent by design —
 * a boot hook that dies or blocks breaks OTA/updates on the device.
 *
 * Build: NDK clang armv7 (see build.sh).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void spawn(const char *path, char *const argv[])
{
   if (access(path, X_OK)) return;
   pid_t p = fork();
   if (p != 0) return;
   setsid();
   int dn = open("/dev/null", O_RDWR);
   if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
   execv(path, argv);
   _exit(127);
}

int main(int argc, char **argv)
{
   (void)argc;
   char *termd[] = { "termd", "8080", NULL };
   char *svc[]   = { "n64svc", "8090", NULL };

   spawn("/data/linux/bin/termd", termd);              /* ompterm terminal */
   spawn("/data/local/n64/n64svc", svc);               /* this app's helper */

   /* chain with our own argv, but make argv[0] the real binary's path: the
    * vendor daemon keys its own bookkeeping off argv[0]. */
   static const char real[] = "/data/local/service/updater/updater-daemon.real";
   if (argc > 0) argv[0] = (char *)real;
   execv(real, argv);
   return 127;
}
