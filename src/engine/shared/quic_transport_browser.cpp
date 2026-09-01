#include "quic_transport.h"

// The browser brings its own WebTransport client, which this drives. Serving
// and native QUIC are in quic_transport.cpp.
#if defined(CONF_PLATFORM_EMSCRIPTEN)

#include <base/hash_ctxt.h>
#include <base/mem.h>
#include <base/secure.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/shared/game_wire.h>
#include <engine/shared/transport_pin.h>

#include <emscripten/emscripten.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{
	constexpr CQuicSessionId SESSION(1);
	constexpr int RESUME_TIMEOUT_SECONDS = 5;
	// The browser hands data over in chunks of at most this size, a map chunk
	// behind the 4 bytes of its stream id.
	constexpr size_t EVENT_CHUNK_SIZE = 32 * 1024;
	constexpr size_t MAX_EVENT_SIZE = EVENT_CHUNK_SIZE + 4;
	constexpr size_t MAX_CONTROL_FRAME_SIZE = 64 * 1024 + 16;
	constexpr size_t MAX_MAP_HEADER_FRAME_SIZE = 4 * 1024;
	constexpr size_t MAX_DATAGRAM_SIZE = 1000;

	enum class EBrowserEvent
	{
		NONE = 0,
		READY = 1,
		CONTROL_DATA = 2,
		DATAGRAM = 3,
		UNI_START = 4,
		UNI_DATA = 5,
		UNI_END = 6,
		CLOSED = 7,
		FAILED_UNAVAILABLE = 8,
		FAILED_IDENTITY = 9,
		FAILED_PROTOCOL = 10,
	};

	// clang-format off
	EM_JS(int, BrowserWebTransportAvailable, (), {
		return typeof WebTransport === 'undefined' ? 0 : 1;
	});

	EM_JS(int, BrowserWebTransportStart, (const char *pUrl, const unsigned char *pCertificateHashes, int CertificateHashCount), {
		if(typeof WebTransport === 'undefined')
			return -1;
		if(!Module.ddnetWebTransportStates) {
			Module.ddnetWebTransportStates = new Map();
			Module.ddnetNextWebTransportHandle = 1;
		}
		const states = Module.ddnetWebTransportStates;
		const handle = Module.ddnetNextWebTransportHandle++;
		const state = {
			events: [], mapEvents: [], controlSends: [], datagramSends: [],
			controlDraining: false, datagramDraining: false, terminal: false,
			transport: null, controlWriter: null, datagramWriter: null, mapGeneration: 0, mapActive: false,
		};
		states.set(handle, state);
		const active = () => states.get(handle) === state && !state.terminal;
		const bytes = value => value instanceof Uint8Array ? value : new Uint8Array(value);
		const errorReason = error => error && typeof error.message === 'string' ? error.message.slice(0, 255) : String(error || 'WebTransport failed').slice(0, 255);
		const fail = (kind, reason) => {
			if(!active())
				return;
			state.events.length = 0;
			state.mapEvents.length = 0;
			state.events.push({kind, payload: new Uint8Array(), reason: reason || 'WebTransport failed'});
			state.terminal = true;
			try { if(state.transport) state.transport.close({closeCode: 2, reason: 'transport failure'}); } catch(_) {}
		};
		const push = (kind, payload, reason = "") => {
			if(!active())
				return false;
			if(state.events.length >= 256) {
				fail(10, 'WebTransport event queue full');
				return false;
			}
			state.events.push({kind, payload: payload || new Uint8Array(), reason});
			return true;
		};
		const pushChunks = (kind, value) => {
			const data = bytes(value);
			for(let offset = 0; offset < data.length; offset += 32768) {
				if(!push(kind, data.slice(offset, Math.min(offset + 32768, data.length))))
					return false;
			}
			return true;
		};
		const pushMap = async (kind, id, value) => {
			const data = value ? bytes(value) : new Uint8Array();
			for(let offset = 0; offset < Math.max(data.length, 1); offset += 32768) {
				while(active() && state.mapEvents.length >= 16)
					await new Promise(resolve => setTimeout(resolve, 1));
				if(!active())
					return false;
				const chunk = data.slice(offset, Math.min(offset + 32768, data.length));
				const payload = new Uint8Array(4 + chunk.length);
				payload[0] = id & 255;
				payload[1] = (id >>> 8) & 255;
				payload[2] = (id >>> 16) & 255;
				payload[3] = (id >>> 24) & 255;
				payload.set(chunk, 4);
				state.mapEvents.push({kind, payload, reason: ""});
			}
			return true;
		};
		state.drainControl = async () => {
			if(state.controlDraining || !state.controlWriter)
				return;
			state.controlDraining = true;
			try {
				while(active() && state.controlSends.length) {
					await state.controlWriter.ready;
					await state.controlWriter.write(state.controlSends.shift());
				}
			} catch(error) {
				if(active())
					fail(7, errorReason(error));
			}
			state.controlDraining = false;
		};
		state.drainDatagrams = async () => {
			if(state.datagramDraining || !state.datagramWriter)
				return;
			state.datagramDraining = true;
			while(active() && state.datagramSends.length) {
				const datagram = state.datagramSends.shift();
				try {
					await state.datagramWriter.ready;
					await state.datagramWriter.write(datagram);
				} catch(_) {}
			}
			state.datagramDraining = false;
		};
		const readControl = async readable => {
			try {
				const reader = readable.getReader();
				while(active()) {
					const result = await reader.read();
					if(result.done)
						break;
					if(!pushChunks(2, result.value))
						return;
				}
			} catch(error) {
				if(active())
					fail(7, errorReason(error));
			}
		};
		const readDatagrams = async readable => {
			try {
				const reader = readable.getReader();
				while(active()) {
					const result = await reader.read();
					if(result.done)
						break;
					if(state.events.length < 256)
						push(3, bytes(result.value).slice());
				}
			} catch(error) {
				if(active())
					fail(7, errorReason(error));
			}
		};
		const readMap = async (stream, generation) => {
			if(state.mapActive) {
				try { await stream.cancel('concurrent map stream'); } catch(_) {}
				fail(10, 'concurrent WebTransport map stream');
				return;
			}
			state.mapActive = true;
			try {
				const reader = stream.getReader();
				if(!await pushMap(4, generation))
					return;
				while(active() && generation === state.mapGeneration) {
					const result = await reader.read();
					if(result.done)
						break;
					if(!await pushMap(5, generation, result.value))
						return;
				}
				if(active() && generation === state.mapGeneration)
					await pushMap(6, generation);
			} catch(error) {
				if(active() && generation === state.mapGeneration)
					fail(10, errorReason(error));
			} finally {
				if(generation === state.mapGeneration)
					state.mapActive = false;
			}
		};
		const readIncomingStreams = async readable => {
			try {
				const reader = readable.getReader();
				while(active()) {
					const result = await reader.read();
					if(result.done)
						break;
					const generation = ++state.mapGeneration;
					readMap(result.value, generation);
				}
			} catch(error) {
				if(active())
					fail(7, errorReason(error));
			}
		};
		const url = UTF8ToString(pUrl);
		const hashes = [];
		for(let i = 0; i < CertificateHashCount; ++i) {
			const hash = HEAPU8.slice(pCertificateHashes + i * 32, pCertificateHashes + (i + 1) * 32);
			hashes.push({algorithm: 'sha-256', value: hash.buffer});
		}
		(async () => {
			try {
				const options = {requireUnreliable: true};
				if(hashes.length)
					options.serverCertificateHashes = hashes;
				state.transport = new WebTransport(url, options);
				state.transport.closed.then(() => {
					if(active())
						fail(7, 'WebTransport connection closed');
				}).catch(error => {
					if(active())
						fail(7, errorReason(error));
				});
				await state.transport.ready;
				if(state.transport.reliability === 'reliable-only' || !state.transport.datagrams || !state.transport.datagrams.readable || !state.transport.incomingUnidirectionalStreams) {
					fail(8, 'WebTransport datagrams are unavailable');
					return;
				}
				const datagramWritable = state.transport.datagrams.writable || (state.transport.datagrams.createWritable && state.transport.datagrams.createWritable());
				const maxDatagramSize = Number(state.transport.datagrams.maxDatagramSize || 0);
				if(!datagramWritable || maxDatagramSize <= 0) {
					fail(8, 'WebTransport datagram writer is unavailable');
					return;
				}
				const control = await state.transport.createBidirectionalStream();
				state.controlWriter = control.writable.getWriter();
				state.datagramWriter = datagramWritable.getWriter();
				const boundedSize = Math.min(maxDatagramSize, 1000) >>> 0;
				const ready = new Uint8Array([boundedSize & 255, (boundedSize >>> 8) & 255, (boundedSize >>> 16) & 255, (boundedSize >>> 24) & 255]);
				if(!push(1, ready))
					return;
				readControl(control.readable);
				readDatagrams(state.transport.datagrams.readable);
				readIncomingStreams(state.transport.incomingUnidirectionalStreams);
			} catch(error) {
				fail(error && error.name === 'NotSupportedError' ? 8 : 9, errorReason(error));
			}
		})();
		return handle;
	});

	EM_JS(int, BrowserWebTransportPoll, (int Handle, unsigned char *pPayload, int PayloadCapacity, int *pPayloadSize, char *pReason, int ReasonCapacity), {
		const state = Module.ddnetWebTransportStates && Module.ddnetWebTransportStates.get(Handle);
		if(!state)
			return 0;
		const event = state.events.shift() || state.mapEvents.shift();
		if(!event)
			return 0;
		if(event.payload.length > PayloadCapacity) {
			HEAP32[pPayloadSize >> 2] = 0;
			stringToUTF8('WebTransport event exceeds buffer', pReason, ReasonCapacity);
			return 10;
		}
		HEAPU8.set(event.payload, pPayload);
		HEAP32[pPayloadSize >> 2] = event.payload.length;
		stringToUTF8(event.reason || "", pReason, ReasonCapacity);
		return event.kind;
	});

	EM_JS(int, BrowserWebTransportSendControl, (int Handle, const unsigned char *pData, int DataSize), {
		const state = Module.ddnetWebTransportStates && Module.ddnetWebTransportStates.get(Handle);
		if(!state || state.terminal || !state.controlWriter || state.controlSends.length >= 128)
			return 0;
		state.controlSends.push(HEAPU8.slice(pData, pData + DataSize));
		state.drainControl();
		return 1;
	});

	EM_JS(int, BrowserWebTransportSendDatagram, (int Handle, const unsigned char *pData, int DataSize), {
		const state = Module.ddnetWebTransportStates && Module.ddnetWebTransportStates.get(Handle);
		if(!state || state.terminal || !state.datagramWriter || state.datagramSends.length >= 256)
			return 0;
		state.datagramSends.push(HEAPU8.slice(pData, pData + DataSize));
		state.drainDatagrams();
		return 1;
	});

	EM_JS(void, BrowserWebTransportClose, (int Handle, int CloseCode, const char *pReason), {
		const states = Module.ddnetWebTransportStates;
		const state = states && states.get(Handle);
		if(!state)
			return;
		state.terminal = true;
		try { if(state.transport) state.transport.close({closeCode: CloseCode >>> 0, reason: UTF8ToString(pReason).slice(0, 255)}); } catch(_) {}
		states.delete(Handle);
	});
	// clang-format on

	rust::Slice<const uint8_t> Slice(const unsigned char *pData, size_t Size)
	{
		return {pData, Size};
	}

	uint32_t ReadUint32(const unsigned char *pData)
	{
		return pData[0] | (pData[1] << 8) | (pData[2] << 16) | (static_cast<uint32_t>(pData[3]) << 24);
	}
}

