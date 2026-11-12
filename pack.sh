#!/bin/sh
# Build the app package (n64.zip) from app/.
# Version comes from the manifest and is stamped into every URL/constant so
# Gecko's disk cache can never serve a stale script.
set -e
cd "$(dirname "$0")"
VER=$(python3 -c "import json;print(json.load(open('app/manifest.webmanifest'))['version'])")
sed -i -E "s/\?v=[0-9][0-9.]*/?v=$VER/g" app/index.html
sed -i -E "s/var VER = '[^']*'/var VER = '$VER'/" app/n64.js
rm -f n64.zip
( cd app && zip -q ../n64.zip manifest.webmanifest index.html style.css n64.js icon.png )
echo "n64.zip built: v$VER $(stat -c%s n64.zip) bytes"
