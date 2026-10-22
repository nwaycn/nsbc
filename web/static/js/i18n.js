// NSBC i18n — minimal runtime translation layer
// - t(key) returns NSBC_DICT[key] when the zh dictionary is loaded,
//   otherwise falls back to the key itself (English).
// - Language is chosen at page load (localStorage > browser) and applied
//   via a full reload, so no reactivity wiring is needed.
// - This file MUST load after lang-zh.js (when zh is active) and before app-core.js.
(function () {
  window.t = function (key) {
    var d = window.NSBC_DICT;
    return (d && Object.prototype.hasOwnProperty.call(d, key)) ? d[key] : key;
  };
  window.getLang = function () { return window.NSBC_LANG || 'en'; };
  window.setLang = function (lang) {
    try { localStorage.setItem('nsbc_lang', lang); } catch (e) {}
    location.reload();
  };
  document.title = window.t('NSBC Management Console');
})();