class CQuicTransport::CImpl
{
public:
	std::string m_Url;
	std::vector<unsigned char> m_vCertificateHashes;
	NETADDR m_PeerAddress;
	bool m_Sixup;
	EQuicConnectFailure m_Failure = EQuicConnectFailure::NONE;

	int m_Handle = -1;
	bool m_Connected = false;
	bool m_Resuming = false;
	int64_t m_ResumeDeadline = 0;
	std::vector<unsigned char> m_vResumeBinding;
	std::vector<unsigned char> m_vRawEvent = std::vector<unsigned char>(MAX_EVENT_SIZE);
	char m_aEventReason[256] = {};
	std::vector<unsigned char> m_vEventPayload;

	std::vector<unsigned char> m_vControlBuffer;
	// Where the undecoded part of the control buffer starts. Decoding moves this
	// forward instead of shifting the rest of the buffer down after every frame,
	// which a peer packing many small frames into one burst would make quadratic.
	size_t m_ControlBufferOffset = 0;

	size_t m_LocalMaxDatagramSize = 0;
	size_t m_PeerMaxDatagramSize = 0;
	uint64_t m_DatagramSequence = 0;
	bool m_HasReceivedDatagram = false;
	uint64_t m_LastReceivedDatagramSequence = 0;
	std::vector<unsigned char> m_vDatagram;
	rust::Vec<GameWire::DatagramMessage> m_vDatagramMessages;
	size_t m_NextDatagramMessage = 0;

