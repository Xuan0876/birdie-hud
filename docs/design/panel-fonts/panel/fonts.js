// font comparison only (docs/design/panel-fonts): ?font=<key>&page=<0|2|3> swaps the Latin faces of the real panel.
// Chinese stays on the Noto SC subsets; the browser falls back per glyph (Coherent GT would need merged fonts).
(function () {
  var C = {
    now:  null,
    jost: { sans: 'Jost', sansW: 300, serif: 'Cormorant Garamond', serifW: 500, serifScale: 1.12 },
    nunito: { sans: 'Nunito Sans', sansW: 300, serif: 'Cormorant Garamond', serifW: 500, serifScale: 1.12 },
    garamond: { sans: 'EB Garamond', sansW: 400, sansScale: 1.06, serif: 'Cormorant Garamond', serifW: 500, serifScale: 1.12 },
    fraunces: { sans: 'Figtree', sansW: 300, serif: 'Fraunces', serifW: 300, serifScale: 1 },
    news: { sans: 'Source Sans 3', sansW: 300, sansScale: 1.03, serif: 'Newsreader', serifW: 400, serifScale: 1.04 }
  };
  var q = location.search;
  var key = (q.match(/[?&]font=(\w+)/) || [])[1] || 'now';
  var page = (q.match(/[?&]page=(\d)/) || [])[1] || '0';
  var c = C[key];
  var css = '*{transition-duration:0s!important;transition-delay:0s!important;animation-duration:0s!important;animation-delay:0s!important}.pvwrap{display:none!important}html.web .pocket{margin:0 auto!important;box-shadow:none!important}html.web #birdie{background:#261c21}';
  if (c) {
    [c.sans, c.serif].forEach(function (f) {
      css += '@font-face{font-family:"' + f + '";src:url("cand/' + f.replace(/ /g, '') + '.ttf");font-weight:100 900}';
    });
    if (c.serif === 'Fraunces') css += '.serif,.gh,.bd-state{font-variation-settings:"SOFT" 100}';
    css += '#birdie,.picks .pick.word{font-family:"' + c.sans + '","Birdie Sans",sans-serif!important;font-weight:' + c.sansW + '}' +
      '.serif,.gh,.bd-state{font-family:"' + c.serif + '","Birdie Serif",serif!important;font-weight:' + c.serifW + '!important}';
    if (c.sansScale) css += '#birdie{font-size:' + c.sansScale + 'rem}';
    if (c.serifScale !== 1) css += '.voice{font-size:' + (1.57 * c.serifScale) + 'rem!important}.back .serif{font-size:' + (1.36 * c.serifScale) + 'rem!important}' +
      '.bd-state{font-size:' + (1.14 * c.serifScale) + 'rem!important}';
  }
  var s = document.createElement('style');
  s.textContent = css;
  document.head.appendChild(s);
  if (page === '3' && location.hash !== '#border') location.hash = '#border';
  window.addEventListener('load', function () {
    var tries = 0;
    (function nav() {
      var p = document.getElementById('panel');
      if (page !== '2' || (p && p.getAttribute('data-p') === '2') || ++tries > 40) return;
      var n = document.querySelector('#page0 [data-go="2"]'); if (n) n.click();
      setTimeout(nav, 150);
    })();
  });
})();
