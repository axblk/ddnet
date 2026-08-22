#include "quic_transport.h"

#include "game_wire.h"

#include <base/hash_ctxt.h>
#include <base/mem.h>
#include <base/secure.h>
#include <base/str.h>
#include <base/time.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(CONF_QUIC)
#include <engine/shared/quic.h>
#endif

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#endif

namespace
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	constexpr int WEBTRANSPORT_RESUME_TIMEOUT_SECONDS = 5;

	enum class EBrowserWebTransportEvent
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
		const push = (kind, payload, reason = '') => {
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
				state.mapEvents.push({kind, payload, reason: ''});
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
		stringToUTF8(event.reason || '', pReason, ReasonCapacity);
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
#endif

#if defined(CONF_QUIC)
	bool ReadFile(const char *pPath, size_t MaxSize, std::vector<uint8_t> &vData, char *pError, size_t ErrorSize)
	{
		std::ifstream File(pPath, std::ios::binary | std::ios::ate);
		if(!File)
		{
			str_format(pError, ErrorSize, "could not open '%s'", pPath);
			return false;
		}
		const std::streamsize Size = File.tellg();
		if(Size <= 0 || static_cast<uint64_t>(Size) > MaxSize)
		{
			str_format(pError, ErrorSize, "invalid file size for '%s'", pPath);
			return false;
		}
		vData.resize(static_cast<size_t>(Size));
		File.seekg(0);
		if(!File.read(reinterpret_cast<char *>(vData.data()), Size))
		{
			str_format(pError, ErrorSize, "could not read '%s'", pPath);
			return false;
		}
		return true;
	}

	rust::Slice<const uint8_t> Slice(const std::vector<uint8_t> &vData)
	{
		return {vData.data(), vData.size()};
	}

	struct CPreparedServerIdentity
	{
		SHA256_DIGEST m_CertificateSha256 = {};
		SHA256_DIGEST m_NextCertificateSha256 = {};
		CServerIdentityBinding m_ServerIdentity = {};
		bool m_HasNextCertificate = false;
	};

	bool PrepareServerIdentity(const std::vector<uint8_t> &vCertificate, const std::vector<uint8_t> &vNextCertificate, const char *pIdentityPath, bool RawQuic, CPreparedServerIdentity &Prepared, char *pError, size_t ErrorSize)
	{
		try
		{
			const auto CertificateDer = ModernQuic::quic_leaf_certificate_der(Slice(vCertificate));
			const auto NextCertificateDer = vNextCertificate.empty() ? rust::Vec<uint8_t>{} : ModernQuic::quic_leaf_certificate_der(Slice(vNextCertificate));
			Prepared.m_CertificateSha256 = sha256(CertificateDer.data(), CertificateDer.size());
			Prepared.m_NextCertificateSha256 = NextCertificateDer.empty() ? SHA256_DIGEST{} : sha256(NextCertificateDer.data(), NextCertificateDer.size());
			Prepared.m_HasNextCertificate = !NextCertificateDer.empty() && Prepared.m_NextCertificateSha256 != Prepared.m_CertificateSha256;
			if(!RawQuic)
				return true;
			const auto Identity = ModernQuic::quic_server_identity_binding(
				pIdentityPath,
				{Prepared.m_CertificateSha256.data, SHA256_DIGEST_LENGTH},
				{Prepared.m_NextCertificateSha256.data, Prepared.m_HasNextCertificate ? size_t{SHA256_DIGEST_LENGTH} : size_t{0}});
			if(Identity.public_key.size() != SERVER_IDENTITY_PUBLIC_KEY_SIZE ||
				Identity.certificate_signature.size() != SERVER_IDENTITY_SIGNATURE_SIZE ||
				(Prepared.m_HasNextCertificate && Identity.next_certificate_signature.size() != SERVER_IDENTITY_SIGNATURE_SIZE) ||
				(!Prepared.m_HasNextCertificate && !Identity.next_certificate_signature.empty()))
				throw std::runtime_error("invalid server identity binding size");

			std::copy(Identity.public_key.begin(), Identity.public_key.end(), Prepared.m_ServerIdentity.m_PublicKey.begin());
			std::copy(Identity.certificate_signature.begin(), Identity.certificate_signature.end(), Prepared.m_ServerIdentity.m_CertificateSignature.begin());
			if(!Identity.next_certificate_signature.empty())
			{
				std::copy(Identity.next_certificate_signature.begin(), Identity.next_certificate_signature.end(), Prepared.m_ServerIdentity.m_NextCertificateSignature.begin());
				Prepared.m_ServerIdentity.m_HasNextCertificateSignature = true;
			}
			return true;
		}
		catch(const std::exception &Error)
		{
			str_copy(pError, Error.what(), ErrorSize);
			return false;
		}
	}
#endif
}

class CQuicTransport::CImpl
{
public:
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	int m_WebTransportHandle = -1;
	NETADDR m_PeerAddress = {};
	std::string m_Url;
	std::vector<unsigned char> m_vCertificateHashes;
	std::vector<unsigned char> m_vRawEvent = std::vector<unsigned char>(GameWire::MAX_CONTROL_MESSAGE_SIZE + 16);
	std::vector<unsigned char> m_vEventPayload;
	std::vector<unsigned char> m_vControlBuffer;
	std::vector<unsigned char> m_vPendingDatagram;
	std::vector<unsigned char> m_vResumeBinding;
	std::vector<unsigned char> m_vMapHeaderBuffer;
	uint64_t m_DatagramSequence = 0;
	uint64_t m_LastReceivedDatagramSequence = 0;
	GameWire::CDatagramView m_PendingDatagram = {};
	size_t m_LocalMaxDatagramSize = 0;
	size_t m_PeerMaxDatagramSize = 0;
	uint32_t m_MapStreamId = 0;
	size_t m_MapBytesRemaining = 0;
	SHA256_CTX m_MapSha256Context = {};
	SHA256_DIGEST m_MapExpectedSha256 = {};
	bool m_HasReceivedDatagram = false;
	bool m_HasPendingDatagram = false;
	bool m_WebTransportConnected = false;
	bool m_WebTransportResuming = false;
	bool m_MapHeaderReceived = false;
	bool m_Sixup = false;
	int64_t m_WebTransportResumeDeadline = 0;
	char m_aEventReason[256] = {};
#endif
#if defined(CONF_QUIC)
	rust::Box<ModernQuic::QuicEndpoint> m_Endpoint;
	ModernQuic::QuicEvent m_Event = {};
	ModernQuic::UdpDatagram m_UdpDatagram = {};
	std::unordered_map<uint64_t, NETADDR> m_PeerAddresses;
	std::string m_ManagedIdentityPath;
	int64_t m_ManagedCertificateRotateAt = 0;

	explicit CImpl(rust::Box<ModernQuic::QuicEndpoint> Endpoint) :
		m_Endpoint(std::move(Endpoint))
	{
	}
#endif
};

CQuicTransport::CQuicTransport() = default;
CQuicTransport::~CQuicTransport()
{
	Shutdown();
}

bool CQuicTransport::IsCompiled()
{
#if defined(CONF_QUIC)
	return true;
#else
	return false;
#endif
}

bool CQuicTransport::IsWebTransportCompiled()
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	return true;
#else
	return false;
#endif
}