	uint32_t m_MapStreamId = 0;
	bool m_MapHeaderReceived = false;
	std::vector<unsigned char> m_vMapHeaderBuffer;
	size_t m_MapBytesRemaining = 0;
	SHA256_CTX m_MapSha256Context = {};
	SHA256_DIGEST m_MapExpectedSha256 = {};

	CImpl(const char *pUrl, const NETADDR &PeerAddress, const CModernTransportPin &Pin, bool Sixup);
	bool Open();
	bool Reconnect();
	bool Send(const void *pData, int DataSize, bool Vital);
	bool Poll(CQuicEvent &Event);

private:
	bool SetEvent(CQuicEvent &Event, EQuicEventType Type, const void *pData = nullptr, size_t DataSize = 0, bool Vital = true);
	bool Disconnect(CQuicEvent &Event, EQuicConnectFailure Failure, const char *pReason);
	bool RetryOrDisconnect(CQuicEvent &Event, EQuicConnectFailure Failure, const char *pReason);
	bool MapFailed(CQuicEvent &Event, const char *pReason);
	bool NextControlFrame(CQuicEvent &Event);
	bool NextDatagramMessage(CQuicEvent &Event);
	bool BufferedMapData(CQuicEvent &Event);
	bool OnReady(CQuicEvent &Event, const unsigned char *pData, size_t Size);
	bool OnControlData(CQuicEvent &Event, const unsigned char *pData, size_t Size);
	bool OnDatagram(CQuicEvent &Event, const unsigned char *pData, size_t Size);
	bool OnMapStart(CQuicEvent &Event, const unsigned char *pData, size_t Size);
	bool OnMapData(CQuicEvent &Event, const unsigned char *pData, size_t Size);
	bool OnMapEnd(CQuicEvent &Event, const unsigned char *pData, size_t Size);
	bool OnClosed(CQuicEvent &Event);
};

