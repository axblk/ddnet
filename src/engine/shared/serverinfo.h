#ifndef ENGINE_SHARED_SERVERINFO_H
#define ENGINE_SHARED_SERVERINFO_H

#include "protocol.h"

#include <engine/map.h>
#include <engine/serverbrowser.h>

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
 * What a server advertises about its modern transports in the reserved extra
 * info field of an extended server info answer. This is how a LAN server,
 * which no master server lists, announces them.
 */
struct CQuicServerInfoExtra
{
	bool m_RawQuic;
	SHA256_DIGEST m_IdentityFingerprint;
	bool m_WebTransport;
	CModernTransportPin m_WebTransportPin;
	const char *m_pHostname;
};

enum
{
	// Two certificate hashes and a hostname on top of the base string. The
	// packer takes this as an upper bound, only the actual text is sent.
	QUIC_SERVERINFO_EXTRA_MAXSIZE = 640,
};

void FormatQuicServerInfoExtra(char *pBuffer, int BufferSize, const CQuicServerInfoExtra &Extra);
/**
 * Reads the modern transports of a server from the extra info field of its
 * extended server info answer.
 *
 * @param pInfo Its modern transports are replaced by the ones read.
 * @param pExtraInfo The extra info field.
 * @param Addr The address the answer came from, which the transports share.
 *
 * @return `true` on error.
 */
bool ParseQuicServerInfoExtra(CServerInfo *pInfo, const char *pExtraInfo, const NETADDR &Addr);

#endif // ENGINE_SHARED_SERVERINFO_H
