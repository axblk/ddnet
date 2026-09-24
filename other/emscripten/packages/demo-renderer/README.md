# @ddnet/demo-renderer

Turn a DDNet demo into an MP4 in a browser.

```js
import { renderDemo } from "@ddnet/demo-renderer";

const video = await renderDemo({
	demo: "https://example.org/a.demo",
	width: 1920, height: 1080, fps: 60,
	onProgress: status => console.log(status.progress),
});
```

The demo is drawn without a window in a worker and encoded by the browser,
which needs WebGPU and WebCodecs. A `videoSink` from `showSaveFilePicker`
writes the video as it is made instead of keeping it in memory; `zip` puts
several videos into one file. The types are in `demo-renderer.d.ts`.

The page has to be cross-origin isolated (`Cross-Origin-Opener-Policy:
same-origin`, `Cross-Origin-Embedder-Policy: require-corp`, or
`@ddnet/base/coi-serviceworker.js`), because the program uses threads.
