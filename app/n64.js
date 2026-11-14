/* n64.js — Nintendo 64 front end for KaiOS (TCL Flip 4, KaiOS 4).
 *
 * Talks to n64svc (http://127.0.0.1:8090, installed as a boot service):
 *   GET /roms                 {"dir":"…","roms":[…],"running":0|1}
 *   GET /start?rom=<name>     spawn the emulator (kills an old one first)
 *   GET /stop                 kill it
 *   GET /log?m=<text>         append to /data/local/tmp/n64.log
 *
 * and to the emulator itself (ws://127.0.0.1:8081):
 *   hello  TEXT "MD1 w h fps samplerate"
 *   video  BINARY 'M','D','1',0, w16, h16 + RGB565 LE rows
 *   audio  BINARY 'M','D','A',0 + stereo int16 LE
 *   input  BINARY 2-byte LE bitmask (bit0 '1'=B, bit1 '2'=A, bit4..7 stick,
 *           bit8 '3'=Z, bit10 R (SoftLeft), bit11 '#'=Z, bit12 '7'=C-left,
 *           bit13 '9'=C-right; SoftRight is the save/load menu, not a pad bit)
 *   cmd    TEXT "A1"/"A0" audio on/off
 *          TEXT "L" list state slots   -> TEXT "SL <mtime1>..<mtime5>" (0=empty)
 *          TEXT "S<n>"/"G<n>" save/load -> TEXT "SV|LD <n> ok|err"
 *
 * No CSS transforms, no C-side rotation: the picture is rotated inside the
 * RGB565->RGBA lookup loop (this Gecko misrenders transformed canvases).
 * Orientation is a front-end choice only (right soft key -> Orientation):
 * landscape = phone held sideways, the rotated 240x320 view; portrait = phone
 * upright, the 320x240 frame scaled to 240x180 and letterboxed. Only the
 * arrow cross follows the orientation.
 */
