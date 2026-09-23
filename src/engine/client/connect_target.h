#ifndef ENGINE_CLIENT_CONNECT_TARGET_H
#define ENGINE_CLIENT_CONNECT_TARGET_H

#include <base/hash.h>
#include <base/types.h>

#include <engine/serverbrowser.h>
#include <engine/shared/protocol.h>
#include <engine/shared/transport_pin.h>

#include <functional>
#include <vector>

/**
 * Brings a host name into the form known QUIC hosts are stored under:
 * lower case without a trailing dot, an IPv6 address without brackets.
 *
 * @return Whether it is a valid host name or address without a port.
 */
bool NormalizeQuicTrustHost(const char *pHost, char *pBuffer, int BufferSize);

/**
 * The servers a connect string names, resolved.
 */
class CConnectTarget
{
public:
	NETADDR m_aAddrs[MAX_SERVER_ADDRESSES] = {};
	// The normalized host of each address, as known QUIC hosts are stored.
	char m_aaHosts[MAX_SERVER_ADDRESSES][128] = {};
	int m_NumAddrs = 0;
	bool m_OnlySixup = true;
	// A ddnet+quic:// or ddnet+wt:// link, which names one address and how to
	// check the certificate of the server there.
	bool m_Link = false;
	bool m_LinkWebTransport = false;
	CModernTransportPin m_LinkPin = {};
	// Whether the websocket addresses are secure, -1 without one. Only used in
	// the browser, where every connection is a websocket.
	int m_WebsocketSecure = -1;

	/**
	 * Parses a comma separated connect string and resolves its addresses.
	 * Addresses that cannot be resolved or reached are logged and skipped.
	 *
	 * @param pAddress The connect string.
	 * @param NetTypes The network types the connection can use.
	 * @param Family The address family picked next to the address field.
	 *
	 * @return Whether the connect string is valid. It may still resolve to no address.
	 */
	bool Parse(const char *pAddress, int NetTypes, EConnectAddressFamily Family);
};

/**
 * The settings the transport for a connect is picked by.
 */
class CConnectTransportOptions
{
public:
	// The transport picked next to the address field, -1 for the best there is.
	int m_Protocol = -1;
	// Whether QUIC is offered where a server announces it.
	bool m_Quic = true;
	// A certificate hash QUIC servers must present instead of what they
	// announce, empty for none. Not owned, must outlive the call.
	const char *m_pCertificateSha256 = "";
	// The name certificates are checked for instead of what servers announce,
	// empty for none. Not owned, must outlive the call.
	const char *m_pServerName = "";
};

/**
 * How to start QUIC, or WebTransport in the browser, for a connect.
 */
class CModernTransportStart
{
public:
	NETADDR m_Address = {};
	// The normalized host the address came from, for known QUIC hosts.
	char m_aHost[128] = {};
	// The name the certificate is checked for, empty for an address.
	char m_aServerName[256] = {};
	CModernTransportPin m_Pin = {};
	bool m_Sixup = false;
	bool m_WebTransport = false;
};

enum class EConnectTransport
{
	// Connect with the legacy transport.
	LEGACY,
	// Start the modern transport described by `CModernTransportStart`.
	MODERN,
	// The connect cannot go ahead, the reason is logged.
	FAILED,
};

/**
 * Looks an address up in a server list. Returns `nullptr` for a server that is
 * not listed. The returned info only needs to stay valid until the call it was
 * passed to returns.
 */
typedef std::function<const CServerInfo *(const NETADDR &Addr)> FFindListedServer;

/**
 * Picks the transport a connect goes over: QUIC natively and WebTransport in
 * the browser where the server announces it or it was picked by hand,
 * otherwise the legacy transport.
 *
 * @param Target The parsed connect string, with at least one address.
 * @param Options The settings to pick by.
 * @param FindListedServer Looks the addresses up in the server list, may be empty.
 * @param pStart Set to how to start the modern transport for `EConnectTransport::MODERN`.
 */
EConnectTransport ChooseConnectTransport(const CConnectTarget &Target, const CConnectTransportOptions &Options, const FFindListedServer &FindListedServer, CModernTransportStart *pStart);

/**
 * The QUIC server identities trusted on first use.
 */
class CQuicKnownHosts
{
public:
	static constexpr size_t MAX_HOSTS = 256;

	class CHost
	{
	public:
		char m_aHost[128];
		int m_Port;
		SHA256_DIGEST m_IdentityFingerprint;
	};

	/**
	 * @param pHost A normalized host, see `NormalizeQuicTrustHost`.
	 * @param Port The port of the server.
	 *
	 * @return The entry, valid until the known hosts are next changed.
	 */
	const CHost *Find(const char *pHost, int Port) const;
	/**
	 * Remembers an identity. Adding one that is already known is fine,
	 * adding a different one for a known host is not.
	 *
	 * @return Whether the host is now known with this identity.
	 */
	bool Add(const char *pHost, int Port, const SHA256_DIGEST &IdentityFingerprint);
	/**
	 * Forgets a host, on all ports for port 0.
	 *
	 * @return Whether anything was forgotten.
	 */
	bool Forget(const char *pHost, int Port);
	const std::vector<CHost> &Hosts() const { return m_vHosts; }

private:
	std::vector<CHost> m_vHosts;
};

/**
 * Checks the identity a QUIC server proves on connect against its pin or
 * the known hosts, and remembers it on first use.
 */
class CQuicIdentityCheck
{
public:
	enum class EResult
	{
		OK,
		// The server proved no identity.
		MISSING,
		// The server proved another identity than the known one.
		CHANGED,
		// The identity of a new host could not be remembered.
		NOT_STORED,
		// The identity of a new host was remembered, the known hosts changed.
		STORED,
	};

	void Reset();
	/**
	 * Sets the check up for a start. A known host turns a TOFU pin into an
	 * identity pin.
	 */
	void Prepare(CModernTransportStart *pStart, const CQuicKnownHosts &KnownHosts);
	/**
	 * @param pData The identity fingerprint the server proved.
	 * @param DataSize The size of the fingerprint.
	 * @param pKnownHosts Where the identity of a new host is remembered.
	 */
	EResult Check(const void *pData, int DataSize, CQuicKnownHosts *pKnownHosts);

	// Whether an identity is expected, which is known after a check stored it.
	bool Known() const { return m_Known; }
	const SHA256_DIGEST &Expected() const { return m_Expected; }
	const char *Host() const { return m_aHost; }
	int Port() const { return m_Port; }

private:
	char m_aHost[128] = {};
	int m_Port = 0;
	SHA256_DIGEST m_Expected = {};
	bool m_Required = false;
	bool m_Known = false;
	bool m_Remember = false;
};

#endif
