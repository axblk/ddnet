// The browser's side of web_decoder.cpp: a worker that decodes with the
// browser's decoders and writes the result straight into the program's
// memory. The page's thread starts it and hands it the requests, whichever
// thread makes them, so that no thread of the program has to return to the
// browser for a decode to go on.

addToLibrary({
	// What runs in the worker, as the source it is started from.
	$DDNetDecoderWorker: function () {
		let memory = null;

		// The state of a request, see `CWebDecode`: done or failed, and whoever
		// waits for it woken.
		const finish = (state, ok) => {
			const word = new Int32Array(memory.buffer, state, 1);
			Atomics.store(word, 0, ok ? 2 : 3);
			Atomics.notify(word, 0);
		};

		// libopus' opus_pcm_soft_clip, which libopusfile applies before it
		// makes 16 bits of the sound: what goes beyond full scale is bent back
		// below it from one zero crossing to the next. `memory` carries the
		// bend of each channel over to the next call.
		const softClip = (pcm, frames, channels, memory) => {
			const f = Math.fround;
			for (let i = 0; i < frames * channels; i++) {
				pcm[i] = Math.max(-2, Math.min(2, pcm[i]));
			}
			for (let c = 0; c < channels; c++) {
				const at = i => pcm[i * channels + c];
				const set = (i, v) => { pcm[i * channels + c] = v; };
				let a = memory[c];
				// Go on bending what the last call bent
				for (let i = 0; i < frames; i++) {
					if (at(i) * a >= 0) {
						break;
					}
					set(i, f(at(i) + f(f(a * at(i)) * at(i))));
				}
				let current = 0;
				const first = at(0);
				while (true) {
					let i = current;
					while (i < frames && at(i) <= 1 && at(i) >= -1) {
						i++;
					}
					if (i === frames) {
						a = 0;
						break;
					}
					let peak = i;
					let start = i;
					let end = i;
					let max = Math.abs(at(i));
					while (start > 0 && at(i) * at(start - 1) >= 0) {
						start--;
					}
					while (end < frames && at(i) * at(end) >= 0) {
						if (Math.abs(at(end)) > max) {
							max = Math.abs(at(end));
							peak = end;
						}
						end++;
					}
					// Clipped before the first zero crossing
					const special = start === 0 && at(i) * at(0) >= 0;
					// Such that max + a * max * max = 1, a little more
					a = f(f(max - 1) / f(max * max));
					a = f(a + f(a * f(2.4e-7)));
					if (at(i) > 0) {
						a = -a;
					}
					for (let k = start; k < end; k++) {
						set(k, f(at(k) + f(f(a * at(k)) * at(k))));
					}
					if (special && peak >= 2) {
						// A ramp to the peak, where the last call ended elsewhere
						let offset = f(first - at(0));
						const delta = f(offset / peak);
						for (let k = current; k < peak; k++) {
							offset = f(offset - delta);
							set(k, Math.max(-1, Math.min(1, f(at(k) + offset))));
						}
					}
					current = end;
					if (current === frames) {
						break;
					}
				}
				memory[c] = a;
			}
		};

		// Sound as libopusfile gives it: soft clipped, then to 16 bits.
		const storeSound = (request, pcm, frames, clip) => {
			const { channels } = request;
			softClip(pcm, frames, channels, clip);
			const out = new Int16Array(memory.buffer, request.samples + request.written * channels * 2, frames * channels);
			for (let i = 0; i < out.length; i++) {
				out[i] = Math.max(-32768, Math.min(32767, Math.round(pcm[i] * 32768)));
			}
			request.written += frames;
		};

		// WebCodecs' Opus decoder takes the packets and, from the header,
		// skips the samples to skip. It knows nothing of where the sound
		// ends, which is cut off here.
		const decodeOpus = async request => {
			const { channels, frames } = request;
			const config = { codec: 'opus', sampleRate: 48000, numberOfChannels: channels, description: new Uint8Array(memory.buffer, request.head, request.headSize).slice() };
			if (typeof AudioDecoder === 'undefined' || !(await AudioDecoder.isConfigSupported(config)).supported) {
				// The page decodes the whole file and hands the sound back
				// (`decodedOpus`)
				postMessage(request);
				return undefined;
			}
			const sizes = new Uint32Array(memory.buffer, request.packetSizes, request.packets).slice();
			const packets = new Uint8Array(memory.buffer, request.data, sizes.reduce((sum, size) => sum + size, 0)).slice();
			const clip = new Float32Array(channels);
			let failure = null;
			request.written = 0;
			const decoder = new AudioDecoder({
				output: data => {
					try {
						const take = Math.min(data.numberOfFrames, frames - request.written);
						if (take > 0) {
							const pcm = new Float32Array(data.numberOfFrames * channels);
							data.copyTo(pcm, { planeIndex: 0, format: 'f32' });
							storeSound(request, pcm.subarray(0, take * channels), take, clip);
						}
					} catch (error) {
						failure = error;
					} finally {
						data.close();
					}
				},
				error: error => {
					failure = error;
				},
			});
			try {
				decoder.configure(config);
				let offset = 0;
				for (let i = 0; i < sizes.length; i++) {
					// The time only orders the packets
					decoder.decode(new EncodedAudioChunk({ type: 'key', timestamp: i * 20000, data: packets.subarray(offset, offset + sizes[i]) }));
					offset += sizes[i];
				}
				await decoder.flush();
			} finally {
				if (decoder.state !== 'closed') {
					decoder.close();
				}
			}
			if (failure) {
				throw failure;
			}
			Atomics.store(new Int32Array(memory.buffer, request.decoded, 1), 0, request.written);
			return true;
		};

		// How many samples a packet holds, from its first bytes
		const packetSamples = packet => {
			const toc = packet[0];
			const frameSize = toc & 0x80 ? 120 << (toc >> 3 & 3) : (toc & 0x60) === 0x60 ? (toc & 0x08 ? 960 : 480) : (toc >> 3 & 3) === 3 ? 2880 : 480 << (toc >> 3 & 3);
			const count = [1, 2, 2, packet[1] & 0x3f][toc & 3];
			return frameSize * count;
		};

		// What the page decoded instead, interleaved. It is soft clipped a
		// packet at a time, as libopusfile and WebCodecs give it.
		const decodedOpus = request => {
			if (request.error) {
				throw new Error(request.error);
			}
			const { channels, pcm } = request;
			const sizes = new Uint32Array(memory.buffer, request.packetSizes, request.packets).slice();
			const packets = new Uint8Array(memory.buffer, request.data, sizes.reduce((sum, size) => sum + size, 0));
			const clip = new Float32Array(channels);
			const available = Math.min(pcm.length / channels, request.frames);
			// The samples skipped at the start are gone from the first packets
			let skip = new DataView(memory.buffer, request.head + 10, 2).getUint16(0, true);
			request.written = 0;
			let offset = 0;
			for (const size of sizes) {
				let samples = packetSamples(packets.subarray(offset, offset + size));
				offset += size;
				const skipped = Math.min(skip, samples);
				skip -= skipped;
				samples = Math.min(samples - skipped, available - request.written);
				if (samples > 0) {
					storeSound(request, pcm.subarray(request.written * channels, (request.written + samples) * channels), samples, clip);
				}
			}
			Atomics.store(new Int32Array(memory.buffer, request.decoded, 1), 0, request.written);
			return true;
		};

		const decoders = { opus: decodeOpus, decodedOpus };
		onmessage = async event => {
			const request = event.data;
			if (request.memory) {
				memory = request.memory;
				return;
			}
			let ok = false;
			try {
				ok = await decoders[request.kind](request);
			} catch (error) {
				console.error(`ddnet decoder: ${request.kind}: ${error}`);
			}
			// Left to the page
			if (ok !== undefined) {
				finish(request.state, ok);
			}
		};
	},

	$DDNetDecoder__deps: ['$DDNetDecoderWorker'],
	$DDNetDecoder: {
		worker: null,
		post(request) {
			if (!DDNetDecoder.worker) {
				const source = `(${DDNetDecoderWorker.toString()})();`;
				DDNetDecoder.worker = new Worker(URL.createObjectURL(new Blob([source], { type: 'text/javascript' })), { name: 'ddnet-decoder' });
				DDNetDecoder.worker.onmessage = event => DDNetDecoder.decodeAudioFile(event.data);
				DDNetDecoder.worker.postMessage({ memory: wasmMemory });
			}
			DDNetDecoder.worker.postMessage(request);
		},
		// Where WebCodecs has no Opus: the page's audio decoder, which takes
		// whole files and is not there in workers. At 48 kHz nothing is
		// resampled; it skips and cuts off like libopusfile.
		async decodeAudioFile(request) {
			const reply = { ...request, kind: 'decodedOpus' };
			try {
				const file = new Uint8Array(wasmMemory.buffer, request.file, request.fileSize).slice();
				const buffer = await new OfflineAudioContext(request.channels, 1, 48000).decodeAudioData(file.buffer);
				const pcm = new Float32Array(buffer.length * request.channels);
				for (let c = 0; c < Math.min(request.channels, buffer.numberOfChannels); c++) {
					buffer.getChannelData(c).forEach((sample, i) => {
						pcm[i * request.channels + c] = sample;
					});
				}
				reply.pcm = pcm;
			} catch (error) {
				reply.error = String(error);
			}
			DDNetDecoder.worker.postMessage(reply, reply.pcm ? [reply.pcm.buffer] : []);
		},
	},

	ddnet_web_decode_opus__proxy: 'async',
	ddnet_web_decode_opus__deps: ['$DDNetDecoder'],
	ddnet_web_decode_opus: (file, fileSize, head, headSize, data, packetSizes, packets, channels, samples, frames, decoded, state) => {
		DDNetDecoder.post({ kind: 'opus', file, fileSize, head, headSize, data, packetSizes, packets, channels, samples, frames, decoded, state });
	},
});
