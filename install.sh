#!/bin/sh
# Install the N64 app on a rooted TCL Flip 4 (KaiOS 4 / Android 14).
#
#   adb root            # once per boot; the phone must be rooted
#   ./install.sh        # optional: ROMDIR=/data/media/roms
#
# What it does
#   1. pushes the emulator daemon + helper service to /data/local/n64
#   2. hooks the boot chain so the helper service survives reboots
#   3. installs the web app (http://n64.localhost) into the phone's app DB
#   4. grants the app wakeLock/vibration permissions
#   5. restarts b2g so the app appears in the launcher
#
# Re-running it is safe: every step is idempotent.
set -e
cd "$(dirname "$0")"

APP=n64
ROMDIR=${ROMDIR:-/data/media/roms}
DATADIR=/data/local/n64
HOOK=/data/local/service/updater

adb shell id | grep -q 'uid=0' || {
  echo "need root adb: run 'adb root' (and 'adb wait-for-device')"; exit 1; }

# ---------------------------------------------------------------- 1. binaries
if [ ! -x prebuilt/armv7/n64play_gl ] || [ ! -x prebuilt/armv7/n64svc ] || [ ! -x prebuilt/armv7/n64boot ]; then
  echo "== building helpers (n64svc, n64boot)"
  ./build.sh
fi
[ -x prebuilt/armv7/n64play_gl ] || {
  echo "missing prebuilt/armv7/n64play_gl — build it with daemon/build_gl.sh"; exit 1; }

echo "== pushing binaries to $DATADIR"
adb shell "mkdir -p $DATADIR/Mupen64plus $ROMDIR"
adb push prebuilt/armv7/n64play_gl prebuilt/armv7/n64svc prebuilt/armv7/n64boot "$DATADIR/" >/dev/null
adb push daemon/mupen64plus.ini "$DATADIR/Mupen64plus/" >/dev/null
adb shell "chmod 755 $DATADIR/n64play_gl $DATADIR/n64svc $DATADIR/n64boot
           chmod 644 $DATADIR/Mupen64plus/mupen64plus.ini
           chcon u:object_r:updater_data_file:s0 $DATADIR/n64play_gl $DATADIR/n64svc $DATADIR/n64boot
           chcon u:object_r:updater_data_file:s0 $DATADIR/Mupen64plus/mupen64plus.ini"

# -------------------------------------------------------------- 2. boot hook
# updater-daemon is exec'd from /data at boot; n64boot starts the helper
# service (and the terminal daemon, if that app is installed) before handing
# over to the original binary.
echo "== installing boot hook"
adb shell "cd $HOOK
           [ -f updater-daemon.real ] || cp -f updater-daemon updater-daemon.real
           chmod 755 updater-daemon.real
           chcon u:object_r:updater_data_file:s0 updater-daemon.real"
adb push prebuilt/armv7/n64boot "$HOOK/updater-daemon.new" >/dev/null
adb shell "cd $HOOK
           cp -f updater-daemon.new updater-daemon
           rm -f updater-daemon.new
           chmod 755 updater-daemon
           chcon u:object_r:updater_data_file:s0 updater-daemon"

# start the helper now so the app works without a reboot
echo "== starting n64svc"
adb shell "(setsid $DATADIR/n64svc 8090 '$ROMDIR' >/dev/null 2>&1 </dev/null &) ; sleep 1"
adb forward tcp:18090 tcp:8090 >/dev/null
curl -s -m 4 http://127.0.0.1:18090/health | grep -q ok || {
  echo "n64svc did not come up — check: adb shell ps -A | grep n64svc"; exit 1; }

# ------------------------------------------------------------- 3. web app
echo "== packaging app"
./pack.sh
VER=$(python3 -c "import json;print(json.load(open('app/manifest.webmanifest'))['version'])")

adb shell "mkdir -p /data/local/webapps/installed/$APP"
adb push n64.zip "/data/local/webapps/installed/$APP/application.zip" >/dev/null
adb shell "chmod 600 /data/local/webapps/installed/$APP/application.zip
           chcon u:object_r:b2g_data_file:s0 /data/local/webapps/installed/$APP/application.zip
           ln -sfn /data/local/webapps/installed/$APP /data/local/webapps/vroot/$APP"