bool CQuicTransport::StartServer(const char *pLocalAddress, bool RawQuic, bool WebTransport, const char *pCertificatePath, const char *pNextCertificatePath, const char *pPrivateKeyPath, const char *pIdentityPath)
{
	Shutdown();
	m_Metrics = {};
	m_ClientMode = false;
#if defined(CONF_QUIC)
	std::vector<uint8_t> vCertificate;
	std::vector<uint8_t> vNextCertificate;
	std::vector<uint8_t> vPrivateKey;
	int64_t ManagedCertificateRotateAt = 0;
	if((pCertificatePath[0] == '\0') != (pPrivateKeyPath[0] == '\0'))
	{
		str_copy(m_aError, "TLS certificate and private key must either both be set or both be empty");
		return false;
	}
	if(pCertificatePath[0] == '\0')
	{
		try
		{
			auto Identity = ModernQuic::quic_managed_identity(pIdentityPath, time_timestamp());
			vCertificate.assign(Identity.certificate_der.begin(), Identity.certificate_der.end());
			vPrivateKey.assign(Identity.private_key_der.begin(), Identity.private_key_der.end());
			vNextCertificate.assign(Identity.next_certificate_der.begin(), Identity.next_certificate_der.end());
			ManagedCertificateRotateAt = Identity.rotate_at;
		}
		catch(const std::exception &Error)
		{
			str_copy(m_aError, Error.what());
			return false;
		}
	}
	else if(!ReadFile(pCertificatePath, 64 * 1024, vCertificate, m_aError, sizeof(m_aError)) ||
		!ReadFile(pPrivateKeyPath, 16 * 1024, vPrivateKey, m_aError, sizeof(m_aError)))
		return false;
	if(pNextCertificatePath[0] != '\0' && !ReadFile(pNextCertificatePath, 64 * 1024, vNextCertificate, m_aError, sizeof(m_aError)))
		return false;
	CPreparedServerIdentity Prepared;
	if(!PrepareServerIdentity(vCertificate, vNextCertificate, pIdentityPath, RawQuic, Prepared, m_aError, sizeof(m_aError)))
		return false;
	try
	{
		m_pImpl = std::make_unique<CImpl>(ModernQuic::quic_server_start_external(
			pLocalAddress, RawQuic, WebTransport, Slice(vCertificate), Slice(vNextCertificate), Slice(vPrivateKey),
			pIdentityPath, {Prepared.m_ServerIdentity.m_PublicKey.data(), RawQuic ? Prepared.m_ServerIdentity.m_PublicKey.size() : size_t{0}}));
		if(ManagedCertificateRotateAt != 0)
		{
			m_pImpl->m_ManagedIdentityPath = pIdentityPath;
			m_pImpl->m_ManagedCertificateRotateAt = ManagedCertificateRotateAt;
		}
		m_CertificateSha256 = Prepared.m_CertificateSha256;
		m_HasCertificateSha256 = true;
		if(Prepared.m_HasNextCertificate)
		{
			m_NextCertificateSha256 = Prepared.m_NextCertificateSha256;
			m_HasNextCertificateSha256 = true;
		}
		if(RawQuic)
		{
			m_ServerIdentity = Prepared.m_ServerIdentity;
			m_HasServerIdentity = true;
		}
		return true;
	}
	catch(const std::exception &Error)
	{
		str_copy(m_aError, Error.what());
	}
#else
	str_copy(m_aError, "QUIC support is not compiled in");
#endif
	return false;
}

bool CQuicTransport::MaybeRotateManagedCertificate(bool *pRotated)
{
	*pRotated = false;
#if defined(CONF_QUIC)
	if(!m_pImpl || m_pImpl->m_ManagedIdentityPath.empty() || time_timestamp() < m_pImpl->m_ManagedCertificateRotateAt)
		return true;
	try
	{
		auto Identity = ModernQuic::quic_managed_identity(m_pImpl->m_ManagedIdentityPath, time_timestamp());
		std::vector<uint8_t> vCertificate(Identity.certificate_der.begin(), Identity.certificate_der.end());
		std::vector<uint8_t> vNextCertificate(Identity.next_certificate_der.begin(), Identity.next_certificate_der.end());
		std::vector<uint8_t> vPrivateKey(Identity.private_key_der.begin(), Identity.private_key_der.end());
		CPreparedServerIdentity Prepared;
		if(!PrepareServerIdentity(vCertificate, vNextCertificate, m_pImpl->m_ManagedIdentityPath.c_str(), m_HasServerIdentity, Prepared, m_aError, sizeof(m_aError)))
			throw std::runtime_error(m_aError);
		if(m_HasServerIdentity && Prepared.m_ServerIdentity.m_PublicKey != m_ServerIdentity.m_PublicKey)
			throw std::runtime_error("server identity changed while rotating the TLS certificate");
		ModernQuic::quic_server_update_certificate(
			*m_pImpl->m_Endpoint,
			Slice(vCertificate),
			Slice(vPrivateKey),
			{Prepared.m_CertificateSha256.data, SHA256_DIGEST_LENGTH});
		m_CertificateSha256 = Prepared.m_CertificateSha256;
		m_NextCertificateSha256 = Prepared.m_NextCertificateSha256;
		m_HasNextCertificateSha256 = Prepared.m_HasNextCertificate;
		if(m_HasServerIdentity)
			m_ServerIdentity = Prepared.m_ServerIdentity;
		m_pImpl->m_ManagedCertificateRotateAt = Identity.rotate_at;
		*pRotated = true;
		return true;
	}
	catch(const std::exception &Error)
	{
		str_copy(m_aError, Error.what());
		m_pImpl->m_ManagedCertificateRotateAt = time_timestamp() + 60;
		return false;
	}
#else
	return true;
#endif
}

bool CQuicTransport::StartClient(const char *pBindAddress, const char *pServerAddress, const char *pServerName, const char *pCertificatePath, bool Sixup)
{
	Shutdown();
	m_Metrics = {};
	m_Metrics.m_ConnectAttempts = 1;
	m_ConnectStartTime = time_get_impl();
	m_ClientMode = true;
#if defined(CONF_QUIC)
	std::vector<uint8_t> vCertificate;
	if(!ReadFile(pCertificatePath, 64 * 1024, vCertificate, m_aError, sizeof(m_aError)))
		return false;
	try
	{
		m_pImpl = std::make_unique<CImpl>(ModernQuic::quic_client_start_external(pBindAddress, pServerAddress, pServerName, Slice(vCertificate), Sixup));
		return true;
	}
	catch(const std::exception &Error)
	{
		str_copy(m_aError, Error.what());
	}
#else
	(void)Sixup;
	str_copy(m_aError, "QUIC support is not compiled in");
#endif
	return false;
}

bool CQuicTransport::StartClientSha256(const char *pBindAddress, const char *pServerAddress, const char *pServerName, SHA256_DIGEST CertificateSha256, const SHA256_DIGEST *pNextCertificateSha256, bool Sixup)
{
	Shutdown();
	m_Metrics = {};
	m_Metrics.m_ConnectAttempts = 1;
	m_ConnectStartTime = time_get_impl();
	m_ClientMode = true;
#if defined(CONF_QUIC)
	try
	{
		std::array<uint8_t, SHA256_DIGEST_LENGTH * 2> aCertificateSha256;
		std::memcpy(aCertificateSha256.data(), CertificateSha256.data, SHA256_DIGEST_LENGTH);
		if(pNextCertificateSha256)
			std::memcpy(aCertificateSha256.data() + SHA256_DIGEST_LENGTH, pNextCertificateSha256->data, SHA256_DIGEST_LENGTH);
		const rust::Slice<const uint8_t> CertificateSha256Slice(aCertificateSha256.data(), pNextCertificateSha256 ? aCertificateSha256.size() : SHA256_DIGEST_LENGTH);
		m_pImpl = std::make_unique<CImpl>(ModernQuic::quic_client_start_sha256_external(pBindAddress, pServerAddress, pServerName, CertificateSha256Slice, Sixup));
		return true;
	}
	catch(const std::exception &Error)
	{
		str_copy(m_aError, Error.what());
	}
#else
	(void)Sixup;
	str_copy(m_aError, "QUIC support is not compiled in");
#endif
	return false;
}

