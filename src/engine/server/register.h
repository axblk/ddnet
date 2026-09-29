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
	virtual void OnModernTrustChanged(const char *pQuicFragment, const char *pWebTransportFragment) = 0;
	// Whether 0.7 clients can play the current map. The server is only
	// registered for them while they can and `sv_sixup` lets them.
	virtual void OnSixupMapChange(bool Available) = 0;
	virtual void OnShutdown() = 0;
};

IRegister *CreateRegister(CConfig *pConfig, IConsole *pConsole, IEngine *pEngine, IHttp *pHttp, int ServerPort, unsigned SixupSecurityToken, bool LegacyUdpStarted, bool QuicStarted, bool WebTransportStarted, const char *pRegisterHostname, const char *pQuicFragment, const char *pWebTransportFragment);

#endif
