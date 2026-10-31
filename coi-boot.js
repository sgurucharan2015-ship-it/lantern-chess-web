/* Cross-origin isolation bootstrap for free static hosts without custom headers.
 * Static HTTPS + a service worker. Cloudflare Pages with _headers works directly.
 * No engine code runs until crossOriginIsolated is true; no weaker fallback.
 */
(() => {
  'use strict';
  if (window.crossOriginIsolated) return;
  const supported = location.protocol === 'https:' || ['localhost','127.0.0.1'].includes(location.hostname);
  if (!supported || !('serviceWorker' in navigator)) {
    window.LANTERN_ISOLATION_ERROR = 'This browser needs HTTPS and service-worker support to run 3-thread Lantern.';
    return;
  }
  let reloading = false;
  const maxReloads = 2;
  const count = Number(sessionStorage.getItem('lantern-coi-reloads') || '0');
  function reloadOnce() {
    if (reloading || count >= maxReloads) return;
    reloading = true;
    sessionStorage.setItem('lantern-coi-reloads', String(count + 1));
    location.reload();
  }
  navigator.serviceWorker.addEventListener('controllerchange', reloadOnce);
  navigator.serviceWorker.register('./coi-service-worker.js', {scope: './'})
    .then(registration => {
      if (navigator.serviceWorker.controller && !window.crossOriginIsolated) reloadOnce();
      else if (!navigator.serviceWorker.controller && registration.active) {
        navigator.serviceWorker.ready.then(() => {if (navigator.serviceWorker.controller) reloadOnce();});
      }
    })
    .catch(e => {window.LANTERN_ISOLATION_ERROR = 'Unable to enable browser threads: ' + e.message;});
})();
