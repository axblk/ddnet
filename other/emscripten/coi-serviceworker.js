// GitHub Pages serves no headers of its own, and the client is compiled with
// threads: without `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy`
// the browser withholds `SharedArrayBuffer` and the runtime never starts. A
// service worker is the only thing a static host has that can add a header, so
// it adds those two to every response of this origin and the page reloads once
// through it. Where the headers already arrive from the server, as they do from
// `server.py`, this does nothing.
//
// Since there has to be a worker in front of every request anyway, it is also
// where the data directory is kept: see `src/base/webfs.h` for what fetches it
// and why a file there can be kept for good.
"use strict";

const DATA_CACHE = "ddnet-data";

if (typeof window === "undefined") {
	self.addEventListener("install", () => self.skipWaiting());
	self.addEventListener("activate", event => event.waitUntil(self.clients.claim()));

	// The two headers this worker exists for. An opaque response has none to
	// copy and no body to read, so it is passed on as it came.
	const withIsolationHeaders = response => {
		if (response.status === 0) {
			return response;
		}
		const headers = new Headers(response.headers);
		headers.set("Cross-Origin-Embedder-Policy", "require-corp");
		headers.set("Cross-Origin-Opener-Policy", "same-origin");
		return new Response(response.body, { status: response.status, statusText: response.statusText, headers: headers });
	};

	const dataBase = () => new URL("data/", self.registration.scope);

	// A file of the data directory carries the hash of its contents in its URL,
	// so what is under one is what belongs there and nothing else. The index
	// itself has no such hash and is deliberately not among them.
	const isDataFile = request => {
		if (request.method !== "GET") {
			return false;
		}
		const url = new URL(request.url);
		return url.origin === self.location.origin &&
			url.pathname.startsWith(dataBase().pathname) &&
			url.searchParams.has("v");
	};

	const respondFromDataCache = async request => {
		const cache = await caches.open(DATA_CACHE);
		const cached = await cache.match(request);
		if (cached !== undefined) {
			return withIsolationHeaders(cached);
		}
		const response = await fetch(request);
		if (response.ok) {
			await cache.put(request, response.clone());
		}
		return withIsolationHeaders(response);
	};

	// What is kept is what this build has, which the index says and nothing else
	// knows. A file whose contents changed is a different URL and falls out here
	// along with one that is gone altogether; there is no age to guess at and no
	// version of the cache to bump.
	const sweepDataCache = async () => {
		const base = dataBase();
		const response = await fetch(new URL("index.txt", base), { cache: "no-store" });
		if (!response.ok) {
			return;
		}
		const wanted = new Set();
		for (const line of (await response.text()).split("\n")) {
			if (!line.startsWith("f ")) {
				continue;
			}
			// A file name may hold spaces, so the two fields behind it are taken
			// off the end and whatever is left is the path. Kept in step with
			// `CWebDataIndex::Parse`.
			const hashStart = line.lastIndexOf(" ");
			const sizeStart = line.lastIndexOf(" ", hashStart - 1);
			if (hashStart < 0 || sizeStart < 2) {
				continue;
			}
			wanted.add(new URL(`${line.slice(2, sizeStart)}?v=${line.slice(hashStart + 1)}`, base).href);
		}
		const cache = await caches.open(DATA_CACHE);
		for (const request of await cache.keys()) {
			if (!wanted.has(request.url)) {
				await cache.delete(request);
			}
		}
	};

	self.addEventListener("message", event => {
		if (event.data && event.data.type === "ddnet-data-sweep") {
			event.waitUntil(sweepDataCache());
		}
	});

	self.addEventListener("fetch", event => {
		const request = event.request;
		// A cache-only request of a different origin cannot be answered with a
		// fetch, so it is left to the browser.
		if (request.cache === "only-if-cached" && request.mode !== "same-origin") {
			return;
		}
		if (isDataFile(request)) {
			event.respondWith(respondFromDataCache(request).catch(error => {
				console.error(error);
				return fetch(request).then(withIsolationHeaders);
			}));
			return;
		}
		event.respondWith(fetch(request)
			.then(withIsolationHeaders)
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
