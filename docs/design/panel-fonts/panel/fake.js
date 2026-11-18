// preview only: a fake Birdie, so the page never reaches the real one
(function () {
  var border = { editing: false, can_edit: true, seat: 'L', state: 'drawn', other_own: false, eye_moved: true, cuts_fixed: true, fit_scale: 0.85 };
  window.__border = border;
  function FakeWS() {
    var self = this; this.readyState = 1;
    setTimeout(function () { self.onopen && self.onopen(); }, 10);
    window.__ws = this;
  }
  FakeWS.prototype.send = function (t) { console.log('send', t); var m = JSON.parse(t); if (m.type === 'action' && m.name === 'hello') this.push(); };
  FakeWS.prototype.close = function () {};
  FakeWS.prototype.push = function () {
    var st = { type: 'state', settings: { visible: true, glide_deg: 3, hud_scale: 1, opacity: 1, lead_ms: 0, flare_tol_deg: 0.3, ladder_step_deg: 6, language: '', bird_ra_below_ft: 500, xwind_mode: false },
      status: { showing: true, why: 'show', aircraft: 'FSLabs A321-200 CFM', lang: location.search.indexOf('en') >= 0 ? 'en' : 'zh', on_ground: true, version: '0.9.0-beta', border: window.__border, ra: null, runway: null } };
    var self = this; setTimeout(function () { self.onmessage && self.onmessage({ data: JSON.stringify(st) }); }, 0);
  };
  window.WebSocket = FakeWS;
})();