(function () {
  'use strict';
  var API = 'http://127.0.0.1:8090';
  var EWS = 'ws://127.0.0.1:8081/';
  var VER = '1.2.0';                 /* pack.sh rewrites to manifest version */

  var menuEl, listEl, gameEl, gstat, cv, ctx;
  var mode = 'menu';
  var ws = null, connectTimer = 0, fails = 0;
  var rom = '', roms = [];
  var lut = null, img = null, u32 = null;
  var colmap = null, rowmap = null, colW = 0, rowH = 0;
  var CW = 240, CH = 320;
  var pad = 0, frames = 0, statT = 0, curFps = 0, fw = 0, fh = 0;
  var rafPending = false, statHide = 0;

  function $(id) { return document.getElementById(id); }
  function log(m) { try { fetch(API + '/log?m=' + encodeURIComponent('n64 ' + m), { cache: 'no-store' }); } catch (e) {} }
  window.onerror = function (m, s, l) { log('ERR ' + m + ' @' + l); };
  log('boot v' + VER);

  /* ---------------------------------------------------------------- menu */
  function showMenu() {
    mode = 'menu';
    autoCancel();
    document.body.className = '';
    setStatus('');
    loadRoms();
  }

  function loadRoms() {
    listEl.textContent = '';
    listEl.appendChild(note('loading…'));
    fetch(API + '/roms', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (j) {
        roms = j.roms || [];
        renderRoms(j);
      })
      .catch(function () {
        listEl.textContent = '';
        listEl.appendChild(note('Service not running.\n\nReboot the phone once after\ninstalling, or re-run install.sh.\n\n(* retries)', true));
      });
  }

  function note(t, err) {
    var d = document.createElement('div');
    d.className = 'none' + (err ? ' err' : '');
    d.textContent = t;
    return d;
  }

  function renderRoms(j) {
    listEl.textContent = '';
    if (!roms.length) {
      listEl.appendChild(note('No ROMs in\n' + (j.dir || '/data/media/roms') +
        '\n\nCopy .z64/.n64/.v64 files there,\nthen press * to reload.', true));
      return;
    }
    var first = null;
    roms.forEach(function (name) {
      var b = document.createElement('button');
      b.textContent = name.replace(/\.(z64|n64|v64)$/i, '');
      b.setAttribute('data-rom', name);
      b.addEventListener('click', function () { startRom(name); });
      listEl.appendChild(b);
      if (!first) first = b;
    });
    setTimeout(function () { try { (first || listEl).focus(); } catch (e) {} }, 30);
    log('roms ' + roms.length);
  }

  /* ------------------------------------------------------------- session */
  function api(path) {
    return fetch(API + path, { cache: 'no-store' }).then(function (r) { return r.text(); });
  }

  function startRom(name) {
    closeOvl();
    rom = name;
    fails = 0;
    attached = 0;
    autoCancel();
    mode = 'game';
    document.body.className = 'playing';
    wakelock(true);
    sysbar(true);
    /* click gesture: the only place Gecko lets us create an AudioContext */
    if (soundOn) ensureAudio(gSr);
    setStatus('starting ' + shortName() + '…', true);
    /* Poll for the socket instead of waiting for /start to resolve: n64svc only
     * answers after it has spawned the daemon (which boots the ROM before it
     * listens), and that answer can arrive minutes later — scheduling connect()
     * from it used to close the live socket mid-game. */
    api('/start?rom=' + encodeURIComponent(name))
      .catch(function (e) { log('start fail ' + e); });
    connectTimer = setTimeout(connect, 2000);
  }

  /* One attempt at a time, always tracked by identity: a socket that is still
   * CONNECTING or already OPEN is left alone, and an event from a socket that
   * is no longer current can never close its successor. Gecko closes a
   * CONNECTING socket silently ("can't establish a connection", no onopen), so
   * a second connect() used to kill the attempt that was about to succeed and
   * the ladder never recovered. */
  function connect() {
    if (mode !== 'game') return;
    if (ws && ws.readyState <= 1) return;      /* CONNECTING or OPEN: hands off */
    clearTimeout(connectTimer);
    var s;
    try { s = new WebSocket(EWS); }
    catch (e) { retry('ws failed'); return; }
    ws = s;
    s.binaryType = 'arraybuffer';
    s.onmessage = onMsg;
    s.onerror = function () {};
    s.opened = false;
    s.onopen = function () {
      if (ws !== s) { try { s.close(); } catch (e) {} return; }
      s.opened = true;
      fails = 0;
      clearTimeout(connectTimer);
      frames = 0; statT = Date.now(); pad = 0; lastMsgT = 0;
      try { s.send(new Uint8Array(2)); s.send(soundOn ? 'A1' : 'A0'); } catch (e) {}
      if (soundOn) ensureAudio(gSr);
      attached++;
      if (attached === 1) autoStart();
      log('ws open');
    };
    s.onclose = function (e) {
      if (ws !== s) return;
      ws = null;
      if (mode === 'game') {
        /* one line per drop (max 10): a 1006 here means Gecko or the network
         * tore the socket down, not the app */
        if (s.opened && closeLogged < 10) { closeLogged++; log('ws drop ' + (e && e.code)); }
        retry('link closed');
      }
    };
    /* An attempt that never reaches OPEN must not freeze the ladder: Gecko
     * reports some failures through onerror only (readyState 3, no onclose),
     * and a CONNECTING socket can hang if the daemon is still booting. */
    connectTimer = setTimeout(function () {
      if (ws !== s || s.readyState === 1) return;
      ws = null;
      try { s.onclose = null; s.onmessage = null; s.onerror = null; s.close(); } catch (e) {}
      retry('connect timeout');
    }, 6000);
  }

  /* Never give up while the game screen is up: the daemon boots the ROM before
   * it starts listening, so the first attempts are legitimately refused, and a
   * link that dies mid-game resumes the game that is still running in the
   * daemon. Back always leaves. */
  function retry(why) {
    if (mode !== 'game') return;
    clearTimeout(connectTimer);
    var s = ws;
    ws = null;
    if (s) { try { s.onclose = null; s.onmessage = null; s.onopen = null; s.close(); } catch (e) {} }
    fails++;
    if (fails === 1 || fails % 10 === 0) log('retry ' + fails + ' ' + why);
    if (fails <= 12) setStatus('connecting… ' + fails);
    else setStatus('no link — retrying (Back quits)', true);
    connectTimer = setTimeout(connect, 900);
  }

  function leaveGame() {
    closeOvl();
    mode = 'menu';
    clearTimeout(connectTimer);
    autoCancel();
    if (ws) { try { ws.onclose = null; ws.close(); } catch (e) {} ws = null; }
    closeAudio();
    wakelock(false);
    sysbar(false);
    for (var b in holdT) if (holdT[b]) { clearTimeout(holdT[b]); holdT[b] = 0; }
    pad = 0;
    api('/stop').catch(function () {});
    document.body.className = '';
    showMenu();
    log('left ' + rom);
  }

  /* ---------------------------------------------------------- chrome/ops */
  /* The KaiOS status bar is Gecko chrome (there is no Android SystemUI on this
   * phone), so manifest "fullscreen" is inert — requestFullscreen() from the
   * OK-key gesture is the only knob that hides it. */
  function sysbar(on) {
    try {
      var d = document.documentElement;
      if (on) {
        var p = d.requestFullscreen ? d.requestFullscreen({ navigationUI: 'hide' })
              : (d.mozRequestFullScreen ? d.mozRequestFullScreen() : null);
        if (p && p.catch) p.catch(function () {});
      } else if (document.fullscreenElement || document.mozFullScreenElement) {
        var q = (document.exitFullscreen || document.mozCancelFullScreen).call(document);
        if (q && q.catch) q.catch(function () {});
      }
    } catch (e) {}
  }

  /* The screen timeout is owned by the system app's ScreenManager, which dims
   * the panel unless its ScreenWakeLockManager reports a held lock — that is,
   * a wake lock on topic 'screen' (or 'video-playing') in state
   * 'locked-foreground' (system app js/wake_lock_manager.js). Those states are
   * only produced by the B2G wake lock API, navigator.b2g.requestWakeLock();
   * the W3C navigator.wakeLock and mozPower variants are kept as fallbacks
   * because they are what other Gecko vintages expose. */
  var wl = null, wlTopic = '', wlRetry = true;
  function wakelock(on) {
    try {
      if (on) {
        if (wl || !wlRetry) return;
        /* each API is tried on its own: a permission error from one must not
         * stop the next from being attempted */
        if (navigator.b2g && navigator.b2g.requestWakeLock) {
          /* 'screen' is the canonical topic; 'video-playing' is the second
           * topic the system app counts, and some builds only grant that one
           * to non-certified apps */
          var topics = ['screen', 'video-playing'], i;
          for (i = 0; i < topics.length && !wl; i++) {
            try {
              wl = navigator.b2g.requestWakeLock(topics[i]);
              wlTopic = topics[i];
              wlRetry = false;
              log('wakelock b2g ' + topics[i]);
            } catch (e) { log('wakelock b2g ' + topics[i] + ' fail ' + e); }
          }
        }
        if (!wl) try {
          if (navigator.wakeLock && navigator.wakeLock.request) {
            navigator.wakeLock.request('screen').then(
              function (o) { wl = o; log('wakelock w3c'); },
              function (e) { log('wakelock denied ' + e); });
          }
        } catch (e) { log('wakelock w3c fail ' + e); }
        if (!wl) try {
          if (navigator.getWakeLock) {
            var l = navigator.getWakeLock('screen');
            wl = (l && l.createLock) ? l.createLock() : l;
            log('wakelock getWakeLock');
          }
        } catch (e) { log('wakelock getWakeLock fail ' + e); }
        if (!wl) try {
          if (navigator.mozPower && navigator.mozPower.enableWakeLock) {
            navigator.mozPower.enableWakeLock('screen', 1);
            wl = 'mozPower';
            log('wakelock mozPower');
          }
        } catch (e) { log('wakelock mozPower fail ' + e); }
        if (!wl && !wlRetry) wlRetry = true;
      } else {
        if (wl) {
          try {
            if (wl === 'mozPower') navigator.mozPower.enableWakeLock('screen', 0);
            else if (wl.unlock) wl.unlock();       /* b2g: lock object */
            else if (wl.release) wl.release();     /* W3C / getWakeLock */
          } catch (e) {}
          wl = null;
        }
        wlRetry = true;
      }
    } catch (e) { log('wakelock fail ' + e); }
  }

  /* a foreground page is the only state the system app counts as
   * "locked-foreground", so re-take the lock when we come back to the front */
  document.addEventListener('visibilitychange', function () {
    if (document.visibilityState === 'hidden') return;
    if (mode === 'game') { wlRetry = true; wakelock(true); }
  });

  function setStatus(t, hold) {
    gstat.textContent = t;
    if (!t) { gstat.className = ''; return; }
    gstat.className = 'on';
    clearTimeout(statHide);
    if (hold) statHide = setTimeout(function () { gstat.className = ''; }, 2500);
  }

  function shortName() {
    var n = rom.replace(/\.(z64|n64|v64)$/i, '');
    return n.length > 16 ? n.substring(0, 16) : n;
  }

  /* --------------------------------------------------------------- video */
  function ensureLut() {
    if (lut) return;
    lut = new Uint32Array(65536);
    for (var v = 0; v < 65536; v++) {
      var r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
      r = (r * 255 + 15) / 31 | 0; g = (g * 255 + 31) / 63 | 0; b = (b * 255 + 15) / 31 | 0;
      lut[v] = 0xff000000 | (b << 16) | (g << 8) | r;
    }
  }

  /* A main-thread stall is invisible in the old log: the app just stops reading
   * and the daemon times out after 6 s. Log the gap instead. */
  var lastMsgT = 0, lagLogged = 0, closeLogged = 0;
  function onMsg(ev) {
    var t = Date.now();
    if (lastMsgT && t - lastMsgT > 1200 && lagLogged < 5) {
      lagLogged++;
      log('lag ' + (t - lastMsgT) + 'ms');
    }
    lastMsgT = t;
    try { onMsg1(ev); } catch (e) { log('msg err ' + e); }
  }
  function onMsg1(ev) {
    if (typeof ev.data === 'string') {
      var p = ev.data.split(' ');                 /* MD1 w h fps sr */
      if (p[0] === 'SL') { onSlots(p); return; }  /* slot list (save/load menu) */
      if (p[0] === 'SV' || p[0] === 'LD') { onStateAck(p); return; }
      if (p.length >= 5) {
        gSr = parseInt(p[4], 10) || 44100;
        setStatus(shortName() + ' ' + parseFloat(p[3]).toFixed(0) + 'fps', true);
        if (soundOn) ensureAudio(gSr);
      }
      return;
    }
    var buf = ev.data;
    if (buf.byteLength < 8) return;
    var dv = new DataView(buf);
    if (dv.getUint8(0) !== 0x4d || dv.getUint8(1) !== 0x44) return;   /* 'MD' */
    if (dv.getUint8(2) === 0x41) { onAudio(dv); return; }             /* PCM */
    var w = dv.getUint16(4), h = dv.getUint16(6);
    if (!w || !h || buf.byteLength < 8 + w * 2 * h) return;
    ensureLut();
    /* landscape: rotate the frame CW inside the LUT loop — source rows are
     * read sequentially, the canvas stays native 240x320 and no CSS transform
     * is involved. portrait: no rotation, the 320x240 frame is nearest-neighbour
     * scaled to the screen width (240x180) and centred, so the page background
     * shows as black bars above and below. */
    var port = (orient === 'port');
    var dw = port ? CW : h;
    var dh = port ? Math.round(h * CW / w) : w;
    if (!img || cv.width !== dw || cv.height !== dh ||
        colW !== w || rowH !== h) {
      cv.width = dw; cv.height = dh;
      ctx = cv.getContext('2d');
      img = ctx.createImageData(dw, dh);
      u32 = new Uint32Array(img.data.buffer);
      colmap = new Int32Array(dw); rowmap = new Int32Array(dh);
      for (var mx = 0; mx < dw; mx++) colmap[mx] = (mx * w / dw) | 0;
      for (var my = 0; my < dh; my++) rowmap[my] = (my * h / dh) | 0;
      colW = w; rowH = h;
      cv.style.width = dw + 'px';
      cv.style.height = dh + 'px';
      cv.style.marginTop = Math.max(0, ((gameEl.clientHeight || CH) - dh) >> 1) + 'px';
      log('frame ' + dw + 'x' + dh + ' ' + orient);
    }
    var src = new Uint16Array(buf, 8, w * h);
    var sy, dy, sb, o;
    if (port) {
      for (dy = 0; dy < dh; dy++) {
        sb = rowmap[dy] * w; o = dy * dw;
        for (var px = 0; px < dw; px++) u32[o + px] = lut[src[sb + colmap[px]]];
      }
    } else {
      var ystep = w / dh;                  /* 1.0 for 320x240 sources */
      for (sy = 0; sy < h; sy++) {
        sb = sy * w;
        var dx = h - 1 - sy;
        for (dy = 0; dy < dh; dy++) u32[dy * dw + dx] = lut[src[sb + ((dy * ystep) | 0)]];
      }
    }
    fw = w; fh = h; frames++;
    if (!rafPending && window.requestAnimationFrame) {
      rafPending = true;
      window.requestAnimationFrame(drawNow);
    } else if (!rafPending) { drawNow(); }
  }

  function drawNow() {
    rafPending = false;
    if (!ctx || !img) return;
    ctx.putImageData(img, 0, 0);
    var t = Date.now();
    if (t - statT >= 1000) {
      curFps = frames * 1000 / (t - statT);
      frames = 0; statT = t;
      if (gstat.className === 'on') setStatus(shortName() + ' ' + curFps.toFixed(0) + 'fps ' + fw + 'x' + fh);
    }
  }

  /* --------------------------------------------------------------- audio */
  var actx = null, aproc = null, aGain = null;
  var aq = null, acapF = 0, aqWf = 0, aqRf = 0, ffrac = 0, armed = false;
  /* Sound starts OFF (user request, v1.0.8): the loading screen is the most
   * fragile moment of the session and PCM is the last thing it needs. Key '0'
   * toggles it; AudioContext creation happens inside that keydown, which is a
   * user gesture, so nothing audio-related runs during a boot. */
  var gSr = 44100, soundOn = false, acDiag = 0;

  function availF() { return (aqWf - aqRf + acapF) % acapF; }

  /* The core only produces samples while it runs, so the playback rate follows
   * the measured arrival rate (EMA) instead of a fixed 1.0 — otherwise every
   * sub-60 fps scene underruns. Gecko opens 48 kHz: linear resample on read. */
  function ensureAudio(sr) {
    if (actx || !sr) return;
    try {
      var AC = window.AudioContext || window.webkitAudioContext;
      if (!AC) {
        if (acDiag < 3) {
          acDiag++;
          log('no AudioContext typeof=' + typeof window.AudioContext +
              '/' + typeof window.webkitAudioContext +
              ' media=' + !!(navigator.mediaDevices) +
              ' focus=' + (document.hasFocus ? document.hasFocus() : '?'));
        }
        return;
      }
      actx = new AC();
      aGain = actx.createGain(); aGain.gain.value = 0.9; aGain.connect(actx.destination);
      aproc = actx.createScriptProcessor(2048, 0, 2);
      acapF = sr;
      aq = new Float32Array(acapF * 2);
      aqWf = aqRf = 0; ffrac = 0; armed = false;
      var baseStep = sr / actx.sampleRate;
      var step = baseStep, ratio = 1, prodEst = sr, lastWf = 0;
      var TARGETF = (sr * 0.4) | 0;
      aproc.onaudioprocess = function (ev) {
        var L = ev.outputBuffer.getChannelData(0), R = ev.outputBuffer.getChannelData(1);
        var n = L.length, i;
        if (!actx || !soundOn || actx.state !== 'running') {
          for (i = 0; i < n; i++) { L[i] = 0; R[i] = 0; }
          return;
        }
        var arrived = ((aqWf - lastWf) % acapF + acapF) % acapF;
        lastWf = aqWf;
        prodEst = prodEst * 0.985 + arrived * (actx.sampleRate / n) * 0.015;
        if (!armed) {
          for (i = 0; i < n; i++) { L[i] = 0; R[i] = 0; }
          if (availF() >= ((acapF * 0.4) | 0)) armed = true;
          return;
        }
        var dev = availF() - TARGETF;
        var corr = (dev > TARGETF * 0.1 || dev < -TARGETF * 0.1) ? dev / TARGETF * 0.15 : 0;
        var tr = prodEst / sr + corr;
        if (tr > 1.05) tr = 1.05; else if (tr < 0.15) tr = 0.15;
        ratio += (tr - ratio) * 0.05;
        step = baseStep * ratio;
        for (i = 0; i < n; i++) {
          if (availF() < 2) {
            for (; i < n; i++) { L[i] = 0; R[i] = 0; }
            armed = false; ffrac = 0;
            break;
          }
          var i1 = aqRf + 1; if (i1 >= acapF) i1 = 0;
          var b = aqRf * 2, c = i1 * 2, f = ffrac;
          L[i] = aq[b] + (aq[c] - aq[b]) * f;
          R[i] = aq[b + 1] + (aq[c + 1] - aq[b + 1]) * f;
          ffrac += step;
          if (ffrac >= 1) {
            var adv = ffrac | 0; ffrac -= adv; aqRf += adv;
            while (aqRf >= acapF) aqRf -= acapF;
          }
        }
      };
      aproc.connect(aGain);
      if (actx.state === 'suspended' && actx.resume && soundOn) actx.resume();
      log('audio rate=' + actx.sampleRate);
    } catch (e) { actx = null; log('audio init fail ' + e); }
  }

  /* One Int16Array view per message instead of two DataView.getInt16 calls per
   * sample: 88 k samples/s on a Cortex-A53 is the difference between "audio is
   * free" and "the main thread falls behind the video stream" (which used to
   * end in a 6 s socket stall). Room for the new samples is made in O(1) by
   * advancing the read cursor, never by a per-sample check. */
  function onAudio(dv) {
    if (!aq || !soundOn) return;
    var n = (dv.byteLength - 4) >> 2, i, o;
    if (n <= 0) return;
    var room = acapF - availF();
    if (n > room) { aqRf = (aqRf + (n - room)) % acapF; ffrac = 0; }
    var s = new Int16Array(dv.buffer, dv.byteOffset + 4, n * 2);
    for (i = 0, o = aqWf * 2; i < n; i++) {
      aq[o++] = s[i + i] * 3.0517578125e-05;
      aq[o++] = s[i + i + 1] * 3.0517578125e-05;
      if (++aqWf >= acapF) { aqWf = 0; o = 0; }
    }
  }

  function setSound(on) {
    soundOn = !!on;
    if (ws && ws.readyState === 1) { try { ws.send(soundOn ? 'A1' : 'A0'); } catch (e) {} }
    if (soundOn) {
      ensureAudio(gSr);
      if (actx && actx.state === 'suspended' && actx.resume) actx.resume();
      armed = false;
    } else {
      /* drop whatever is buffered so re-enabling starts clean, then stop the
       * graph — a suspended context costs nothing per frame */
      aqWf = aqRf = 0; ffrac = 0; armed = false;
      if (actx && actx.suspend) actx.suspend();
    }
    log('sound ' + (soundOn ? 'on' : 'off'));
    setStatus(soundOn ? 'SOUND ON' : 'SOUND OFF', true);
  }

  function closeAudio() {
    if (!actx) return;
    try { aproc.disconnect(); aGain.disconnect(); actx.close(); } catch (e) {}
    actx = null; aproc = null; aGain = null; aq = null; armed = false; ffrac = 0;
  }

  /* ------------------------------------------------------- save/load menu */
  /* SoftRight opens this overlay over the running game: first page = the two
   * sections, second page = that section's five slots. Rows are plain buttons
   * (the app never creates an editable element). The daemon owns the slots:
   * "L" lists them, "S<n>"/"G<n>" save/load, replies arrive as TEXT. */
  var SLOTS = 5;
  var ovlEl, olistEl, ostatEl, otitleEl, ohintEl;
  var ovOpen = false, ovBusy = 0, ovTimer = 0, ovSlots = [];
  var ovView = 'root';                /* 'root' | 'S' (save) | 'G' (load) | 'O' */
  var ovFocus = 0;                    /* focused row index inside the view */

  var OV_TITLE = { root: 'MENU', S: 'SAVE STATE', G: 'LOAD STATE', O: 'ORIENTATION' };
  var OV_HINT = {
    root: 'OK select \u00b7 Back resume',
    S: 'OK save into slot \u00b7 Back menu',
    G: 'OK load slot \u00b7 Back menu',
    O: 'OK set \u00b7 Back menu'
  };

  function ostatus(t) { if (ostatEl) ostatEl.textContent = t || ''; }

  function fmtSlot(t) {
    if (!t) return 'empty';
    var d = new Date(t * 1000);
    function z(n) { return n < 10 ? '0' + n : '' + n; }
    return z(d.getMonth() + 1) + '-' + z(d.getDate()) + ' ' +
           z(d.getHours()) + ':' + z(d.getMinutes());
  }

  function ovBtn(label, off, fn) {
    var b = document.createElement('button');
    b.textContent = label;
    if (off) b.className = 'off';
    b.addEventListener('click', fn);
    return b;
  }

  function focusRow(i) {
    var btns = olistEl.querySelectorAll('button');
    if (!btns.length) return;
    if (i < 0 || i >= btns.length) i = 0;
    ovFocus = i;
    try { btns[i].focus(); } catch (e) {}
  }

  function renderOvl() {
    if (!olistEl) return;
    var i;
    olistEl.textContent = '';
    otitleEl.textContent = OV_TITLE[ovView];
    ohintEl.textContent = OV_HINT[ovView];
    if (ovView === 'root') {
      olistEl.appendChild(ovBtn('Load state', 0, function () { enterView('G'); }));
      olistEl.appendChild(ovBtn('Save state', 0, function () { enterView('S'); }));
      olistEl.appendChild(ovBtn('Orientation', 0, function () { enterView('O'); }));
    } else if (ovView === 'O') {
      var modes = [['land', 'Landscape'], ['port', 'Portrait']];
      for (i = 0; i < modes.length; i++) (function (v, label) {
        olistEl.appendChild(ovBtn(label + (orient === v ? '   (on)' : ''), 0,
          function () {
            setOrient(v);
            renderOvl();
            ostatus('orientation: ' + label.toLowerCase());
          }));
      })(modes[i][0], modes[i][1]);
    } else {
      var kind = ovView;
      for (i = 1; i <= SLOTS; i++) (function (n, ts) {
        olistEl.appendChild(ovBtn('Slot ' + n + '   ' + fmtSlot(ts),
                                  kind === 'G' && !ts,
                                  function () { doSlot(kind, n); }));
      })(i, ovSlots[i - 1] || 0);
    }
    setTimeout(function () { focusRow(ovFocus); }, 20);
  }

  function enterView(v) {
    ovView = v;
    ovFocus = 0;
    ostatus('');
    renderOvl();
    if (v === 'S' || v === 'G') stList();   /* fresh timestamps when slots show */
  }

  function stList() {
    if (ws && ws.readyState === 1) {
      try { ws.send('L'); } catch (e) { log('state list fail ' + e); }
      ostatus('reading slots…');
    } else {
      ostatus('emulator link down — Back to resume');
    }
  }

  function doSlot(kind, i) {
    if (!ws || ws.readyState !== 1) { ostatus('emulator link down'); return; }
    if (kind === 'G' && !(ovSlots[i - 1] || 0)) { ostatus('Slot ' + i + ' is empty'); return; }
    ovBusy = 1;
    ostatus((kind === 'S' ? 'saving slot ' : 'loading slot ') + i + '…');
    try { ws.send(kind + i); }
    catch (e) { ovBusy = 0; log('state send fail ' + e); ostatus('send failed'); return; }
    clearTimeout(ovTimer);
    ovTimer = setTimeout(function () {
      if (!ovBusy) return;
      ovBusy = 0;
      ostatus('no reply from the emulator');
    }, 6000);
  }

  /* TEXT replies from the daemon: "SL m1 m2 m3 m4 m5", "SV|LD <n> ok|err" */
  function onSlots(p) {
    ovSlots = [];
    for (var i = 1; i <= SLOTS; i++) ovSlots.push(parseInt(p[i], 10) || 0);
    log('states ' + ovSlots.join(','));
    if (ovOpen) {
      renderOvl();
      if (!ovBusy && ostatEl && ostatEl.textContent === 'reading slots…') ostatus('');
    }
  }

  function onStateAck(p) {
    var kind = p[0], n = parseInt(p[1], 10) || 0, ok = p[2] === 'ok';
    ovBusy = 0;
    clearTimeout(ovTimer);
    log('state ' + kind + ' ' + n + ' ' + (ok ? 'ok' : 'err'));
    if (kind === 'SV') {
      /* refresh first: stList() overwrites the status line, so the result
       * message has to be written after it */
      if (ok) { stList(); ostatus('saved slot ' + n); }
      else ostatus('save failed (slot ' + n + ')');
      return;
    }
    if (ok) {
      closeOvl();
      setStatus(shortName() + ' loaded slot ' + n, true);
    } else {
      ostatus('load failed (slot ' + n + ')');
    }
  }

  function openOvl() {
    if (mode !== 'game' || ovOpen) return;
    ovOpen = true;
    /* nothing of the player's is held while the overlay is up */
    if (pad) { pad = 0; sendPad(); }
    ovView = 'root';
    ovFocus = 0;
    ovSlots = [];
    document.body.className = 'playing overlaying';
    ovlEl.className = 'on';
    renderOvl();
    ostatus((ws && ws.readyState === 1) ? '' : 'emulator link down — Back to resume');
    log('menu open');
  }

  function closeOvl() {
    if (!ovOpen) return;
    ovOpen = false;
    ovBusy = 0;
    clearTimeout(ovTimer);
    if (ovlEl) ovlEl.className = '';
    if (mode === 'game') document.body.className = 'playing';
    log('menu close');
  }

  /* Back: slots -> first page -> game. */
  function ovBack() {
    if (ovView !== 'root') {
      ovView = 'root';
      ovFocus = 0;
      ostatus('');
      renderOvl();
      return;
    }
    closeOvl();
  }

  function ovKey(k, e) {
    if (e.type !== 'keydown') return;
    if (k === 'Backspace' || k === 'BrowserBack' || k === 'Escape' || k === 'SoftRight') {
      ovBack();
      return;
    }
    if (k === 'Enter' && e.repeat) return;
    var btns = olistEl.querySelectorAll('button');
    if (!btns.length) return;
    var idx = -1, i;
    for (i = 0; i < btns.length; i++) if (btns[i] === document.activeElement) idx = i;
    if (k === 'ArrowDown' || k === 'Down' || k === 'ArrowRight' || k === 'Right' || k === '6' || k === '8') {
      focusRow(idx < 0 ? 0 : (idx + 1) % btns.length);
    } else if (k === 'ArrowUp' || k === 'Up' || k === 'ArrowLeft' || k === 'Left' || k === '4' || k === '5') {
      focusRow(idx < 0 ? btns.length - 1 : (idx - 1 + btns.length) % btns.length);
    } else if (k === 'Enter') {
      if (idx >= 0) btns[idx].click();
    }
  }

  /* --------------------------------------------------------------- input */
  /* Landscape (default): the phone is held rotated counter-clockwise (keypad
   * on the right), so the arrow cross is remapped to match the rotated
   * picture. Portrait: the picture is upright, so arrows use the natural
   * directions — exactly the four bits the dialer keys 8/4/5/6 send anyway.
   * Numeric keys, OK, the soft keys, `*`, `#` and Back never depend on
   * orientation. keyup() resolves bits through the same table, so a press can
   * never stick; setOrient() also drops every held bit. */
  /* SoftRight is NOT a pad bit any more: it opens the menu (openOvl). N64 R
   * moved to SoftLeft (top-left button) so the phone keeps every N64 button. */
  var orient = 'land';                /* 'land' | 'port' */
  var KEYBITS = {
    '8': 4, '4': 6, '5': 5, '6': 7,
    Enter: 3, '1': 0, '2': 1, '3': 8,
    '7': 12, '9': 13,
    '*': 2, '#': 11,
    SoftLeft: 10
  };
  var ARROWS_LAND = {
    ArrowUp: 6, ArrowLeft: 5, ArrowDown: 7, ArrowRight: 4,
    Up: 6, Left: 5, Down: 7, Right: 4
  };
  var ARROWS_PORT = {
    ArrowUp: 4, ArrowLeft: 6, ArrowDown: 5, ArrowRight: 7,
    Up: 4, Left: 6, Down: 5, Right: 7
  };

  function keyBit(k) {
    var a = orient === 'port' ? ARROWS_PORT : ARROWS_LAND;
    if (a[k] !== undefined) return a[k];
    return KEYBITS[k];
  }

  function loadOrient() {
    try {
      var v = localStorage.getItem('n64.orient');
      if (v === 'port' || v === 'land') orient = v;
    } catch (e) {}
  }

  function setOrient(o) {
    if (o !== 'port' && o !== 'land') return;
    if (o === orient) return;
    orient = o;
    try { localStorage.setItem('n64.orient', o); } catch (e) {}
    img = null;                 /* next frame rebuilds for the new shape */
    if (pad) { pad = 0; sendPad(); }
    log('orient ' + (o === 'port' ? 'portrait' : 'landscape'));
  }

  var dbgK = 60, dbgP = 12;
  var autoT = [], attached = 0;

  function sendPad() {
    if (ws && ws.readyState === 1) {
      try { ws.send(new Uint8Array([pad & 255, (pad >> 8) & 255])); }
      catch (e) { log('pad send fail ' + e); }
    } else if (dbgP > 0) {
      dbgP--;
      log('pad DROP ws=' + (ws ? ws.readyState : 'none') + ' mask=' + pad.toString(16));
    }
  }

  /* Every pad bit is held until keyup — never on a timer. Copied from the
   * ompterm game.js that works on this phone (kdev): this keypad does not
   * auto-repeat arrow keys, so the old "hold the bit for 260 ms" pulse
   * released the stick while the key was still down and Mario only twitched
   * (n64.log shows a single ArrowUp keydown for a whole hold). */
  function keydown(e) {
    var k = e.key;
    if (mode === 'game') {
      e.preventDefault(); e.stopPropagation();
      if (ovOpen) { ovKey(k, e); return; }
      if (k === 'Backspace' || k === 'BrowserBack' || k === 'Escape') { leaveGame(); return; }
      if (k === 'SoftRight') { if (!e.repeat) openOvl(); return; }
      if (k === '0') { if (!e.repeat) setSound(!soundOn); return; }
      var bit = keyBit(k);
      if (dbgK > 0) {
        dbgK--;
        log('gk ' + k + '->' + bit + ' rep=' + (e.repeat ? 1 : 0) +
            ' act=' + (document.activeElement && document.activeElement.id));
      }
      if (bit !== undefined) {
        /* the player's key takes over a bit that the intro pulse is holding */
        if (autoBit[bit]) { autoBit[bit] = 0; clearTimeout(holdT[bit]); holdT[bit] = 0; }
        if (!(pad & (1 << bit))) { pad |= (1 << bit); sendPad(); }
      }
      if (soundOn) {
        ensureAudio(gSr);   /* retry: the API is not always there at boot */
        if (actx && actx.state === 'suspended' && actx.resume) actx.resume();
      }
      wakelock(true);
      showStatBriefly();
      return;
    }
    /* menu */
    if (k === 'Backspace' || k === 'BrowserBack') {
      e.preventDefault(); e.stopPropagation();
      try { window.close(); } catch (er) {}
      return;
    }
    if (k === '*') { e.preventDefault(); e.stopPropagation(); loadRoms(); return; }
    if (e.type !== 'keydown') return;
    var btns = listEl.querySelectorAll('button');
    if (!btns.length) return;
    var idx = -1;
    for (var i = 0; i < btns.length; i++) if (btns[i] === document.activeElement) idx = i;
    if (k === 'ArrowDown' || k === 'Down' || k === 'ArrowRight' || k === 'Right' || k === '6' || k === '8') {
      e.preventDefault(); e.stopPropagation();
      try { btns[idx < 0 ? 0 : (idx + 1) % btns.length].focus(); } catch (er) {}
    } else if (k === 'ArrowUp' || k === 'Up' || k === 'ArrowLeft' || k === 'Left' || k === '4' || k === '5') {
      e.preventDefault(); e.stopPropagation();
      try { btns[idx < 0 ? 0 : (idx - 1 + btns.length) % btns.length].focus(); } catch (er) {}
    } else if (k === 'Enter') {
      e.preventDefault(); e.stopPropagation();
      if (idx >= 0) btns[idx].click();
    }
  }

  function keyup(e) {
    if (mode !== 'game') return;
    e.preventDefault(); e.stopPropagation();
    if (ovOpen) return;                  /* overlay: pad already released */
    var bit = keyBit(e.key);
    if (bit !== undefined && (pad & (1 << bit))) { pad &= ~(1 << bit); sendPad(); }
    showStatBriefly();
  }

  /* A system overlay (status bar, power dialog) can swallow the keyup; drop
   * everything rather than walking into a wall forever. */
  function releaseAll() {
    if (mode === 'game' && pad) { pad = 0; sendPad(); }
  }

  /* Timed pulse, used ONLY by the intro auto-start (nobody is holding a key).
   * SM64's logo/flyby/file-select screens swallow presses that arrive before
   * the screen finishes loading, so a game started by hand looks dead for the
   * first minute. Sequence proven headless on this phone: START @16/21 s,
   * A @29/39/49 s from the first attach (see mario_gpu.md §6). */
  function pressBit(bit, hold) {
    if (mode !== 'game' || bit === undefined || ovOpen) return;
    var t = hold || 350;
    clearTimeout(holdT[bit]);
    holdT[bit] = setTimeout(function () {
      holdT[bit] = 0;
      /* release only our own pulse: a key the player is holding at that moment
       * owns the bit and keeps it until its keyup */
      if (mode === 'game' && autoBit[bit]) {
        autoBit[bit] = 0;
        if (pad & (1 << bit)) { pad &= ~(1 << bit); sendPad(); }
      }
    }, t);
    if (!(pad & (1 << bit))) { pad |= (1 << bit); autoBit[bit] = 1; sendPad(); }
  }

  var holdT = {};                     /* auto-start pulses only */
  var autoBit = {};                   /* bits the pulse set and must release */

  function autoStart() {
    if (autoT.length) return;
    [[16000, 3], [21000, 3], [29000, 1], [39000, 1], [49000, 1]].forEach(function (t) {
      autoT.push(setTimeout(function () { pressBit(t[1]); }, t[0]));
    });
    log('auto-start armed');
  }

  function autoCancel() {
    if (!autoT.length) return;
    autoT.forEach(function (t) { clearTimeout(t); });
    autoT = [];
  }

  function showStatBriefly() {
    clearTimeout(statHide);
    gstat.className = 'on';
    statHide = setTimeout(function () {
      if (mode === 'game') setStatus(shortName() + ' ' + curFps.toFixed(0) + 'fps ' + fw + 'x' + fh);
    }, 2200);
  }

  /* ---------------------------------------------------------------- boot */
  window.addEventListener('load', function () {
    menuEl = $('menu'); listEl = $('mlist'); gameEl = $('game');
    gstat = $('gstat'); cv = $('gc');
    ovlEl = $('ovl'); olistEl = $('olist'); ostatEl = $('ostat');
    otitleEl = $('otitle'); ohintEl = $('ohint');
    ctx = cv.getContext('2d');
    $('mver').textContent = 'v' + VER;
    /* No editable element exists in this app on purpose: while a text field
     * has focus the KaiOS keyboard app turns dialer keys into IME text
     * ("a"/"d"/".") and eats arrow keys — that was the "letters on screen,
     * arrows dead" report. Plain keydown/keyup on the window are what ompterm
     * uses in game mode. */
    window.addEventListener('keydown', keydown, true);
    window.addEventListener('keyup', keyup, true);
    window.addEventListener('blur', releaseAll);
    document.addEventListener('mozvisibilitychange', function () {
      if (document.mozHidden) releaseAll();
    });
    loadOrient();            /* landscape unless the menu stored portrait */
    showMenu();
  });
})();
