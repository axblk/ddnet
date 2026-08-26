#if defined(CONF_VIDEORECORDER) && defined(__EMSCRIPTEN__)

#include <base/dbg.h>
#include <base/fs.h>
#include <base/log.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/graphics.h>
#include <engine/shared/video.h>
#include <engine/sound.h>

#include <emscripten.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

// The browser encodes and writes the file, so nothing here holds a codec or a
// thread. What is left is the same offscreen readback and the same audio
// accounting the libavcodec export uses, with the read frames handed to a
// VideoEncoder, the mixed samples to an AudioEncoder, and both tracks muxed
// into one fragmented MP4 on the other side.

namespace
{

	// clang-format off
EM_JS(int, BrowserVideoSupported, (), {
	return (typeof VideoEncoder === 'undefined' || typeof VideoFrame === 'undefined') ? 0 : 1;
});

EM_JS(void, BrowserVideoProbe, (), {
	if(Module.ddnetVideoProbe)
		return;
	const probe = {done: false, codecs: []};
	Module.ddnetVideoProbe = probe;
	// One profile per family is enough to offer it, and the more capable
	// profile is tried first because it is what a desktop encoder gives. The
	// start walks the same lists, because what the probe was told about a
	// family is not what every encoder behind it accepts.
	Module.ddnetVideoFamilies = [
		['H.264', ['avc1.640028', 'avc1.4D0028', 'avc1.42E01E']],
		['H.265', ['hvc1.1.6.L120.B0']],
		['AV1', ['av01.0.08M.08']],
	];
	if(typeof VideoEncoder === 'undefined') {
		probe.done = true;
		return;
	}
	const families = Module.ddnetVideoFamilies;
	const firstSupported = async candidates => {
		for(const codec of candidates) {
			try {
				const support = await VideoEncoder.isConfigSupported({codec: codec, width: 1280, height: 720, bitrate: 4000000, framerate: 60});
				if(support.supported)
					return codec;
			} catch(error) {}
		}
		return null;
	};
	Promise.all(families.map(family => firstSupported(family[1]).then(codec => codec === null ? null : {name: codec, display: family[0]})))
		.then(found => { probe.codecs = found.filter(entry => entry !== null); })
		.catch(() => {})
		.then(() => { probe.done = true; });
});

EM_JS(int, BrowserVideoProbeDone, (), {
	return Module.ddnetVideoProbe && Module.ddnetVideoProbe.done ? 1 : 0;
});

EM_JS(int, BrowserVideoProbeCount, (), {
	return Module.ddnetVideoProbe ? Module.ddnetVideoProbe.codecs.length : 0;
});

EM_JS(void, BrowserVideoProbeEntry, (int Index, char *pName, int NameCapacity, char *pDisplay, int DisplayCapacity), {
	const entry = Module.ddnetVideoProbe.codecs[Index];
	stringToUTF8(entry.name, pName, NameCapacity);
	stringToUTF8(entry.display, pDisplay, DisplayCapacity);
});

EM_JS(int, BrowserVideoStart, (char *pCodec, int CodecCapacity, const char *pFileName, int Width, int Height, int Fps, int Bitrate, int SampleRate, int Channels), {
	Module.ddnetVideoStartError = null;
	if(typeof VideoEncoder === 'undefined') {
		Module.ddnetVideoStartError = 'This browser has no VideoEncoder';
		return -1;
	}
	let codec = UTF8ToString(pCodec);
	const fileName = UTF8ToString(pFileName);

	const u16 = value => [(value >>> 8) & 255, value & 255];
	const u32 = value => [(value >>> 24) & 255, (value >>> 16) & 255, (value >>> 8) & 255, value & 255];
	const u64 = value => u32(Math.floor(value / 4294967296)).concat(u32(value % 4294967296));
	const tag = text => [text.charCodeAt(0), text.charCodeAt(1), text.charCodeAt(2), text.charCodeAt(3)];
	const box = (type, ...parts) => {
		let body = [];
		for(const part of parts)
			body = body.concat(Array.from(part));
		return u32(body.length + 8).concat(tag(type), body);
	};
	const fullBox = (type, version, flags, ...parts) => box(type, [version].concat(u32(flags).slice(1)), ...parts);
	// An MPEG-4 descriptor carries its length in seven-bit groups instead of a
	// fixed field, with the high bit set on every group but the last.
	const descriptor = (type, ...parts) => {
		let body = [];
		for(const part of parts)
			body = body.concat(Array.from(part));
		const length = [];
		let left = body.length;
		do {
			length.unshift(left & 0x7F);
			left >>>= 7;
		} while(left);
		for(let i = 0; i < length.length - 1; i++)
			length[i] |= 0x80;
		return [type].concat(length, body);
	};
	const bytesOf = value => ArrayBuffer.isView(value)
		? new Uint8Array(value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength))
		: new Uint8Array(value);

	const newTrack = (id, timescale) => ({id: id, timescale: timescale, samples: [], decodeTime: 0, sampleCount: 0, description: null});
	const state = {
		fps: Fps, width: Width, height: Height, fileName: fileName,
		encoder: null, error: '',
		video: newTrack(1, Fps), audio: null,
		fragments: [], sequence: 1, lastTimestamp: -1,
		submitted: 0, encoded: 0,
	};
	const tracks = () => state.audio === null ? [state.video] : [state.video, state.audio];

	const flush = () => {
		// Both tracks go into one fragment, each with its own run, and the
		// payload of the runs follows in the same order in a single 'mdat'. A
		// track the encoder has not described yet waits, because it may still
		// turn out not to be in the header at all, and a run in a fragment
		// would then point at a track nothing declares.
		const pending = tracks().filter(track => track.samples.length && track.description !== null);
		if(!pending.length)
			return;
		let payloadSize = 0;
		for(const track of pending)
			for(const sample of track.samples)
				payloadSize += sample.data.length;
		const moofFor = dataOffsets => box('moof',
			fullBox('mfhd', 0, 0, u32(state.sequence)),
			...pending.map((track, index) => box('traf',
				fullBox('tfhd', 0, 0x020000, u32(track.id)),
				fullBox('tfdt', 1, 0, u64(track.decodeTime)),
				fullBox('trun', 0, 0x000701, u32(track.samples.length), u32(dataOffsets[index]),
					track.samples.reduce((entries, sample) => entries.concat(u32(sample.duration), u32(sample.data.length), u32(sample.key ? 0x02000000 : 0x01010000)), [])))));
		// The offsets are counted from the start of the 'moof', whose size does
		// not depend on them because every offset is a fixed four bytes.
		let offset = moofFor(pending.map(() => 0)).length + 8;
		const dataOffsets = pending.map(track => {
			const start = offset;
			for(const sample of track.samples)
				offset += sample.data.length;
			return start;
		});
		const moof = moofFor(dataOffsets);
		const fragment = new Uint8Array(moof.length + 8 + payloadSize);
		fragment.set(moof, 0);
		fragment.set(u32(payloadSize + 8).concat(tag('mdat')), moof.length);
		offset = moof.length + 8;
		for(const track of pending) {
			for(const sample of track.samples) {
				fragment.set(sample.data, offset);
				offset += sample.data.length;
				track.decodeTime += sample.duration;
			}
			track.sampleCount += track.samples.length;
			track.samples = [];
		}
		state.fragments.push(fragment);
		state.sequence++;
	};

	const header = () => {
		// The durations are only known once the last frame is in, which is why
		// the header is built here and not when the export starts.
		const list = tracks();
		const trackDuration = track => Math.round(track.decodeTime * 1000 / track.timescale);
		const movieDuration = Math.max(...list.map(trackDuration));
		const trackBox = (track, media) => box('trak',
			fullBox('tkhd', 0, 3, u32(0), u32(0), u32(track.id), u32(0), u32(trackDuration(track)),
				u32(0), u32(0), u16(0), u16(0), u16(media.volume), u16(0),
				u32(0x00010000), u32(0), u32(0), u32(0), u32(0x00010000), u32(0), u32(0), u32(0), u32(0x40000000),
				u32(media.width * 65536), u32(media.height * 65536)),
			box('mdia',
				fullBox('mdhd', 0, 0, u32(0), u32(0), u32(track.timescale), u32(track.decodeTime), u16(0x55C4), u16(0)),
				fullBox('hdlr', 0, 0, u32(0), tag(media.handler), u32(0), u32(0), u32(0), Array.from(new TextEncoder().encode(media.name)).concat([0])),
				box('minf',
					media.header,
					box('dinf', fullBox('dref', 0, 0, u32(1), fullBox('url ', 0, 1))),
					box('stbl',
						fullBox('stsd', 0, 0, u32(1), media.entry),
						fullBox('stts', 0, 0, u32(0)),
						fullBox('stsc', 0, 0, u32(0)),
						fullBox('stsz', 0, 0, u32(0), u32(0)),
						fullBox('stco', 0, 0, u32(0))))));
		const videoTrack = trackBox(state.video, {
			volume: 0, width: Width, height: Height, handler: 'vide', name: 'VideoHandler',
			header: fullBox('vmhd', 0, 1, u16(0), u16(0), u16(0), u16(0)),
			entry: box(sampleEntryType,
				[0, 0, 0, 0, 0, 0], u16(1),
				u16(0), u16(0), u32(0), u32(0), u32(0),
				u16(Width), u16(Height),
				u32(0x00480000), u32(0x00480000),
				u32(0), u16(1),
				new Array(32).fill(0),
				u16(0x0018), u16(0xFFFF),
				box(configType, state.video.description)),
		});
		const audioTrack = state.audio === null ? [] : [trackBox(state.audio, {
			volume: 0x0100, width: 0, height: 0, handler: 'soun', name: 'SoundHandler',
			header: fullBox('smhd', 0, 0, u16(0), u16(0)),
			// The elementary stream descriptor is where an MP4 keeps the audio
			// specific config that the encoder handed over.
			entry: box('mp4a',
				[0, 0, 0, 0, 0, 0], u16(1),
				u32(0), u32(0),
				u16(state.audio.channels), u16(16), u16(0), u16(0),
				u32(state.audio.timescale * 65536),
				fullBox('esds', 0, 0,
					descriptor(0x03, u16(state.audio.id), [0x00],
						descriptor(0x04, [0x40, 0x15], [0, 0, 0], u32(0), u32(0),
							descriptor(0x05, state.audio.description)),
						descriptor(0x06, [0x02])))),
		})];
		const moov = box('moov',
			fullBox('mvhd', 0, 0, u32(0), u32(0), u32(1000), u32(movieDuration),
				u32(0x00010000), u16(0x0100), u16(0), u32(0), u32(0),
				u32(0x00010000), u32(0), u32(0), u32(0), u32(0x00010000), u32(0), u32(0), u32(0), u32(0x40000000),
				u32(0), u32(0), u32(0), u32(0), u32(0), u32(0), u32(list.length + 1)),
			videoTrack, ...audioTrack,
			box('mvex', ...list.map(track => fullBox('trex', 0, 0, u32(track.id), u32(1), u32(0), u32(0), u32(0)))));
		return new Uint8Array(box('ftyp', tag('isom'), u32(0x200), tag('isom'), tag('iso2'), tag(sampleEntryType), tag('mp41'), tag('iso5')).concat(moov));
	};

	// Says what kept the file from being written, and nothing when it was. A
	// file the browser cannot describe its own tracks for does not play, so it
	// is refused rather than handed over, and the reason travels back to the
	// export instead of leaving it looking like it worked.
	state.finish = () => {
		// A browser that turned out not to encode audio still gets its video,
		// so the track is only kept once the encoder has described it.
		if(state.audio !== null && state.audio.description === null)
			state.audio = null;
		flush();
		if(!state.encoded)
			return 'the browser encoded no frames';
		if(state.video.description === null || !state.video.sampleCount)
			return 'the browser did not describe the video track';
		// The file is assembled in memory, which is a few hundred megabytes for
		// a long export, and is what the browser wants for a download anyway.
		const url = URL.createObjectURL(new Blob([header()].concat(state.fragments), {type: 'video/mp4'}));
		const link = document.createElement('a');
		link.href = url;
		link.download = state.fileName;
		link.textContent = 'Save ' + state.fileName;
		link.style.cssText = 'background:#1b1b1b;color:#fff;border:1px solid #555;border-radius:4px;padding:6px 10px;text-decoration:none;white-space:nowrap';
		// A page is allowed to start a download by itself once, and after that
		// the browser drops the attempt without telling anyone: the second
		// export renders in full, builds its file, and simply never arrives.
		// So the file is also put on screen as something to click, which no
		// policy refuses. The click below still fires and still works the first
		// time; what it cannot do is report that it was ignored.
		let shelf = document.getElementById('ddnet-video-shelf');
		if(!shelf) {
			shelf = document.createElement('div');
			shelf.id = 'ddnet-video-shelf';
			shelf.style.cssText = 'position:fixed;top:8px;right:8px;z-index:2147483647;display:flex;flex-direction:column;gap:4px;font:14px sans-serif';
			document.body.appendChild(shelf);
		}
		shelf.appendChild(link);
		// Only a real click means the file has been taken. The one below is not
		// trusted and may have been dropped, so it must not clear the offer -
		// and a video nobody has saved is never taken off the screen.
		link.addEventListener('click', event => {
			if(!event.isTrusted)
				return;
			setTimeout(() => {
				link.remove();
				if(!shelf.childElementCount)
					shelf.remove();
				URL.revokeObjectURL(url);
			}, 1000);
		});
		link.click();
		return null;
	};

	try {
		state.encoder = new VideoEncoder({
			output: (chunk, metadata) => {
				if(state.video.description === null && metadata && metadata.decoderConfig && metadata.decoderConfig.description)
					state.video.description = bytesOf(metadata.decoderConfig.description);
				// The frames are written in the order they arrive, which is the order
				// they are shown in as long as the encoder does not reorder them. None
				// of the browser encoders does, and a file that claimed otherwise
				// would play its frames shuffled, so say so instead.
				if(chunk.timestamp < state.lastTimestamp) {
					state.error = 'The browser video encoder reordered the frames';
					return;
				}
				state.lastTimestamp = chunk.timestamp;
				const data = new Uint8Array(chunk.byteLength);
				chunk.copyTo(data);
				state.video.samples.push({data: data, key: chunk.type === 'key', duration: 1});
				state.encoded++;
				if(state.video.samples.length >= state.fps)
					flush();
			},
			error: error => { state.error = String((error && error.message) || error).slice(0, 255); },
		});
	} catch(error) {
		Module.ddnetVideoStartError = String((error && error.message) || error).slice(0, 255);
		return -1;
	}
	// The probe only asks whether a family is supported somewhere, and the
	// answer may well come from a hardware encoder that 'quality' then rules
	// out: what is left of H.264 in Chrome is Constrained Baseline. So every
	// profile of the family is offered, each of them once on the quality path
	// and once with the browser left to pick the encoder.
	const familyEntry = (Module.ddnetVideoFamilies || []).find(entry => entry[1].includes(codec));
	let configured = null;
	for(const candidate of familyEntry ? familyEntry[1] : [codec]) {
		for(const quality of [true, false]) {
			const config = {codec: candidate, width: Width, height: Height, bitrate: Bitrate, framerate: Fps};
			if(quality)
				config.latencyMode = 'quality';
			if(candidate.startsWith('avc1') || candidate.startsWith('avc3'))
				config.avc = {format: 'avc'};
			else if(candidate.startsWith('hvc1') || candidate.startsWith('hev1'))
				// 'hevc', not 'hvc1': the enum names the bitstream format and not
				// the sample entry the file carries it in. A name it does not know
				// makes configure() throw while reading the dictionary, before it
				// has looked at the codec at all - which is why passing this for
				// every codec broke H.264 as well.
				config.hevc = {format: 'hevc'};
			try {
				state.encoder.configure(config);
				configured = candidate;
				break;
			} catch(error) {
				Module.ddnetVideoStartError = candidate + ': ' + String((error && error.message) || error).slice(0, 200);
			}
		}
		if(configured)
			break;
	}
	if(!configured) {
		try { state.encoder.close(); } catch(error) {}
		return -1;
	}
	codec = configured;
	stringToUTF8(codec, pCodec, CodecCapacity);
	// Every candidate that was turned down left its reason behind; the one that
	// was taken makes all of them stale.
	Module.ddnetVideoStartError = null;

	// The sample entry and the configuration box only differ by name between
	// the families; what goes into the configuration box is whatever the
	// encoder handed over as its decoder description.
	const family = codec.slice(0, 4);
	const sampleEntryType = family === 'av01' ? 'av01' : (family === 'hvc1' || family === 'hev1') ? family : 'avc1';
	const configType = family === 'av01' ? 'av1C' : (family === 'hvc1' || family === 'hev1') ? 'hvcC' : 'avcC';

	if(SampleRate > 0) {
		// Audio is AAC because that is what an MP4 carries everywhere. A
		// browser that cannot encode it loses the sound, not the export.
		const audio = newTrack(2, SampleRate);
		audio.channels = Channels;
		audio.stopped = false;
		try {
			audio.encoder = new AudioEncoder({
				output: (chunk, metadata) => {
					if(audio.description === null && metadata && metadata.decoderConfig && metadata.decoderConfig.description)
						audio.description = bytesOf(metadata.decoderConfig.description);
					const data = new Uint8Array(chunk.byteLength);
					chunk.copyTo(data);
					// Every packet of an audio codec stands on its own, and an
					// AAC frame is 1024 samples where the chunk does not say.
					audio.samples.push({data: data, key: true, duration: chunk.duration ? Math.round(chunk.duration * SampleRate / 1000000) : 1024});
				},
				// A track that stops early is still a track; what is already
				// written stays, and nothing more is handed over.
				error: error => { audio.stopped = true; },
			});
			audio.encoder.configure({codec: 'mp4a.40.2', sampleRate: SampleRate, numberOfChannels: Channels, bitrate: 128000});
			state.audio = audio;
		} catch(error) {}
	}
	Module.ddnetVideo = state;
	return 0;
});