CQuicTransport::CImpl::CImpl(const char *pUrl, const NETADDR &PeerAddress, const CModernTransportPin &Pin, bool Sixup) :
	m_Url(pUrl), m_PeerAddress(PeerAddress), m_Sixup(Sixup)
{
	if(Pin.m_Trust == EModernTransportTrust::CERTIFICATE_HASH)
	{
		m_vCertificateHashes.insert(m_vCertificateHashes.end(), std::begin(Pin.m_Fingerprint.data), std::end(Pin.m_Fingerprint.data));
		if(Pin.m_HasNextFingerprint)
			m_vCertificateHashes.insert(m_vCertificateHashes.end(), std::begin(Pin.m_NextFingerprint.data), std::end(Pin.m_NextFingerprint.data));
	}
}

bool CQuicTransport::CImpl::Open()
{
	m_Handle = BrowserWebTransportStart(m_Url.c_str(), m_vCertificateHashes.empty() ? nullptr : m_vCertificateHashes.data(), m_vCertificateHashes.size() / SHA256_DIGEST_LENGTH);
	return m_Handle >= 0;
}

bool CQuicTransport::CImpl::Reconnect()
{
	if(m_Handle < 0 || m_vResumeBinding.empty())
		return false;
	BrowserWebTransportClose(m_Handle, 0, "reconnect");
	if(!Open())
		return false;
	if(!m_Resuming)
		m_ResumeDeadline = time_get() + RESUME_TIMEOUT_SECONDS * time_freq();
	m_Connected = false;
	m_Resuming = true;
	m_vControlBuffer.clear();
	m_ControlBufferOffset = 0;
	m_LocalMaxDatagramSize = 0;
	m_PeerMaxDatagramSize = 0;
	m_DatagramSequence = 0;
	m_HasReceivedDatagram = false;
	m_LastReceivedDatagramSequence = 0;
	m_NextDatagramMessage = m_vDatagramMessages.size();
	m_MapStreamId = 0;
	m_MapHeaderReceived = false;
	m_vMapHeaderBuffer.clear();
	m_MapBytesRemaining = 0;
	return true;
}

bool CQuicTransport::CImpl::Send(const void *pData, int DataSize, bool Vital)
{
	if(m_Handle < 0 || DataSize <= 0)
		return false;
	// What is sent while resuming is lost like on any other bad link.
	if(!m_Connected)
		return m_Resuming;
	const rust::Slice<const uint8_t> Message(static_cast<const uint8_t *>(pData), DataSize);
	if(Vital)
	{
		const rust::Vec<uint8_t> Frame = GameWire::encode_frame(GameWire::FrameType::Message, Message);
		return !Frame.empty() && BrowserWebTransportSendControl(m_Handle, Frame.data(), Frame.size());
	}
	const rust::Vec<uint8_t> Datagram = GameWire::encode_datagram(m_DatagramSequence, Message);
	if(Datagram.empty() || Datagram.size() > std::min(m_LocalMaxDatagramSize, m_PeerMaxDatagramSize) ||
		!BrowserWebTransportSendDatagram(m_Handle, Datagram.data(), Datagram.size()))
		return false;
	m_DatagramSequence++;
	return true;
}

bool CQuicTransport::CImpl::SetEvent(CQuicEvent &Event, EQuicEventType Type, const void *pData, size_t DataSize, bool Vital)
{
	Event = {Type, {SESSION, m_PeerAddress, Vital, pData, static_cast<int>(DataSize)}, nullptr};
	Event.m_Sixup = m_Sixup;
	Event.m_WebTransport = true;
	return true;
}