bool CQuicTransport::StartClientWebPki(const char *pBindAddress, const char *pServerAddress, const char *pServerName, bool Sixup)
{
	Shutdown();
	m_Metrics = {};
	m_Metrics.m_ConnectAttempts = 1;
	m_ConnectStartTime = time_get_impl();
	m_ClientMode = true;
#if defined(CONF_QUIC)
	try
	{
		m_pImpl = std::make_unique<CImpl>(ModernQuic::quic_client_start_webpki_external(pBindAddress, pServerAddress, pServerName, Sixup));
		return true;
	}
	catch(const std::exception &Error)
	{
		str_copy(m_aError, Error.what());
	}
#else
	(void)pBindAddress;
	(void)pServerAddress;
	(void)pServerName;
	(void)Sixup;
	str_copy(m_aError, "QUIC support is not compiled in");
#endif
	return false;
}

bool CQuicTransport::StartClientIdentity(const char *pBindAddress, const char *pServerAddress, const char *pServerName, SHA256_DIGEST IdentityFingerprint, bool Sixup)
{
	Shutdown();
	m_Metrics = {};
	m_Metrics.m_ConnectAttempts = 1;
	m_ConnectStartTime = time_get_impl();
	m_ClientMode = true;
#if defined(CONF_QUIC)
	try
	{
		m_pImpl = std::make_unique<CImpl>(ModernQuic::quic_client_start_identity_external(
			pBindAddress, pServerAddress, pServerName, {IdentityFingerprint.data, SHA256_DIGEST_LENGTH}, Sixup));
		return true;
	}
	catch(const std::exception &Error)
	{
		str_copy(m_aError, Error.what());
	}
#else
	(void)pBindAddress;
	(void)pServerAddress;
	(void)pServerName;
	(void)IdentityFingerprint;
	(void)Sixup;
	str_copy(m_aError, "QUIC support is not compiled in");
#endif
	return false;
}

bool CQuicTransport::StartClientTofu(const char *pBindAddress, const char *pServerAddress, const char *pServerName, bool Sixup)
{
	Shutdown();
	m_Metrics = {};
	m_Metrics.m_ConnectAttempts = 1;
	m_ConnectStartTime = time_get_impl();
	m_ClientMode = true;
#if defined(CONF_QUIC)
	try
	{
		m_pImpl = std::make_unique<CImpl>(ModernQuic::quic_client_start_tofu_external(pBindAddress, pServerAddress, pServerName, Sixup));
		return true;
	}
	catch(const std::exception &Error)
	{
		str_copy(m_aError, Error.what());
	}
#else
	(void)pBindAddress;
	(void)pServerAddress;
	(void)pServerName;
	(void)Sixup;
	str_copy(m_aError, "QUIC support is not compiled in");
#endif
	return false;
}

bool CQuicTransport::StartWebTransportClient(const char *pUrl, const NETADDR *pPeerAddress, bool UseCertificateHashes, SHA256_DIGEST CertificateSha256, const SHA256_DIGEST *pNextCertificateSha256, bool Sixup)
{
	Shutdown();
	m_Metrics = {};
	m_Metrics.m_ConnectAttempts = 1;
	m_ConnectStartTime = time_get_impl();
	m_ClientMode = true;
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	if(!pUrl || pUrl[0] == '\0' || !pPeerAddress)
	{
		str_copy(m_aError, "invalid WebTransport endpoint");
		return false;
	}
	auto pImpl = std::make_unique<CImpl>();
	pImpl->m_Url = pUrl;
	pImpl->m_PeerAddress = *pPeerAddress;
	pImpl->m_Sixup = Sixup;
	if(UseCertificateHashes)
	{
		pImpl->m_vCertificateHashes.insert(pImpl->m_vCertificateHashes.end(), CertificateSha256.data, CertificateSha256.data + SHA256_DIGEST_LENGTH);
		if(pNextCertificateSha256)
			pImpl->m_vCertificateHashes.insert(pImpl->m_vCertificateHashes.end(), pNextCertificateSha256->data, pNextCertificateSha256->data + SHA256_DIGEST_LENGTH);
	}
	pImpl->m_WebTransportHandle = BrowserWebTransportStart(
		pImpl->m_Url.c_str(),
		pImpl->m_vCertificateHashes.empty() ? nullptr : pImpl->m_vCertificateHashes.data(),
		static_cast<int>(pImpl->m_vCertificateHashes.size() / SHA256_DIGEST_LENGTH));
	if(pImpl->m_WebTransportHandle < 0)
	{
		m_ConnectFailure = EQuicConnectFailure::NETWORK;
		m_Metrics.m_ConnectFailuresNetwork++;
		str_copy(m_aError, "WebTransport is unavailable in this browser");
		return false;
	}
	m_pImpl = std::move(pImpl);
	return true;
#else
	(void)pUrl;
	(void)pPeerAddress;
	(void)UseCertificateHashes;
	(void)CertificateSha256;
	(void)pNextCertificateSha256;
	(void)Sixup;
	str_copy(m_aError, "WebTransport support is not compiled in");
	return false;
#endif
}

bool CQuicTransport::IsRunning() const
{
	return m_pImpl != nullptr;
}

bool CQuicTransport::Send(CQuicSessionId Session, const void *pData, int DataSize, bool Vital)
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	if(!m_pImpl || m_pImpl->m_WebTransportHandle < 0 || !Session.IsValid() || DataSize <= 0)
		return false;
	if(!m_pImpl->m_WebTransportConnected)
	{
		if(m_pImpl->m_WebTransportResuming)
		{
			m_Metrics.m_ResumeSendDrops++;
			return true;
		}
		return false;
	}
	std::vector<unsigned char> vPayload;
	bool Queued;
	if(Vital)
	{
		Queued = GameWire::EncodeFrame(
				 GameWire::EFrameType::MESSAGE,
				 {static_cast<const unsigned char *>(pData), static_cast<size_t>(DataSize)},
				 vPayload) &&
			 BrowserWebTransportSendControl(m_pImpl->m_WebTransportHandle, vPayload.data(), static_cast<int>(vPayload.size()));
	}
	else
	{
		const std::vector<GameWire::CByteView> vMessages = {{static_cast<const unsigned char *>(pData), static_cast<size_t>(DataSize)}};
		const size_t MaxDatagramSize = std::min(m_pImpl->m_LocalMaxDatagramSize, m_pImpl->m_PeerMaxDatagramSize);
		Queued = MaxDatagramSize > 0 &&
			 GameWire::EncodeDatagram(m_pImpl->m_DatagramSequence, vMessages, vPayload) &&
			 vPayload.size() <= MaxDatagramSize &&
			 BrowserWebTransportSendDatagram(m_pImpl->m_WebTransportHandle, vPayload.data(), static_cast<int>(vPayload.size()));
		if(Queued)
			m_pImpl->m_DatagramSequence++;
	}
	if(Queued)
	{
		(Vital ? m_Metrics.m_ReliableSent : m_Metrics.m_DatagramsSent)++;
		m_Metrics.m_BytesSent += DataSize;
	}
	else
		(Vital ? m_Metrics.m_ReliableQueueFull : m_Metrics.m_DatagramsDropped)++;
	return Queued;