EM_JS(int, BrowserAudioStarted, (), {
	return Module.ddnetVideo && Module.ddnetVideo.audio ? 1 : 0;
});

EM_JS(void, BrowserAudioSubmit, (const short *pSamples, int Frames, double TimestampMicros), {
	const state = Module.ddnetVideo;
	if(!state || !state.audio || state.audio.stopped)
		return;
	const audio = state.audio;
	try {
		const data = new AudioData({
			format: 's16', sampleRate: audio.timescale, numberOfFrames: Frames, numberOfChannels: audio.channels,
			timestamp: TimestampMicros, data: HEAP16.subarray(pSamples >> 1, (pSamples >> 1) + Frames * audio.channels),
		});
		audio.encoder.encode(data);
		data.close();
	} catch(error) {
		audio.stopped = true;
	}
});

EM_ASYNC_JS(int, BrowserVideoSubmit, (const unsigned char *pData, int Format, double TimestampMicros, int KeyFrame), {
	const state = Module.ddnetVideo;
	if(!state)
		return -1;
	// The encoder runs on its own thread and the export would otherwise hand it
	// frames faster than it retires them, which is memory nobody bounded.
	while(!state.error && state.encoder.encodeQueueSize > 8)
		await new Promise(resolve => setTimeout(resolve, 0));
	if(state.error)
		return -1;
	const width = state.width;
	const height = state.height;
	const init = {timestamp: TimestampMicros, duration: 1000000 / state.fps, codedWidth: width, codedHeight: height};
	let size;
	if(Format === 1) {
		init.format = 'NV12';
		init.layout = [{offset: 0, stride: width}, {offset: width * height, stride: width}];
		size = width * height * 3 / 2;
	} else if(Format === 2) {
		init.format = 'I420';
		init.layout = [{offset: 0, stride: width}, {offset: width * height, stride: width / 2}, {offset: width * height * 5 / 4, stride: width / 2}];
		size = width * height * 3 / 2;
	} else {
		init.format = 'RGBA';
		init.layout = [{offset: 0, stride: width * 4}];
		size = width * height * 4;
	}
	try {
		const frame = new VideoFrame(HEAPU8.subarray(pData, pData + size), init);
		state.encoder.encode(frame, {keyFrame: KeyFrame !== 0});
		frame.close();
		state.submitted++;
	} catch(error) {
		state.error = String((error && error.message) || error).slice(0, 255);
		return -1;
	}
	return 0;
});