bool CQuicTransport::CImpl::Disconnect(CQuicEvent &Event, EQuicConnectFailure Failure, const char *pReason)
{
	m_Failure = Failure;
	str_copy(m_aEventReason, pReason && pReason[0] ? pReason : "WebTransport connection closed");
	m_Connected = false;
	m_Resuming = false;
	SetEvent(Event, EQuicEventType::DISCONNECTED);
	Event.m_pReason = m_aEventReason;
	return true;
}

bool CQuicTransport::CImpl::RetryOrDisconnect(CQuicEvent &Event, EQuicConnectFailure Failure, const char *pReason)
{
	if(m_Resuming && time_get() < m_ResumeDeadline && Reconnect())
		return false;
	return Disconnect(Event, Failure, pReason);
}

bool CQuicTransport::CImpl::MapFailed(CQuicEvent &Event, const char *pReason)
{
	str_copy(m_aEventReason, pReason);
	SetEvent(Event, EQuicEventType::MAP_FAILED);
	Event.m_pReason = m_aEventReason;
	return true;
}

bool CQuicTransport::CImpl::Poll(CQuicEvent &Event)
{
	if(m_Handle < 0)
		return false;
	if(NextControlFrame(Event) || NextDatagramMessage(Event) || BufferedMapData(Event))
		return true;
	if(m_Resuming && time_get() >= m_ResumeDeadline)
		return RetryOrDisconnect(Event, EQuicConnectFailure::NETWORK, "WebTransport resume timed out");

	int Size = 0;
	m_aEventReason[0] = '\0';
	const EBrowserEvent Type = static_cast<EBrowserEvent>(BrowserWebTransportPoll(m_Handle, m_vRawEvent.data(), m_vRawEvent.size(), &Size, m_aEventReason, sizeof(m_aEventReason)));
	if(Type == EBrowserEvent::NONE)
		return false;
	if(Size < 0 || static_cast<size_t>(Size) > m_vRawEvent.size())
		return Disconnect(Event, EQuicConnectFailure::PROTOCOL, "invalid WebTransport event size");
	const unsigned char *pData = m_vRawEvent.data();
	switch(Type)
	{
	case EBrowserEvent::NONE: return false;
	case EBrowserEvent::READY: return OnReady(Event, pData, Size);
	case EBrowserEvent::CONTROL_DATA: return OnControlData(Event, pData, Size);
	case EBrowserEvent::DATAGRAM: return OnDatagram(Event, pData, Size);
	case EBrowserEvent::UNI_START: return OnMapStart(Event, pData, Size);
	case EBrowserEvent::UNI_DATA: return OnMapData(Event, pData, Size);
	case EBrowserEvent::UNI_END: return OnMapEnd(Event, pData, Size);
	case EBrowserEvent::CLOSED: return OnClosed(Event);
	case EBrowserEvent::FAILED_UNAVAILABLE: return RetryOrDisconnect(Event, EQuicConnectFailure::NETWORK, m_aEventReason);
	case EBrowserEvent::FAILED_IDENTITY: return RetryOrDisconnect(Event, EQuicConnectFailure::IDENTITY, m_aEventReason);
	case EBrowserEvent::FAILED_PROTOCOL: return RetryOrDisconnect(Event, EQuicConnectFailure::PROTOCOL, m_aEventReason);
	}
	return false;
}