#elif defined(CONF_QUIC)
	if(!m_pImpl || !Session.IsValid() || DataSize <= 0)
		return false;
	const rust::Slice<const uint8_t> Payload(static_cast<const uint8_t *>(pData), static_cast<size_t>(DataSize));
	const bool Queued = Vital ?
				    ModernQuic::quic_send_control(*m_pImpl->m_Endpoint, Session.Value(), Payload) :
				    ModernQuic::quic_send_datagram(*m_pImpl->m_Endpoint, Session.Value(), Payload);
	m_Metrics.m_CommandQueueHighWater = std::max(m_Metrics.m_CommandQueueHighWater, ModernQuic::quic_command_queue_high_water(*m_pImpl->m_Endpoint));
	if(!Queued && m_ClientMode && !ModernQuic::quic_session_active(*m_pImpl->m_Endpoint, Session.Value()))
	{
		m_Metrics.m_ResumeSendDrops++;
		return true;
	}
	if(Queued)
	{
		(Vital ? m_Metrics.m_ReliableSent : m_Metrics.m_DatagramsSent)++;
		m_Metrics.m_BytesSent += DataSize;
	}
	else
		(Vital ? m_Metrics.m_ReliableQueueFull : m_Metrics.m_DatagramsDropped)++;
	return Queued;
#else
	return false;
#endif
}

bool CQuicTransport::Close(CQuicSessionId Session, const char *pReason)
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	if(!m_pImpl || m_pImpl->m_WebTransportHandle < 0 || !Session.IsValid())
		return false;
	BrowserWebTransportClose(m_pImpl->m_WebTransportHandle, 0, pReason ? pReason : "");
	m_pImpl->m_WebTransportHandle = -1;
	m_pImpl->m_WebTransportConnected = false;
	return true;
#elif defined(CONF_QUIC)
	return m_pImpl && ModernQuic::quic_close_session(*m_pImpl->m_Endpoint, Session.Value(), pReason ? pReason : "");
#else
	return false;
#endif
}

bool CQuicTransport::SetMap(uint32_t MapId, const char *pName, uint32_t Crc, const unsigned char *pSha256, const void *pData, size_t DataSize)
{
#if defined(CONF_QUIC)
	if(!m_pImpl || !pName || !pSha256 || !pData || DataSize == 0)
		return false;
	const rust::Slice<const uint8_t> Name(reinterpret_cast<const uint8_t *>(pName), str_length(pName));
	const rust::Slice<const uint8_t> Sha256(pSha256, 32);
	const rust::Slice<const uint8_t> Data(static_cast<const uint8_t *>(pData), DataSize);
	return ModernQuic::quic_set_map(*m_pImpl->m_Endpoint, MapId, Name, Crc, Sha256, Data);
#else
	return false;
#endif
}

bool CQuicTransport::SendMap(CQuicSessionId Session, uint32_t MapId)
{
#if defined(CONF_QUIC)
	return m_pImpl && Session.IsValid() && ModernQuic::quic_send_map(*m_pImpl->m_Endpoint, Session.Value(), MapId);
#else
	return false;
#endif
}

bool CQuicTransport::IssueResume(CQuicSessionId Session, uint64_t LogicalSessionId, const unsigned char *pToken, size_t TokenSize)
{
#if defined(CONF_QUIC)
	if(!m_pImpl || !Session.IsValid() || !pToken || TokenSize == 0)
		return false;
	const rust::Slice<const uint8_t> Token(pToken, TokenSize);
	return ModernQuic::quic_issue_resume(*m_pImpl->m_Endpoint, Session.Value(), LogicalSessionId, Token);
#else
	return false;
#endif
}

bool CQuicTransport::Reconnect(CQuicSessionId Session)
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	if(!m_pImpl || m_pImpl->m_WebTransportHandle < 0 || !Session.IsValid() || m_pImpl->m_vResumeBinding.empty())
		return false;
	const bool StartResumeWindow = !m_pImpl->m_WebTransportResuming;
	BrowserWebTransportClose(m_pImpl->m_WebTransportHandle, 0, "reconnect");
	m_pImpl->m_WebTransportHandle = BrowserWebTransportStart(
		m_pImpl->m_Url.c_str(),
		m_pImpl->m_vCertificateHashes.empty() ? nullptr : m_pImpl->m_vCertificateHashes.data(),
		static_cast<int>(m_pImpl->m_vCertificateHashes.size() / SHA256_DIGEST_LENGTH));
	if(m_pImpl->m_WebTransportHandle < 0)
		return false;
	m_pImpl->m_WebTransportConnected = false;
	m_pImpl->m_WebTransportResuming = true;
	if(StartResumeWindow)
		m_pImpl->m_WebTransportResumeDeadline = time_get_impl() + WEBTRANSPORT_RESUME_TIMEOUT_SECONDS * time_freq();
	m_pImpl->m_DatagramSequence = 0;
	m_pImpl->m_LastReceivedDatagramSequence = 0;
	m_pImpl->m_HasReceivedDatagram = false;
	m_pImpl->m_HasPendingDatagram = false;
	m_pImpl->m_LocalMaxDatagramSize = 0;
	m_pImpl->m_PeerMaxDatagramSize = 0;
	m_pImpl->m_vControlBuffer.clear();
	m_pImpl->m_MapStreamId = 0;
	m_pImpl->m_MapBytesRemaining = 0;
	m_pImpl->m_MapHeaderReceived = false;
	m_pImpl->m_vMapHeaderBuffer.clear();
	m_Metrics.m_ConnectAttempts++;
	m_ConnectStartTime = time_get_impl();
	return true;
#elif defined(CONF_QUIC)
	const bool Started = m_pImpl && Session.IsValid() && ModernQuic::quic_reconnect(*m_pImpl->m_Endpoint, Session.Value());
	if(Started)
	{
		m_Metrics.m_ConnectAttempts++;
		m_ConnectStartTime = time_get_impl();
	}
	return Started;
#else
	return false;
#endif
}