EM_ASYNC_JS(void, BrowserVideoStop, (int Cancel, char *pError, int ErrorCapacity), {
	const state = Module.ddnetVideo;
	if(!state)
		return;
	Module.ddnetVideo = null;
	// The finish drops the sound track when the browser never described it, so
	// the encoder to close is the one that was there before it ran.
	const audio = state.audio;
	let reason = null;
	try {
		if(!Cancel && !state.error) {
			await state.encoder.flush();
			// A sound track that fails to drain must not cost the picture, so
			// the export goes on with whatever it did hand over.
			if(state.audio && !state.audio.stopped) {
				try { await state.audio.encoder.flush(); } catch(error) { state.audio.stopped = true; }
			}
			reason = state.finish();
		}
	} catch(error) {
		reason = String((error && error.message) || error).slice(0, 200);
	}
	try { state.encoder.close(); } catch(error) {}
	if(audio) {
		try { audio.encoder.close(); } catch(error) {}
	}
	if(reason)
		stringToUTF8(reason, pError, ErrorCapacity);
});

EM_JS(int, BrowserVideoEncoded, (), {
	return Module.ddnetVideo ? Module.ddnetVideo.encoded : 0;
});

EM_JS(int, BrowserVideoError, (char *pError, int ErrorCapacity), {
	// Before there is an export there is no state to carry the reason, and the
	// start is where the browser is most likely to have one.
	const message = Module.ddnetVideo ? Module.ddnetVideo.error : Module.ddnetVideoStartError;
	if(!message)
		return 0;
	stringToUTF8(message, pError, ErrorCapacity);
	return 1;
});
// clang-format on

