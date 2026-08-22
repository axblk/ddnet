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
	SHA256_DIGEST m_QuicCertificateSha256;
	SHA256_DIGEST m_QuicNextCertificateSha256;
	SHA256_DIGEST m_QuicIdentityFingerprint;
	int m_QuicPort;
	int m_QuicCapabilities;
	bool m_QuicSharedPort;
	bool m_HasQuicNextCertificateSha256;
	bool m_HasQuicIdentityFingerprint;
	bool m_WebTransport;
	CServerInfo::EWebTransportCertificateMode m_WebTransportCertificateMode;
	char m_aWebTransportPath[16];
	char m_aWebTransportUrl[256];

	bool operator==(const CServerInfo2 &Other) const;
	bool operator!=(const CServerInfo2 &Other) const { return !(*this == Other); }
	static bool FromJson(CServerInfo2 *pOut, const json_value *pJson);
	static bool FromJsonRaw(CServerInfo2 *pOut, const json_value *pJson);
	bool Validate() const;

	operator CServerInfo() const;
};

bool ParseCrc(unsigned int *pResult, const char *pString);
bool FormatWebTransportUrl(char *pBuffer, int BufferSize, const char *pHostname, int Port);
bool ValidateWebTransportUrl(const char *pUrl, int Port);
bool ParseQuicDirectLinkFingerprint(const char *pUrl, SHA256_DIGEST *pFingerprint);
void FormatQuicServerInfoExtra(char *pBuffer, int BufferSize, SHA256_DIGEST IdentityFingerprint);
bool ParseQuicServerInfoExtra(CServerInfo *pInfo, const char *pExtraInfo, int Port);
void PreserveWebTransportMetadata(CServerInfo *pInfo, const CServerInfo &PreviousInfo);

#endif // ENGINE_SHARED_SERVERINFO_H
