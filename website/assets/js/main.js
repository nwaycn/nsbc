/* NSBC 官网交互脚本 */
(function () {
  'use strict';

  // 移动端导航
  var header = document.querySelector('.site-header');
  var toggle = document.querySelector('.nav-toggle');

  if (header && toggle) {
    toggle.addEventListener('click', function () {
      var open = header.classList.toggle('is-open');
      toggle.setAttribute('aria-expanded', open ? 'true' : 'false');
    });

    document.addEventListener('click', function (e) {
      if (!header.contains(e.target) && header.classList.contains('is-open')) {
        header.classList.remove('is-open');
        toggle.setAttribute('aria-expanded', 'false');
      }
    });
  }

  // 文档页滚动高亮侧边导航
  var sideLinks = document.querySelectorAll('.side-nav a[href^="#"]');
  if (sideLinks.length) {
    var sections = [];
    sideLinks.forEach(function (link) {
      var el = document.querySelector(link.getAttribute('href'));
      if (el) sections.push({ link: link, el: el });
    });

    var onScroll = function () {
      var pos = window.scrollY + 120;
      var current = sections[0];
      sections.forEach(function (s) {
        if (s.el.offsetTop <= pos) current = s;
      });
      sideLinks.forEach(function (l) { l.classList.remove('is-active'); });
      if (current) current.link.classList.add('is-active');
    };

    window.addEventListener('scroll', onScroll, { passive: true });
    onScroll();
  }
})();