bool CQuicTransport::CImpl::NextControlFrame(CQuicEvent &Event)
{
	while(m_ControlBufferOffset < m_vControlBuffer.size())
	{
		const unsigned char *pBuffer = m_vControlBuffer.data() + m_ControlBufferOffset;
		const GameWire::Frame Frame = GameWire::decode_frame(Slice(pBuffer, m_vControlBuffer.size() - m_ControlBufferOffset));
		if(Frame.status == GameWire::DecodeStatus::NeedMore)
			return false;
		if(Frame.status != GameWire::DecodeStatus::Ok)
			return Disconnect(Event, EQuicConnectFailure::PROTOCOL, "invalid WebTransport control frame");
		m_ControlBufferOffset += Frame.size;
		const unsigned char *pPayload = pBuffer + Frame.payload_offset;
		const size_t PayloadSize = Frame.payload_size;
		if(!m_Connected)
		{
			const size_t PeerMaxDatagramSize = Frame.frame_type == static_cast<uint64_t>(GameWire::FrameType::ServerHello) ? GameWire::decode_server_hello(Slice(pPayload, PayloadSize), m_Sixup) : 0;
			if(PeerMaxDatagramSize == 0)
				return Disconnect(Event, EQuicConnectFailure::PROTOCOL, "invalid WebTransport ServerHello");
			m_PeerMaxDatagramSize = std::min(PeerMaxDatagramSize, MAX_DATAGRAM_SIZE);
			m_Connected = true;
			m_Failure = EQuicConnectFailure::NONE;
			if(m_Resuming)
			{
				m_Resuming = false;
				continue;
			}
			return SetEvent(Event, EQuicEventType::CONNECTED);
		}
		if(Frame.frame_type == static_cast<uint64_t>(GameWire::FrameType::Message))
			return SetEvent(Event, EQuicEventType::MESSAGE, pPayload, PayloadSize);
		if(Frame.frame_type == static_cast<uint64_t>(GameWire::FrameType::Resume))
		{
			if(!GameWire::decode_resume(Slice(pPayload, PayloadSize)).valid)
				return Disconnect(Event, EQuicConnectFailure::PROTOCOL, "invalid WebTransport resume binding");
			m_vResumeBinding.assign(pPayload, pPayload + PayloadSize);
			continue;
		}
		if(Frame.frame_type == static_cast<uint64_t>(GameWire::FrameType::Disconnect))
		{
			char aReason[256];
			if(PayloadSize >= sizeof(aReason))
				return Disconnect(Event, EQuicConnectFailure::PROTOCOL, "invalid WebTransport disconnect reason");
			str_truncate(aReason, sizeof(aReason), reinterpret_cast<const char *>(pPayload), PayloadSize);
			if(!str_utf8_check(aReason) || str_length(aReason) != static_cast<int>(PayloadSize))
				return Disconnect(Event, EQuicConnectFailure::PROTOCOL, "invalid WebTransport disconnect reason");
			return Disconnect(Event, EQuicConnectFailure::NONE, aReason[0] ? aReason : "server disconnected");
		}
		if(!Frame.skippable)
			return Disconnect(Event, EQuicConnectFailure::PROTOCOL, "unexpected WebTransport control frame");
	}
	m_vControlBuffer.clear();
	m_ControlBufferOffset = 0;
	return false;
}

bool CQuicTransport::CImpl::NextDatagramMessage(CQuicEvent &Event)
{
	if(m_NextDatagramMessage >= m_vDatagramMessages.size())
		return false;
	const GameWire::DatagramMessage &Message = m_vDatagramMessages[m_NextDatagramMessage++];
	return SetEvent(Event, EQuicEventType::MESSAGE, m_vDatagram.data() + Message.offset, Message.size, false);
}

bool CQuicTransport::CImpl::BufferedMapData(CQuicEvent &Event)
{
	if(!m_MapHeaderReceived || m_vMapHeaderBuffer.empty())
		return false;
	if(m_vMapHeaderBuffer.size() > m_MapBytesRemaining)
		return MapFailed(Event, "WebTransport map stream exceeds declared size");
	m_vEventPayload.swap(m_vMapHeaderBuffer);
	m_vMapHeaderBuffer.clear();
	m_MapBytesRemaining -= m_vEventPayload.size();
	sha256_update(&m_MapSha256Context, m_vEventPayload.data(), m_vEventPayload.size());
	return SetEvent(Event, EQuicEventType::MAP_DATA, m_vEventPayload.data(), m_vEventPayload.size());
}

bool CQuicTransport::CImpl::OnReady(CQuicEvent &Event, const unsigned char *pData, size_t Size)
{
	const uint32_t MaxDatagramSize = Size == 4 ? ReadUint32(pData) : 0;
	if(MaxDatagramSize == 0 || MaxDatagramSize > MAX_DATAGRAM_SIZE)
		return Disconnect(Event, EQuicConnectFailure::PROTOCOL, "invalid WebTransport datagram size");
	m_LocalMaxDatagramSize = MaxDatagramSize;
	unsigned char aNonce[32];
	secure_random_fill(aNonce, sizeof(aNonce));
	const rust::Vec<uint8_t> Hello = GameWire::encode_client_hello(m_Sixup, MaxDatagramSize, Slice(aNonce, sizeof(aNonce)), Slice(m_vResumeBinding.data(), m_vResumeBinding.size()));
	if(Hello.empty() || !BrowserWebTransportSendControl(m_Handle, Hello.data(), Hello.size()))
		return Disconnect(Event, EQuicConnectFailure::PROTOCOL, "could not send WebTransport ClientHello");
	return false;
}

bool CQuicTransport::CImpl::OnControlData(CQuicEvent &Event, const unsigned char *pData, size_t Size)
{
	m_vControlBuffer.erase(m_vControlBuffer.begin(), m_vControlBuffer.begin() + m_ControlBufferOffset);
	m_ControlBufferOffset = 0;
	if(m_vControlBuffer.size() + Size > MAX_CONTROL_FRAME_SIZE + EVENT_CHUNK_SIZE)
		return Disconnect(Event, EQuicConnectFailure::PROTOCOL, "WebTransport control buffer exceeded limit");
	m_vControlBuffer.insert(m_vControlBuffer.end(), pData, pData + Size);
	return NextControlFrame(Event);
}