/**
 * What a frame handed to the browser holds, which is what the other side turns
 * into the format of the VideoFrame it builds.
 */
enum class EFrameFormat
{
	RGBA = 0,
	NV12 = 1,
	I420 = 2,
};

/**
 * Turns the quality setting into a bit rate, which is what a browser encoder
 * takes. The constant-quality modes that libavcodec has are not reliably there
 * behind WebCodecs, so the rate stands in for them: every six steps of the
 * scale halve it, the same way a quantiser does.
 */
int BitRateForQuality(const CVideoExportSettings &Settings)
{
	const double BitsPerPixel = 0.15 * std::pow(2.0, (23.0 - Settings.m_Crf) / 6.0);
	const double BitRate = BitsPerPixel * Settings.m_Width * Settings.m_Height * Settings.m_FPS;
	return (int)std::clamp(BitRate, 100000.0, 200000000.0);
}

class CVideoWebCodecs : public IVideo
{
public:
	CVideoWebCodecs(IGraphics *pGraphics, ISound *pSound, CVideoExportSettings Settings, int64_t LocalStartTime, const char *pName, bool PauseLiveAudio) :
		m_pGraphics(pGraphics),
		m_pSound(pSound),
		m_Settings(Settings),
		m_LocalStartTime(LocalStartTime),
		m_HasAudio(Settings.m_Audio),
		m_PauseLiveAudio(PauseLiveAudio)
	{
		const char *pFileName = fs_filename(pName);
		str_copy(m_aFileName, pFileName[0] == '\0' ? "video.mp4" : pFileName);
		m_TickTime = m_Settings.m_FPS > 0 ? time_freq() / m_Settings.m_FPS : 0;
		dbg_assert(ms_pCurrentVideo == nullptr, "ms_pCurrentVideo is NOT set to nullptr while creating a new Video.");
		ms_pCurrentVideo = this;
	}

