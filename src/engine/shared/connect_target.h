#ifndef ENGINE_SHARED_CONNECT_TARGET_H
#define ENGINE_SHARED_CONNECT_TARGET_H

#include <base/types.h>

#include <engine/shared/protocol.h>

class CNetClient;

/**
 * Where a connect goes: the addresses of a connect string, looked up, and
 * what its QUIC and WebSocket addresses carry besides.
 */
class CConnectTarget
{
public:
	NETADDR m_aAddrs[MAX_SERVER_ADDRESSES];
	int m_NumAddrs = 0;
	/**
	 * Whether every address speaks 0.7.
	 */
	bool m_Sixup = false;
	/**
	 * The host name the first modern address came as, which a browser
	 * connects by; empty for an IP address.
	 */
	char m_aHost[128] = "";
	/**
	 * The fragment of the modern addresses: the identity they pin, the
	 * certificates a browser takes; empty if none has one.
	 */
	char m_aFragment[256] = "";

	/**
	 * Looks up the addresses of a comma-separated connect string. An
	 * address that cannot be found is logged and left out.
	 *
	 * @param pAddresses The connect string.
	 * @param NetType The address families to look names up in.
	 *
	 * @return Whether there is an address to connect to.
	 */
	bool Parse(const char *pAddresses, int NetType);

	/**
	 * Takes the server another connection reached, the same way and with
	 * the identity the server showed it.
	 *
	 * @param Connection The other connection.
	 * @param Sixup Whether it speaks 0.7.
	 */
	void SameServer(const CNetClient &Connection, bool Sixup);

	/**
	 * Starts connecting to the target.
	 *
	 * @param NetClient The connection to start, it stays the caller's.
	 */
	void Start(CNetClient &NetClient) const;
};

#endif
