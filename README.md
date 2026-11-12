# n64 — Nintendo 64 emulator app for the TCL Flip 4 (KaiOS 4)

A standalone KaiOS app that plays Nintendo 64 ROMs **on the GPU** on a rooted
TCL Flip 4 (T440W, KaiOS 4 / Android 14, Snapdragon 4 Gen 2 / Adreno 611,
32-bit armv7).

Measured on the device: **60 fps in Super Mario 64 gameplay**, 57–60 fps at the
title screen, with sound, driven by the phone keypad. The emulator core
(mupen64plus-libretro-nx) runs as a native root daemon and renders through
**GLideN64 / GLES3**; the app is the display and input surface.

```
 launcher → app (Gecko, http://n64.localhost)
                │  ▲  canvas frames (RGB565 over WebSocket) + key bitmask
                ▼  │
        n64svc (root, 127.0.0.1:8090)      ← ROM list, spawn/kill
                │
                ▼
        n64play_gl (root daemon)  ──► mupen64plus core + HLE RSP
                                    └─► GLideN64 GLES3 on the Adreno
```

There is no compositor on this phone, so a native process cannot draw to the
screen: the GPU benefit is inside the daemon, and the finished frame travels to
the app canvas. A 320×240 landscape frame is rotated to the phone's portrait
screen inside the app's pixel loop (no CSS transforms).

---

## Install

Requirements: a **rooted** TCL Flip 4 with `adb` access, and the phone connected
by USB. Root is kept across reboots only with the usual KaiOS root patches; if
`adb root` fails, re-root first.

```sh
git clone <this repo> && cd nintendo64_emulator
adb root && adb wait-for-device
./install.sh
```

`install.sh` pushes everything, registers the app, restarts the phone's UI and
verifies that the app is being served. It needs no build tools: the armv7
binaries are in `prebuilt/armv7/`.

Then copy ROMs to the phone:

```sh
adb push "Super Mario 64 (USA).z64" /data/media/roms/
```

Open **N64** in the app list, pick the ROM, press **OK**.

Uninstall with `./uninstall.sh` (your ROMs and save files are left alone).

---

## Controls

Hold the phone rotated **counter-clockwise** (keypad on the right, screen
upright) — the picture is rotated for that grip and the arrow cross follows it.

| key | N64 | action |
|---|---|---|
| arrows | Control Stick | move (full deflection = run) |
| `8` `4` `5` `6` | Control Stick | same four directions (orientation-free fallback) |
| `2` | A | jump |
| `1` | B | punch / attack |
| `3` or `#` | Z | crouch / ground pound |
| `7` / `9` | C-left / C-right | rotate camera |
| `*` | L | camera recentre |
| OK | Start | |
| `0` | | sound on/off (**starts enabled**) |
| Back | | stop the game, back to the ROM list |

Keys are read from `keydown`/`keyup` only, and a pad bit stays set until its key
is released: this keypad does **not** auto-repeat arrow keys (`n64.log` shows a
single `ArrowUp` for a whole hold), so any "hold the stick for N ms" pulse would
release it while the finger is still down and Mario would only twitch. Arrows
keep their `ArrowUp`/`ArrowLeft`/… names; `8/4/5/6` are the same four directions
for firmware that names them differently. `gk <key>-><bit> rep=<0|1> act=<focus>`
lines in the log show exactly what the firmware delivered.

The app deliberately contains **no editable element** — no `<input>`, no
`<textarea>`. On this KaiOS build a focused text field hands the dialer keys to
the system keyboard app, which inserts letters (`ime input data="a" …` in
`n64.log`) and turns arrows into caret movement. With no field in the document
every key arrives as a plain `keydown` on the window, nothing reaches the IME,
and no focus juggling is needed. `window blur` / page-hide releases every held
bit so a system overlay cannot leave the stick deflected.

Menu: arrows to move, **OK** to play, **`*`** to rescan the ROM folder,
**Back** to leave the app.

SM64 note: the intro dialogue swallows input — press **A** (`2`) to clear each
message page before the stick responds.

### Controls while the emulator starts

SM64's logo / intro flyby / file select swallow presses that arrive before each
screen is drawn, so a hand-started game looks unresponsive for the first minute.
The app replays the headless-proven sequence after the first connect: **Start at
16 s and 21 s, A at 29 s, 39 s and 49 s** (`auto-start armed` in the log). Press
the keys yourself at any time; the sequence only adds those five short taps.

The tapped bits are held for 350 ms, released by a timer only — nobody is holding
a key for them, so the latch described above does not apply.

### Sound

