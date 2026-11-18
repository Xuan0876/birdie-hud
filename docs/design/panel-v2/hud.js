// Birdie panel v2 proposals: the HUD preview, shared by the three mockups.
// Drawing is copied from panel/.../birdie.js build() (Birdie's own canvas coordinates, 1280 x 880 around the boresight),
// so every proposal shows the same strokes the real panel and the real HUD draw.
window.BirdieHUD = (function () {
  'use strict';
  var D = 36.5714, CX = 640, HY = 420, BORE = 329.143;

  var DEFAULT = {
    visible: true, glide_deg: 3.0, hud_scale: 1.0, opacity: 0.85, lead_ms: 40, flare_tol_deg: 0.3, ladder_step_deg: 6,
    show_bird: true, show_fpa_num: true, show_vs_num: true, show_flare: true, show_glide: true, show_horizon: true,
    show_hdg: true, show_pitch_minor: true, show_runway: true, show_att: true, show_speed: true, show_alt: true,
    show_vs: true, show_roll: true, show_gs: true, show_bird_ra: true
  };

  // [key, setting, zh, en, zh description, icon]
  var GLYPHS = {
    path: [
      ['bird', 'show_bird', '小鸟', 'Bird', '飞机真正飞去的方向',
        '<circle class="ic" cx="17" cy="19" r="4.5"/><path class="ic" d="M12.5 19H3M21.5 19H31M17 14.5V8"/>'],
      ['fpa', 'show_fpa_num', '航迹角', 'Path angle', '小鸟右边：现在的航迹角',
        '<circle class="ic" cx="9" cy="19" r="3.5"/><path class="ic" d="M12.5 19h4"/><text class="t" x="18" y="22" font-size="11">3.0</text>'],
      ['vsn', 'show_vs_num', '升降速度', 'Vert. speed', '小鸟左边：升降速度（ft/min）',
        '<text class="t" x="1" y="22" font-size="11">700</text><path class="ic" d="M19 19h3.5"/><circle class="ic" cx="26" cy="19" r="3.5"/>'],
      ['bra', 'show_bird_ra', 'RA 高度', 'Radio alt', '小鸟下方：无线电高度，2500 ft 以下显示',
        '<circle class="ic" cx="17" cy="10" r="3.5"/><path class="ic" d="M13.5 10H7M20.5 10H27"/><text class="t" x="17" y="27" font-size="11" text-anchor="middle">320</text>'],
      ['flare', 'show_flare', '拉平提示', 'Flare cue', '50 ft 以下在小鸟上方闪：+ 正好，++ 拉多了',
        '<path class="ic" d="M5 17h9M9.5 12.5v9M20 17h9M24.5 12.5v9"/>'],
      ['glide', 'show_glide', '下滑线', 'Glide line', '理想下滑角在地面上的那条线',
        '<path class="ic" d="M3 13h28" opacity=".5"/><path class="ic" d="M3 21h6M12 21h6M21 21h6"/>']],
    world: [
      ['horizon', 'show_horizon', '地平线', 'Horizon', '真实的地平线',
        '<path class="ic" d="M2 18h30"/><path class="ic" d="M11 18a6 6 0 0 1 12 0" opacity=".55"/>'],
      ['hdg', 'show_hdg', '航向', 'Heading', '地平线上的航向刻度',
        '<path class="ic" d="M2 13h30M6 13v5M14 13v8M22 13v5M30 13v8"/>'],
      ['minor', 'show_pitch_minor', '俯仰刻度', 'Pitch marks', '每 2.5° 的短刻度',
        '<path class="ic" d="M12 9h10M12 17h10M12 25h10"/>'],
      ['runway', 'show_runway', '跑道', 'Runway', '选中的跑道轮廓',
        '<path class="ic" d="M14 7h6l7 21H7z"/>']],
    inst: [
      ['att', 'show_att', '机头', 'Aircraft', '机头指向：读俯仰和坡度',
        '<path class="ic" d="M3 17h9v4M31 17h-9v4"/><path class="ic" d="M15 15h4v4h-4z"/>'],
      ['speed', 'show_speed', '速度', 'Speed', '左侧速度带',
        '<path class="ic" d="M20 4v26M14 9h6M14 15h6M14 21h6M14 27h6"/><path class="ic" d="M23 17l5-2.5v5z"/>'],
      ['alt', 'show_alt', '高度', 'Altitude', '右侧高度带与无线电高度',
        '<path class="ic" d="M12 4v26M12 9h5M12 15h5M12 21h5M12 27h5"/><path class="ic" d="M17 13h12v8H17z"/>'],
      ['vs', 'show_vs', '升降', 'VS needle', '高度带右边的升降速度指针',
        '<path class="ic" d="M30 13L8 24"/><path class="ic" d="M6 10v18" opacity=".5"/>'],
      ['roll', 'show_roll', '坡度', 'Bank', '顶上的坡度刻度',
        '<path class="ic" d="M4 24a13 13 0 0 1 26 0"/><path class="ic" d="M17 11l-3 5h6z"/>'],
      ['gs', 'show_gs', '地速与风', 'GS & wind', '地速，风向风速',
        '<text class="t" x="2" y="15" font-size="11">GS</text><path class="ic" d="M22 28V12M18 16l4-4 4 4"/>']]
  };
  var ALL = [].concat(GLYPHS.path, GLYPHS.world, GLYPHS.inst);
  function glyph(k) { for (var i = 0; i < ALL.length; i++) if (ALL[i][0] === k) return ALL[i]; return null; }

  function f1(v) { return v.toFixed(1); }
  function P(d, cls, extra) { return '<path class="' + (cls || 'stroke') + '" d="' + d + '" pathLength="1" ' + (extra || '') + '/>'; }
  function TX(x, y, s, size, anchor) {
    return '<text class="htxt" x="' + x + '" y="' + y + '" font-size="' + size + '" text-anchor="' + (anchor || 'middle') + '">' + s + '</text>';
  }
  function runwayHalf(y) { return 13 + (y - 452) / (700 - 452) * (92 - 13); }
  function fixedTransform(k) { return 'translate(' + CX + ' ' + BORE + ') scale(' + k.toFixed(4) + ') translate(' + (-CX) + ' ' + (-BORE) + ')'; }

  function build(s, rw) {
    var out = [], h, k, y, e, j, side, glide = s.glide_deg, ladder = s.ladder_step_deg, scale = s.hud_scale;
    rw = rw || '31';
    out.push('<g class="el" data-el="horizon">' + P('M-40 ' + HY + 'H' + (CX - runwayHalf(452) - 6) + 'M' + (CX + runwayHalf(452) + 6) + ' ' + HY + 'H1320', 'stroke draw') + '</g>');
    var hd = '', hl = '';
    for (h = 50; h <= 95; h += 5) {
      var x = CX + (h - 71) * D, ten = h % 10 === 0;
      hd += 'M' + f1(x) + ' ' + HY + 'v' + (ten ? 14 : 8);
      if (ten) hl += TX(f1(x), HY + 40, String(h / 10), 21);
    }
    out.push('<g class="el" data-el="hdg">' + P(hd, 'stroke thin draw') + '<g class="fadein" style="animation-delay:.9s">' + hl + '</g></g>');
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
      if (!(y > 452 && y < 720)) mn += 'M' + (CX - 12) + ' ' + f1(y) + 'H' + (CX + 12);
    }
    out.push('<g class="el" data-el="minor">' + (lad ? P(lad, 'stroke draw') : '') + (mn ? P(mn, 'stroke thin draw') : '') +
      '<g class="fadein" style="animation-delay:1s">' + ll + '</g></g>');
    out.push('<g class="el" data-el="runway">' + P('M' + (CX - 13) + ' 452H' + (CX + 13) + 'L' + (CX + 92) + ' 700H' + (CX - 92) + 'Z', 'stroke draw') +
      '<g class="fadein" style="animation-delay:1.1s">' + TX(CX, 730, rw, 20) + '</g></g>');
    var gy = HY + glide * D, span = Math.min(18, 10 * scale) * D;
    var gap = (gy > 452 && gy < 700) ? runwayHalf(gy) + 4 : 0;
    var gl = gap ? 'M' + (CX - span) + ' ' + f1(gy) + 'H' + (CX - gap) + 'M' + (CX + gap) + ' ' + f1(gy) + 'H' + (CX + span)
                 : 'M' + (CX - span) + ' ' + f1(gy) + 'H' + (CX + span);
    out.push('<g class="el" data-el="glide"><path class="stroke dash" d="' + gl + '"/>' + TX(CX + span + 6, f1(gy + 8), '-' + glide.toFixed(1), 20, 'start') + '</g>');
    var by = HY + 3.0 * D, r = 17;
    out.push('<g class="birdfloat">' +
      '<g class="el" data-el="bird">' + P('M' + (CX - r) + ' ' + f1(by) + 'a' + r + ' ' + r + ' 0 1 0 ' + (2 * r) + ' 0a' + r + ' ' + r + ' 0 1 0 ' + (-2 * r) + ' 0M' +
        (CX - r) + ' ' + f1(by) + 'H' + (CX - 3 * r) + 'M' + (CX + r) + ' ' + f1(by) + 'H' + (CX + 3 * r) + 'M' + CX + ' ' + f1(by - r) + 'V' + f1(by - 2.1 * r), 'stroke draw') + '</g>' +
      '<g class="el" data-el="fpa">' + TX(CX + 3 * r + 10, f1(by + 9), '−3.0', 22, 'start') + '</g>' +
      '<g class="el" data-el="vsn">' + TX(CX - 3 * r - 10, f1(by + 9), '−700', 22, 'end') + '</g>' +
      '<g class="el" data-el="bra">' + TX(CX, f1(by + r + 30), '320', 22) + '</g>' +
      '<g class="el" data-el="flare"><g class="flarecue" opacity="0">' + P('M' + (CX - 30) + ' ' + f1(by - 70) + 'h18M' + (CX - 21) + ' ' + f1(by - 79) + 'v18M' +
        (CX + 12) + ' ' + f1(by - 70) + 'h18M' + (CX + 21) + ' ' + f1(by - 79) + 'v18') + '</g></g></g>');
    var fx = [], v, sp = 'M140.9 328.9V692.1M68.1 328.9H161M68.1 692.1H161', sl = '';
    for (v = 100; v <= 180; v += 10) {
      y = 510.5 - (v - 140) * 4.25;
      if (y < 333 || y > 688) continue;
      sp += 'M128.9 ' + f1(y) + 'H140.9';
      if (v % 20 === 0) sl += TX(94, f1(y + 9.3), String(v), 25);
    }
    fx.push('<g class="el" data-el="speed">' + P(sp, 'stroke draw') + '<path class="solid" d="M119.5 509.5V512.6H147.3L160.6 517.5V504.6L147.3 509.5Z"/><g class="fadein" style="animation-delay:.8s">' + sl + '</g></g>');
    fx.push('<g class="el" data-el="gs"><g class="fadein" style="animation-delay:1s">' + TX(68, 300, 'GS 142', 24, 'start') + TX(298, 258, '264', 22, 'end') +
      TX(306, 257, '/', 20) + TX(315, 258, '8', 22, 'start') + '</g>' + P('M276 298V268M269.5 280L276 268L282.5 280', 'stroke draw', 'transform="rotate(193 276 283)"') + '</g>');
    var al = 'M1057.4 692.1H1133.2M1113.1 328.9V692.1M1057.4 328.9H1133.2', all = '';
    for (v = -300; v <= 1000; v += 100) {
      y = 510.5 - (v - 340) * 0.32;
      if (y < 333 || y > 688 || (y > 476 && y < 545)) continue;
      al += 'M1104.6 ' + f1(y) + 'H1113.1';
      if (v % 500 === 0 && !(y > 468 && y < 559)) all += TX(1081, f1(y + 8.5), String(v / 100), 25);
    }
    al += 'M1113.1 529.6H1057.3V491.4H1113.1V480.1H1150.8V540.9H1113.1V529.6';
    fx.push('<g class="el" data-el="alt">' + P(al, 'stroke draw') + '<g class="fadein" style="animation-delay:.9s">' + all + TX(1110, 522, '3', 34, 'end') +
      TX(1132, 520, '40', 26) + TX(1095, 730, '1013', 26) + TX(1095, 772, 'RA 320', 26) + '</g></g>');
    var yo = 95.27;
    fx.push('<g class="el" data-el="vs"><path class="solid" d="M1165 508.5H1185V512.5H1165Z"/>' + P('M1220 ' + f1(510.5 + 0.26 * yo) + 'L1183 ' + f1(510.5 + yo), 'stroke draw', 'stroke-width="4"') +
      '<g class="fadein" style="animation-delay:1s">' + TX(1192, f1(518 + yo + 14), '-07', 22) + '</g></g>');
    var arc = '';
    for (j = 0; j <= 39; j++) {
      var a = (155.16 + (24.84 - 155.16) * j / 39) * Math.PI / 180;
      arc += (j ? 'L' : 'M') + f1(640 + 190 * Math.cos(a)) + ' ' + f1(329.1 - 190 * Math.sin(a));
    }
    arc += 'M735.2 164L749.3 139.5M705.1 150L712.8 129.3M673 141.5L677 119.3M544.8 164L530.7 139.5M574.9 150L567.2 129.3M607 141.5L603 119.3M774.5 194.5L794.5 174.5M505.5 194.5L485.5 174.5M813 249L832 240M467 249L448 240';
    arc += 'M640 138.4L652.2 118.2H627.8ZM640.2 140L630.2 161.9H650.2ZM629 164.9L621 181H659L651 164.9Z';
    fx.push('<g class="el" data-el="roll">' + P(arc, 'stroke draw') + '</g>');
    fx.push('<g class="el" data-el="att">' + P('M625 335V329H595M637 332H643V326H637ZM655 335V329H685', 'stroke draw', 'stroke-width="3.4"') + '</g>');
    out.push('<g class="fixed" transform="' + fixedTransform(scale) + '">' + fx.join('') + '</g>');
    return out.join('');
  }

  function lights() {
    var c = '';
    for (var i = 0; i <= 9; i++) {
      var t = i / 9, y = 452 + t * 248, h = runwayHalf(y) + 3, r = (1 + t * 2.2).toFixed(1);
      c += '<circle cx="' + f1(CX - h) + '" cy="' + f1(y) + '" r="' + r + '" style="animation-delay:' + (i * 0.37).toFixed(2) + 's"/>' +
           '<circle cx="' + f1(CX + h) + '" cy="' + f1(y) + '" r="' + r + '" style="animation-delay:' + (i * 0.37 + 0.2).toFixed(2) + 's"/>';
    }
    return c;
  }

  // the dusk sky drawn in canvas units, far past every edge, so any viewBox / aspect still shows sky (horizon at HY)
  var SKY = '<defs><linearGradient id="bh-sky" gradientUnits="userSpaceOnUse" x1="0" y1="-400" x2="0" y2="1400">' +
    '<stop offset="0" stop-color="#1d1622"/><stop offset=".2" stop-color="#2f2234"/><stop offset=".33" stop-color="#5b3a4f"/>' +
    '<stop offset=".405" stop-color="#a2606a"/><stop offset=".44" stop-color="#e0927a"/><stop offset=".4555" stop-color="#f3c497"/>' +
    '<stop offset=".4556" stop-color="#3a2a2c"/><stop offset=".56" stop-color="#2a1f22"/><stop offset="1" stop-color="#1a1416"/></linearGradient>' +
    '<radialGradient id="bh-sun" gradientUnits="userSpaceOnUse" cx="640" cy="420" r="520" gradientTransform="translate(640 420) scale(1 .38) translate(-640 -420)">' +
    '<stop offset="0" stop-color="#ffd6a8" stop-opacity=".5"/><stop offset="1" stop-color="#ffd6a8" stop-opacity="0"/></radialGradient></defs>' +
    '<rect x="-3000" y="-3000" width="7280" height="7000" fill="url(#bh-sky)"/><rect class="bh-sunr" x="-400" y="-200" width="2080" height="1240" fill="url(#bh-sun)"/>';

  function cls(el, c, on) {
    var cur = (el.getAttribute('class') || '').split(/\s+/).filter(function (x) { return x && x !== c; });
    if (on) cur.push(c);
    el.setAttribute('class', cur.join(' '));
  }

  // A preview: one <svg> (viewBox chosen by the page). Returns an object to re-apply settings / focus.
  function Preview(svg, s, opts) {
    opts = opts || {};
    var rw = opts.runway || '31', key = '', hud;
    svg.innerHTML = (opts.sky ? SKY : '') + '<g class="lights">' + lights() + '</g><g class="hud"></g>';
    hud = svg.querySelector('.hud');
    var self = {
      focus: null,
      update: function () {
        var k = [s.glide_deg, s.ladder_step_deg, rw].join('|');
        if (k !== key) {
          var first = !key;
          key = k;
          hud.innerHTML = build(s, rw);
          if (!first) { var n = hud.querySelectorAll('.draw, .fadein'); for (var i = 0; i < n.length; i++) n[i].setAttribute('class', n[i].getAttribute('class').replace(/ ?(draw|fadein)/g, '')); }
        }
        var floor = opts.floor || 0;
        hud.setAttribute('opacity', Math.max(floor, s.opacity).toFixed(2));
        cls(hud, 'away', !s.visible);
        cls(hud, 'spot', !!self.focus);
        var els = hud.querySelectorAll('.el');
        for (var j = 0; j < els.length; j++) {
          var g = glyph(els[j].getAttribute('data-el'));
          cls(els[j], 'off', g ? !s[g[1]] : false);
          cls(els[j], 'focus', self.focus === els[j].getAttribute('data-el'));
        }
        var fl = hud.querySelector('.flarecue');
        if (fl) { var on = self.focus === 'flare' && s.show_flare; cls(fl, 'blink', on); fl.setAttribute('opacity', on ? '1' : '0'); }
        var fixed = hud.querySelector('.fixed');
        if (fixed) fixed.setAttribute('transform', fixedTransform(s.hud_scale));
      },
      setFocus: function (k) { self.focus = k; self.update(); },
      hud: function () { return hud; }
    };
    self.update();
    return self;
  }

  var CSS =
    '.hud{transition:opacity 1.2s cubic-bezier(.22,.61,.36,1)}.hud.away{opacity:0!important}' +
    '.hud .el{transition:opacity .9s cubic-bezier(.22,.61,.36,1),filter .9s}.hud .el.off{opacity:0;filter:blur(5px)}' +
    '.hud.spot .el{opacity:.16}.hud.spot .el.focus{opacity:1}.hud.spot .el.off.focus{opacity:.35;filter:none}' +
    '.hud .el.focus{filter:drop-shadow(0 0 6px rgba(242,180,138,.95))}' +
    '.stroke{fill:none;stroke:#57f27f;stroke-width:3;stroke-linecap:square;stroke-linejoin:bevel}.stroke.thin{stroke-width:2.2}' +
    '.solid{fill:#57f27f}.htxt{fill:#57f27f;font-family:"Barlow Semi Condensed","Bahnschrift","Arial Narrow",sans-serif;font-weight:500}' +
    '.dash{stroke-dasharray:14 9}.draw{stroke-dasharray:1;stroke-dashoffset:1;animation:bh-draw 1.8s cubic-bezier(.22,.61,.36,1) forwards}' +
    '@keyframes bh-draw{to{stroke-dashoffset:0}}.fadein{opacity:0;animation:bh-fade 1.4s cubic-bezier(.22,.61,.36,1) forwards}' +
    '@keyframes bh-fade{to{opacity:1}}.birdfloat{animation:bh-float 6.5s ease-in-out infinite}' +
    '@keyframes bh-float{0%,100%{transform:translate(0,0)}33%{transform:translate(2px,-2px)}66%{transform:translate(-2px,1.5px)}}' +
    '.flarecue.blink{animation:bh-blink 1.6s ease-in-out infinite}@keyframes bh-blink{0%,100%{opacity:.2}50%{opacity:1}}' +
    '.lights circle{fill:#ffd9a8;animation:bh-tw 5s ease-in-out infinite}@keyframes bh-tw{0%,100%{opacity:.35}50%{opacity:.9}}' +
    '.sky{background:linear-gradient(180deg,#2f2234 0%,#5b3a4f 17%,#a2606a 32%,#e0927a 40.5%,#f3c497 43.8%,#3a2a2c 43.95%,#2a1f22 63%,#1d1618 100%)}' +
    '.bh-sunr{animation:bh-sun 22s ease-in-out infinite alternate}@keyframes bh-sun{from{transform:translateX(-26px);opacity:.8}to{transform:translateX(26px);opacity:1}}' +
    '@media (prefers-reduced-motion:reduce){.draw{stroke-dashoffset:0;animation:none}.fadein{opacity:1;animation:none}.birdfloat,.lights circle{animation:none}}';
  var st = document.createElement('style');
  st.textContent = CSS;
  document.head.appendChild(st);

  // the voice, as in the real panel
  var VOICE = {
    show: ['在你身边', 'ZSNB 31 已对准 · 下滑 {g}°'],
    off: ['我先安静一会儿', '点一下圆，或按 Ctrl+Alt+Insert 叫我回来']
  };

  return { DEFAULT: DEFAULT, GLYPHS: GLYPHS, ALL: ALL, glyph: glyph, Preview: Preview, cls: cls, VOICE: VOICE, VIEW_W: 1280 };
})();
