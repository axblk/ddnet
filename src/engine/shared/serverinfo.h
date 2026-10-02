#ifndef ENGINE_SHARED_SERVERINFO_H
#define ENGINE_SHARED_SERVERINFO_H

#include "protocol.h"

#include <base/hash.h>

#include <engine/map.h>
#include <engine/serverbrowser.h>
#include <engine/shared/transport_pin.h>

typedef struct _json_value json_value;
class CServerInfo;

class CServerInfo2
{
public:
	class CClient
	{
	public:
		char m_aName[MAX_NAME_LENGTH];
		char m_aClan[MAX_CLAN_LENGTH];
		int m_Country;
		int m_Score;
		bool m_IsPlayer;
		bool m_IsAfk;
		// skin info 0.6
		char m_aSkin[MAX_SKIN_LENGTH];
		bool m_CustomSkinColors;
		int m_CustomSkinColorBody;
		int m_CustomSkinColorFeet;
		// skin info 0.7
		char m_aaSkin7[protocol7::NUM_SKINPARTS][protocol7::MAX_SKIN_LENGTH];
		bool m_aUseCustomSkinColor7[protocol7::NUM_SKINPARTS];
		int m_aCustomSkinColor7[protocol7::NUM_SKINPARTS];
	};

	CClient m_aClients[SERVERINFO_MAX_CLIENTS];
	int m_MaxClients;
	int m_NumClients; // Indirectly serialized.
	int m_MaxPlayers;
	int m_NumPlayers; // Not serialized.
	CServerInfo::EClientScoreKind m_ClientScoreKind;
	bool m_Passworded;
	char m_aGameType[16];
	char m_aName[64];
	char m_aMapName[MAX_MAP_LENGTH];
	char m_aVersion[32];
	bool m_RequiresLogin;

	bool operator==(const CServerInfo2 &Other) const;
	bool operator!=(const CServerInfo2 &Other) const { return !(*this == Other); }
	static bool FromJson(CServerInfo2 *pOut, const json_value *pJson);
	static bool FromJsonRaw(CServerInfo2 *pOut, const json_value *pJson);
	bool Validate() const;

	operator CServerInfo() const;
};

bool ParseCrc(unsigned int *pResult, const char *pString);

/**
 * What a server advertises about its modern transports, in the reserved extra
 * info field of an extended server info answer and in `experimental.transports`
 * of the info it registers. This is how a LAN server, which no master server
 * lists, announces them, and how the others do until the masters list them.
 */
struct CQuicServerInfoExtra
{
	bool m_RawQuic;
	// What a raw QUIC link pins, see `EModernTransportTrust::SPKI_HASH`.
	SHA256_DIGEST m_QuicSpkiSha256;
	bool m_WebTransport;
	CModernTransportPin m_WebTransportPin;
	const char *m_pHostname;
	// WebSockets, with TLS for `ddnet+wss://`, on the same port over TCP.
	bool m_Websocket;
	bool m_WebsocketTls;
};

enum
{
	// Two certificate hashes and a hostname on top of the base string. The
	// packer takes this as an upper bound, only the actual text is sent.
	QUIC_SERVERINFO_EXTRA_MAXSIZE = 640,
	// An address with a host name of the maximum length and two certificate hashes.
	QUIC_SERVERINFO_URL_MAXSIZE = 384,
	// Raw QUIC, WebTransport and a WebSocket.
	QUIC_SERVERINFO_MAX_URLS = 3,
};

void FormatQuicServerInfoExtra(char *pBuffer, int BufferSize, const CQuicServerInfoExtra &Extra);
/**
 * Reads the modern transports of a server from the extra info field of its
 * extended server info answer, or from the info it registers.
 *
 * @param pExtraInfo The text the server describes them with.
 * @param Addr A UDP address of the server, which the transports share.
 * @param paUrls Receives the addresses of the transports, with what they are
 * pinned by as the fragment, like a master lists them.
 *
 * @return The number of addresses, -1 if the text cannot be read.
 */
int ParseQuicServerInfoExtra(const char *pExtraInfo, const NETADDR &Addr, char (*paUrls)[QUIC_SERVERINFO_URL_MAXSIZE]);

#endif // ENGINE_SHARED_SERVERINFO_H
