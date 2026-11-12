#!/bin/sh
# Remove the N64 app, its helper service and the boot hook.
set -e
cd "$(dirname "$0")"
adb shell id | grep -q 'uid=0' || { echo "need root adb: run 'adb root'"; exit 1; }

echo "== stopping service and emulator"
adb shell '/data/local/n64/n64svc 2>/dev/null & sleep 0; killall n64play_gl 2>/dev/null
           cd /data/local/n64 && kill $(pidof n64svc) 2>/dev/null; true'

echo "== restoring boot hook"
adb shell 'cd /data/local/service/updater
           [ -f updater-daemon.real ] && cp -f updater-daemon.real updater-daemon
           chmod 755 updater-daemon
           chcon u:object_r:updater_data_file:s0 updater-daemon
           true'

echo "== deleting app row"
adb shell 'setprop ctl.stop api-daemon'; sleep 2
adb pull /data/local/webapps/db/apps.sqlite /tmp/n64-apps.sqlite >/dev/null 2>&1 || true
python3 - <<'PY' 2>/dev/null || true
import sqlite3
db = sqlite3.connect('/tmp/n64-apps.sqlite')
db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
db.execute("delete from apps where name='n64'")
db.commit(); db.execute("PRAGMA wal_checkpoint(TRUNCATE)"); db.close()
PY
adb push /tmp/n64-apps.sqlite /data/local/webapps/db/apps.sqlite >/dev/null 2>&1 || true
adb shell 'rm -f /data/local/webapps/db/apps.sqlite-wal /data/local/webapps/db/apps.sqlite-shm
           chmod 600 /data/local/webapps/db/apps.sqlite
           chcon u:object_r:b2g_data_file:s0 /data/local/webapps/db/apps.sqlite
           rm -rf /data/local/webapps/installed/n64 /data/local/webapps/vroot/n64
           setprop ctl.start api-daemon
           setprop ctl.restart b2g' || true

echo "== removing files"
adb shell 'rm -rf /data/local/n64; rm -f /data/local/tmp/n64.log /data/local/tmp/n64play_gl.log'
echo "Done. (Save files next to your ROMs were left untouched.)"
