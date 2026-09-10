// Runs the Emscripten client under Node instead of a browser, for a test
// against a native server: `node -r scripts/emscripten/node-xhr-stub.js DDNet.js
// "connect ..."`. Node has no XMLHttpRequest, which emscripten_fetch calls, so
// this stand-in fails every request right away; the client then takes the map
// from the server and goes without the master's list. `-r` reaches the worker
// threads as well.
class XMLHttpRequestStub {
	constructor() {
		this.readyState = 0;
		this.status = 0;
		this.statusText = "";
		this.response = null;
		this.responseText = "";
	}
	open() {
		this.readyState = 1;
	}
	setRequestHeader() {}
	overrideMimeType() {}
	getAllResponseHeaders() {
		return "";
	}
	getResponseHeader() {
		return null;
	}
	addEventListener(name, f) {
		this["on" + name] = f;
	}
	abort() {}
	send() {
		setTimeout(() => {
			this.readyState = 4;
			this.status = 0;
			if(this.onreadystatechange) this.onreadystatechange();
			if(this.onerror) this.onerror({});
			if(this.onloadend) this.onloadend({});
		}, 1);
	}
}
globalThis.XMLHttpRequest = XMLHttpRequestStub;