	~CVideoWebCodecs() override
	{
		dbg_assert(m_Stopped, "Video must be stopped before it is destroyed");
		dbg_assert(ms_pCurrentVideo != this, "Stopped video must not remain current");
	}

	bool Start() override;
	void Stop() override;
	void Cancel() override
	{
		m_Cancelled = true;
		Stop();
	}
	bool IsStopped() const override { return m_Stopped; }
	void Pause(bool Pause) override
	{
		if(ms_pCurrentVideo == this)
			m_Recording = !Pause;
	}
	bool IsRecording() const override { return m_Recording; }
	bool HasError() const override { return m_aError[0] != '\0'; }
	bool HasAudio() const override { return m_HasAudio; }
	CVideoExportStatus Status() const override;
	const CVideoExportSettings &Settings() const override { return m_Settings; }

	void NextVideoFrame() override
	{
		if(m_Recording)
		{
			m_Time += m_TickTime;
			m_LocalTime = (m_Time - m_LocalStartTime) / (float)time_freq();
		}
	}
	bool BeginVideoFrameRender() override;
	void EndVideoFrameRender() override;

	void NextAudioFrame(ISoundMixFunc Mix) override
	{
		if(!m_Recording || !m_HasAudio)
			return;
		Mix(m_aAudioBuffer, AUDIO_FRAMES_PER_MIX);
		BrowserAudioSubmit(m_aAudioBuffer, (int)AUDIO_FRAMES_PER_MIX, (double)m_AudioSampleCount * 1000000.0 / m_AudioSampleRate);
		m_AudioSampleCount += AUDIO_FRAMES_PER_MIX;
	}

	void NextAudioFrameTimeline(ISoundMixFunc Mix) override
	{
		// The same accounting the libavcodec export does: mix until the sound
		// has caught up with the video frame that was just drawn.
		if(!m_Recording || !m_HasAudio)
			return;
		while(m_AudioFrameSampleCount >= m_AudioSampleCount)
			NextAudioFrame(Mix);
		m_AudioFrameSampleCount += (double)m_AudioSampleRate / m_Settings.m_FPS;
	}

	int64_t Time() const override { return m_Time; }
	float LocalTime() const override { return m_LocalTime; }
	void SetLocalStartTime(int64_t LocalStartTime) override { m_LocalStartTime = LocalStartTime; }

private:
	// The submitted frame is read back from the frame after it, so that the
	// graphics card is never waited on for the frame that was just drawn.
	static constexpr size_t READBACK_SLOT_COUNT = 2;
	// A key frame every two seconds, which is what a player seeking through the
	// finished file expects to find.
	static constexpr int KEY_FRAME_INTERVAL_SECONDS = 2;
	static constexpr std::chrono::milliseconds RATE_SAMPLE_INTERVAL{500};
	// What the mixer is asked for at a time, matching the libavcodec export.
	static constexpr unsigned AUDIO_FRAMES_PER_MIX = 1024;
	static constexpr int AUDIO_CHANNELS = 2;

	class CReadbackSlot
	{
	public:
		IGraphics::CTextureHandle m_Target;
		IGraphics::CTextureHandle m_YuvTarget;
		std::unique_ptr<IGraphics::ITextureReadback> m_pReadback;
		uint64_t m_FrameIndex = 0;
	};

