/* Same-origin isolation shim for hosts (like GitHub Pages) where HTTP headers
 * cannot be customized. Works only on HTTPS / localhost. Do not cache chess
 * files here: ordinary browser and CDN caching remains in effect.
 */
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));
self.addEventListener('fetch', event => {
  const u = new URL(event.request.url);
  if (u.origin !== self.location.origin) return;
  event.respondWith((async () => {
    const response = await fetch(event.request);
    if (response.type === 'opaque' || response.type === 'opaqueredirect') return response;
    const headers = new Headers(response.headers);
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    headers.set('Cross-Origin-Resource-Policy', 'same-origin');
    return new Response(response.body, {
      status: response.status,
      statusText: response.statusText,
      headers,
    });
  })());
});