Sound is **on from launch**: tapping a ROM is the only user gesture Gecko
accepts, so the app creates its `AudioContext` there and sends `A1` as soon as
the socket opens; the daemon answers with PCM (735 stereo frames per batch,
`MDA` + 4-byte header). `0` toggles at any time — the app sends `A0`/`A1` on the
live socket, and a muted session receives no audio frames at all.

Audio never blocks video. The daemon writes every socket message non-blocking
and keeps two drop-oldest queues (a single video-frame slot and a 512 KiB PCM
ring): if the app falls behind, the newest frame replaces the pending one and
old PCM is discarded (`vdrop=`/`adrop=` in the daemon log) instead of the daemon
blocking on a full socket buffer and losing the session. The app decodes each
batch with one `Int16Array` view over the received buffer and keeps O(1) ring
bookkeeping, so a slow renderer costs dropped audio, not a dropped link.

### Screen timeout

The panel dims after ~15 s of no input (`screen.timeout` is handled by the
system app's `ScreenManager`). While a game is running the app holds a B2G
`screen` wake lock — `navigator.b2g.requestWakeLock('screen')`, the only lock
state that `ScreenWakeLockManager` accepts (`locked-foreground`) — and releases
it when you leave the game. The lock is re-taken when the page returns to the
foreground. Confirm with `grep wakelock /data/local/tmp/n64.log` →
`n64 wakelock b2g screen`.

---

## Where things go on the phone

| path | contents |
|---|---|
| `/data/media/roms` | ROMs (`*.z64`, `*.n64`, `*.v64`) and their `.srm` saves |
| `/data/local/n64` | `n64play_gl`, `n64svc`, `n64boot`, `Mupen64plus/mupen64plus.ini` |
| `/data/local/webapps/installed/n64` | app package (`application.zip`) |
| `/data/local/tmp/n64play_gl.log` | emulator log (fps, pad bits, errors) |
| `/data/local/tmp/n64.log` | app log (`boot v…`, key trace, audio state) |

Logs are the first place to look when something misbehaves:

```sh
adb shell 'tail -30 /data/local/tmp/n64play_gl.log'
adb shell 'grep -E "boot v|gk |fps" /data/local/tmp/n64.log'
```

---

## Performance on this hardware

| build | SM64 title | gameplay |
|---|---|---|
| software RDP (angrylion, older build) | 26–32 fps | 15–23 fps |
| **this build** (GLideN64 GLES3, HLE RSP) | 57–60 fps | **60 fps** |

Per-frame budget measured in the daemon: 13.3–13.8 ms core+render, 4.2–5.2 ms
capture (`glFinish` + `glReadPixels`), 0.7–1.1 ms send — i.e. it is paced by the
N64's own 60 Hz, not by the GPU.

RSP choice: **HLE** is the default because cxd4 (LLE) costs 12–18 ms/frame here
and pins the title screen at 26–32 fps; on the static screens compared, both
produce identical pictures. For accuracy-first runs:

```sh
adb shell 'killall n64play_gl; cd /data/local/n64 && (setsid env N64RSP=cxd4 ./n64play_gl "/data/media/roms/game.z64" 8081 >/tmp/lle.log 2>&1 </dev/null &)'
```

Software-RDP fallback (if the GPU path ever fails):
`env N64GL=0 ./n64play_gl <rom> 8081` or `./n64play_gl --no-gl <rom>`.

---

## What ships here

| path | what it is |
|---|---|
| `app/` | the KaiOS app itself (hand-edited, no build step) |
| `svc/`, `boot/` | two small C helpers: the ROM-list/spawn service and the boot hook |
| `daemon/` | the libretro frontend that drives the core (`n64play.c`, `n64gl.c`, `n64stubs.c`) + `build_gl.sh` |
| `prebuilt/armv7/` | armv7 PIE binaries `install.sh` pushes (no NDK needed to install) |
| `install.sh`, `uninstall.sh`, `pack.sh`, `build.sh` | deploy / remove / package / build helpers |

`prebuilt/armv7/n64play_gl` (5.6 MB, stripped, libstdc++ linked statically) is
built from the three local files in `daemon/` plus the upstream core. The
upstream snapshot it was built from is pinned by sha256 in
`daemon/nx.tar.gz.sha256`; `daemon/build_gl.sh` refuses any other tarball, so a
rebuild from `prebuilt/` sources is the same code. Everything local is in this
repo; the upstream half is
[mupen64plus-libretro-nx](https://github.com/libretro/mupen64plus-libretro-nx)
(GPL-2.0+) with [GLideN64](https://github.com/gonetz/GLideN64) (GPL-2.0), both
unmodified — see `LICENSE` for the licence text that covers this work.

The binaries are 32-bit ARM for the TCL Flip 4 (T440W) only; nothing here is
useful on another architecture.

## Build from source

The two helpers need only the Android NDK (tested r26d):

```sh
NDK=/path/to/android-ndk-r26d ./build.sh          # n64svc + n64boot → prebuilt/armv7
```

The emulator daemon additionally needs the upstream core sources, which
`daemon/build_gl.sh` fetches (mupen64plus-libretro-nx `develop` tarball):

```sh
NDK=/path/to/android-ndk-r26d ./daemon/build_gl.sh
```

The app package is plain files + zip:

```sh
./pack.sh      # n64.zip (version comes from app/manifest.webmanifest)
```

Local code is small and readable:

| file | role |
|---|---|
| `app/n64.js` | UI, WebSocket client, RGB565→canvas rotation, audio ring/resampler, key mapping |
| `app/index.html`, `app/style.css`, `app/manifest.webmanifest` | app shell + KaiOS manifest (hosted app `n64`, `core: true`) |
| `svc/n64svc.c` | root helper: `/roms`, `/start?rom=`, `/stop`, `/status`, `/log` |
| `boot/n64boot.c` | boot hook: starts the helper (and the terminal daemon if installed), then chains to `updater-daemon.real` |
| `daemon/n64play.c` | libretro frontend: WS server, input bitmask → N64 pad, frames, audio, core options |
| `daemon/n64gl.c` | EGL/GLES3 offscreen context + `glReadPixels` capture |
| `daemon/n64stubs.c` | stubs the core expects from its build system |

### Two fixes in the daemon worth knowing

Both are "the frontend did not answer a documented core option", and both are
in `n64play.c` `corevar()`:

1. `mupen64plus-MaxTxCacheSize=1500` — the core's global defaults to 0, which
   makes `TextureCache::_checkCacheSize()` evict from an **empty** list
   (`pop_back()` on a sentinel node → `free()` of an interior pointer). Symptom:
   SIGSEGV ~1.3 s in, on the Nintendo logo.
2. `mupen64plus-astick-sensitivity=100` / `-astick-deadzone=15` — sensitivity
   defaults to 0 and the controller plugin multiplies the stick radius by
   `sensitivity/100`, pinning the analogue stick at neutral. Symptom: A/B/Z work
   but Mario never walks.

C buttons come only from `RETRO_DEVICE_INDEX_ANALOG_RIGHT` in this core's
default mapping, so the frontend feeds that device from pad bits 12–15.

---

## Troubleshooting

| symptom | cause / fix |
|---|---|
| ROM list says "Service not running" | helper not up: `adb shell '/data/local/n64/n64svc 8090 /data/media/roms &'`, or reboot (boot hook) |
| app not in the launcher | re-run `./install.sh` (registers the app and restarts b2g) |
| keys do nothing | check `gk …->bit` lines in `n64.log`; the window must have focus (a system overlay steals it — the app releases all held bits on `blur`) |
| arrows move Mario only a step | every bit is held until `keyup`, so a hold walks; if the log shows one `gk ArrowUp->6` per press but Mario stops early, a stuck keyup came from an overlay — tap Back and reopen |
| low fps | check the daemon log; fps is limited by the N64 (60 Hz); if `run=` is large, another app is competing for CPU |
| no sound | check `grep -E "audio rate|no AudioContext" /data/local/tmp/n64.log`: `no AudioContext` means the gesture rule blocked it — reopen the game from the ROM list; `soundOn` toggling logs nothing, so press `0` twice and re-check the daemon for `audio on` |
| screen dims after ~15 s | wake lock not held: check `grep wakelock /data/local/tmp/n64.log`; re-run `./install.sh` (grants `wakeLock`/`power`) and restart the app |
| sound crackles / drops | daemon log shows `adrop=` growing: the phone is behind the stream, so old PCM is discarded on purpose (the game keeps running); `vdrop=` growing means whole frames are being skipped for the same reason |

---

## Credits and licence

* [mupen64plus-libretro-nx](https://github.com/libretro/mupen64plus-libretro-nx) —
  mupen64plus-next core, ARM dynarec, RSP plugins (GPL-2.0+)
* [GLideN64](https://github.com/gonetz/GLideN64) — GLES3 RDP (GPL-2.0)
* angrylion RDP — built in as the `--no-gl` fallback (GPL-2.0)
* KaiOS app packaging follows the hosted-app layout (`<name>.localhost`,
  `apps.sqlite`) of the TCL Flip 4 firmware.

This project is distributed under the **GPL-2.0** (see `LICENSE`); the prebuilt
`prebuilt/armv7/n64play_gl` was built from the sources in `daemon/` plus the
upstream tree fetched by `daemon/build_gl.sh`.

No ROMs are included. Use only ROMs you are legally entitled to.