	bool CreateOffscreenTargets();
	void DestroyOffscreenTargets();
	bool FinishReadbackSlot(size_t SlotIndex);
	void DrainReadbackSlots();
	void SubmitFrame(const CImageInfo &Image);
	void SetError(const char *pError);

	IGraphics *m_pGraphics;
	ISound *m_pSound;
	const CVideoExportSettings m_Settings;
	char m_aFileName[IO_MAX_PATH_LENGTH] = {};

	int64_t m_TickTime = 0;
	int64_t m_LocalStartTime;
	int64_t m_Time = 0;
	float m_LocalTime = 0.0f;

	bool m_Started = false;
	bool m_Stopped = false;
	bool m_Recording = false;
	bool m_Cancelled = false;
	bool m_Offscreen = false;
	bool m_OffscreenFrameActive = false;
	// The backend hands the frames over already converted, so the browser does
	// not have to touch a pixel between the graphics card and the encoder.
	bool m_YuvReadback = false;
	IGraphics::EPlanarYuvFormat m_YuvFormat = IGraphics::EPlanarYuvFormat::NV12;
	char m_aError[256] = {};

	bool m_HasAudio;
	bool m_PauseLiveAudio;
	int m_AudioSampleRate = 0;
	int64_t m_AudioSampleCount = 0;
	double m_AudioFrameSampleCount = 0.0;
	short m_aAudioBuffer[AUDIO_FRAMES_PER_MIX * AUDIO_CHANNELS] = {};

	std::array<CReadbackSlot, READBACK_SLOT_COUNT> m_aReadbackSlots;
	// A read frame is eight megabytes at 1080p, which is more than filling it
	// costs, so the same one goes back into the next read.
	CImageInfo m_RecycledImage;
	size_t m_CurrentReadbackSlot = 0;
	uint64_t m_VideoFrameIndex = 0;
	uint64_t m_SubmittedFrames = 0;

	mutable std::chrono::nanoseconds m_RateSampleTime{0};
	mutable uint64_t m_RateSampleFrames = 0;
	mutable float m_FramesPerSecond = 0.0f;
};

void CVideoWebCodecs::SetError(const char *pError)
{
	if(m_aError[0] == '\0')
		str_copy(m_aError, pError);
	log_error("videorecorder", "%s", pError);
}

bool CVideoWebCodecs::CreateOffscreenTargets()
{
	IGraphics::CTextureDesc Desc;
	Desc.m_Width = m_Settings.m_Width;
	Desc.m_Height = m_Settings.m_Height;
	Desc.m_Mipmaps = IGraphics::ETextureMipmaps::NONE;
	Desc.m_Usage = IGraphics::TEXTURE_USAGE_SAMPLED | IGraphics::TEXTURE_USAGE_COLOR_TARGET | IGraphics::TEXTURE_USAGE_COPY_SOURCE;
	// Four YUV bytes travel in one pixel of the packed target, so it is a
	// quarter as wide, and the chroma adds half a frame of rows below the luma
	// plane. The sizes that would split a chroma pair or leave a plane short
	// are the same ones the libavcodec export rejects.
	IGraphics::CTextureDesc YuvDesc = Desc;
	YuvDesc.m_Width = Desc.m_Width / 4;
	YuvDesc.m_Height = Desc.m_Height + Desc.m_Height / 2;
	m_YuvReadback = m_pGraphics->PlanarYuvConversionSupported() && Desc.m_Width % 8 == 0 && Desc.m_Height % 4 == 0;

	for(auto &Slot : m_aReadbackSlots)
	{
		Slot.m_Target = m_pGraphics->CreateTexture(Desc);
		if(!Slot.m_Target.IsValid())
		{
			DestroyOffscreenTargets();
			return false;
		}
		if(!m_YuvReadback)
			continue;
		Slot.m_YuvTarget = m_pGraphics->CreateTexture(YuvDesc);
		if(!Slot.m_YuvTarget.IsValid())
		{
			log_info("videorecorder", "No packed target at %dx%d, converting in the browser", (int)YuvDesc.m_Width, (int)YuvDesc.m_Height);
			m_YuvReadback = false;
		}
	}
	m_CurrentReadbackSlot = 0;
	return true;
}

void CVideoWebCodecs::DestroyOffscreenTargets()
{
	m_OffscreenFrameActive = false;
	for(auto &Slot : m_aReadbackSlots)
	{
		Slot.m_pReadback.reset();
		Slot.m_FrameIndex = 0;
		m_pGraphics->UnloadTexture(&Slot.m_Target);
		if(Slot.m_YuvTarget.IsValid())
			m_pGraphics->UnloadTexture(&Slot.m_YuvTarget);
	}
	m_Offscreen = false;
	m_YuvReadback = false;
}

