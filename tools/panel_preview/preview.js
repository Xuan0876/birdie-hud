// preview only (tools/panel_preview): ?page=N opens a sub page (11 = Camera source), ?still turns animations off for screenshots
(function () {
  var q = location.search;
  var page = (q.match(/[?&]page=(\d+)/) || [])[1] || '0';
  if (/[?&]still/.test(q)) {
    var s = document.createElement('style');
    s.textContent = '*,*:before,*:after{transition-duration:0s!important;transition-delay:0s!important;animation-duration:0s!important;animation-delay:0s!important}html.web .pocket{margin:0!important;box-shadow:none!important}html.web body,html.web #birdie{background:#202020!important}';
    document.head.appendChild(s);
  }
  if (/[?&]peek/.test(q)) document.documentElement.className += ' peek';  // the tray card (really only in Birdie's window)
  if (page === '3' && location.hash !== '#border') location.hash = '#border';
  window.addEventListener('load', function () {
    var tries = 0;
    (function nav() {
      var p = document.getElementById('panel');
      if (page === '0' || page === '3' || (p && p.getAttribute('data-p') === page) || ++tries > 40) return;
      var n = document.querySelector('[data-go="' + page + '"]'); if (n) n.click();
      setTimeout(nav, 150);
    })();
  });
})();
