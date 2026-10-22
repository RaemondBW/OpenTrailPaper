// Fills the star count on the nav's GitHub button. The button works without
// it: if the API call fails or is rate-limited the count just stays hidden.
(function () {
  var REPO = 'RaemondBW/OpenTrailPaper';
  var KEY = 'otp-gh-stars';
  var els = document.querySelectorAll('header.nav a.gh .gh-stars');
  if (!els.length) return;

  function show(n) {
    var text = n >= 1000 ? (n / 1000).toFixed(n >= 10000 ? 0 : 1).replace(/\.0$/, '') + 'k' : String(n);
    els.forEach(function (el) {
      el.querySelector('.n').textContent = text;
      el.hidden = false;
      el.closest('a.gh').setAttribute('aria-label', 'GitHub, ' + n + ' stars');
    });
  }

  try {
    var cached = JSON.parse(sessionStorage.getItem(KEY) || 'null');
    if (cached && Date.now() - cached.at < 3600e3) { show(cached.n); return; }
  } catch (e) {}

  fetch('https://api.github.com/repos/' + REPO, { headers: { Accept: 'application/vnd.github+json' } })
    .then(function (r) { return r.ok ? r.json() : null; })
    .then(function (j) {
      if (!j || typeof j.stargazers_count !== 'number') return;
      show(j.stargazers_count);
      try { sessionStorage.setItem(KEY, JSON.stringify({ n: j.stargazers_count, at: Date.now() })); } catch (e) {}
    })
    .catch(function () {});
})();