bool CQuicTransport::Poll(CQuicEvent &Event)
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	if(!m_pImpl || m_pImpl->m_WebTransportHandle < 0)
		return false;
	const CQuicSessionId Session(1);
	auto DisconnectEvent = [&](EQuicConnectFailure Failure, const char *pReason) {
		m_ConnectFailure = Failure;
		if(Failure == EQuicConnectFailure::NETWORK)
			m_Metrics.m_ConnectFailuresNetwork++;
		else if(Failure == EQuicConnectFailure::IDENTITY)
			m_Metrics.m_ConnectFailuresIdentity++;
		else if(Failure == EQuicConnectFailure::PROTOCOL)
			m_Metrics.m_ConnectFailuresProtocol++;
		str_copy(m_pImpl->m_aEventReason, pReason ? pReason : "WebTransport connection closed");
		m_pImpl->m_WebTransportConnected = false;
		m_pImpl->m_WebTransportResuming = false;
		Event = {EQuicEventType::DISCONNECTED, {Session, m_pImpl->m_PeerAddress, true, nullptr, 0}, m_pImpl->m_aEventReason};
		return true;
	};
	auto RetryOrDisconnect = [&](EQuicConnectFailure Failure, const char *pReason) {
		if(m_pImpl->m_WebTransportResuming && time_get_impl() < m_pImpl->m_WebTransportResumeDeadline && Reconnect(Session))
			return false;
		m_Metrics.m_Disconnections++;
		return DisconnectEvent(Failure, pReason);
	};
	auto MapFailedEvent = [&](const char *pReason) {
		m_Metrics.m_MapTransfersFailed++;
		str_copy(m_pImpl->m_aEventReason, pReason);
		Event = {EQuicEventType::MAP_FAILED, {Session, m_pImpl->m_PeerAddress, true, nullptr, 0}, m_pImpl->m_aEventReason};
		return true;
	};
	auto PollControlFrame = [&]() {
		while(!m_pImpl->m_vControlBuffer.empty())
		{
			GameWire::CFrameView Frame;
			const GameWire::EDecodeResult Result = GameWire::DecodeFrame(m_pImpl->m_vControlBuffer.data(), m_pImpl->m_vControlBuffer.size(), Frame);
			if(Result == GameWire::EDecodeResult::NEED_MORE)
				return false;
			if(Result != GameWire::EDecodeResult::OK)
				return DisconnectEvent(EQuicConnectFailure::PROTOCOL, "invalid WebTransport control frame");
			m_pImpl->m_vEventPayload.assign(Frame.m_Payload.m_pData, Frame.m_Payload.m_pData + Frame.m_Payload.m_Size);
			m_pImpl->m_vControlBuffer.erase(m_pImpl->m_vControlBuffer.begin(), m_pImpl->m_vControlBuffer.begin() + Frame.m_BytesConsumed);
			if(!m_pImpl->m_WebTransportConnected)
			{
				GameWire::CHelloView Hello;
				if(Frame.m_Type != static_cast<uint64_t>(GameWire::EFrameType::SERVER_HELLO) ||
					GameWire::DecodeHello({m_pImpl->m_vEventPayload.data(), m_pImpl->m_vEventPayload.size()}, Hello) != GameWire::EDecodeResult::OK ||
					Hello.m_ProtocolVersion != GameWire::VERSION_MAJOR ||
					(Hello.m_Capabilities & (GameWire::CAPABILITY_DATAGRAM | GameWire::CAPABILITY_MAP_STREAM | GameWire::CAPABILITY_RESUME)) != (GameWire::CAPABILITY_DATAGRAM | GameWire::CAPABILITY_MAP_STREAM | GameWire::CAPABILITY_RESUME) ||
					((Hello.m_Capabilities & GameWire::CAPABILITY_GAME_PROTOCOL_7) != 0) != m_pImpl->m_Sixup ||
					Hello.m_MaxDatagramSize == 0)
					return DisconnectEvent(EQuicConnectFailure::PROTOCOL, "invalid WebTransport ServerHello");
				m_pImpl->m_PeerMaxDatagramSize = std::min(static_cast<size_t>(Hello.m_MaxDatagramSize), GameWire::MAX_DATAGRAM_SIZE);
				m_pImpl->m_WebTransportConnected = true;
				m_Metrics.m_LastHandshakeMilliseconds = m_ConnectStartTime == 0 ? 0 : (time_get_impl() - m_ConnectStartTime) * 1000 / time_freq();
				m_ConnectFailure = EQuicConnectFailure::NONE;
				if(m_pImpl->m_WebTransportResuming)
				{
					m_pImpl->m_WebTransportResuming = false;
					m_Metrics.m_PathChanges++;
					return false;
				}
				m_Metrics.m_Connections++;
				Event = {EQuicEventType::CONNECTED, {Session, m_pImpl->m_PeerAddress, true, nullptr, 0}, nullptr};
				Event.m_Sixup = m_pImpl->m_Sixup;
				Event.m_WebTransport = true;
				return true;
			}
			if(Frame.m_Type == static_cast<uint64_t>(GameWire::EFrameType::MESSAGE))
			{
				m_Metrics.m_ReliableReceived++;
				m_Metrics.m_BytesReceived += m_pImpl->m_vEventPayload.size();
				Event = {EQuicEventType::MESSAGE, {Session, m_pImpl->m_PeerAddress, true, m_pImpl->m_vEventPayload.data(), static_cast<int>(m_pImpl->m_vEventPayload.size())}, nullptr};
				return true;
			}
			if(Frame.m_Type == static_cast<uint64_t>(GameWire::EFrameType::RESUME))
			{
				GameWire::CResumeView Resume;
				if(GameWire::DecodeResume({m_pImpl->m_vEventPayload.data(), m_pImpl->m_vEventPayload.size()}, Resume) != GameWire::EDecodeResult::OK)
					return DisconnectEvent(EQuicConnectFailure::PROTOCOL, "invalid WebTransport resume binding");
				m_pImpl->m_vResumeBinding = m_pImpl->m_vEventPayload;
				continue;
			}
			if(Frame.m_Type == static_cast<uint64_t>(GameWire::EFrameType::DISCONNECT))
			{
				char aReason[256];
				if(m_pImpl->m_vEventPayload.size() >= sizeof(aReason))
					return DisconnectEvent(EQuicConnectFailure::PROTOCOL, "invalid WebTransport disconnect reason");
				mem_copy(aReason, m_pImpl->m_vEventPayload.data(), m_pImpl->m_vEventPayload.size());
				aReason[m_pImpl->m_vEventPayload.size()] = '\0';
				if(!str_utf8_check(aReason))
					return DisconnectEvent(EQuicConnectFailure::PROTOCOL, "invalid WebTransport disconnect reason");
				return DisconnectEvent(EQuicConnectFailure::NONE, aReason[0] ? aReason : "server disconnected");
			}
			if(!Frame.m_Skippable)
				return DisconnectEvent(EQuicConnectFailure::PROTOCOL, "unexpected WebTransport control frame");
		}
		return false;
	};
	auto PollDatagramMessage = [&]() {
		if(!m_pImpl->m_HasPendingDatagram)
			return false;
		GameWire::CByteView Message;
		if(!GameWire::NextDatagramMessage(m_pImpl->m_PendingDatagram, Message))
		{
			m_pImpl->m_HasPendingDatagram = false;
			return false;
		}
		m_pImpl->m_vEventPayload.assign(Message.m_pData, Message.m_pData + Message.m_Size);
		if(m_pImpl->m_PendingDatagram.m_MessagesRemaining == 0)
			m_pImpl->m_HasPendingDatagram = false;
		m_Metrics.m_DatagramsReceived++;
		m_Metrics.m_BytesReceived += m_pImpl->m_vEventPayload.size();
		Event = {EQuicEventType::MESSAGE, {Session, m_pImpl->m_PeerAddress, false, m_pImpl->m_vEventPayload.data(), static_cast<int>(m_pImpl->m_vEventPayload.size())}, nullptr};
		return true;
	};
	auto PollBufferedMapData = [&]() {
		if(!m_pImpl->m_MapHeaderReceived || m_pImpl->m_vMapHeaderBuffer.empty())
			return false;
		if(m_pImpl->m_vMapHeaderBuffer.size() > m_pImpl->m_MapBytesRemaining)
			return MapFailedEvent("WebTransport map stream exceeds declared size");
		m_pImpl->m_vEventPayload = std::move(m_pImpl->m_vMapHeaderBuffer);
		m_pImpl->m_vMapHeaderBuffer.clear();
		m_pImpl->m_MapBytesRemaining -= m_pImpl->m_vEventPayload.size();
		sha256_update(&m_pImpl->m_MapSha256Context, m_pImpl->m_vEventPayload.data(), m_pImpl->m_vEventPayload.size());
		m_Metrics.m_MapBytesReceived += m_pImpl->m_vEventPayload.size();
		Event = {EQuicEventType::MAP_DATA, {Session, m_pImpl->m_PeerAddress, true, m_pImpl->m_vEventPayload.data(), static_cast<int>(m_pImpl->m_vEventPayload.size())}, nullptr};
		return true;
	};
	if(PollControlFrame() || PollDatagramMessage() || PollBufferedMapData())
		return true;
	if(m_pImpl->m_WebTransportResuming && time_get_impl() >= m_pImpl->m_WebTransportResumeDeadline)
		return RetryOrDisconnect(EQuicConnectFailure::NETWORK, "WebTransport resume timed out");

	int PayloadSize = 0;
	m_pImpl->m_aEventReason[0] = '\0';
	const EBrowserWebTransportEvent BrowserEvent = static_cast<EBrowserWebTransportEvent>(BrowserWebTransportPoll(
		m_pImpl->m_WebTransportHandle,
		m_pImpl->m_vRawEvent.data(),
		static_cast<int>(m_pImpl->m_vRawEvent.size()),
		&PayloadSize,
		m_pImpl->m_aEventReason,
		sizeof(m_pImpl->m_aEventReason)));
	if(BrowserEvent == EBrowserWebTransportEvent::NONE)
		return false;
	if(PayloadSize < 0 || static_cast<size_t>(PayloadSize) > m_pImpl->m_vRawEvent.size())
		return DisconnectEvent(EQuicConnectFailure::PROTOCOL, "invalid WebTransport event size");
	const unsigned char *pPayload = m_pImpl->m_vRawEvent.data();
	if(BrowserEvent == EBrowserWebTransportEvent::READY)
	{
		if(PayloadSize != 4)
			return DisconnectEvent(EQuicConnectFailure::PROTOCOL, "invalid WebTransport ready event");
		const uint32_t MaxDatagramSize = static_cast<uint32_t>(pPayload[0]) | (static_cast<uint32_t>(pPayload[1]) << 8) | (static_cast<uint32_t>(pPayload[2]) << 16) | (static_cast<uint32_t>(pPayload[3]) << 24);
		if(MaxDatagramSize == 0 || MaxDatagramSize > GameWire::MAX_DATAGRAM_SIZE)
			return DisconnectEvent(EQuicConnectFailure::PROTOCOL, "invalid WebTransport datagram size");
		m_pImpl->m_LocalMaxDatagramSize = MaxDatagramSize;
		GameWire::CHelloView Hello = {};
		Hello.m_Major = GameWire::VERSION_MAJOR;
		Hello.m_Minor = GameWire::VERSION_MINOR;
		Hello.m_ProtocolVersion = GameWire::VERSION_MAJOR;
		Hello.m_Capabilities = GameWire::CAPABILITY_DATAGRAM | GameWire::CAPABILITY_MAP_STREAM | GameWire::CAPABILITY_RESUME |
				       (m_pImpl->m_Sixup ? GameWire::CAPABILITY_GAME_PROTOCOL_7 : 0);
		Hello.m_MaxDatagramSize = MaxDatagramSize;
		secure_random_fill(Hello.m_aNonce, sizeof(Hello.m_aNonce));
		Hello.m_ResumeToken = {m_pImpl->m_vResumeBinding.data(), m_pImpl->m_vResumeBinding.size()};
		std::vector<unsigned char> vHello;
		std::vector<unsigned char> vControl;
		if(!GameWire::EncodeHello(Hello, vHello) ||
			!GameWire::EncodeStreamHeader(GameWire::EStreamKind::CONTROL, vControl) ||
			!GameWire::EncodeFrame(GameWire::EFrameType::CLIENT_HELLO, {vHello.data(), vHello.size()}, vControl) ||
			!BrowserWebTransportSendControl(m_pImpl->m_WebTransportHandle, vControl.data(), static_cast<int>(vControl.size())))
			return DisconnectEvent(EQuicConnectFailure::PROTOCOL, "could not send WebTransport ClientHello");
		return false;
	}
	if(BrowserEvent == EBrowserWebTransportEvent::CONTROL_DATA)
	{
		if(m_pImpl->m_vControlBuffer.size() + PayloadSize > GameWire::MAX_CONTROL_MESSAGE_SIZE + 32768 + 16)
			return DisconnectEvent(EQuicConnectFailure::PROTOCOL, "WebTransport control buffer exceeded limit");
		m_pImpl->m_vControlBuffer.insert(m_pImpl->m_vControlBuffer.end(), pPayload, pPayload + PayloadSize);
		return PollControlFrame();
	}
	if(BrowserEvent == EBrowserWebTransportEvent::DATAGRAM)
	{
		m_pImpl->m_vPendingDatagram.assign(pPayload, pPayload + PayloadSize);
		GameWire::CDatagramView Datagram;
		if(GameWire::DecodeDatagram(m_pImpl->m_vPendingDatagram.data(), m_pImpl->m_vPendingDatagram.size(), Datagram) != GameWire::EDecodeResult::OK)
			return DisconnectEvent(EQuicConnectFailure::PROTOCOL, "invalid WebTransport datagram");
		if(m_pImpl->m_HasReceivedDatagram && Datagram.m_Sequence <= m_pImpl->m_LastReceivedDatagramSequence)
			return false;
		m_pImpl->m_HasReceivedDatagram = true;
		m_pImpl->m_LastReceivedDatagramSequence = Datagram.m_Sequence;
		m_pImpl->m_PendingDatagram = Datagram;
		m_pImpl->m_HasPendingDatagram = true;
		return PollDatagramMessage();
	}
	auto ReadStreamId = [&]() {
		return PayloadSize >= 4 ? static_cast<uint32_t>(pPayload[0]) | (static_cast<uint32_t>(pPayload[1]) << 8) | (static_cast<uint32_t>(pPayload[2]) << 16) | (static_cast<uint32_t>(pPayload[3]) << 24) : 0;
	};
	if(BrowserEvent == EBrowserWebTransportEvent::UNI_START)
	{
		const uint32_t StreamId = ReadStreamId();
		if(PayloadSize != 4 || StreamId == 0 || m_pImpl->m_MapStreamId != 0)
			return MapFailedEvent("invalid WebTransport map stream");
		m_pImpl->m_MapStreamId = StreamId;
		m_pImpl->m_MapBytesRemaining = 0;
		m_pImpl->m_MapHeaderReceived = false;
		m_pImpl->m_vMapHeaderBuffer.clear();
		return false;
	}
	if(BrowserEvent == EBrowserWebTransportEvent::UNI_DATA)
	{
		const uint32_t StreamId = ReadStreamId();
		if(PayloadSize < 4 || StreamId == 0 || StreamId != m_pImpl->m_MapStreamId)
			return MapFailedEvent("unexpected WebTransport map stream data");
		const unsigned char *pData = pPayload + 4;
		const size_t DataSize = PayloadSize - 4;
		if(DataSize == 0)
			return false;
		if(m_pImpl->m_MapHeaderReceived)
		{
			if(DataSize > m_pImpl->m_MapBytesRemaining)
				return MapFailedEvent("WebTransport map stream exceeds declared size");
			m_pImpl->m_vEventPayload.assign(pData, pData + DataSize);
			m_pImpl->m_MapBytesRemaining -= DataSize;
			sha256_update(&m_pImpl->m_MapSha256Context, pData, DataSize);
			m_Metrics.m_MapBytesReceived += DataSize;
			Event = {EQuicEventType::MAP_DATA, {Session, m_pImpl->m_PeerAddress, true, m_pImpl->m_vEventPayload.data(), static_cast<int>(DataSize)}, nullptr};
			return true;
		}
		if(m_pImpl->m_vMapHeaderBuffer.size() + DataSize > GameWire::MAX_MAP_HEADER_SIZE + 32768 + 16)
			return MapFailedEvent("WebTransport map header exceeded limit");
		m_pImpl->m_vMapHeaderBuffer.insert(m_pImpl->m_vMapHeaderBuffer.end(), pData, pData + DataSize);
		GameWire::CStreamHeader StreamHeader;
		GameWire::EDecodeResult Result = GameWire::DecodeStreamHeader(m_pImpl->m_vMapHeaderBuffer.data(), m_pImpl->m_vMapHeaderBuffer.size(), StreamHeader);
		if(Result == GameWire::EDecodeResult::NEED_MORE)
			return false;
		if(Result != GameWire::EDecodeResult::OK || StreamHeader.m_Kind != GameWire::EStreamKind::MAP)
			return MapFailedEvent("unsupported WebTransport map stream");
		GameWire::CFrameView Frame;
		Result = GameWire::DecodeFrame(m_pImpl->m_vMapHeaderBuffer.data() + StreamHeader.m_BytesConsumed, m_pImpl->m_vMapHeaderBuffer.size() - StreamHeader.m_BytesConsumed, Frame);
		if(Result == GameWire::EDecodeResult::NEED_MORE)
			return false;
		if(Result != GameWire::EDecodeResult::OK || Frame.m_Type != static_cast<uint64_t>(GameWire::EFrameType::MAP_HEADER))
			return MapFailedEvent("invalid WebTransport map header frame");
		GameWire::CMapHeaderView Header;
		if(GameWire::DecodeMapHeader(Frame.m_Payload, Header) != GameWire::EDecodeResult::OK)
			return MapFailedEvent("invalid WebTransport map header");
		m_pImpl->m_MapBytesRemaining = static_cast<size_t>(Header.m_Size);
		std::memcpy(m_pImpl->m_MapExpectedSha256.data, Header.m_aSha256, sizeof(m_pImpl->m_MapExpectedSha256.data));
		sha256_init(&m_pImpl->m_MapSha256Context);
		m_pImpl->m_MapHeaderReceived = true;
		m_pImpl->m_vEventPayload.assign(Frame.m_Payload.m_pData, Frame.m_Payload.m_pData + Frame.m_Payload.m_Size);
		const size_t Consumed = StreamHeader.m_BytesConsumed + Frame.m_BytesConsumed;
		m_pImpl->m_vMapHeaderBuffer.erase(m_pImpl->m_vMapHeaderBuffer.begin(), m_pImpl->m_vMapHeaderBuffer.begin() + Consumed);
		m_Metrics.m_MapTransfersReceived++;
		Event = {EQuicEventType::MAP_HEADER, {Session, m_pImpl->m_PeerAddress, true, m_pImpl->m_vEventPayload.data(), static_cast<int>(m_pImpl->m_vEventPayload.size())}, nullptr};
		return true;
	}
	if(BrowserEvent == EBrowserWebTransportEvent::UNI_END)
	{
		const uint32_t StreamId = ReadStreamId();
		if(PayloadSize != 4 || StreamId != m_pImpl->m_MapStreamId || !m_pImpl->m_MapHeaderReceived || m_pImpl->m_MapBytesRemaining != 0)
			return MapFailedEvent("WebTransport map stream ended early");
		if(sha256_finish(&m_pImpl->m_MapSha256Context) != m_pImpl->m_MapExpectedSha256)
			return MapFailedEvent("WebTransport map SHA-256 mismatch");
		m_pImpl->m_MapStreamId = 0;
		m_pImpl->m_MapHeaderReceived = false;
		Event = {EQuicEventType::MAP_END, {Session, m_pImpl->m_PeerAddress, true, nullptr, 0}, nullptr};
		return true;
	}
	if(BrowserEvent == EBrowserWebTransportEvent::FAILED_UNAVAILABLE)
		return RetryOrDisconnect(EQuicConnectFailure::NETWORK, m_pImpl->m_aEventReason);
	if(BrowserEvent == EBrowserWebTransportEvent::FAILED_IDENTITY)
		return RetryOrDisconnect(EQuicConnectFailure::IDENTITY, m_pImpl->m_aEventReason);
	if(BrowserEvent == EBrowserWebTransportEvent::FAILED_PROTOCOL)
		return RetryOrDisconnect(EQuicConnectFailure::PROTOCOL, m_pImpl->m_aEventReason);
	if(BrowserEvent == EBrowserWebTransportEvent::CLOSED)
	{
		const bool WasConnected = m_pImpl->m_WebTransportConnected;
		if(WasConnected && !m_pImpl->m_vResumeBinding.empty() && Reconnect(Session))
			return false;
		return RetryOrDisconnect(WasConnected || m_pImpl->m_WebTransportResuming ? EQuicConnectFailure::NETWORK : EQuicConnectFailure::PROTOCOL, m_pImpl->m_aEventReason);
	}
	return false;
