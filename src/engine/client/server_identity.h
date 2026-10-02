#ifndef ENGINE_CLIENT_SERVER_IDENTITY_H
#define ENGINE_CLIENT_SERVER_IDENTITY_H

#include <base/hash.h>
#include <base/types.h>

#include <engine/shared/transport_pin.h>

#include <vector>

/**
 * Brings a host name into the form known QUIC hosts are stored under:
 * lower case without a trailing dot, an IPv6 address without brackets.
 *
 * @return Whether it is a valid host name or address without a port.
 */
bool NormalizeQuicTrustHost(const char *pHost, char *pBuffer, int BufferSize);

/**
 * Where what the identity of a server is checked against came from.
 */
enum class EServerIdentitySource
{
	// What an address without a fragment means.
	ADDRESS,
	LIST,
	LINK,
	// The key of a known host, trusted on first use.
	REMEMBERED,
};

/**
 * How to start QUIC, WebTransport or a secure websocket for a connect, as far
 * as the identity of the server goes.
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
	EServerIdentitySource m_PinSource = EServerIdentitySource::ADDRESS;
	bool m_Sixup = false;
	bool m_WebTransport = false;
};

/**
 * Sets a start up for one address of a connect.
 *
 * @param pStart The start to set.
 * @param Addr The address, its scheme's flags tell the transport.
 * @param pHost The host name the address came as, empty for an IP address.
 * @param pFragment The fragment of the address, without the `#`.
 * @param Listed Whether the server list gave the fragment.
 *
 * @return Whether the transport shows the server's identity: QUIC,
 * WebTransport and secure websockets. Also false for a fragment that cannot
 * be read.
 */
bool SetModernTransportStart(CModernTransportStart *pStart, const NETADDR &Addr, const char *pHost, const char *pFragment, bool Listed);

/**
 * @return "QUIC", "WebTransport" or "wss", as the warning names the transport.
 */
const char *ModernTransportName(const CModernTransportStart &Start);

/**
 * The keys of QUIC servers trusted on first use, as the SHA-256 of their DER
 * SubjectPublicKeyInfo. Native secure websockets share them.
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
	 * Brings the key of a known host up to date with one that was proven by
	 * other means, such as a pin. An unknown host stays unknown.
	 *
	 * @return Whether the key of a known host changed.
	 */
	bool Update(const char *pHost, int Port, const SHA256_DIGEST &SpkiSha256);
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
 * Checks the key a QUIC or native secure websocket server holds, which TLS
 * proves on connect, against its pin or the known hosts, remembers it on first
 * use and brings a remembered one up to date with a pin.
 */
class CQuicIdentityCheck
{
public:
	enum class EResult
	{
		OK,
		// The transport reported no key.
		MISSING,
		// The server holds another key than the pinned or known one.
		CHANGED,
		// The key of a new host could not be remembered.
		NOT_STORED,
		// The key of a new host was remembered, or the one of a known host was
		// brought up to date with a pin: the known hosts changed.
		STORED,
	};

	void Reset();
	/**
	 * Sets the check up for a start. A known host turns a TOFU pin into a
	 * pin of its key, and a pin of a known host updates its key.
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
	bool m_Pinned = false;
	bool m_Known = false;
	bool m_Remember = false;
	bool m_Update = false;
};

/**
 * A server whose identity could not be verified: the key or certificate it
 * showed was not the one expected, or not valid for Web PKI.
 */
class CServerIdentityFailure
{
public:
	// The normalized host, see `NormalizeQuicTrustHost`.
	char m_aHost[128] = {};
	int m_Port = 0;
	// "QUIC", "WebTransport" or "wss".
	const char *m_pTransport = "";
	CModernTransportPin m_Pin = {};
	EServerIdentitySource m_Source = EServerIdentitySource::ADDRESS;
	// The SHA-256 of the key or of the certificate the server showed, as the pin has it.
	bool m_HasPresented = false;
	SHA256_DIGEST m_Presented = {};

	/**
	 * What is expected of a server a modern transport or a secure websocket
	 * is started to.
	 *
	 * @param Start How it is started, after `CQuicIdentityCheck::Prepare`.
	 * @param pTransport "QUIC", "WebTransport" or "wss".
	 */
	static CServerIdentityFailure Expected(const CModernTransportStart &Start, const char *pTransport);
};

/**
 * The reason a connection ends with when the identity of the server could not
 * be verified, the same for every transport.
 */
extern const char *const SERVER_IDENTITY_DISCONNECT_REASON;

/**
 * @return The title of the warning about a server whose identity could not be verified.
 */
const char *ServerIdentityWarningTitle();

/**
 * Writes the warning about a server whose identity could not be verified: the
 * server and transport, where the expectation came from, the expected and the
 * received key or certificate where they are known, and what to do next.
 * This is the one place it is made, for QUIC, WebTransport and wss alike.
 *
 * @param pBuffer The buffer to write to.
 * @param BufferSize The size of the buffer.
 * @param Failure What failed.
 */
void FormatServerIdentityWarning(char *pBuffer, int BufferSize, const CServerIdentityFailure &Failure);

#endif