bool CVideoWebCodecs::Start()
{
	dbg_assert(!m_Started, "Already started");
	if(!BrowserVideoSupported())
	{
		SetError("This browser cannot encode video");
		return false;
	}
	if(m_HasAudio && !m_pSound->IsSoundEnabled())
	{
		// A missing mixer is not a reason to refuse the whole export. The user
		// asked for a video of the demo, so produce one and say it is mute.
		log_warn("videorecorder", "The sound mixer is unavailable, exporting without audio");
		m_HasAudio = false;
	}
	if(m_Settings.m_Width <= 0 || m_Settings.m_Height <= 0 || m_Settings.m_Width > 8192 || m_Settings.m_Height > 8192 || m_Settings.m_Width % 2 != 0 || m_Settings.m_Height % 2 != 0)
	{
		SetError("Video resolution is invalid or exceeds the export size limit");
		return false;
	}
	if(m_Settings.m_FPS < 1 || m_Settings.m_FPS > 1000 || m_Settings.m_Crf < 0 || m_Settings.m_Crf > 51)
	{
		SetError("Invalid video settings");
		return false;
	}

	const std::vector<CVideoEncoder> &vEncoders = VideoEncoders();
	// The browser is free to end up on another profile of the same family, and
	// says which one it took by writing it back here.
	char aCodec[sizeof(CVideoEncoder::m_aName)];
	str_copy(aCodec, m_Settings.m_aVideoCodec[0] == '\0' ? vEncoders.front().m_aName : m_Settings.m_aVideoCodec);
	if(aCodec[0] == '\0')
	{
		SetError("This browser has no video encoder");
		return false;
	}

	m_pGraphics->WaitForIdle();
	m_Offscreen = CreateOffscreenTargets();
	if(!m_Offscreen && (m_Settings.m_Width != m_pGraphics->ScreenWidth() || m_Settings.m_Height != m_pGraphics->ScreenHeight()))
	{
		SetError("Could not create offscreen targets at the export resolution");
		return false;
	}
	m_AudioSampleRate = m_HasAudio ? m_pSound->MixingRate() : 0;
	if(BrowserVideoStart(aCodec, sizeof(aCodec), m_aFileName, m_Settings.m_Width, m_Settings.m_Height, m_Settings.m_FPS, BitRateForQuality(m_Settings), m_AudioSampleRate, AUDIO_CHANNELS) != 0)
	{
		DestroyOffscreenTargets();
		char aReason[192] = {};
		if(!BrowserVideoError(aReason, sizeof(aReason)))
			str_copy(aReason, "the browser gave no reason");
		char aError[sizeof(m_aError)];
		str_format(aError, sizeof(aError), "Could not start the browser video encoder at %dx%d for '%s': %s",
			m_Settings.m_Width, m_Settings.m_Height, aCodec, aReason);
		SetError(aError);
		return false;
	}
	if(m_HasAudio && !BrowserAudioStarted())
	{
		log_warn("videorecorder", "This browser cannot encode audio, exporting without sound");
		m_HasAudio = false;
	}
	log_info("videorecorder", "Encoding %dx%d with '%s' in the browser %s sound, converting on the %s",
		m_Settings.m_Width, m_Settings.m_Height, aCodec, m_HasAudio ? "with" : "without", m_YuvReadback ? "graphics card" : "processor");

	if(m_PauseLiveAudio && m_HasAudio)
		m_pSound->PauseAudioDevice();
	m_Recording = true;
	m_Started = true;
	m_Time = time_get();
	m_LocalTime = (m_Time - m_LocalStartTime) / (float)time_freq();
	m_RateSampleTime = time_get_nanoseconds();
	return true;
}

void CVideoWebCodecs::Stop()
{
	if(m_Stopped)
		return;
	m_Stopped = true;
	m_Recording = false;
	if(m_Started)
	{
		DrainReadbackSlots();
		DestroyOffscreenTargets();
		// Whatever keeps the browser from writing the file is the last thing
		// this export can still say, and it says it here rather than ending
		// without a file and without a reason.
		char aReason[192] = {};
		BrowserVideoStop(m_Cancelled || HasError() ? 1 : 0, aReason, sizeof(aReason));
		if(aReason[0] != '\0')
		{
			char aError[sizeof(m_aError)];
			str_format(aError, sizeof(aError), "The browser could not write the video: %s", aReason);
			SetError(aError);
		}
		if(m_PauseLiveAudio && m_HasAudio)
			m_pSound->UnpauseAudioDevice();
	}
	if(ms_pCurrentVideo == this)
		ms_pCurrentVideo = nullptr;
}

CVideoExportStatus CVideoWebCodecs::Status() const
{
	CVideoExportStatus Status;
	Status.m_SubmittedFrames = m_SubmittedFrames;
	Status.m_EncodedFrames = (uint64_t)BrowserVideoEncoded();
	Status.m_HasError = m_aError[0] != '\0';
	str_copy(Status.m_aError, m_aError);

	const std::chrono::nanoseconds Now = time_get_nanoseconds();
	if(Now - m_RateSampleTime >= RATE_SAMPLE_INTERVAL)
	{
		const float Seconds = std::chrono::duration_cast<std::chrono::duration<float>>(Now - m_RateSampleTime).count();
		m_FramesPerSecond = (Status.m_EncodedFrames - m_RateSampleFrames) / Seconds;
		m_RateSampleTime = Now;
		m_RateSampleFrames = Status.m_EncodedFrames;
	}
	Status.m_FramesPerSecond = m_FramesPerSecond;
	return Status;
}

bool CVideoWebCodecs::BeginVideoFrameRender()
{
	if(!m_Recording || m_OffscreenFrameActive)
		return false;
	if(!FinishReadbackSlot(m_CurrentReadbackSlot))
	{
		SetError("Video frame readback failed");
		return false;
	}
	if(m_Offscreen && !m_pGraphics->BeginOffscreenFrame(m_aReadbackSlots[m_CurrentReadbackSlot].m_Target))
	{
		SetError("Could not begin offscreen video frame");
		return false;
	}
	m_OffscreenFrameActive = true;
	return true;
}

