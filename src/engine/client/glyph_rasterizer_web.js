// The browser's side of the glyph rasterizer in glyph_rasterizer_web.cpp: the
// browser's text drawing on an OffscreenCanvas, with the font files added as
// FontFaces. Everything runs on the thread that calls it, the one the text
// renderer runs on: the page's in the client, a worker in the web tools.

addToLibrary({
	$DDNetGlyphs: {
		// The canvas the glyphs are drawn on, side by side, and read back from
		// at once. It keeps the size of the largest batch.
		canvas: null,
		context: null,
		font: '',
		context2d() {
			if (!DDNetGlyphs.context) {
				DDNetGlyphs.canvas = new OffscreenCanvas(256, 64);
				// Read back every time glyphs are added: kept in memory rather
				// than on the GPU, which makes reading it back 10-50x faster.
				DDNetGlyphs.context = DDNetGlyphs.canvas.getContext('2d', { willReadFrequently: true });
			}
			return DDNetGlyphs.context;
		},
		setFont(context, faceId, fontSize) {
			// The system's fonts for the face that stands for them
			const font = faceId < 0 ? `${fontSize}px sans-serif` : `${fontSize}px ddnet-glyphs-${faceId}`;
			if (DDNetGlyphs.font !== font) {
				context.font = font;
				DDNetGlyphs.font = font;
			}
		},
	},

	ddnet_glyphs_add_face__deps: ['$DDNetGlyphs'],
	ddnet_glyphs_add_face: (faceId, data, size) => {
		const fonts = globalThis.document ? document.fonts : self.fonts;
		if (!fonts || typeof OffscreenCanvas === 'undefined') {
			return 0;
		}
		// A copy: FontFace does not take shared memory, and the face is ready
		// to draw with as soon as it is made from bytes.
		const face = new FontFace(`ddnet-glyphs-${faceId}`, HEAPU8.slice(data, data + size).buffer);
		if (face.status === 'error') {
			return 0;
		}
		fonts.add(face);
		return 1;
	},

	ddnet_glyphs_measure__deps: ['$DDNetGlyphs'],
	ddnet_glyphs_measure: (faceId, fontSize, character, box) => {
		const context = DDNetGlyphs.context2d();
		DDNetGlyphs.setFont(context, faceId, fontSize);
		const metrics = context.measureText(String.fromCodePoint(character));
		const out = box >> 2;
		HEAPF32[out] = metrics.width;
		HEAPF32[out + 1] = metrics.actualBoundingBoxLeft;
		HEAPF32[out + 2] = metrics.actualBoundingBoxRight;
		HEAPF32[out + 3] = metrics.actualBoundingBoxAscent;
		HEAPF32[out + 4] = metrics.actualBoundingBoxDescent;
	},

	ddnet_glyphs_kerning__deps: ['$DDNetGlyphs'],
	ddnet_glyphs_kerning: (faceId, fontSize, left, right) => {
		const context = DDNetGlyphs.context2d();
		DDNetGlyphs.setFont(context, faceId, fontSize);
		const a = String.fromCodePoint(left);
		const b = String.fromCodePoint(right);
		return context.measureText(a + b).width - context.measureText(a).width - context.measureText(b).width;
	},

	// Draws the glyphs of `SWebGlyph`s on one strip, reads the strip back once
	// and puts the coverage of each where its `m_pPixels` points.
	ddnet_glyphs_draw__deps: ['$DDNetGlyphs'],
	ddnet_glyphs_draw: (glyphs, count) => {
		const GAP = 2;
		const list = [];
		let width = 0;
		let height = 0;
		for (let i = 0; i < count; i++) {
			const at = (glyphs >> 2) + i * 8;
			const glyph = {
				faceId: HEAP32[at],
				fontSize: HEAP32[at + 1],
				character: HEAP32[at + 2],
				offsetX: HEAP32[at + 3],
				offsetY: HEAP32[at + 4],
				width: HEAP32[at + 5],
				height: HEAP32[at + 6],
				pixels: HEAPU32[at + 7],
				x: width,
			};
			list.push(glyph);
			width += glyph.width + GAP;
			height = Math.max(height, glyph.height);
		}
		const context = DDNetGlyphs.context2d();
		const canvas = DDNetGlyphs.canvas;
		if (canvas.width < width || canvas.height < height) {
			// Resizing resets the context
			canvas.width = Math.max(canvas.width, width);
			canvas.height = Math.max(canvas.height, height);
			DDNetGlyphs.font = '';
		}
		context.clearRect(0, 0, width, height);
		context.fillStyle = '#fff';
		context.textAlign = 'left';
		context.textBaseline = 'alphabetic';
		for (const glyph of list) {
			DDNetGlyphs.setFont(context, glyph.faceId, glyph.fontSize);
			// The top left of the glyph's box at (x, 0): the pen left of it by
			// its offset, the baseline below it by the box's top
			context.fillText(String.fromCodePoint(glyph.character), glyph.x - glyph.offsetX, glyph.offsetY + glyph.height);
		}
		// White ink on nothing: the alpha is the coverage
		const rgba = context.getImageData(0, 0, width, height).data;
		for (const glyph of list) {
			for (let y = 0; y < glyph.height; y++) {
				const from = (y * width + glyph.x) * 4 + 3;
				const to = glyph.pixels + y * glyph.width;
				for (let x = 0; x < glyph.width; x++) {
					HEAPU8[to + x] = rgba[from + x * 4];
				}
			}
		}
	},
});
