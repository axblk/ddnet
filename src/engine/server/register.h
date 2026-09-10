#ifndef ENGINE_SERVER_REGISTER_H
#define ENGINE_SERVER_REGISTER_H

class CConfig;
class IConsole;
class IEngine;
class IHttp;
struct CNetChunk;

class IRegister
{
public:
	virtual ~IRegister() = default;

	virtual void Update() = 0;
	// Call `OnConfigChange` if you change relevant config variables
	// without going through the console.
	virtual void OnConfigChange() = 0;
	// Returns `true` if the packet was a packet related to registering
	// code and doesn't have to processed furtherly.
	virtual bool OnPacket(const CNetChunk *pPacket) = 0;
	// `pInfo` must be an encoded JSON object.
	virtual void OnNewInfo(const char *pInfo) = 0;
	// The fragments clients trust the server by changed: `pIdentityFragment`
	// for the transports pinned by identity, `pWebTransportFragment` for
	// the certificate hashes browsers accept.
	virtual void OnModernTrustChanged(const char *pIdentityFragment, const char *pWebTransportFragment) = 0;
	virtual void OnShutdown() = 0;
};

// Which transports the server actually started; only those are registered.
class CRegisterTransports
{
public:
	bool m_LegacyUdp = false;
	// 0.7 over UDP on top of `m_LegacyUdp`; the library only accepts it
	// when it was built with `src/net/libtw2-patches`.
	bool m_LegacySixupUdp = false;
	bool m_Quic = false;
	bool m_WebTransport = false;
	bool m_Websocket = false;
	// Whether the WebSocket listener shows a certificate browsers trust
	// (`sv_tls_cert`), so the address is `ddnet+wss://`; the self-made
	// certificate is only good for WebTransport, which takes its hash.
	bool m_WebsocketTls = false;
};

IRegister *CreateRegister(CConfig *pConfig, IConsole *pConsole, IEngine *pEngine, IHttp *pHttp, int ServerPort, unsigned SixupSecurityToken, const CRegisterTransports &Transports, const char *pRegisterHostname, const char *pIdentityFragment, const char *pWebTransportFragment);

#endif