#elif defined(CONF_QUIC)
	while(m_pImpl && ModernQuic::quic_poll_event(*m_pImpl->m_Endpoint, m_pImpl->m_Event))
	{
		const CQuicSessionId Session(m_pImpl->m_Event.session_id);
		if(m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::Rebound)
		{
			m_Metrics.m_PathChanges++;
			continue;
		}
		if(m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::PeerMigrated)
		{
			NETADDR PeerAddress = {};
			const std::string Detail(m_pImpl->m_Event.detail);
			if(net_addr_from_str(&PeerAddress, Detail.c_str()) != 0)
				continue;
			m_pImpl->m_PeerAddresses[Session.Value()] = PeerAddress;
			m_Metrics.m_PathChanges++;
			Event = {EQuicEventType::PEER_MIGRATED, {Session, PeerAddress, true, nullptr, 0}, nullptr};
			return true;
		}
		NETADDR PeerAddress = {};
		if(m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::Connected)
		{
			const std::string Detail(m_pImpl->m_Event.detail);
			if(net_addr_from_str(&PeerAddress, Detail.c_str()) != 0)
				continue;
			m_pImpl->m_PeerAddresses[Session.Value()] = PeerAddress;
			m_Metrics.m_Connections++;
			m_Metrics.m_LastHandshakeMilliseconds = m_ConnectStartTime == 0 ? 0 : (time_get_impl() - m_ConnectStartTime) * 1000 / time_freq();
			Event = {EQuicEventType::CONNECTED, {Session, PeerAddress, true, m_pImpl->m_Event.payload.data(), static_cast<int>(m_pImpl->m_Event.payload.size())}, nullptr};
			Event.m_Sixup = m_pImpl->m_Event.sixup;
			Event.m_WebTransport = m_pImpl->m_Event.webtransport;
			return true;
		}
		if(m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::MasterChallenge)
		{
			Event = {EQuicEventType::MASTER_CHALLENGE, {Session, PeerAddress, true, m_pImpl->m_Event.payload.data(), static_cast<int>(m_pImpl->m_Event.payload.size())}, nullptr};
			Event.m_WebTransport = m_pImpl->m_Event.webtransport;
			return true;
		}
		if(const auto It = m_pImpl->m_PeerAddresses.find(Session.Value()); It != m_pImpl->m_PeerAddresses.end())
			PeerAddress = It->second;
		if(m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::Control || m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::Datagram)
		{
			const bool Reliable = m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::Control;
			(Reliable ? m_Metrics.m_ReliableReceived : m_Metrics.m_DatagramsReceived)++;
			m_Metrics.m_BytesReceived += m_pImpl->m_Event.payload.size();
			Event = {EQuicEventType::MESSAGE, {Session, PeerAddress, Reliable, m_pImpl->m_Event.payload.data(), static_cast<int>(m_pImpl->m_Event.payload.size())}, nullptr};
			return true;
		}
		if(m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::MapHeader || m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::MapData)
		{
			const CQuicMessage Message = {Session, PeerAddress, true, m_pImpl->m_Event.payload.data(), static_cast<int>(m_pImpl->m_Event.payload.size())};
			if(m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::MapHeader)
			{
				m_Metrics.m_MapTransfersReceived++;
				Event = {EQuicEventType::MAP_HEADER, Message, nullptr};
			}
			else
			{
				m_Metrics.m_MapBytesReceived += m_pImpl->m_Event.payload.size();
				Event = {EQuicEventType::MAP_DATA, Message, nullptr};
			}
			return true;
		}
		if(m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::MapEnd)
		{
			Event = {EQuicEventType::MAP_END, {Session, PeerAddress, true, nullptr, 0}, nullptr};
			return true;
		}
		if(m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::MapFailed)
		{
			m_Metrics.m_MapTransfersFailed++;
			Event = {EQuicEventType::MAP_FAILED, {Session, PeerAddress, true, nullptr, 0}, m_pImpl->m_Event.detail.c_str()};
			return true;
		}
		if(m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::Disconnected)
		{
			m_pImpl->m_PeerAddresses.erase(Session.Value());
			m_Metrics.m_Disconnections++;
			Event = {EQuicEventType::DISCONNECTED, {Session, PeerAddress, true, nullptr, 0}, m_pImpl->m_Event.detail.c_str()};
			return true;
		}
		if(m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::ConnectFailedNetwork ||
			m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::ConnectFailedIdentity ||
			m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::ConnectFailedProtocol)
		{
			if(m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::ConnectFailedNetwork)
			{
				m_ConnectFailure = EQuicConnectFailure::NETWORK;
				m_Metrics.m_ConnectFailuresNetwork++;
			}
			else if(m_pImpl->m_Event.kind == ModernQuic::QuicEventKind::ConnectFailedIdentity)
			{
				m_ConnectFailure = EQuicConnectFailure::IDENTITY;
				m_Metrics.m_ConnectFailuresIdentity++;
			}
			else
			{
				m_ConnectFailure = EQuicConnectFailure::PROTOCOL;
				m_Metrics.m_ConnectFailuresProtocol++;
			}
			Event = {EQuicEventType::DISCONNECTED, {Session, PeerAddress, true, nullptr, 0}, m_pImpl->m_Event.detail.c_str()};
			return true;
		}
	}
