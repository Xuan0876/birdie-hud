// preview only: a fake Birdie, so the page never reaches the real one. ?why=<status.why> = the HUD is hidden for that reason
// ?cam=ok|outside|none|nosim|replay = status.camera.state, ?camerr=<text> = its last error; a "set" is echoed back
(function () {
  var border = { editing: false, can_edit: true, seat: 'L', state: 'drawn', other_own: false, eye_moved: true, cuts_fixed: true, fit_scale: 0.85 };
  window.__border = border;
  function FakeWS() {
    var self = this; this.readyState = 1;
    setTimeout(function () { self.onopen && self.onopen(); }, 10);
    window.__ws = this;
  }
  var sets = {};  // what the page set, echoed back like Birdie does
  FakeWS.prototype.send = function (t) {
    console.log('send', t); var m = JSON.parse(t);
    if (m.type === 'action' && m.name === 'hello') this.push();
    if (m.type === 'set') { sets[m.key] = m.value; this.push(); }
  };
  FakeWS.prototype.close = function () {};
  FakeWS.prototype.push = function () {
    var st = { type: 'state', settings: { visible: true, glide_deg: 3, hud_scale: 1, opacity: 1, lead_ms: 0, flare_tol_deg: 0.3, ladder_step_deg: 6, language: '', bird_ra_below_ft: 500, xwind_mode: false },
      status: { showing: !/[?&]why=/.test(location.search), why: ((location.search.match(/[?&]why=(\w+)/) || [])[1]) || 'show', aircraft: 'FSLabs A321-200 CFM', lang: location.search.indexOf('en') >= 0 ? 'en' : 'zh', on_ground: true, version: '2.0.0-beta', border: window.__border, ra: null, runway: null,
        ingame: { state: ((location.search.match(/[?&]ig=(\w+)/) || [])[1]) || 'none', dir: 'C:\\Users\\me\\AppData\\Local\\Packages\\Microsoft.Limitless_8wekyb3d8bbwe\\LocalCache\\Packages\\Community',
                  chosen: false, has_pack: true, note: ((location.search.match(/[?&]ignote=(\w+)/) || [])[1]) || '', err: '', sim_running: true } } };
    // ?set=key:value,key:value overrides settings (numbers, true/false, or text)
    var m = /[?&]set=([^&]*)/.exec(location.search);
    if (m) decodeURIComponent(m[1]).split(',').forEach(function (kv) {
      var i = kv.indexOf(':'), k = kv.slice(0, i), v = kv.slice(i + 1);
      st.settings[k] = v === 'true' ? true : v === 'false' ? false : isNaN(+v) ? v : +v;
    });
    for (var k in sets) st.settings[k] = sets[k];
    var cam = ((location.search.match(/[?&]cam=(\w+)/) || [])[1]) || 'ok', err = /[?&]camerr=([^&]*)/.exec(location.search);
    st.status.camera = { source: st.settings.camera_source || 'chaseplane', state: cam, vfov: cam === 'ok' || cam === 'outside' ? 58.2 : null,
                         errors: err ? 12 : 0, error: err ? decodeURIComponent(err[1]) : '', availability: 0, aircraft: 'FSLabs A321-200 CFM', origin: 21.38 };
    // the Terrain page: a cache, the Alps package installed (tile counts and sizes as the real store reports them)
    var en = st.status.lang === 'en', MB = 1048576;
    var rg = [['cn_sw', '中国西南（云贵川渝）', 'SW China (Yunnan, Guizhou, Sichuan)', 169, 486729464], ['tibet', '青藏高原', 'Tibetan Plateau', 260, 748814560],
      ['cn_se', '中国东南沿海', 'SE China coast', 188, 541450528], ['cn_n', '华北 · 华中', 'North & central China', 170, 489609520],
      ['cn_ne', '东北', 'NE China', 247, 646093832], ['cn_nw', '新疆 · 甘肃', 'Xinjiang & Gansu', 448, 1290265088],
      ['taiwan', '台湾', 'Taiwan', 14, 40320784], ['korea', '韩国', 'Korea', 32, 92161792], ['japan', '日本', 'Japan', 148, 426248288],
      ['himalaya', '尼泊尔 · 喜马拉雅', 'Nepal & Himalaya', 45, 129602520], ['alps', '阿尔卑斯', 'Alps', 59, 169923304],
      ['iberia', '伊比利亚南部 · 直布罗陀', 'Southern Iberia & Gibraltar', 27, 77761512], ['norway', '挪威', 'Norway', 291, 437776296],
      ['us_west', '美国西部山区', 'Western US mountains', 349, 1005139544], ['alaska', '阿拉斯加南部', 'Southern Alaska', 171, 285129576],
      ['mexico', '墨西哥中南部', 'Central & southern Mexico', 60, 172803360], ['nz', '新西兰', 'New Zealand', 63, 181443528]];
    st.status.terrain = { cache_tiles: 23, cache_bytes: 610 * MB, pkg_tiles: 59, pkg_bytes: 169923304, disk_free: 91 * 1024 * MB, import_files: 0,
      dir: 'C:\\Users\\me\\AppData\\Local\\Birdie\\dem', is_default: true, moving: false, stream: 'on',
      job: { kind: '', done: 0, total: 0, rate: 0, error: '', failed: 0, label: '', tile: '' },
      regions: rg.map(function (r) { return { id: r[0], name: en ? r[2] : r[1], tiles: r[3], bytes: r[4], have: r[0] === 'alps' ? r[3] : 0 }; }) };
    var self = this; setTimeout(function () { self.onmessage && self.onmessage({ data: JSON.stringify(st) }); }, 0);
  };
  window.WebSocket = FakeWS;
})();