bool CQuicTransport::CImpl::OnDatagram(CQuicEvent &Event, const unsigned char *pData, size_t Size)
{
	m_vDatagram.assign(pData, pData + Size);
	GameWire::Datagram Datagram = GameWire::decode_datagram(Slice(m_vDatagram.data(), m_vDatagram.size()));
	if(!Datagram.valid)
		return Disconnect(Event, EQuicConnectFailure::PROTOCOL, "invalid WebTransport datagram");
	m_NextDatagramMessage = 0;
	m_vDatagramMessages = std::move(Datagram.messages);
	// Late datagrams are dropped rather than applied after newer ones.
	if(m_HasReceivedDatagram && Datagram.sequence <= m_LastReceivedDatagramSequence)
		m_NextDatagramMessage = m_vDatagramMessages.size();
	m_HasReceivedDatagram = true;
	m_LastReceivedDatagramSequence = std::max(m_LastReceivedDatagramSequence, static_cast<uint64_t>(Datagram.sequence));
	return NextDatagramMessage(Event);
}

bool CQuicTransport::CImpl::OnMapStart(CQuicEvent &Event, const unsigned char *pData, size_t Size)
{
	const uint32_t StreamId = Size == 4 ? ReadUint32(pData) : 0;
	if(StreamId == 0 || m_MapStreamId != 0)
		return MapFailed(Event, "invalid WebTransport map stream");
	m_MapStreamId = StreamId;
	m_MapHeaderReceived = false;
	m_vMapHeaderBuffer.clear();
	m_MapBytesRemaining = 0;
	return false;
}

bool CQuicTransport::CImpl::OnMapData(CQuicEvent &Event, const unsigned char *pData, size_t Size)
{
	if(Size < 4 || ReadUint32(pData) == 0 || ReadUint32(pData) != m_MapStreamId)
		return MapFailed(Event, "unexpected WebTransport map stream data");
	pData += 4;
	Size -= 4;
	if(Size == 0)
		return false;
	if(m_MapHeaderReceived)
	{
		if(Size > m_MapBytesRemaining)
			return MapFailed(Event, "WebTransport map stream exceeds declared size");
		m_MapBytesRemaining -= Size;
		sha256_update(&m_MapSha256Context, pData, Size);
		return SetEvent(Event, EQuicEventType::MAP_DATA, pData, Size);
	}
	if(m_vMapHeaderBuffer.size() + Size > MAX_MAP_HEADER_FRAME_SIZE + EVENT_CHUNK_SIZE)
		return MapFailed(Event, "WebTransport map header exceeded limit");
	m_vMapHeaderBuffer.insert(m_vMapHeaderBuffer.end(), pData, pData + Size);
	const GameWire::Frame Frame = GameWire::decode_map_stream_start(Slice(m_vMapHeaderBuffer.data(), m_vMapHeaderBuffer.size()));
	if(Frame.status == GameWire::DecodeStatus::NeedMore)
		return false;
	if(Frame.status != GameWire::DecodeStatus::Ok)
		return MapFailed(Event, "invalid WebTransport map header frame");
	const GameWire::MapHeader Header = GameWire::decode_map_header(Slice(m_vMapHeaderBuffer.data() + Frame.payload_offset, Frame.payload_size));
	if(!Header.valid)
		return MapFailed(Event, "invalid WebTransport map header");
	m_MapHeaderReceived = true;
	m_MapBytesRemaining = Header.size;
	mem_copy(m_MapExpectedSha256.data, Header.sha256.data(), sizeof(m_MapExpectedSha256.data));
	sha256_init(&m_MapSha256Context);
	m_vEventPayload.assign(m_vMapHeaderBuffer.begin() + Frame.payload_offset, m_vMapHeaderBuffer.begin() + Frame.payload_offset + Frame.payload_size);
	m_vMapHeaderBuffer.erase(m_vMapHeaderBuffer.begin(), m_vMapHeaderBuffer.begin() + Frame.size);
	return SetEvent(Event, EQuicEventType::MAP_HEADER, m_vEventPayload.data(), m_vEventPayload.size());
}

bool CQuicTransport::CImpl::OnMapEnd(CQuicEvent &Event, const unsigned char *pData, size_t Size)
{
	if(Size != 4 || ReadUint32(pData) != m_MapStreamId || !m_MapHeaderReceived || m_MapBytesRemaining != 0)
		return MapFailed(Event, "WebTransport map stream ended early");
	if(sha256_finish(&m_MapSha256Context) != m_MapExpectedSha256)
		return MapFailed(Event, "WebTransport map SHA-256 mismatch");
	m_MapStreamId = 0;
	m_MapHeaderReceived = false;
	return SetEvent(Event, EQuicEventType::MAP_END);
}