#endif
	return false;
}

int CQuicTransport::PollUdpSend(NETADDR *pAddress, unsigned char **ppData)
{
#if defined(CONF_QUIC)
	if(!m_pImpl || !ModernQuic::quic_udp_poll_transmit(*m_pImpl->m_Endpoint, m_pImpl->m_UdpDatagram))
		return 0;
	const auto &SourceIp = m_pImpl->m_UdpDatagram.source_ip;
	const size_t IpSize = m_pImpl->m_UdpDatagram.source_is_ipv6 ? 16 : 4;
	if(SourceIp.size() != IpSize)
		return 0;
	*pAddress = {};
	pAddress->type = m_pImpl->m_UdpDatagram.source_is_ipv6 ? NETTYPE_IPV6 : NETTYPE_IPV4;
	std::memcpy(pAddress->ip, SourceIp.data(), IpSize);
	pAddress->port = m_pImpl->m_UdpDatagram.source_port;
	*ppData = m_pImpl->m_UdpDatagram.payload.data();
	return static_cast<int>(m_pImpl->m_UdpDatagram.payload.size());
#else
	return 0;
#endif
}

bool CQuicTransport::FeedUdp(const NETADDR *pAddress, const void *pData, int DataSize)
{
#if defined(CONF_QUIC)
	if(!m_pImpl || DataSize <= 0)
		return false;
	const bool Ipv6 = (pAddress->type & NETTYPE_IPV6) != 0;
	const rust::Slice<const uint8_t> Ip(pAddress->ip, Ipv6 ? 16 : 4);
	const rust::Slice<const uint8_t> Payload(static_cast<const uint8_t *>(pData), static_cast<size_t>(DataSize));
	return ModernQuic::quic_udp_feed(*m_pImpl->m_Endpoint, Ip, pAddress->port, Ipv6, Payload);
#else
	return false;
#endif
}

