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
	// the browser, where all websockets share one scheme.
	int m_WebsocketSecure = -1;
	// Whether the address was given without a scheme.
	bool m_aSchemeless[MAX_SERVER_ADDRESSES] = {};

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

	/**
	 * The addresses the legacy transport connects to: those the socket has a
	 * type for. An address without a scheme is a websocket one where the socket
	 * has websockets but no UDP, as in a browser.
	 *
	 * @param NetTypes The network types of the socket.
	 * @param pAddrs Receives up to `m_NumAddrs` addresses.
	 *
	 * @return The number of addresses.
	 */
	int LegacyAddresses(int NetTypes, NETADDR *pAddrs) const;
};

/**
 * What this client connects over. This is the one rule for which endpoints of
 * a server are used: natively UDP and QUIC, in a browser websockets and
 * WebTransport. Of what is left, the 0.7 endpoints are dropped where a DDNet one
 * remains. A server without an endpoint left is not shown, and an endpoint that
 * was dropped is neither offered next to the address field nor written into it.
 */
class CConnectPlatform
{
public:
	// Websockets and WebTransport instead of UDP and QUIC.
	bool m_Browser = false;
	// Whether QUIC, or WebTransport in a browser, can be used.
	bool m_Modern = false;
	// Whether only secure websockets can be opened, as from a page served over https.
	bool m_SecureWebsocketsOnly = false;

	/**
	 * @return What this client connects over, with QUIC as `cl_quic` allows.
	 */
	static CConnectPlatform ThisClient();
	/**
	 * @return QUIC natively, WebTransport in a browser.
	 */
	EConnectProtocol ModernProtocol() const { return m_Browser ? EConnectProtocol::WEBTRANSPORT : EConnectProtocol::QUIC; }
};

/**
 * One address of a server and the transport it is reached with.
 */
class CServerEndpoint
{
public:
	EConnectProtocol m_Protocol;
	NETADDR m_Address;
};

/**
 * The endpoints of a server that the rule of `CConnectPlatform` leaves, in the
 * order they are listed in.
 */
class CServerEndpoints
{
public:
	CServerEndpoint m_aEndpoints[2 * MAX_SERVER_ADDRESSES];
	int m_NumEndpoints = 0;
	// What the modern endpoints are, for their pin and host name. Null without one.
	const CModernTransportInfo *m_pModern = nullptr;

	CServerEndpoints(const CServerInfo &Info, const CConnectPlatform &Platform);

	/**
	 * @return The first endpoint of the transport in the address family, or null.
	 */
	const CServerEndpoint *Find(EConnectProtocol Protocol, EConnectAddressFamily Family) const;
	bool Has(EConnectProtocol Protocol) const;
	bool Has(EConnectAddressFamily Family) const;
};

/**
 * @return The address family of an address.
 */
EConnectAddressFamily ConnectAddressFamily(const NETADDR &Address);

/**
 * What can be picked next to the address field, the best first. Anything with a
 * single entry is not a choice.
 */
class CConnectChoices
{
public:
	EConnectProtocol m_aProtocols[(int)EConnectProtocol::COUNT] = {EConnectProtocol::LEGACY};
	int m_NumProtocols = 0;
	EConnectAddressFamily m_aFamilies[(int)EConnectAddressFamily::COUNT] = {EConnectAddressFamily::IPV6};
	int m_NumFamilies = 0;
};

/**
 * The transports and address families that can be picked for what is in the
 * address field: those of the endpoints of the server it belongs to, or only
 * what the address itself says for an address no listed server has.
 *
 * @param pServer The server the address belongs to, null for none.
 * @param pAddress The address field.
 * @param Platform What this client connects over.
 */
CConnectChoices ConnectChoicesFor(const CServerInfo *pServer, const char *pAddress, const CConnectPlatform &Platform);

/**
 * @return The transport the address field is connected with.
 */
EConnectProtocol ConnectProtocolOf(const char *pAddress, const CConnectPlatform &Platform);

/**
 * @param pAddress The address field.
 * @param pResult Set to its first address, with the types of its scheme.
 *
 * @return Whether its first address is an IP address.
 */
bool FirstConnectAddress(const char *pAddress, NETADDR *pResult);

/**
 * Writes the one endpoint of a server that is connected to, in the form it is
 * connected with: with its scheme, and for QUIC and WebTransport with its pin.
 * The transport is the one picked if the server has it, otherwise the best it
 * has, and the address family likewise. Where the server has no endpoint for
 * both, the choice that was just made wins and the other one falls back to what
 * there is with it.
 *
 * @param pBuffer The buffer to write to.
 * @param BufferSize The size of the buffer.
 * @param Server The server.
 * @param Platform What this client connects over.
 * @param Protocol The transport picked, -1 for the best there is.
 * @param Family The address family picked.
 * @param FamilyFirst Whether the address family was just picked, and wins over the transport.
 *
 * @return Whether the server has an endpoint this client can use.
 */
bool FormatConnectAddress(char *pBuffer, int BufferSize, const CServerInfo &Server, const CConnectPlatform &Platform, int Protocol, EConnectAddressFamily Family, bool FamilyFirst);

/**
 * @return Whether the first address in the address field is one of the server.
 */
bool ServerHasConnectAddress(const CServerInfo &Server, const char *pAddress);

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
 * The keys of QUIC servers trusted on first use, as the SHA-256 of their DER
 * SubjectPublicKeyInfo.
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
		SHA256_DIGEST m_SpkiSha256;
	};

	/**
	 * @param pHost A normalized host, see `NormalizeQuicTrustHost`.
	 * @param Port The port of the server.
	 *
	 * @return The entry, valid until the known hosts are next changed.
	 */
	const CHost *Find(const char *pHost, int Port) const;
	/**
	 * Remembers a key. Adding one that is already known is fine, adding a
	 * different one for a known host is not.
	 *
	 * @return Whether the host is now known with this key.
	 */
	bool Add(const char *pHost, int Port, const SHA256_DIGEST &SpkiSha256);
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
 * Checks the key a QUIC server holds, which TLS proves on connect, against
 * its pin or the known hosts, and remembers it on first use.
 */
class CQuicIdentityCheck
{
public:
	enum class EResult
	{
		OK,
		// The transport reported no key.
		MISSING,
		// The server holds another key than the known one.
		CHANGED,
		// The key of a new host could not be remembered.
		NOT_STORED,
		// The key of a new host was remembered, the known hosts changed.
		STORED,
	};

	void Reset();
	/**
	 * Sets the check up for a start. A known host turns a TOFU pin into a
	 * pin of its key.
	 */
	void Prepare(CModernTransportStart *pStart, const CQuicKnownHosts &KnownHosts);
	/**
	 * @param pData The SHA-256 of the SubjectPublicKeyInfo of the server.
	 * @param DataSize The size of the hash.
	 * @param pKnownHosts Where the key of a new host is remembered.
	 */
	EResult Check(const void *pData, int DataSize, CQuicKnownHosts *pKnownHosts);

	// Whether a key is expected, which is known after a check stored it.
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