bool CQuicTransport::CImpl::OnClosed(CQuicEvent &Event)
{
	// A connection that is lost after it was set up is resumed.
	if(m_Connected && Reconnect())
		return false;
	return RetryOrDisconnect(Event, m_Connected || m_Resuming ? EQuicConnectFailure::NETWORK : EQuicConnectFailure::PROTOCOL, m_aEventReason);
}

CQuicTransport::CQuicTransport() = default;
CQuicTransport::~CQuicTransport()
{
	Shutdown();
}

bool CQuicTransport::IsCompiled()
{
	return false;
}

bool CQuicTransport::IsWebTransportClientAvailable()
{
	return BrowserWebTransportAvailable() != 0;
}

bool CQuicTransport::StartServer(bool RawQuic, bool WebTransport, const char *pCertificatePath, const char *pNextCertificatePath, const char *pPrivateKeyPath, const char *pIdentityPath, const unsigned char *pCidKey, int CidKeySize)
{
	str_copy(m_aError, "a browser cannot serve QUIC");
	return false;
}

bool CQuicTransport::UpdateCidKey(const unsigned char *pCidKey, int CidKeySize)
{
	str_copy(m_aError, "a browser cannot serve QUIC");
	return false;
}

bool CQuicTransport::MaybeRotateManagedCertificate(bool *pRotated)
{
	*pRotated = false;
	return true;
}

bool CQuicTransport::StartClient(const NETADDR &Address, const char *pServerName, const CModernTransportPin &Pin, bool Sixup)
{
	Shutdown();
	char aHost[NETADDR_MAXSTRSIZE];
	if(pServerName[0] == '\0')
		net_addr_str(&Address, aHost, sizeof(aHost), false);
	else
		str_copy(aHost, pServerName);
	char aUrl[256];
	if(!FormatWebTransportUrl(aUrl, sizeof(aUrl), aHost, Address.port))
	{
		str_copy(m_aError, "invalid WebTransport endpoint");
		return false;
	}
	auto pImpl = std::make_unique<CImpl>(aUrl, Address, Pin, Sixup);
	if(!pImpl->Open())
	{
		m_ConnectFailure = EQuicConnectFailure::NETWORK;
		str_copy(m_aError, "WebTransport is unavailable in this browser");
		return false;
	}
	m_pImpl = std::move(pImpl);
	return true;
}

bool CQuicTransport::IsRunning() const
{
	return m_pImpl != nullptr;
}

bool CQuicTransport::Send(CQuicSessionId Session, const void *pData, int DataSize, bool Vital)
{
	return m_pImpl && Session.IsValid() && m_pImpl->Send(pData, DataSize, Vital);
}

bool CQuicTransport::SetMap(uint32_t MapId, const char *pName, uint32_t Crc, const SHA256_DIGEST &Sha256, const void *pData, size_t DataSize) { return false; }
bool CQuicTransport::SendMap(CQuicSessionId Session, uint32_t MapId) { return false; }
bool CQuicTransport::IssueResume(CQuicSessionId Session, uint64_t LogicalSessionId, const unsigned char *pToken, size_t TokenSize) { return false; }

bool CQuicTransport::Reconnect(CQuicSessionId Session)
{
	return m_pImpl && Session.IsValid() && m_pImpl->Reconnect();
}

bool CQuicTransport::Close(CQuicSessionId Session, const char *pReason)
{
	if(!m_pImpl || m_pImpl->m_Handle < 0 || !Session.IsValid())
		return false;
	BrowserWebTransportClose(m_pImpl->m_Handle, 0, pReason ? pReason : "");
	m_pImpl->m_Handle = -1;
	m_pImpl->m_Connected = false;
	return true;
}

bool CQuicTransport::Poll(CQuicEvent &Event)
{
	if(!m_pImpl)
		return false;
	const bool Polled = m_pImpl->Poll(Event);
	m_ConnectFailure = m_pImpl->m_Failure;
	return Polled;
}

bool CQuicTransport::FeedUdp(const NETADDR *pAddress, const void *pData, int DataSize) { return false; }
int CQuicTransport::PollUdpSend(NETADDR *pAddress, unsigned char **ppData) { return 0; }
bool CQuicTransport::SetLegacyPeer(const NETADDR *pAddress, bool Known) { return false; }
int64_t CQuicTransport::NextTimeoutMicroseconds() const { return -1; }
void CQuicTransport::LocalAddressChanged() {}

void CQuicTransport::Shutdown()
{
	if(m_pImpl && m_pImpl->m_Handle >= 0)
		BrowserWebTransportClose(m_pImpl->m_Handle, 0, "shutdown");
	m_pImpl.reset();
	m_ConnectFailure = EQuicConnectFailure::NONE;
}

#endif