bool CQuicTransport::SetLegacyPeer(const NETADDR *pAddress, bool Known)
{
#if defined(CONF_QUIC)
	if(!m_pImpl)
		return false;
	const bool Ipv6 = (pAddress->type & NETTYPE_IPV6) != 0;
	const rust::Slice<const uint8_t> Ip(pAddress->ip, Ipv6 ? 16 : 4);
	return ModernQuic::quic_udp_set_legacy_peer(*m_pImpl->m_Endpoint, Ip, pAddress->port, Ipv6, Known);
#else
	return false;
#endif
}

uint64_t CQuicTransport::RawDropCount() const
{
#if defined(CONF_QUIC)
	return m_pImpl ? ModernQuic::quic_udp_drop_count(*m_pImpl->m_Endpoint) : 0;
#else
	return 0;
#endif
}

int64_t CQuicTransport::NextTimeoutMicroseconds() const
{
#if defined(CONF_QUIC)
	return m_pImpl ? ModernQuic::quic_next_timeout_microseconds(*m_pImpl->m_Endpoint) : -1;
#else
	return -1;
#endif
}

void CQuicTransport::LocalAddressChanged()
{
#if defined(CONF_QUIC)
	if(m_pImpl)
		ModernQuic::quic_local_address_changed(*m_pImpl->m_Endpoint);
#endif
}

void CQuicTransport::Shutdown()
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	if(m_pImpl && m_pImpl->m_WebTransportHandle >= 0)
		BrowserWebTransportClose(m_pImpl->m_WebTransportHandle, 0, "shutdown");
#elif defined(CONF_QUIC)
	if(m_pImpl)
		ModernQuic::quic_shutdown(*m_pImpl->m_Endpoint);
#endif
	m_pImpl.reset();
	m_ConnectFailure = EQuicConnectFailure::NONE;
	m_ConnectStartTime = 0;
	m_CertificateSha256 = {};
	m_NextCertificateSha256 = {};
	m_ServerIdentity = {};
	m_HasCertificateSha256 = false;
	m_HasNextCertificateSha256 = false;
	m_HasServerIdentity = false;
}