void CVideoWebCodecs::EndVideoFrameRender()
{
	if(!m_OffscreenFrameActive)
		return;
	m_OffscreenFrameActive = false;
	auto &Slot = m_aReadbackSlots[m_CurrentReadbackSlot];
	Slot.m_pReadback = m_Offscreen ? m_pGraphics->EndOffscreenFrame(std::move(m_RecycledImage), Slot.m_YuvTarget, m_YuvFormat) : m_pGraphics->PresentAndReadbackAsync(std::move(m_RecycledImage));
	if(Slot.m_pReadback == nullptr)
	{
		if(!m_Offscreen)
			m_pGraphics->Swap();
		SetError("Could not read back the rendered video frame");
		return;
	}
	Slot.m_FrameIndex = ++m_VideoFrameIndex;
	m_CurrentReadbackSlot = (m_CurrentReadbackSlot + 1) % READBACK_SLOT_COUNT;
}

bool CVideoWebCodecs::FinishReadbackSlot(size_t SlotIndex)
{
	auto &Slot = m_aReadbackSlots[SlotIndex];
	if(Slot.m_pReadback == nullptr)
		return true;
	CImageInfo Image;
	const bool ReadSucceeded = Slot.m_pReadback->Wait(Image);
	Slot.m_pReadback.reset();
	const uint64_t FrameIndex = Slot.m_FrameIndex;
	Slot.m_FrameIndex = 0;
	const size_t ExpectedWidth = m_YuvReadback ? (size_t)m_Settings.m_Width / 4 : (size_t)m_Settings.m_Width;
	const size_t ExpectedHeight = m_YuvReadback ? (size_t)m_Settings.m_Height + (size_t)m_Settings.m_Height / 2 : (size_t)m_Settings.m_Height;
	if(!ReadSucceeded || Image.m_Width != ExpectedWidth || Image.m_Height != ExpectedHeight || Image.m_Format != CImageInfo::FORMAT_RGBA)
		return false;
	// The very first frame of an offscreen export is the one that was still
	// being drawn when the recording started, the same as in the file export.
	if(FrameIndex >= 2)
		SubmitFrame(Image);
	// Only one read is ever outstanding, so the frame that was just handed over
	// is the buffer the next one is read into.
	m_RecycledImage = std::move(Image);
	return true;
}

void CVideoWebCodecs::DrainReadbackSlots()
{
	for(size_t Offset = 0; Offset < READBACK_SLOT_COUNT; ++Offset)
	{
		if(!FinishReadbackSlot((m_CurrentReadbackSlot + Offset) % READBACK_SLOT_COUNT))
			log_error("videorecorder", "Failed to drain video frame readback");
	}
}

void CVideoWebCodecs::SubmitFrame(const CImageInfo &Image)
{
	EFrameFormat Format = EFrameFormat::RGBA;
	if(m_YuvReadback)
		Format = m_YuvFormat == IGraphics::EPlanarYuvFormat::NV12 ? EFrameFormat::NV12 : EFrameFormat::I420;
	const double TimestampMicros = (double)m_SubmittedFrames * 1000000.0 / m_Settings.m_FPS;
	const int KeyFrame = m_SubmittedFrames % (uint64_t)(m_Settings.m_FPS * KEY_FRAME_INTERVAL_SECONDS) == 0;
	if(BrowserVideoSubmit(Image.m_pData, (int)Format, TimestampMicros, KeyFrame) != 0)
	{
		char aError[sizeof(m_aError)] = {};
		if(!BrowserVideoError(aError, sizeof(aError)))
			str_copy(aError, "The browser video encoder failed");
		SetError(aError);
		return;
	}
	++m_SubmittedFrames;
}

} // namespace

bool VideoEncodersProbed()
{
	return BrowserVideoProbeDone() != 0;
}

const std::vector<CVideoEncoder> &VideoEncoders()
{
	// The list is not known before the browser has answered for every codec, so
	// it is built on the first call after that and left alone from then on.
	static std::vector<CVideoEncoder> s_vEncoders;
	static bool s_Built = false;
	if(!s_Built && VideoEncodersProbed())
	{
		s_vEncoders.clear();
		const int Count = BrowserVideoProbeCount();
		for(int i = 0; i < Count; ++i)
		{
			CVideoEncoder &Encoder = s_vEncoders.emplace_back();
			BrowserVideoProbeEntry(i, Encoder.m_aName, sizeof(Encoder.m_aName), Encoder.m_aDisplayName, sizeof(Encoder.m_aDisplayName));
		}
		s_Built = true;
	}
	if(s_vEncoders.empty())
	{
		// Something has to be offered while the probe runs, and a browser that
		// answers with nothing has nothing else to offer either.
		s_vEncoders.emplace_back();
		str_copy(s_vEncoders.front().m_aDisplayName, "Default");
	}
	return s_vEncoders;
}

void ProbeVideoEncoders(IEngine *pEngine)
{
	// Asking the browser does not block, so this needs no thread of its own.
	BrowserVideoProbe();
}

void InitVideoBackend()
{
	BrowserVideoProbe();
}

std::unique_ptr<IVideo> CreateVideo(IGraphics *pGraphics, ISound *pSound, IStorage *pStorage,
	CVideoExportSettings Settings, int64_t LocalStartTime, const char *pName, int OutputStorageType,
	bool AllowOverwrite, bool PauseLiveAudio)
{
	// The browser writes the file itself, so the storage takes no part in the
	// export.
	return std::make_unique<CVideoWebCodecs>(pGraphics, pSound, Settings, LocalStartTime, pName, PauseLiveAudio);
}

#endif