echo "== registering app in apps.sqlite (v$VER)"
adb shell 'setprop ctl.stop api-daemon'; sleep 2
adb pull /data/local/webapps/db/apps.sqlite /tmp/n64-apps.sqlite >/dev/null 2>&1 || true
VER=$VER python3 - <<'PY'
import os, sqlite3, time
db = sqlite3.connect('/tmp/n64-apps.sqlite')
db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
t = int(time.time() * 1000)
db.execute("""insert or replace into apps
 (name,version,removable,manifest_url,update_manifest_url,update_url,preloaded,status,
  install_state,update_state,install_time,update_time,manifest_hash,package_hash,manifest_etag,deeplinks)
 values (?,?,1,'http://n64.localhost/manifest.webmanifest','','',0,'Enabled','Installed','Idle',
         ?,?,'','','',NULL)""", ("n64", os.environ["VER"], t, t))
db.commit(); db.execute("PRAGMA wal_checkpoint(TRUNCATE)"); db.close()
PY
adb push /tmp/n64-apps.sqlite /data/local/webapps/db/apps.sqlite >/dev/null
adb shell 'rm -f /data/local/webapps/db/apps.sqlite-wal /data/local/webapps/db/apps.sqlite-shm
           chmod 600 /data/local/webapps/db/apps.sqlite
           chcon u:object_r:b2g_data_file:s0 /data/local/webapps/db/apps.sqlite
           setprop ctl.start api-daemon'

# --------------------------------------------------------- 4. permissions
# b2g caches permissions.sqlite; grant with b2g stopped.
echo "== granting permissions"
adb shell 'setprop ctl.stop b2g'; sleep 2
PROFILE=$(adb shell 'ls -d /data/b2g/mozilla/*.default 2>/dev/null | head -1' | tr -d '\r')
if [ -n "$PROFILE" ]; then
  adb pull "$PROFILE/permissions.sqlite" /tmp/n64-perms.sqlite >/dev/null 2>&1 || true
  python3 - <<'PY'
import sqlite3
db = sqlite3.connect('/tmp/n64-perms.sqlite')
origin = 'http://n64.localhost'
for t in ('wakeLock', 'power', 'vibration', 'lock-orientation', 'push',
          'networkstats-perm', 'storageAccessAPI'):
    if db.execute("select 1 from moz_perms where origin=? and type=?", (origin, t)).fetchone():
        continue
    db.execute("insert into moz_perms (origin,type,permission,expireType,expireTime,modificationTime)"
               " values (?,?,1,0,0,0)", (origin, t))
db.commit(); db.close()
PY
  adb push /tmp/n64-perms.sqlite "$PROFILE/permissions.sqlite" >/dev/null
  adb shell "chmod 600 $PROFILE/permissions.sqlite
             chcon u:object_r:b2g_data_file:s0 $PROFILE/permissions.sqlite
             rm -f $PROFILE/permissions.sqlite-wal $PROFILE/permissions.sqlite-shm"
else
  echo "   (no b2g profile found — skipping; wake-lock may not be granted)"
fi
adb shell 'setprop ctl.start b2g'; sleep 5

# ------------------------------------------------------------- 5. verify
echo "== verifying served app"
adb forward tcp:18880 tcp:80 >/dev/null
python3 - <<'PY'
import urllib.request, zlib, sys
try:
    r = urllib.request.Request('http://127.0.0.1:18880/n64.js',
                               headers={'Host': 'n64.localhost'})
    d = urllib.request.urlopen(r, timeout=8).read()
    try: t = zlib.decompress(d, -15).decode()
    except Exception: t = d.decode('utf-8', 'replace')
    print('   served n64.js:', len(t), 'bytes, boot const found:', 'var VER' in t)
except Exception as e:
    print('   serve check failed:', e, '(restart the phone and reopen the app)', file=sys.stderr)
PY
echo
echo "Done. ROMs go in $ROMDIR"
echo "Open the N64 app on the phone: list -> OK to play, Back to exit."
