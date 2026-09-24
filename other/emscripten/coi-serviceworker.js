// GitHub Pages serves no headers of its own, and the client is compiled with
// threads: without `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy`
// the browser withholds `SharedArrayBuffer` and the runtime never starts. A
// service worker is the only thing a static host has that can add a header, so
// it adds those two to every response of this origin and the page reloads once
// through it. Where the headers already arrive from the server, as they do from
// `server.py`, this does nothing.
"use strict";

if (typeof window === "undefined") {
	self.addEventListener("install", () => self.skipWaiting());
	self.addEventListener("activate", event => event.waitUntil(self.clients.claim()));
	self.addEventListener("fetch", event => {
		const request = event.request;
		// A cache-only request of a different origin cannot be answered with a
		// fetch, so it is left to the browser.
		if (request.cache === "only-if-cached" && request.mode !== "same-origin") {
			return;
		}
		event.respondWith(fetch(request)
			.then(response => {
				// An opaque response has no headers to copy and no body to read.
				if (response.status === 0) {
					return response;
				}
				const headers = new Headers(response.headers);
				headers.set("Cross-Origin-Embedder-Policy", "require-corp");
				headers.set("Cross-Origin-Opener-Policy", "same-origin");
				return new Response(response.body, { status: response.status, statusText: response.statusText, headers: headers });
			})
			.catch(error => console.error(error)));
	});
} else if (!window.crossOriginIsolated && window.isSecureContext && navigator.serviceWorker) {
	const scriptUrl = document.currentScript.src;
	navigator.serviceWorker.register(scriptUrl).then(registration => {
		// The worker only sees requests made after it took over, so the page it
		// was registered from is loaded again once it has.
		registration.addEventListener("updatefound", () => window.location.reload());
		if (registration.active && !navigator.serviceWorker.controller) {
			window.location.reload();
		}
	}, error => console.error(error));
}
