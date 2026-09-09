#ifndef ENGINE_SHARED_CONNECT_CHOICE_H
#define ENGINE_SHARED_CONNECT_CHOICE_H

#include <base/types.h>

class CServerInfo;
class IServerBrowser;

/**
 * The transport a connect to a listed server uses. A pick, not a
 * preference: the connect address holds the one address of that transport,
 * and the connect uses it and nothing else.
 *
 * The values are stored in `cl_connect_protocol`, so they must not be
 * reordered.
 */
enum class EConnectProtocol
{
	LEGACY = 0,
	QUIC,
	WEBSOCKET,
	WEBTRANSPORT,
	COUNT,
};

/**
 * The address family a connect to a listed server uses, where the server
 * has an address of it.
 *
 * The values are stored in `cl_connect_address_family`, so they must not
 * be reordered.
 */
enum class EConnectAddressFamily
{
	IPV4 = 0,
	IPV6,
	COUNT,
};

/**
 * @param Address The address, its scheme's flags tell the transport.
 *
 * @return The transport of the address.
 */
EConnectProtocol AddressConnectProtocol(const NETADDR &Address);

/**
 * The transports and address families a connect address can go over, best
 * first: those of the endpoints the listed server has for this client, and
 * the one the address itself connects with, which is the current one. An
 * address of no listed server leaves nothing else to choose.
 */
class CConnectChoices
{
public:
	EConnectProtocol m_aProtocols[(int)EConnectProtocol::COUNT];
	int m_NumProtocols = 0;
	EConnectAddressFamily m_aFamilies[(int)EConnectAddressFamily::COUNT];
	int m_NumFamilies = 0;
	/**
	 * The index of the transport the address connects with, -1 without an
	 * address.
	 */
	int m_CurrentProtocol = -1;
	/**
	 * The index of the family of the address, -1 without an address or for
	 * a host name.
	 */
	int m_CurrentFamily = -1;

	/**
	 * @param pServer The listed server the address is one of, or `nullptr`.
	 * Only read during the call.
	 * @param pAddress The connect address, or `nullptr` for the server's
	 * own choices.
	 */
	CConnectChoices(const CServerInfo *pServer, const char *pAddress);

	/**
	 * @param Picked The pick as stored in `cl_connect_protocol`.
	 *
	 * @return The index of the transport the address connects with, without
	 * one that of the picked transport, 0 where it is not one of the
	 * choices.
	 */
	int ProtocolIndex(int Picked) const;

	/**
	 * @param Picked The pick as stored in `cl_connect_address_family`.
	 *
	 * @return The index of the family of the address, without an IP
	 * address that of the picked family, 0 where it is not one of the
	 * choices.
	 */
	int FamilyIndex(int Picked) const;
};

/**
 * @param Protocol The transport.
 * @param pAddress The connect address, which tells WebSockets over TLS
 * apart; may be `nullptr`.
 *
 * @return The short name of the transport, not localized.
 */
const char *ConnectProtocolShortName(EConnectProtocol Protocol, const char *pAddress);

/**
 * Whether a connect address is one of the server's addresses.
 *
 * @param Server The server.
 * @param pAddress The connect address.
 */
bool ServerHasAddress(const CServerInfo &Server, const char *pAddress);

/**
 * The listed server the first address of a connect string is one of.
 *
 * @param Browser The server browser to look in.
 * @param pAddresses The comma-separated connect string.
 *
 * @return The server, owned by the browser and valid until its list
 * changes; `nullptr` if none is listed with the address.
 */
const CServerInfo *FindListedServer(IServerBrowser &Browser, const char *pAddresses);

/**
 * Which pick wins where the server has no endpoint for both: the one just
 * made. The other falls back to what the server has with it.
 */
enum class EConnectPrecedence
{
	PROTOCOL,
	ADDRESS_FAMILY,
};

/**
 * Writes the one address of a server that a connect with the stored picks
 * uses, with its scheme and the fragment the master listed for it; it goes
 * into the address box, which is what is connected to.
 *
 * Transport and family are picked independently: each is the pick where the
 * server has it, otherwise the best it has of those this client speaks, then
 * IPv6. Where the server has no endpoint for both picks, the one with
 * precedence stays and the other falls back to what there is with it.
 *
 * @param Server The server.
 * @param PickedProtocol The pick as stored in `cl_connect_protocol`.
 * @param PickedFamily The pick as stored in `cl_connect_address_family`.
 * @param Precedence The pick that wins, the one just made; the transport
 * where none was.
 * @param pBuffer The buffer, left alone if the server has no address this
 * client uses.
 * @param BufferSize The size of the buffer.
 *
 * @return Whether an address was written.
 */
bool ConnectAddressFor(const CServerInfo &Server, int PickedProtocol, int PickedFamily, EConnectPrecedence Precedence, char *pBuffer, int BufferSize);

#endif
