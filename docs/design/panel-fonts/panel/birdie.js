// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// The settings panel, v2 "pocket": the companion's voice, the HUD switch, three scales, and two pages that slide in
// (symbols, fine tuning). A preview unfolds on top only while the real HUD is not on the screen; hovering a symbol
// lights it up on the real HUD (focus message) and in the preview.
// Talks to Birdie.exe over ws://127.0.0.1 (see src/app/panel_proto.h for the messages). Every control sends its
// change at once; Birdie applies it to the HUD, saves it, and sends the new state back to every open panel.
// Inside Birdie's own window (WebView2) it also talks to the window: its height, close, drag (settings_window.h).
(function () {
  'use strict';

  var PORTS = [47820, 47821, 47822, 47823];  // src/app/app.cpp kPanelPort, kPanelPortTries
  var D = 36.5714, CX = 640, HY = 420, BORE = 329.143;  // HUD canvas: px per degree, centre, horizon y, boresight y
  var DESIGN_W = 352;  // px at 1rem = 14px
  var PREVIEW_MIN_OPACITY = 0.45;  // the preview never fades below this (the value is written next to it)

  // what the panel shows; settings mirror settings.json, status is Birdie's view of the sim
  var S = {
    online: false, lang: 'zh', focus: null, page: 0,
    set: { visible: true, glide_deg: 3.0, hud_scale: 1.0, opacity: 1.0, lead_ms: 0, flare_tol_deg: 0.3, ladder_step_deg: 6,
           language: '', show_bird: true, show_fpa_num: true, show_vs_num: true, show_flare: true, show_glide: true,
           show_horizon: true, show_hdg: true, show_pitch_minor: true, show_runway: true, show_att: true, show_speed: true,
           show_alt: true, show_vs: true, show_roll: true, show_gs: true, show_bird_ra: true, bird_ra_below_ft: 500, xwind_mode: false },
    st: null
  };
  var touched = {};  // key -> time of our own last change (incoming state does not fight a drag)

  // ---------------------------------------------------------------- words
  var T = {
    zh: {
      sym: '符号', symCount: '{n} / {all} 显示中', fine: '细调', path: '飞行路径', world: '天地', inst: '仪表',
      size: '大小', bright: '亮度', glide: '下滑角', lead: '显示延迟补偿', tol: '拉平提示容差', ladder: '俯仰刻度间隔',
      rah: '小鸟下的 RA 从多高开始出现', ra: '无线电高度校准', xw: '侧风模式：仪表带收窄',
      raBtn: '停在地面时，记下现在的读数', raBtnWait: '停稳在地面上才能校准', raClear: '清除', raNone: '这架飞机还没校准；第一次停稳在地面时会自动校准',
      power: '显示 HUD', raNoAc: '连上模拟器后，按机型分别记住', raFmt: '读数减去 {ft} ft', off: '关', auto: '开机时陪着你', log: '日志', quit: '让 Birdie 休息',
      quitAsk: '再点一次，它就去睡了', gIdle: '悬停一个符号，HUD 上那一笔会亮起来', nowOff: '现在关着，点一下打开',
      pv: '预览', pvAway: '预览 · HUD 此刻不在屏幕上', pvOffline: '预览', pvDim: '按 {min}% 画，实际 {v}%',
      border: 'HUD 边框', seatL: '机长位', seatR: '副驾位', bdNone: '还没描，HUD 用的是整块合像镜', bdDrawn: '已经描好',
      bdMirrored: '按另一座镜像', bdShortDrawn: '已描', bdShortMirrored: '镜像', bdShortNone: '未设',
      bdEdit: '在画面上描 ›', bdEditNo: '进驾驶舱视角后才能描', bdEditing: '正在画面上描 · Enter 存好 · Esc 不改',
      bdEye: '眼点和描的时候不一样了，边框可能偏，可以重描', bdCut: '仪表有一部分在边框外，会淡掉',
      bdFit: '缩到 {n}% 放得下', bdNoFit: '缩到最小也放不下', bdMirror: '镜像到另一座', bdClear: '清除', bdClearAsk: '再点一次清除',
      bdDesc: '边框跟着驾驶舱走：改缩放、转头，它都贴着窗框；最后 1° 慢慢淡出。另一座没描时，按这边镜像。'
    },
    en: {
      sym: 'Symbols', symCount: '{n} / {all} shown', fine: 'Fine tuning', path: 'Flight path', world: 'Sky & ground', inst: 'Instruments',
      size: 'Size', bright: 'Bright', glide: 'Glide', lead: 'Display latency', tol: 'Flare cue tolerance', ladder: 'Pitch scale every',
      rah: 'Radio altitude under the bird from', xw: 'Crosswind mode: narrow tapes',
      ra: 'Radio altitude zero', raBtn: 'Take the reading now (on the ground)', raBtnWait: 'Stand still on the ground to calibrate',
      raClear: 'clear', raNone: 'not set for this aircraft yet; it sets itself the first time you stand still on the ground',
      raNoAc: 'kept per aircraft once the sim is connected', raFmt: 'reading minus {ft} ft', off: 'off', auto: 'With Windows',
      power: 'Show HUD', log: 'Log', quit: 'Let Birdie rest', quitAsk: 'Once more, and it goes to sleep', gIdle: 'Hover a symbol and its stroke lights up on the HUD',
      nowOff: 'now off, tap to show', pv: 'Preview', pvAway: 'Preview · the HUD is not on the screen now', pvOffline: 'Preview',
      pvDim: 'drawn at {min}%, set to {v}%',
      border: 'HUD border', seatL: 'left seat', seatR: 'right seat', bdNone: 'Not traced: the HUD uses its whole combiner', bdDrawn: 'Traced',
      bdMirrored: 'Mirrored from the other seat', bdShortDrawn: 'traced', bdShortMirrored: 'mirrored', bdShortNone: 'none',
      bdEdit: 'Trace it on the screen ›', bdEditNo: 'Trace it from the cockpit view', bdEditing: 'Tracing on the screen · Enter saves · Esc cancels',
      bdEye: 'The eye point moved since tracing; the border may be off — trace again', bdCut: 'Part of the instruments is outside the border and fades',
      bdFit: 'shrink to {n}% to fit', bdNoFit: 'does not fit even at the smallest size', bdMirror: 'mirror to the other seat', bdClear: 'clear',
      bdClearAsk: 'once more to clear',
      bdDesc: 'The border stays with the cockpit through zoom and head turns, fading over its last degree. The other seat mirrors it until traced.'
    }
  };
  // the companion's voice: [line, fact] per state from Birdie (status.why)
  var VOICE = {
    show:     { zh: ['在你身边', '{rw} · 下滑 {g}°'], en: ['Right here with you', '{rw} · glide {g}°'] },
    look:     { zh: ['转回前方，我就在', '视线偏开时先藏起来'], en: ['Look ahead, I’m there', 'Hidden while you look aside'] },
    ext:      { zh: ['回到驾驶舱，我们一起看', '外部视角下不显示'], en: ['Back in the cockpit, together', 'Not shown in outside views'] },
    taxi:     { zh: ['滑行时，先休息一下', '起飞滑跑超过 40 kt 时出现'], en: ['Resting while you taxi', 'Appears above 40 kt on the roll'] },
    away:     { zh: ['你回来，我就回来', 'MSFS 不在前台'], en: ['Come back and so will I', 'MSFS is not in front'] },
    pause:    { zh: ['等你', '模拟器暂停了，或者数据停了'], en: ['Waiting for you', 'The sim is paused, or the data stopped'] },
    nosim:    { zh: ['等你起飞', '打开 MSFS 后自动连上'], en: ['Ready when you fly', 'Connects by itself when MSFS starts'] },
    wait:     { zh: ['我在听', '等模拟器送来第一帧数据'], en: ['Listening', 'Waiting for the first data from the sim'] },
    starting: { zh: ['我醒了', '正在找模拟器'], en: ['Awake', 'Looking for the simulator'] },
    nocp:     { zh: ['还看不到你的视线', '目前需要 ChasePlane 驾驶舱视角'], en: ['I can’t see your view yet', 'ChasePlane cockpit view needed for now'] },
    off:      { zh: ['我先安静一会儿', '点右边的圆，或按 Ctrl+Alt+Insert 叫我回来'], en: ['I’ll stay quiet for now', 'Tap the circle, or Ctrl+Alt+Insert, to bring me back'] },
    offline:  { zh: ['我还没醒', '打开 Birdie.exe，这里会自己连上'], en: ['Not awake yet', 'Start Birdie.exe and this connects by itself'] }
  };
  // [key, setting, zh, en, zh description, en description, icon]
  var GLYPHS = {
    path: [
      ['bird', 'show_bird', '小鸟', 'Bird', '飞机真正飞去的方向', 'Where the aircraft is really going',
        '<circle class="ic" cx="17" cy="19" r="4.5"/><path class="ic" d="M12.5 19H3M21.5 19H31M17 14.5V8"/>'],
      ['fpa', 'show_fpa_num', '航迹角', 'Path angle', '小鸟右边：现在的航迹角', 'Right of the bird: flight path angle',
        '<circle class="ic" cx="9" cy="19" r="3.5"/><path class="ic" d="M12.5 19h4"/><text class="t" x="18" y="22" font-size="11">3.0</text>'],
      ['vsn', 'show_vs_num', '升降速度', 'Vert. speed', '小鸟左边：升降速度（ft/min）', 'Left of the bird: vertical speed (ft/min)',
        '<text class="t" x="1" y="22" font-size="11">700</text><path class="ic" d="M19 19h3.5"/><circle class="ic" cx="26" cy="19" r="3.5"/>'],
      ['bra', 'show_bird_ra', 'RA 高度', 'Radio alt', '小鸟下方：无线电高度，低于细调里设的高度才出现', 'Under the bird: radio altitude, below the height set in Fine tuning',
        '<circle class="ic" cx="17" cy="10" r="3.5"/><path class="ic" d="M13.5 10H7M20.5 10H27"/><text class="t" x="17" y="27" font-size="11" text-anchor="middle">320</text>'],
      ['flare', 'show_flare', '拉平提示', 'Flare cue', '50 ft 以下在小鸟上方闪：+ 正好，++ 拉多了', 'Below 50 ft above the bird: + on profile, ++ too shallow',
        '<path class="ic" d="M5 17h9M9.5 12.5v9M20 17h9M24.5 12.5v9"/>'],
      ['glide', 'show_glide', '下滑线', 'Glide line', '理想下滑角在地面上的那条线', 'The line your ideal glide path makes on the ground',
        '<path class="ic" d="M3 13h28" opacity=".5"/><path class="ic" d="M3 21h6M12 21h6M21 21h6"/>']],
    world: [
      ['horizon', 'show_horizon', '地平线', 'Horizon', '真实的地平线', 'The true horizon',
        '<path class="ic" d="M2 18h30"/><path class="ic" d="M11 18a6 6 0 0 1 12 0" opacity=".55"/>'],
      ['hdg', 'show_hdg', '航向', 'Heading', '地平线上的航向刻度', 'Heading marks on the horizon',
        '<path class="ic" d="M2 13h30M6 13v5M14 13v8M22 13v5M30 13v8"/>'],
      ['minor', 'show_pitch_minor', '俯仰刻度', 'Pitch marks', '每 2.5° 的短刻度', 'Short marks every 2.5°',
        '<path class="ic" d="M12 9h10M12 17h10M12 25h10"/>'],
      ['runway', 'show_runway', '跑道', 'Runway', '选中的跑道轮廓', 'Outline of the runway you are lined up with',
        '<path class="ic" d="M14 7h6l7 21H7z"/>']],
    inst: [
      ['att', 'show_att', '机头', 'Aircraft', '机头指向：读俯仰和坡度', 'Where the nose points',
        '<path class="ic" d="M3 17h9v4M31 17h-9v4"/><path class="ic" d="M15 15h4v4h-4z"/>'],
      ['speed', 'show_speed', '速度', 'Speed', '左侧速度带', 'Speed tape, left',
        '<path class="ic" d="M20 4v26M14 9h6M14 15h6M14 21h6M14 27h6"/><path class="ic" d="M23 17l5-2.5v5z"/>'],
      ['alt', 'show_alt', '高度', 'Altitude', '右侧高度带与无线电高度', 'Altitude tape and radio altitude, right',
        '<path class="ic" d="M12 4v26M12 9h5M12 15h5M12 21h5M12 27h5"/><path class="ic" d="M17 13h12v8H17z"/>'],
      ['vs', 'show_vs', '升降', 'VS needle', '高度带右边的升降速度指针', 'Vertical speed needle',
        '<path class="ic" d="M30 13L8 24"/><path class="ic" d="M6 10v18" opacity=".5"/>'],
      ['roll', 'show_roll', '坡度', 'Bank', '顶上的坡度刻度', 'Bank scale at the top',
        '<path class="ic" d="M4 24a13 13 0 0 1 26 0"/><path class="ic" d="M17 11l-3 5h6z"/>'],
      ['gs', 'show_gs', '地速与风', 'GS & wind', '地速，风向风速', 'Ground speed and wind',
        '<text class="t" x="2" y="15" font-size="11">GS</text><path class="ic" d="M22 28V12M18 16l4-4 4 4"/>']]
  };
  var ALL = [].concat(GLYPHS.path, GLYPHS.world, GLYPHS.inst);
  function glyph(k) { for (var i = 0; i < ALL.length; i++) if (ALL[i][0] === k) return ALL[i]; return null; }

  function $(id) { return document.getElementById(id); }
  function L() { return T[S.lang]; }
  function fmt(s, o) { return s.replace(/\{(\w+)\}/g, function (_, k) { return o[k]; }); }
  function f1(v) { return v.toFixed(1); }
  function each(list, fn) { for (var i = 0; i < list.length; i++) fn(list[i], i); }

  // ---------------------------------------------------------------- where the page runs
  var COUI = !!(window.coherent || /Coherent/i.test(navigator.userAgent));
  var HOST = !COUI && !!(window.chrome && window.chrome.webview);  // Birdie's own window (WebView2)
  var PEEK = HOST && /[?&]peek\b/.test(location.search);  // the strip that floats up over the tray icon
  function host(m) { if (HOST) window.chrome.webview.postMessage(JSON.stringify(m)); }

  // ---------------------------------------------------------------- the link to Birdie.exe
  var ws = null, portIx = 0, retry = null;
  function ports() {
    var p = parseInt(location.port, 10);  // served by Birdie itself: that port first
    return (location.protocol === 'http:' && p && PORTS.indexOf(p) >= 0) ? [p].concat(PORTS.filter(function (x) { return x !== p; })) : PORTS;
  }
  function connect() {
    clearTimeout(retry);
    var list = ports(), port = list[portIx % list.length], opened = false;
    try { ws = new WebSocket('ws://127.0.0.1:' + port + '/'); } catch (e) { return later(); }
    ws.onopen = function () { opened = true; send({ type: 'action', name: 'hello' }); };
    ws.onmessage = function (ev) {
      var m; try { m = JSON.parse(ev.data); } catch (e) { return; }
      if (m.type === 'state') onState(m);
      else if (m.type === 'notice') notice(m.text);
    };
    ws.onclose = function () {
      ws = null;
      if (S.online) { S.online = false; paint(); }
      if (!opened) portIx++;  // nobody there: try the next port
      later();
    };
    ws.onerror = function () { try { ws && ws.close(); } catch (e) { /* closed */ } };
  }
  function later() { clearTimeout(retry); retry = setTimeout(connect, portIx % ports().length ? 300 : 2000); }
  function send(m) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(m)); }

  function set(key, value) {
    S.set[key] = value;
    touched[key] = Date.now();
    send({ type: 'set', key: key, value: value });
  }
  function act(name, value) { var m = { type: 'action', name: name }; if (value !== undefined) m.value = value; send(m); }

  function onState(m) {
    var now = Date.now();
    for (var k in m.settings) if (!(touched[k] && now - touched[k] < 700)) S.set[k] = m.settings[k];
    var wasLang = S.lang;
    S.st = m.status;
    S.lang = (m.status && m.status.lang === 'en') ? 'en' : 'zh';
    var first = !S.online;
    S.online = true;
    if (first || wasLang !== S.lang) words(); else paint();
  }

  // ---------------------------------------------------------------- HUD drawing (canvas coordinates, as the program)
  function P(d, cls, extra) { return '<path class="' + (cls || 'stroke') + '" d="' + d + '" ' + (extra || '') + '/>'; }
  function TX(x, y, s, size, anchor) {
    return '<text class="htxt" x="' + x + '" y="' + y + '" font-size="' + size + '" text-anchor="' + (anchor || 'middle') + '">' + s + '</text>';
  }
  function runwayHalf(y) { return 13 + (y - 452) / (700 - 452) * (92 - 13); }

  function build() {
    var s = S.set, out = [], h, k, y, e, j, side;
    var glide = s.glide_deg, ladder = s.ladder_step_deg, scale = s.hud_scale;
    var rw = (S.st && S.st.runway) ? S.st.runway.name : '07L';
    // ---- conformal: stays with the world
    out.push('<g class="el" data-el="horizon">' + P('M-40 ' + HY + 'H' + (CX - runwayHalf(452) - 6) + 'M' + (CX + runwayHalf(452) + 6) + ' ' + HY + 'H1320') + '</g>');
    var hd = '', hl = '';
    for (h = 50; h <= 95; h += 5) {
      var x = CX + (h - 71) * D, ten = h % 10 === 0;
      hd += 'M' + f1(x) + ' ' + HY + 'v' + (ten ? 14 : 8);
      if (ten) hl += TX(f1(x), HY + 40, String(h / 10), 21);
    }
    out.push('<g class="el" data-el="hdg">' + P(hd, 'stroke thin') + hl + '</g>');
    var lad = '', ll = '', inner = 121.74, outer = 192.84, tick = 11;
    if (ladder > 0) for (k = -Math.floor(30 / ladder); k <= Math.floor(30 / ladder); k++) {
      if (!k) continue;
      e = k * ladder; y = HY - e * D;
      if (y < 70 || y > 870) continue;
      for (side = -1; side <= 1; side += 2) {
        if (e > 0) lad += 'M' + (CX + side * inner) + ' ' + f1(y) + 'H' + (CX + side * outer) + 'v' + tick;
        else for (j = 0; j < 4; j++) {
          var a0 = inner + j * 19.7, a1 = a0 + 12;
          lad += 'M' + (CX + side * a0) + ' ' + f1(y) + 'H' + (CX + side * a1) + (j === 3 ? 'v' + (-tick) : '');
        }
        ll += TX(CX + side * (outer + 4), f1(y + 9), String(Math.round(e * 10) / 10), 21, side < 0 ? 'end' : 'start');
      }
    }
    var mn = '';
    for (j = -12; j <= 12; j++) {
      e = j * 2.5; y = HY - e * D;
      if (!j || y < 70 || y > 870 || (ladder > 0 && Math.abs(e / ladder - Math.round(e / ladder)) < 1e-6)) continue;
      if (!(y > 452 && y < 720)) mn += 'M' + (CX - 12) + ' ' + f1(y) + 'H' + (CX + 12);  // clipped across the runway
    }
    out.push('<g class="el" data-el="minor">' + (lad ? P(lad) : '') + (mn ? P(mn, 'stroke thin') : '') + ll + '</g>');
    out.push('<g class="el" data-el="runway">' + P('M' + (CX - 13) + ' 452H' + (CX + 13) + 'L' + (CX + 92) + ' 700H' + (CX - 92) + 'Z') + TX(CX, 730, rw, 20) + '</g>');
    // glide line: span grows with size, cut where it crosses the runway
    var gy = HY + glide * D, span = Math.min(18, 10 * scale) * D;
    var gap = (gy > 452 && gy < 700) ? runwayHalf(gy) + 4 : 0;
    var gl = gap ? 'M' + (CX - span) + ' ' + f1(gy) + 'H' + (CX - gap) + 'M' + (CX + gap) + ' ' + f1(gy) + 'H' + (CX + span)
                 : 'M' + (CX - span) + ' ' + f1(gy) + 'H' + (CX + span);
    out.push('<g class="el" data-el="glide"><path class="stroke dash" d="' + gl + '"/>' + TX(CX + span + 6, f1(gy + 8), '-' + glide.toFixed(1), 20, 'start') + '</g>');
    // bird (floats a little) + numbers + flare cue
    var by = HY + 3.0 * D, r = 17;
    out.push('<g id="birdfloat">' +
      '<g class="el" data-el="bird">' + P('M' + (CX - r) + ' ' + f1(by) + 'a' + r + ' ' + r + ' 0 1 0 ' + (2 * r) + ' 0a' + r + ' ' + r + ' 0 1 0 ' + (-2 * r) + ' 0M' +
        (CX - r) + ' ' + f1(by) + 'H' + (CX - 3 * r) + 'M' + (CX + r) + ' ' + f1(by) + 'H' + (CX + 3 * r) + 'M' + CX + ' ' + f1(by - r) + 'V' + f1(by - 2.1 * r)) + '</g>' +
      '<g class="el" data-el="fpa">' + TX(CX + 3 * r + 10, f1(by + 9), '−3.0', 22, 'start') + '</g>' +
      '<g class="el" data-el="vsn">' + TX(CX - 3 * r - 10, f1(by + 9), '−700', 22, 'end') + '</g>' +
      '<g class="el" data-el="bra">' + TX(CX, f1(by + r + 30), '320', 22) + '</g>' +
      '<g class="el" data-el="flare"><g id="flare" opacity="0">' + P('M' + (CX - 30) + ' ' + f1(by - 70) + 'h18M' + (CX - 21) + ' ' + f1(by - 79) + 'v18M' +
        (CX + 12) + ' ' + f1(by - 70) + 'h18M' + (CX + 21) + ' ' + f1(by - 79) + 'v18') + '</g></g></g>');
    // ---- airframe-fixed: scales with size around the boresight
    // crosswind mode (scene.cpp kXw*): tapes 55 % as tall, speed 50 left, altitude 60 right, no VS needle
    var xw = !!s.xwind_mode, tt = xw ? 510.5 - 181.6 * 0.55 : 328.9, tb = xw ? 510.5 + 181.6 * 0.55 : 692.1;
    var spd = xw ? ' transform="translate(-50 0)"' : '', altd = xw ? ' transform="translate(60 0)"' : '';
    var fx = [], v, sp = 'M140.9 ' + f1(tt) + 'V' + f1(tb) + 'M68.1 ' + f1(tt) + 'H161M68.1 ' + f1(tb) + 'H161', sl = '';
    for (v = 100; v <= 180; v += 10) {
      y = 510.5 - (v - 140) * 4.25;
      if (y < tt + 4 || y > tb - 4) continue;
      sp += 'M128.9 ' + f1(y) + 'H140.9';
      if (v % 20 === 0) sl += TX(94, f1(y + 9.3), String(v), 25);
    }
    fx.push('<g class="el" data-el="speed"' + spd + '>' + P(sp) + '<path class="solid" d="M119.5 509.5V512.6H147.3L160.6 517.5V504.6L147.3 509.5Z"/>' + sl + '</g>');
    fx.push('<g class="el" data-el="gs"' + spd + '>' + TX(68, 300, 'GS 142', 24, 'start') + TX(298, 258, '264', 22, 'end') +
      TX(306, 257, '/', 20) + TX(315, 258, '8', 22, 'start') + P('M276 298V268M269.5 280L276 268L282.5 280', 'stroke', 'transform="rotate(193 276 283)"') + '</g>');
    var al = 'M1057.4 ' + f1(tb) + 'H1133.2M1113.1 ' + f1(tt) + 'V' + f1(tb) + 'M1057.4 ' + f1(tt) + 'H1133.2', all = '';
    for (v = -300; v <= 1000; v += 100) {
      y = 510.5 - (v - 340) * 0.32;
      if (y < tt + 4 || y > tb - 4 || (y > 476 && y < 545)) continue;
      al += 'M1104.6 ' + f1(y) + 'H1113.1';
      if (v % 500 === 0 && !(y > 468 && y < 559)) all += TX(1081, f1(y + 8.5), String(v / 100), 25);
    }
    al += 'M1113.1 529.6H1057.3V491.4H1113.1V480.1H1150.8V540.9H1113.1V529.6';
    fx.push('<g class="el" data-el="alt"' + altd + '>' + P(al) + all + TX(1110, 522, '3', 34, 'end') +
      TX(1132, 520, '40', 26) + TX(1095, f1(730 - (692.1 - tb)), '1013', 26) + TX(1095, f1(772 - (692.1 - tb)), 'RA 320', 26) + '</g>');
    var yo = 95.27;
    if (!xw) fx.push('<g class="el" data-el="vs"><path class="solid" d="M1165 508.5H1185V512.5H1165Z"/>' + P('M1220 ' + f1(510.5 + 0.26 * yo) + 'L1183 ' + f1(510.5 + yo), 'stroke', 'stroke-width="4"') +
      TX(1192, f1(518 + yo + 14), '-07', 22) + '</g>');
    var arc = '';
    for (j = 0; j <= 39; j++) {
      var a = (155.16 + (24.84 - 155.16) * j / 39) * Math.PI / 180;
      arc += (j ? 'L' : 'M') + f1(640 + 190 * Math.cos(a)) + ' ' + f1(329.1 - 190 * Math.sin(a));
    }
    arc += 'M735.2 164L749.3 139.5M705.1 150L712.8 129.3M673 141.5L677 119.3M544.8 164L530.7 139.5M574.9 150L567.2 129.3M607 141.5L603 119.3M774.5 194.5L794.5 174.5M505.5 194.5L485.5 174.5M813 249L832 240M467 249L448 240';
    arc += 'M640 138.4L652.2 118.2H627.8ZM640.2 140L630.2 161.9H650.2ZM629 164.9L621 181H659L651 164.9Z';
    fx.push('<g class="el" data-el="roll">' + P(arc) + '</g>');
    fx.push('<g class="el" data-el="att">' + P('M625 335V329H595M637 332H643V326H637ZM655 335V329H685', 'stroke', 'stroke-width="3.4"') + '</g>');
    out.push('<g id="fixed" transform="' + fixedTransform(scale) + '">' + fx.join('') + '</g>');
    return out.join('');
  }
  function fixedTransform(k) { return 'translate(' + CX + ' ' + BORE + ') scale(' + k.toFixed(4) + ') translate(' + (-CX) + ' ' + (-BORE) + ')'; }

  var hud = null, shownScale = 1, scaleAnim = null, builtKey = '';
  function render() {
    var s = S.set, key = [s.glide_deg, s.ladder_step_deg, s.xwind_mode, S.st && S.st.runway ? S.st.runway.name : ''].join('|');
    if (key !== builtKey) {  // rebuild only for what changes geometry; the rest is classes and attributes
      builtKey = key;
      hud.innerHTML = build();
      shownScale = s.hud_scale;
    }
    apply();
  }
  function apply() {
    var s = S.set;
    hud.setAttribute('opacity', Math.max(PREVIEW_MIN_OPACITY, s.opacity).toFixed(2));
    cls(hud, 'spot', !!S.focus);
    each(hud.querySelectorAll('.el'), function (el) {
      var k = el.getAttribute('data-el'), g = glyph(k);
      cls(el, 'off', g ? !s[g[1]] : false);
      cls(el, 'focus', S.focus === k);
    });
    var fl = $('flare');
    if (fl) { var on = S.focus === 'flare' && s.show_flare; cls(fl, 'blink', on); fl.setAttribute('opacity', on ? '1' : '0'); }
    tweenScale(s.hud_scale);
  }
  function tweenScale(target) {  // the instruments breathe to their new size
    var fixed = $('fixed');
    if (!fixed || Math.abs(target - shownScale) < 1e-4) { if (fixed) fixed.setAttribute('transform', fixedTransform(target)); shownScale = target; return; }
    var from = shownScale, t0 = Date.now(), dur = 500;
    if (scaleAnim) clearInterval(scaleAnim);
    scaleAnim = setInterval(function () {
      var t = Math.min(1, (Date.now() - t0) / dur), e = 1 - Math.pow(1 - t, 3);
      shownScale = from + (target - from) * e;
      var f = $('fixed'); if (f) f.setAttribute('transform', fixedTransform(shownScale));
      if (t >= 1) { clearInterval(scaleAnim); scaleAnim = null; }
    }, 16);
  }
  // class helper that also works on SVG elements in older engines
  function cls(el, c, on) {
    var cur = (el.getAttribute('class') || '').split(/\s+/).filter(function (x) { return x && x !== c; });
    if (on) cur.push(c);
    el.setAttribute('class', cur.join(' '));
  }
  function hasCls(el, c) { return (' ' + (el.getAttribute('class') || '') + ' ').indexOf(' ' + c + ' ') >= 0; }

  // runway approach lights: a few warm points along the edges
  function lights() {
    var c = '';
    for (var i = 0; i <= 9; i++) {
      var t = i / 9, y = 452 + t * 248, h = runwayHalf(y) + 3, r = (1 + t * 2.2).toFixed(1);
      c += '<circle cx="' + f1(CX - h) + '" cy="' + f1(y) + '" r="' + r + '" style="animation-delay:' + (i * 0.37).toFixed(2) + 's"/>' +
           '<circle cx="' + f1(CX + h) + '" cy="' + f1(y) + '" r="' + r + '" style="animation-delay:' + (i * 0.37 + 0.2).toFixed(2) + 's"/>';
    }
    $('lights').innerHTML = c;
  }

  // ---------------------------------------------------------------- preview: only while the real HUD is away
  var pvShown = null;
  function hudOnScreen() { return S.online && !!(S.st && S.st.showing); }
  function preview() {
    var want = !PEEK && !hudOnScreen(), l = L();
    $('pv-l').textContent = !S.online ? l.pvOffline : (S.set.visible ? l.pvAway : l.pv);
    $('pv-r').textContent = S.set.opacity < PREVIEW_MIN_OPACITY - 1e-6
      ? fmt(l.pvDim, { min: Math.round(PREVIEW_MIN_OPACITY * 100), v: Math.round(S.set.opacity * 100) }) : '';
    if (want === pvShown) return;
    pvShown = want;
    var w = $('pvwrap');
    cls($('pocket'), 'pvon', want);
    w.style.height = (want ? $('pv').offsetHeight : 0) + 'px';
    fitHost();
  }

  // ---------------------------------------------------------------- pages
  function go(p) {
    S.page = p;
    $('panel').setAttribute('data-p', String(p));
    if (p !== 1) focusOn(null);
    fitPanel();
    // keyboard users keep their place; focusing never scrolls the sliding pages (overflow: hidden still scrolls)
    var first = $('page' + p).querySelector('[role="button"]');
    if (first && document.activeElement && document.activeElement !== document.body) {
      try { first.focus({ preventScroll: true }); } catch (e) { first.focus(); }
    }
    $('panel').scrollLeft = 0;
    setTimeout(function () { $('panel').scrollLeft = 0; }, 0);
  }
  function fitPanel() {
    var page = $('page' + S.page);
    if (page) $('panel').style.height = page.offsetHeight + 'px';
    fitHost();
  }
  // Birdie's own window follows the page's height: at once when growing, after the slide when shrinking
  var hostH = 0, shrinkTimer = null;
  function fitHost() {
    if (!HOST) return;
    var page = PEEK ? $('peekcard') : $('page' + S.page), pv = pvShown ? $('pv').offsetHeight : 0;
    var h = Math.ceil((page ? page.offsetHeight : 0) + pv);  // css px (1rem = 14 px in the window)
    clearTimeout(shrinkTimer);
    if (h >= hostH || PEEK) { hostH = h; host({ host: 'size', h: h }); }  // the strip has no slide to wait for
    else shrinkTimer = setTimeout(function () { hostH = h; host({ host: 'size', h: h }); }, 820);
  }

  // ---------------------------------------------------------------- symbol switches
  function focusOn(k) {
    if (S.focus === k) return;
    S.focus = k;
    apply();
    var g = k && glyph(k);
    send({ type: 'focus', key: g ? g[1] : null });
    describe(k);
  }
  function glyphs() {
    for (var group in GLYPHS) {
      var html = '';
      GLYPHS[group].forEach(function (g) {
        html += '<div class="g" role="button" tabindex="0" data-k="' + g[0] + '"><svg viewBox="0 0 34 34">' + g[6] + '</svg><span class="nm">' +
          (S.lang === 'zh' ? g[2] : g[3]) + '</span></div>';
      });
      $('g-' + group).innerHTML = html;
    }
    each(document.querySelectorAll('.g'), function (b) {
      var k = b.getAttribute('data-k'), g = glyph(k);
      press(b, function () { set(g[1], !S.set[g[1]]); paint(); describe(k, true); });
      var enter = function () { focusOn(k); };
      var leave = function () { focusOn(null); };
      b.addEventListener('mouseenter', enter); b.addEventListener('focus', enter);
      b.addEventListener('mouseleave', leave); b.addEventListener('blur', leave);
    });
  }
  var descTimer = null;
  function describe(k, now) {
    var box = $('gdesc');
    var put = function () {
      var l = L();
      if (!k) box.innerHTML = l.gIdle;
      else {
        var g = glyph(k), on = S.set[g[1]];
        box.innerHTML = '<b>' + (S.lang === 'zh' ? g[2] : g[3]) + '</b>' + (S.lang === 'zh' ? g[4] : g[5]) + (on ? '' : (S.lang === 'zh' ? '。' : '. ') + l.nowOff);
      }
      box.style.opacity = 1;
    };
    clearTimeout(descTimer);
    if (now) { put(); return; }
    box.style.opacity = 0;
    descTimer = setTimeout(put, 200);
  }

  // ---------------------------------------------------------------- scales (own slider: Coherent has no range input)
  var SCALES = {
    bright: { key: 'opacity', min: 25, max: 100, step: 5, get: function (v) { return Math.round(v * 100); }, put: function (x) { return x / 100; }, out: function (x) { return x + '%'; } },
    size:   { key: 'hud_scale', min: 60, max: 115, step: 5, get: function (v) { return Math.round(v * 100); }, put: function (x) { return x / 100; }, out: function (x) { return x + '%'; } },
    glide:  { key: 'glide_deg', min: 2.0, max: 5.9, step: 0.1, get: function (v) { return v; }, put: function (x) { return Math.round(x * 10) / 10; }, out: function (x) { return x.toFixed(1) + '°'; } }
  };
  function scales() {
    Object.keys(SCALES).forEach(function (id) {
      var c = SCALES[id], box = $('s-' + id);
      box.innerHTML = '<div class="lbl" id="t-' + id + '"></div><div class="track ease" role="button" tabindex="0"><div class="rail"></div>' +
        '<div class="fill"></div><div class="thumb"></div></div><div class="out"></div>';
      var track = box.querySelector('.track'), sent = 0, last = null;
      var valueAt = function (clientX) {
        var r = track.getBoundingClientRect(), p = Math.max(0, Math.min(1, (clientX - r.left) / r.width));
        return Math.round((c.min + p * (c.max - c.min)) / c.step) * c.step;
      };
      var update = function (x, final) {
        x = Math.max(c.min, Math.min(c.max, Math.round(x / c.step) * c.step));
        var v = c.put(x);
        S.set[c.key] = v;
        touched[c.key] = Date.now();
        paintScale(id);
        if (id === 'glide') { render(); fact(); } else apply();
        preview();
        var now = Date.now();
        if (final || (v !== last && now - sent > 70)) { if (v !== last || final) send({ type: 'set', key: c.key, value: v }); sent = now; last = v; }
      };
      var move = function (e) { update(valueAt(e.clientX)); };
      var up = function (e) {
        document.removeEventListener('mousemove', move); document.removeEventListener('mouseup', up);
        cls(track, 'drag', false); cls(track, 'ease', true); update(valueAt(e.clientX), true);
      };
      track.addEventListener('mousedown', function (e) {
        e.preventDefault(); cls(track, 'drag', true); cls(track, 'ease', false); last = null; update(valueAt(e.clientX));
        document.addEventListener('mousemove', move); document.addEventListener('mouseup', up);
      });
      track.addEventListener('keydown', function (e) {
        var d = (e.keyCode === 37 || e.keyCode === 40) ? -1 : (e.keyCode === 39 || e.keyCode === 38) ? 1 : 0;
        if (d) { e.preventDefault(); update(c.get(S.set[c.key]) + d * c.step, true); }
      });
    });
  }
  function paintScale(id) {
    var c = SCALES[id], box = $('s-' + id), x = c.get(S.set[c.key]);
    var p = Math.max(0, Math.min(1, (x - c.min) / (c.max - c.min))) * 100;
    box.querySelector('.fill').style.width = p.toFixed(2) + '%';
    box.querySelector('.thumb').style.left = p.toFixed(2) + '%';
    box.querySelector('.out').textContent = c.out(x);
  }

  // ---------------------------------------------------------------- fine tuning
  var PICKS = {
    lead: { key: 'lead_ms', vals: [0, 20, 40, 60, 80, 100, 150], label: function (v) { return String(v); }, unit: ' ms' },
    tol: { key: 'flare_tol_deg', vals: [0.2, 0.3, 0.5, 0.8], label: function (v) { return '±' + v.toFixed(1) + '°'; }, unit: '' },
    ladder: { key: 'ladder_step_deg', vals: [0, 3, 5, 6, 10], label: function (v) { return v ? v + '°' : L().off; }, unit: '' },
    rah: { key: 'bird_ra_below_ft', vals: [200, 300, 500, 1000, 1500, 2500], label: function (v) { return String(v); }, unit: ' ft' }
  };
  function picks() {
    Object.keys(PICKS).forEach(function (id) {
      var c = PICKS[id], cur = S.set[c.key], vals = c.vals.slice();
      if (vals.indexOf(cur) < 0 && typeof cur === 'number') { vals.push(cur); vals.sort(function (a, b) { return a - b; }); }  // e.g. set by hotkey
      var box = $('p-' + id);
      box.innerHTML = vals.map(function (v, i) {
        return '<span class="pick' + (v === cur ? ' sel' : '') + (id === 'ladder' && !v ? ' word' : '') + '" role="button" tabindex="0" data-v="' + v + '">' +
          c.label(v) + (i === vals.length - 1 ? c.unit : '') + '</span>';
      }).join('');
      var ps = box.querySelectorAll('.pick');
      each(ps, function (b) {
        press(b, function () {
          set(c.key, parseFloat(b.getAttribute('data-v')));
          each(ps, function (o) { cls(o, 'sel', o === b); });
          if (id === 'ladder') render();
          summary();
        });
      });
    });
  }
  function summary() {  // the line on the home page that says what fine tuning is set to
    var s = S.set, l = L();
    $('n-fine').textContent = s.lead_ms + ' ms · ±' + s.flare_tol_deg.toFixed(1) + '° · ' + (s.ladder_step_deg ? s.ladder_step_deg + '°' : l.off);
    var n = 0;
    ALL.forEach(function (g) { if (s[g[1]]) n++; });
    $('n-sym').textContent = fmt(l.symCount, { n: n, all: ALL.length });
  }

  // ---------------------------------------------------------------- the voice
  var voiceKey = '', noticeUntil = 0, noticeTimer = null;
  function voiceFor() {
    if (!S.online) return 'offline';
    var w = S.st ? S.st.why : 'starting';
    return VOICE[w] ? w : 'wait';
  }
  function factText(key) {
    var f = VOICE[key][S.lang][1], rw = S.lang === 'zh' ? '还没对准跑道' : 'no runway lined up';
    if (S.st && S.st.runway) rw = S.st.runway.icao + ' ' + S.st.runway.name + (S.lang === 'zh' ? ' 已对准' : ' lined up');
    return fmt(f, { rw: rw, g: S.set.glide_deg.toFixed(1) });
  }
  // the voice is said in two places: the home page and the peek card
  var VOICE_EL = ['voice', 'pk-voice'], FACT_EL = ['fact', 'pk-fact'];
  function fact() {
    if (Date.now() <= noticeUntil) return;
    var t = factText(voiceKey || voiceFor());
    FACT_EL.forEach(function (id) { $(id).textContent = t; cls($(id), 'notice', false); });
  }
  function voice(quiet) {
    var key = voiceFor();
    var put = function () { VOICE_EL.forEach(function (id) { $(id).textContent = VOICE[key][S.lang][0]; }); fact(); };
    var swap = function (on) { VOICE_EL.concat(FACT_EL).forEach(function (id) { cls($(id), 'swap', on); }); };
    if (quiet || key === voiceKey) { voiceKey = key; put(); return; }
    voiceKey = key;
    swap(true);
    setTimeout(function () { put(); swap(false); }, 520);
  }
  function notice(text) {  // the HUD's flash message, said here too for a moment
    noticeUntil = Date.now() + 2600;
    FACT_EL.forEach(function (id) { $(id).textContent = text; cls($(id), 'notice', true); });
    clearTimeout(noticeTimer);
    noticeTimer = setTimeout(function () { noticeUntil = 0; voice(true); }, 2700);
  }

  // the wordmark: the bird sits where the dot of the i would be (the i is dotless; measured once the font is in)
  function dotTheI() {
    try {
      var b = $('wm-i').getBBox();
      if (b.width > 0) $('wm-dot').setAttribute('transform', 'translate(' + (b.x + b.width / 2 + 2.5).toFixed(1) + ' ' + (b.y + 9).toFixed(1) + ')');
    } catch (e) { /* keep the drawn position */ }
  }

  // ---------------------------------------------------------------- everything that follows the state
  function paint() {
    var s = S.set, st = S.st || {}, l = L();
    cls($('birdie'), 'offline', !S.online);
    cls($('birdie'), 'on', S.online && s.visible);
    $('power').setAttribute('aria-pressed', s.visible ? 'true' : 'false');
    each(document.querySelectorAll('.g'), function (el) {
      var g = glyph(el.getAttribute('data-k')), on = !!s[g[1]];
      cls(el, 'sel', on); cls(el, 'unsel', !on);
    });
    Object.keys(SCALES).forEach(paintScale);
    Object.keys(PICKS).forEach(function (id) {
      var ps = $('p-' + id).querySelectorAll('.pick'), cur = S.set[PICKS[id].key], found = false;
      each(ps, function (p) { var on = parseFloat(p.getAttribute('data-v')) === cur; found = found || on; cls(p, 'sel', on); });
      if (!found) picks();
    });
    summary();
    cls($('lang-zh'), 'sel', S.lang === 'zh');
    cls($('lang-en'), 'sel', S.lang === 'en');
    // RA calibration for the aircraft Birdie sees now
    $('ra-ac').textContent = st.aircraft ? ' · ' + st.aircraft : '';
    var when = st.ra && /^\d{4}-\d\d-\d\d \d\d:\d\d/.exec(st.ra.note || '');  // the note is in the language of its day
    $('ra-note').innerHTML = !st.aircraft ? l.raNoAc : !st.ra ? l.raNone
      : st.ra.ft ? fmt(l.raFmt, { ft: '<span class="v num">' + st.ra.ft.toFixed(1) + '</span>' }) + (when ? ' \u00b7 ' + when[0] : '') : (st.ra.note || '');
    $('ra-btn').textContent = st.on_ground ? l.raBtn : l.raBtnWait;
    cls($('ra-btn'), 'dim', !st.on_ground);
    $('ra-clear').style.display = st.ra && st.ra.ft ? '' : 'none';
    cls($('auto'), 'sel', !!st.autostart);
    $('auto').setAttribute('aria-pressed', st.autostart ? 'true' : 'false');
    cls($('xw'), 'sel', !!s.xwind_mode);
    $('xw').setAttribute('aria-pressed', s.xwind_mode ? 'true' : 'false');
    $('ver').textContent = st.version ? 'Birdie ' + st.version : '';
    paintBorder();
    render();
    voice();
    preview();
    fitPanel();
  }

  // ---------------------------------------------------------------- HUD border (docs/HUD_BORDER.md)
  var clearAsked = 0;
  function paintBorder() {
    var l = L(), st = S.st || {}, b = st.border || null, state = b ? b.state : 'none';
    $('bd-who').textContent = !st.aircraft ? l.raNoAc : st.aircraft + ' · ' + (b && b.seat === 'R' ? l.seatR : l.seatL);
    $('bd-state').textContent = state === 'drawn' ? l.bdDrawn : state === 'mirrored' ? l.bdMirrored : l.bdNone;
    $('n-border').textContent = state === 'drawn' ? l.bdShortDrawn : state === 'mirrored' ? l.bdShortMirrored : l.bdShortNone;
    var can = !!(b && b.can_edit), editing = !!(b && b.editing);
    $('bd-edit').textContent = editing ? l.bdEditing : can ? l.bdEdit : l.bdEditNo;
    cls($('bd-edit'), 'dim', !can || editing);
    $('bd-eye').textContent = l.bdEye;
    $('bd-eye').style.display = b && b.eye_moved ? '' : 'none';
    $('bd-cut').style.display = b && b.cuts_fixed ? '' : 'none';
    $('bd-cut-t').textContent = l.bdCut;
    var fit = b && b.fit_scale;
    $('bd-fit').textContent = fit ? fmt(l.bdFit, { n: Math.round(fit * 100) }) : l.bdNoFit;
    cls($('bd-fit'), 'dim', !fit);
    $('bd-mirror').textContent = l.bdMirror;
    $('bd-mirror').style.display = b && b.other_own ? '' : 'none';
    if (Date.now() - clearAsked > 3500) { $('bd-clear').textContent = l.bdClear; cls($('bd-clear'), 'ask', false); }
    $('bd-clear').style.display = state !== 'none' ? '' : 'none';
  }

  function words() {
    var l = L(), map = { 't-sym': 'sym', 't-sym2': 'sym', 't-fine': 'fine', 't-fine2': 'fine', 't-path': 'path', 't-world': 'world', 't-inst': 'inst',
      't-size': 'size', 't-bright': 'bright', 't-glide': 'glide', 't-lead': 'lead', 't-tol': 'tol', 't-ladder': 'ladder', 't-rah': 'rah', 't-ra': 'ra',
      't-xw': 'xw', 'ra-clear': 'raClear', 't-auto': 'auto', 't-log': 'log', 't-quit': 'quit', 't-pk': 'power',
      't-border': 'border', 't-border2': 'border', 'bd-desc': 'bdDesc' };
    for (var id in map) $(id).textContent = l[map[id]];
    document.documentElement.lang = S.lang === 'zh' ? 'zh-CN' : 'en';
    glyphs(); picks(); describe(null, true);
    voiceKey = ''; pvShown = null; paint();
  }

  // click / Enter / Space
  function press(el, fn) {
    el.addEventListener('click', function (e) { e.preventDefault(); fn(e); });
    el.addEventListener('keydown', function (e) { if (e.keyCode === 13 || e.keyCode === 32) { e.preventDefault(); fn(e); } });
  }

  function controls() {
    press($('power'), function () { set('visible', !S.set.visible); paint(); });
    press($('pk-power'), function () { set('visible', !S.set.visible); paint(); });
    press($('xw'), function () { set('xwind_mode', !S.set.xwind_mode); paint(); });
    press($('lang-zh'), function () { set('language', 'zh-CN'); S.lang = 'zh'; words(); });
    press($('lang-en'), function () { set('language', 'en'); S.lang = 'en'; words(); });
    press($('ra-btn'), function () { if (S.st && S.st.on_ground) act('ra_calibrate'); });
    press($('ra-clear'), function () { act('ra_clear'); });
    press($('auto'), function () { act('autostart', !(S.st && S.st.autostart)); });
    press($('t-log'), function () { act('open_log'); });
    press($('bd-edit'), function () { var b = S.st && S.st.border; if (b && b.can_edit && !b.editing) act('border_edit'); });
    press($('bd-fit'), function () { var b = S.st && S.st.border; if (b && b.fit_scale) act('border_fit'); });
    press($('bd-mirror'), function () { act('border_mirror'); });
    press($('bd-clear'), function () {  // two gentle steps: a trace takes a minute to redo
      var el = $('bd-clear');
      if (Date.now() - clearAsked < 3500) { clearAsked = 0; act('border_clear'); return; }
      clearAsked = Date.now();
      el.textContent = L().bdClearAsk; cls(el, 'ask', true);
      setTimeout(function () { if (Date.now() - clearAsked >= 3400) paintBorder(); }, 3500);
    });
    each(document.querySelectorAll('[data-go]'), function (b) { press(b, function () { go(parseInt(b.getAttribute('data-go'), 10)); }); });
    $('panel').addEventListener('scroll', function () { if ($('panel').scrollLeft) $('panel').scrollLeft = 0; });  // Tab into a hidden page
    var asked = 0, quit = $('t-quit');
    press(quit, function () {  // two gentle steps: it is easy to mean "hide the HUD" here
      if (Date.now() - asked < 3500) { act('quit'); return; }
      asked = Date.now();
      quit.textContent = L().quitAsk; cls(quit, 'ask', true);
      setTimeout(function () { quit.textContent = L().quit; cls(quit, 'ask', false); }, 3500);
    });
    document.addEventListener('keydown', function (e) {
      if (e.keyCode !== 27) return;  // Esc: back, then away
      if (S.page === 3) go(2); else if (S.page) go(0); else host({ host: 'close' });
    });
    if (PEEK) {  // a click anywhere but the switch opens the window; the strip lifts its line in each time it shows
      $('peekcard').addEventListener('click', function (e) {
        if (e.target.closest && e.target.closest('.knob')) return;
        host({ host: 'open' });
      });
      window.chrome.webview.addEventListener('message', function (e) {
        if (e.data !== 'show') return;
        fitHost();
        each(document.querySelectorAll('.peekcard .rise'), function (el) { cls(el, 'rise', false); void el.offsetWidth; cls(el, 'rise', true); });
      });
    } else if (HOST) {
      press($('x'), function () { focusOn(null); host({ host: 'close' }); });
      // fallback when the browser has no app-region support: the page asks the window to move
      var drag = function (e) {
        if (e.button !== 0 || (e.target.closest && e.target.closest('[role="button"]'))) return;
        host({ host: 'drag' });
      };
      $('grip').addEventListener('mousedown', drag);
      $('head').addEventListener('mousedown', drag);
      window.addEventListener('blur', function () { focusOn(null); });
    }
  }

  // the root size follows the panel's width (the sim lets the user resize it)
  var lastW = 0;
  function fit() {
    var w = COUI ? ($('birdie').clientWidth || window.innerWidth) : DESIGN_W;  // the window and the peek: 14 px
    if (w !== lastW && w > 0) {
      lastW = w;
      document.documentElement.style.fontSize = Math.max(11, Math.min(22, w / DESIGN_W * 14)).toFixed(2) + 'px';
      if (pvShown) $('pvwrap').style.height = $('pv').offsetHeight + 'px';
      fitPanel();
    }
  }

  var started = false;
  function start() {
    if (started) return;
    started = true;
    cls(document.documentElement, COUI ? 'coui' : HOST ? 'host' : 'web', true);
    if (PEEK) {
      cls(document.documentElement, 'peek', true);
      if (document.fonts && document.fonts.ready) document.fonts.ready.then(function () { dotTheI(); fitHost(); });
    }
    var pref = (navigator.language || '').toLowerCase();
    S.lang = pref.indexOf('zh') === 0 || !pref ? 'zh' : 'en';
    hud = $('hud');
    lights(); scales(); controls();
    words();
    if (location.hash === '#border') go(3);  // back from tracing the border in Birdie's own window
    fit();
    window.addEventListener('resize', fit);
    if (COUI) setInterval(fit, 500);  // the sim resizes panels without a resize event
    window.addEventListener('load', fitPanel);
    setTimeout(fitPanel, 300);  // fonts arrive late
    connect();
  }

  // the sim's panel element (TemplateElement comes from /JS/common.js); a plain element in a browser
  var Base = window.TemplateElement || HTMLElement;
  class BirdiePanel extends Base {
    connectedCallback() {
      if (super.connectedCallback) super.connectedCallback();
      if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
      else setTimeout(start, 0);
    }
  }
  window.customElements.define('ingamepanel-birdie', BirdiePanel);
  if (typeof checkAutoload === 'function') checkAutoload();
})();
