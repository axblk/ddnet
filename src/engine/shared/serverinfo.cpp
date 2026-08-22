#include "serverinfo.h"

#include "json.h"

#include <base/mem.h>
#include <base/net.h>
#include <base/str.h>

#include <engine/external/json-parser/json.h>

#include <algorithm>
#include <cstdio>

static bool IsAllowedHex(char c)
{
	static const char ALLOWED[] = "0123456789abcdefABCDEF";
	for(int i = 0; i < (int)sizeof(ALLOWED) - 1; i++)
	{
		if(c == ALLOWED[i])
		{
			return true;
		}
	}
	return false;
}

bool ParseCrc(unsigned int *pResult, const char *pString)
{
	if(str_length(pString) != 8)
	{
		return true;
	}
	for(int i = 0; i < 8; i++)
	{
		if(!IsAllowedHex(pString[i]))
		{
			return true;
		}
	}
	return sscanf(pString, "%08x", pResult) != 1;
}

bool ValidateWebTransportUrl(const char *pUrl, int Port)
{
	if(!in_range(Port, 1, 65535) || str_length(pUrl) >= 256)
		return false;
	const char *pAuthority = str_startswith(pUrl, "https://");
	if(!pAuthority)
		return false;
	const char *pPath = str_find(pAuthority, "/");
	if(!pPath || str_comp(pPath, "/ddnet") != 0 || pPath == pAuthority)
		return false;

	const char *pPort = nullptr;
	if(*pAuthority == '[')
	{
		const char *pClosingBracket = str_find(pAuthority, "]");
		if(!pClosingBracket || pClosingBracket == pAuthority + 1 || pClosingBracket + 1 >= pPath || pClosingBracket[1] != ':')
			return false;
		char aAddress[NETADDR_MAXSTRSIZE];
		const int AddressLength = pClosingBracket - pAuthority + 1;
		if(AddressLength >= (int)sizeof(aAddress))
			return false;
		str_copy(aAddress, pAuthority, AddressLength + 1);
		NETADDR Address;
		if(net_addr_from_str(&Address, aAddress) != 0 || Address.type != NETTYPE_IPV6)
			return false;
		pPort = pClosingBracket + 2;
	}
	else
	{
		for(const char *pCurrent = pAuthority; pCurrent < pPath; pCurrent++)
		{
			if(*pCurrent == ':')
			{
				if(pPort)
					return false;
				pPort = pCurrent + 1;
			}
		}
		if(!pPort)
			return false;
		const char *pLabelStart = pAuthority;
		for(const char *pCurrent = pAuthority; pCurrent < pPort - 1; pCurrent++)
		{
			const bool AlphaNumeric = (*pCurrent >= 'a' && *pCurrent <= 'z') || (*pCurrent >= 'A' && *pCurrent <= 'Z') || (*pCurrent >= '0' && *pCurrent <= '9');
			if(*pCurrent == '.')
			{
				if(pCurrent == pLabelStart || pCurrent[-1] == '-')
					return false;
				pLabelStart = pCurrent + 1;
			}
			else if(!AlphaNumeric && *pCurrent != '-')
				return false;
			else if(pCurrent == pLabelStart && *pCurrent == '-')
				return false;
		}
		if(pLabelStart == pPort - 1 || pPort[-2] == '-')
			return false;
	}
	if(!pPort || pPort == pAuthority + 1 || pPort == pPath)
		return false;
	int ParsedPort = 0;
	for(const char *pCurrent = pPort; pCurrent < pPath; pCurrent++)
	{
		if(*pCurrent < '0' || *pCurrent > '9')
			return false;
		ParsedPort = ParsedPort * 10 + (*pCurrent - '0');
		if(ParsedPort > 65535)
			return false;
	}
	for(const char *pCurrent = pAuthority; pCurrent < pPort - 1; pCurrent++)
	{
		if(static_cast<unsigned char>(*pCurrent) <= 0x20 || static_cast<unsigned char>(*pCurrent) >= 0x7f || *pCurrent == '@' || *pCurrent == '?' || *pCurrent == '#')
			return false;
	}
	return ParsedPort == Port;
}

bool FormatWebTransportUrl(char *pBuffer, int BufferSize, const char *pHostname, int Port)
{
	if(pHostname[0] == '\0' || str_format(pBuffer, BufferSize, "https://%s:%d/ddnet", pHostname, Port) >= BufferSize)
		return false;
	return ValidateWebTransportUrl(pBuffer, Port);
}

static bool ParseFingerprintList(const char *pValue, SHA256_DIGEST *pFingerprint, SHA256_DIGEST *pNextFingerprint, bool *pHasNextFingerprint)
{
	const char *pSeparator = str_find(pValue, ",");
	if(!pSeparator)
	{
		*pHasNextFingerprint = false;
		return sha256_from_str(pFingerprint, pValue) == 0;
	}
	if(pSeparator - pValue != SHA256_DIGEST_LENGTH * 2 || str_find(pSeparator + 1, ","))
		return false;
	char aFingerprint[SHA256_MAXSTRSIZE];
	str_copy(aFingerprint, pValue, sizeof(aFingerprint));
	aFingerprint[SHA256_DIGEST_LENGTH * 2] = '\0';
	*pHasNextFingerprint = true;
	return sha256_from_str(pFingerprint, aFingerprint) == 0 && sha256_from_str(pNextFingerprint, pSeparator + 1) == 0 && *pFingerprint != *pNextFingerprint;
}

bool ParseModernTransportUrl(const char *pUrl, bool *pWebTransport, EModernTransportTrust *pTrust, SHA256_DIGEST *pFingerprint, SHA256_DIGEST *pNextFingerprint, bool *pHasNextFingerprint)
{
	const char *pHost = str_startswith(pUrl, "ddnet+quic://");
	if(!pHost)
		pHost = str_startswith(pUrl, "tw-0.7+quic://");
	*pWebTransport = false;
	if(!pHost)
	{
		pHost = str_startswith(pUrl, "ddnet+wt://");
		if(!pHost)
			pHost = str_startswith(pUrl, "tw-0.7+wt://");
		*pWebTransport = pHost != nullptr;
	}
	if(!pHost)
		return false;

	*pFingerprint = {};
	*pNextFingerprint = {};
	*pHasNextFingerprint = false;
	const char *pFragment = str_find(pHost, "#");
	const char *pPath = str_find(pHost, "/");
	const char *pQuery = str_find(pHost, "?");
	const char *pUserInfo = str_find(pHost, "@");
	if(pHost[0] == '\0' || (pPath && (!pFragment || pPath < pFragment)) || (pQuery && (!pFragment || pQuery < pFragment)) || (pUserInfo && (!pFragment || pUserInfo < pFragment)))
		return false;
	if(!pFragment)
	{
		*pTrust = *pWebTransport ? EModernTransportTrust::WEBPKI : EModernTransportTrust::TOFU;
		return true;
	}
	if(pFragment == pHost || pFragment[1] == '\0')
		return false;
	const char *pValue = str_startswith(pFragment, "#cert-sha256=");
	if(pValue)
	{
		*pTrust = EModernTransportTrust::CERTIFICATE_HASH;
		return ParseFingerprintList(pValue, pFingerprint, pNextFingerprint, pHasNextFingerprint);
	}
	if(str_comp(pFragment, "#webpki") == 0)
	{
		*pTrust = EModernTransportTrust::WEBPKI;
		return true;
	}
	if(!*pWebTransport)
	{
		pValue = str_startswith(pFragment, "#identity-sha256=");
		if(!pValue)
			pValue = str_startswith(pFragment, "#sha256=");
		if(pValue)
		{
			*pTrust = EModernTransportTrust::IDENTITY;
			return sha256_from_str(pFingerprint, pValue) == 0;
		}
	}
	return false;
}

bool ParseQuicDirectLinkFingerprint(const char *pUrl, SHA256_DIGEST *pFingerprint)
{
	bool WebTransport;
	EModernTransportTrust Trust;
	SHA256_DIGEST NextFingerprint;
	bool HasNextFingerprint;
	return ParseModernTransportUrl(pUrl, &WebTransport, &Trust, pFingerprint, &NextFingerprint, &HasNextFingerprint) && !WebTransport && Trust == EModernTransportTrust::IDENTITY;
}

static constexpr char QUIC_SERVERINFO_EXTRA_V1_PREFIX[] = "ddnet-transport-v1|quic|tls-certificate-sha256=";
static constexpr char QUIC_SERVERINFO_EXTRA_V2_PREFIX[] = "ddnet-transport-v2|quic|identity-sha256=";
static constexpr char QUIC_SERVERINFO_EXTRA_SUFFIX[] = "|capabilities=datagram,map-stream,resume-v1,game-protocol-7";

void FormatQuicServerInfoExtra(char *pBuffer, int BufferSize, SHA256_DIGEST IdentityFingerprint)
{
	char aIdentityFingerprint[SHA256_MAXSTRSIZE];
	sha256_str(IdentityFingerprint, aIdentityFingerprint, sizeof(aIdentityFingerprint));
	str_format(pBuffer, BufferSize, "%s%s%s", QUIC_SERVERINFO_EXTRA_V2_PREFIX, aIdentityFingerprint, QUIC_SERVERINFO_EXTRA_SUFFIX);
}

bool ParseQuicServerInfoExtra(CServerInfo *pInfo, const char *pExtraInfo, int Port)
{
	static constexpr int SUFFIX_LENGTH = sizeof(QUIC_SERVERINFO_EXTRA_SUFFIX) - 1;
	const char *pFingerprint = str_startswith(pExtraInfo, QUIC_SERVERINFO_EXTRA_V2_PREFIX);
	const bool Identity = pFingerprint != nullptr;
	if(!pFingerprint)
		pFingerprint = str_startswith(pExtraInfo, QUIC_SERVERINFO_EXTRA_V1_PREFIX);
	if(!pFingerprint || !in_range(Port, 1, 65535) || str_length(pFingerprint) != SHA256_DIGEST_LENGTH * 2 + SUFFIX_LENGTH ||
		str_comp(pFingerprint + SHA256_DIGEST_LENGTH * 2, QUIC_SERVERINFO_EXTRA_SUFFIX) != 0)
		return true;

	char aFingerprint[SHA256_MAXSTRSIZE];
	str_copy(aFingerprint, pFingerprint, sizeof(aFingerprint));
	SHA256_DIGEST Fingerprint;
	if(sha256_from_str(&Fingerprint, aFingerprint))
		return true;

	if(Identity)
	{
		pInfo->m_QuicIdentityFingerprint = Fingerprint;
		pInfo->m_HasQuicIdentityFingerprint = true;
	}
	else
		pInfo->m_QuicCertificateSha256 = Fingerprint;
	pInfo->m_QuicPort = Port;
	pInfo->m_QuicCapabilities = CServerInfo::QUIC_CAPABILITY_DATAGRAM | CServerInfo::QUIC_CAPABILITY_MAP_STREAM | CServerInfo::QUIC_CAPABILITY_RESUME | CServerInfo::QUIC_CAPABILITY_GAME_PROTOCOL_7;
	pInfo->m_QuicSharedPort = true;
	return false;
}

void PreserveWebTransportMetadata(CServerInfo *pInfo, const CServerInfo &PreviousInfo)
{
	pInfo->m_QuicCapabilities |= PreviousInfo.m_QuicCapabilities;
	pInfo->m_QuicCertificateSha256 = PreviousInfo.m_QuicCertificateSha256;
	pInfo->m_QuicNextCertificateSha256 = PreviousInfo.m_QuicNextCertificateSha256;
	pInfo->m_HasQuicNextCertificateSha256 = PreviousInfo.m_HasQuicNextCertificateSha256;
	pInfo->m_QuicTrust = PreviousInfo.m_QuicTrust;
	pInfo->m_WebTransport = true;
	pInfo->m_WebTransportCertificateMode = PreviousInfo.m_WebTransportCertificateMode;
	str_copy(pInfo->m_aWebTransportPath, PreviousInfo.m_aWebTransportPath);
	str_copy(pInfo->m_aWebTransportUrl, PreviousInfo.m_aWebTransportUrl);
	str_copy(pInfo->m_aModernHostname, PreviousInfo.m_aModernHostname);
}

bool CServerInfo2::FromJson(CServerInfo2 *pOut, const json_value *pJson)
{
	bool Result = FromJsonRaw(pOut, pJson);
	if(Result)
	{
		return Result;
	}
	return pOut->Validate();
}

bool CServerInfo2::Validate() const
{
	bool Error = false;
	Error = Error || m_MaxClients < m_MaxPlayers;
	Error = Error || m_NumClients < m_NumPlayers;
	Error = Error || m_MaxClients < m_NumClients;
	Error = Error || m_MaxPlayers < m_NumPlayers;
	return Error;
}

static bool ParseCertificatePins(const json_value &Object, SHA256_DIGEST *pCertificateSha256, SHA256_DIGEST *pNextCertificateSha256, bool *pHasNextCertificateSha256)
{
	const json_value &CertificateSha256 = Object["tls_certificate_sha256"];
	const json_value &NextCertificateSha256 = Object["tls_certificate_sha256_next"];
	if(CertificateSha256.type != json_string || (NextCertificateSha256.type != json_none && NextCertificateSha256.type != json_string) ||
		sha256_from_str(pCertificateSha256, CertificateSha256))
		return true;
	*pHasNextCertificateSha256 = NextCertificateSha256.type == json_string;
	return *pHasNextCertificateSha256 && (sha256_from_str(pNextCertificateSha256, NextCertificateSha256) || *pNextCertificateSha256 == *pCertificateSha256);
}

static void ParseQuicTransport(CServerInfo2 *pOut, const json_value &ServerInfo)
{
	const json_value &Transport = ServerInfo["transport"];
	if(Transport.type != json_object)
		return;

	const json_value &UdpPort = Transport["udp_port"];
	const json_value &Quic = Transport["quic"];
	const json_value &WebTransport = Transport["webtransport"];
	if(UdpPort.type != json_integer || (Quic.type != json_none && Quic.type != json_boolean) || (WebTransport.type != json_none && WebTransport.type != json_object))
		return;
	if(UdpPort.u.integer < 1 || UdpPort.u.integer > 65535)
		return;
	const int Port = UdpPort.u.integer;
	const bool HasQuic = Quic.type == json_boolean && (bool)Quic;
	const bool HasWebTransport = WebTransport.type == json_object;
	if(!HasQuic && !HasWebTransport)
		return;

	SHA256_DIGEST ParsedCertificateSha256;
	SHA256_DIGEST ParsedNextCertificateSha256 = {};
	bool HasNextCertificateSha256 = false;
	if(ParseCertificatePins(Transport, &ParsedCertificateSha256, &ParsedNextCertificateSha256, &HasNextCertificateSha256))
		return;
	if(HasWebTransport)
	{
		const json_value &Url = WebTransport["url"];
		const json_value &CertificateMode = WebTransport["certificate_mode"];
		if(CertificateMode.type != json_string || (Url.type != json_none && (Url.type != json_string || !ValidateWebTransportUrl(Url, Port))))
			return;
		if(str_comp(CertificateMode, "webpki") == 0)
			pOut->m_WebTransportCertificateMode = CServerInfo::EWebTransportCertificateMode::WEBPKI;
		else if(str_comp(CertificateMode, "hash") == 0)
			pOut->m_WebTransportCertificateMode = CServerInfo::EWebTransportCertificateMode::HASH;
		else
			return;
		if(Url.type == json_none && pOut->m_WebTransportCertificateMode != CServerInfo::EWebTransportCertificateMode::HASH)
			return;
		str_copy(pOut->m_aWebTransportPath, "/ddnet");
		if(Url.type == json_string)
			str_copy(pOut->m_aWebTransportUrl, Url);
	}
	pOut->m_QuicCertificateSha256 = ParsedCertificateSha256;
	pOut->m_QuicNextCertificateSha256 = ParsedNextCertificateSha256;
	pOut->m_QuicPort = Port;
	pOut->m_QuicCapabilities = CServerInfo::QUIC_CAPABILITY_DATAGRAM | CServerInfo::QUIC_CAPABILITY_MAP_STREAM | CServerInfo::QUIC_CAPABILITY_RESUME | CServerInfo::QUIC_CAPABILITY_GAME_PROTOCOL_7;
	pOut->m_QuicSharedPort = HasQuic;
	pOut->m_QuicTrust = HasQuic ? EModernTransportTrust::CERTIFICATE_HASH : EModernTransportTrust::INVALID;
	pOut->m_HasQuicNextCertificateSha256 = HasNextCertificateSha256;
	pOut->m_WebTransport = HasWebTransport;
}

static bool ParseModernHashes(const json_value &Hashes, SHA256_DIGEST *pCertificateSha256, SHA256_DIGEST *pNextCertificateSha256, bool *pHasNextCertificateSha256)
{
	if(Hashes.type != json_array || Hashes.u.array.length < 1 || Hashes.u.array.length > 2 || Hashes[0].type != json_string || sha256_from_str(pCertificateSha256, Hashes[0]))
		return false;
	*pHasNextCertificateSha256 = Hashes.u.array.length == 2;
	return !*pHasNextCertificateSha256 || (Hashes[1].type == json_string && sha256_from_str(pNextCertificateSha256, Hashes[1]) == 0 && *pCertificateSha256 != *pNextCertificateSha256);
}

static bool ParseProtocolDescriptor(const json_value &Descriptor, bool AllowIdentity, EModernTransportTrust *pTrust, SHA256_DIGEST *pFingerprint, SHA256_DIGEST *pNextFingerprint, bool *pHasNextFingerprint)
{
	if(Descriptor.type != json_object || Descriptor["verify"].type != json_string)
		return false;
	const json_value &Verify = Descriptor["verify"];
	const json_value &Hashes = Descriptor["sha256"];
	if(str_comp(Verify, "webpki") == 0)
	{
		*pTrust = EModernTransportTrust::WEBPKI;
		return Hashes.type == json_none;
	}
	if(str_comp(Verify, "hash") == 0)
	{
		*pTrust = EModernTransportTrust::CERTIFICATE_HASH;
		return ParseModernHashes(Hashes, pFingerprint, pNextFingerprint, pHasNextFingerprint);
	}
	if(AllowIdentity && str_comp(Verify, "identity") == 0 && Hashes.type == json_string && sha256_from_str(pFingerprint, Hashes) == 0)
	{
		*pTrust = EModernTransportTrust::IDENTITY;
		return true;
	}
	return false;
}

static void ParseExperimentalProtocol(CServerInfo2 *pOut, const json_value &ServerInfo)
{
	const json_value &Protocol = ServerInfo["experimental"]["proto"];
	if(Protocol.type != json_object)
		return;
	int Port = pOut->m_QuicPort;
	const json_value &UdpPort = Protocol["udp_port"];
	if(UdpPort.type != json_none)
	{
		if(UdpPort.type != json_integer || !in_range((int)UdpPort.u.integer, 1, 65535))
			return;
		Port = UdpPort.u.integer;
	}
	const json_value &Hostname = Protocol["hostname"];
	char aHostname[256] = {};
	if(Hostname.type != json_none)
	{
		char aUrl[256];
		if(Hostname.type != json_string || !FormatWebTransportUrl(aUrl, sizeof(aUrl), Hostname, Port != 0 ? Port : 443))
			return;
		str_copy(aHostname, Hostname);
	}

	const json_value &Quic = Protocol["quic"];
	const json_value &WebTransport = Protocol["webtransport"];
	if((Quic.type != json_none && Quic.type != json_object) || (WebTransport.type != json_none && WebTransport.type != json_object) || (Quic.type == json_none && WebTransport.type == json_none))
		return;
	EModernTransportTrust QuicTrust = EModernTransportTrust::INVALID;
	EModernTransportTrust WebTransportTrust = EModernTransportTrust::INVALID;
	SHA256_DIGEST CertificateSha256 = {};
	SHA256_DIGEST NextCertificateSha256 = {};
	SHA256_DIGEST IdentityFingerprint = {};
	bool HasNextCertificateSha256 = false;
	if(Quic.type == json_object)
	{
		SHA256_DIGEST QuicFingerprint = {};
		SHA256_DIGEST QuicNextFingerprint = {};
		bool HasQuicNextFingerprint = false;
		if(!ParseProtocolDescriptor(Quic, true, &QuicTrust, &QuicFingerprint, &QuicNextFingerprint, &HasQuicNextFingerprint))
			return;
		if(QuicTrust == EModernTransportTrust::IDENTITY)
			IdentityFingerprint = QuicFingerprint;
		else if(QuicTrust == EModernTransportTrust::CERTIFICATE_HASH)
		{
			CertificateSha256 = QuicFingerprint;
			NextCertificateSha256 = QuicNextFingerprint;
			HasNextCertificateSha256 = HasQuicNextFingerprint;
		}
	}
	if(WebTransport.type == json_object)
	{
		SHA256_DIGEST WebTransportFingerprint = {};
		SHA256_DIGEST WebTransportNextFingerprint = {};
		bool HasWebTransportNextFingerprint = false;
		if(!ParseProtocolDescriptor(WebTransport, false, &WebTransportTrust, &WebTransportFingerprint, &WebTransportNextFingerprint, &HasWebTransportNextFingerprint))
			return;
		if(WebTransportTrust == EModernTransportTrust::WEBPKI && aHostname[0] == '\0')
			return;
		if(WebTransportTrust == EModernTransportTrust::CERTIFICATE_HASH)
		{
			if(QuicTrust == EModernTransportTrust::CERTIFICATE_HASH && (CertificateSha256 != WebTransportFingerprint || HasNextCertificateSha256 != HasWebTransportNextFingerprint || (HasNextCertificateSha256 && NextCertificateSha256 != WebTransportNextFingerprint)))
				return;
			CertificateSha256 = WebTransportFingerprint;
			NextCertificateSha256 = WebTransportNextFingerprint;
			HasNextCertificateSha256 = HasWebTransportNextFingerprint;
		}
	}
	if(QuicTrust == EModernTransportTrust::WEBPKI && aHostname[0] == '\0')
		return;

	pOut->m_QuicCertificateSha256 = CertificateSha256;
	pOut->m_QuicNextCertificateSha256 = NextCertificateSha256;
	pOut->m_QuicIdentityFingerprint = IdentityFingerprint;
	pOut->m_QuicPort = Port;
	pOut->m_QuicCapabilities = CServerInfo::QUIC_CAPABILITY_DATAGRAM | CServerInfo::QUIC_CAPABILITY_MAP_STREAM | CServerInfo::QUIC_CAPABILITY_RESUME | CServerInfo::QUIC_CAPABILITY_GAME_PROTOCOL_7;
	pOut->m_QuicSharedPort = Quic.type == json_object;
	pOut->m_HasQuicNextCertificateSha256 = HasNextCertificateSha256;
	pOut->m_HasQuicIdentityFingerprint = QuicTrust == EModernTransportTrust::IDENTITY;
	pOut->m_QuicTrust = QuicTrust;
	pOut->m_WebTransport = WebTransport.type == json_object;
	pOut->m_WebTransportCertificateMode = WebTransportTrust == EModernTransportTrust::WEBPKI ? CServerInfo::EWebTransportCertificateMode::WEBPKI : WebTransportTrust == EModernTransportTrust::CERTIFICATE_HASH ? CServerInfo::EWebTransportCertificateMode::HASH :
																										      CServerInfo::EWebTransportCertificateMode::NONE;
	str_copy(pOut->m_aWebTransportPath, pOut->m_WebTransport ? "/ddnet" : "");
	pOut->m_aWebTransportUrl[0] = '\0';
	if(pOut->m_WebTransport && aHostname[0] != '\0' && Port != 0)
		FormatWebTransportUrl(pOut->m_aWebTransportUrl, sizeof(pOut->m_aWebTransportUrl), aHostname, Port);
	str_copy(pOut->m_aModernHostname, aHostname);
}

bool CServerInfo2::FromJsonRaw(CServerInfo2 *pOut, const json_value *pJson)
{
	static constexpr const char *SKIN_PART_NAMES[protocol7::NUM_SKINPARTS] = {"body", "marking", "decoration", "hands", "feet", "eyes"};
	mem_zero(pOut, sizeof(*pOut));
	bool Error;

	const json_value &ServerInfo = *pJson;
	const json_value &MaxClients = ServerInfo["max_clients"];
	const json_value &MaxPlayers = ServerInfo["max_players"];
	const json_value &ClientScoreKind = ServerInfo["client_score_kind"];
	const json_value &Passworded = ServerInfo["passworded"];
	const json_value &GameType = ServerInfo["game_type"];
	const json_value &Name = ServerInfo["name"];
	const json_value &MapName = ServerInfo["map"]["name"];
	const json_value &Version = ServerInfo["version"];
	const json_value &Clients = ServerInfo["clients"];
	const json_value &RequiresLogin = ServerInfo["requires_login"];

	Error = false;
	Error = Error || MaxClients.type != json_integer;
	Error = Error || MaxPlayers.type != json_integer;
	Error = Error || Passworded.type != json_boolean;
	Error = Error || (ClientScoreKind.type != json_none && ClientScoreKind.type != json_string);
	Error = Error || GameType.type != json_string || str_has_cc(GameType);
	Error = Error || Name.type != json_string || str_has_cc(Name);
	Error = Error || MapName.type != json_string || str_has_cc(MapName);
	Error = Error || Version.type != json_string || str_has_cc(Version);
	Error = Error || Clients.type != json_array;
	if(Error)
	{
		return true;
	}
	pOut->m_MaxClients = json_int_get(&MaxClients);
	pOut->m_MaxPlayers = json_int_get(&MaxPlayers);
	pOut->m_ClientScoreKind = CServerInfo::CLIENT_SCORE_KIND_UNSPECIFIED;
	if(ClientScoreKind.type == json_string && str_startswith(ClientScoreKind, "points"))
	{
		pOut->m_ClientScoreKind = CServerInfo::CLIENT_SCORE_KIND_POINTS;
	}
	else if(ClientScoreKind.type == json_string && str_startswith(ClientScoreKind, "time"))
	{
		pOut->m_ClientScoreKind = CServerInfo::CLIENT_SCORE_KIND_TIME;
	}
	pOut->m_RequiresLogin = false;
	if(RequiresLogin.type == json_boolean)
	{
		pOut->m_RequiresLogin = RequiresLogin;
	}
	pOut->m_Passworded = Passworded;
	str_copy(pOut->m_aGameType, GameType);
	str_copy(pOut->m_aName, Name);
	str_copy(pOut->m_aMapName, MapName);
	str_copy(pOut->m_aVersion, Version);
	ParseQuicTransport(pOut, ServerInfo);
	ParseExperimentalProtocol(pOut, ServerInfo);

	pOut->m_NumClients = 0;
	pOut->m_NumPlayers = 0;
	for(unsigned i = 0; i < Clients.u.array.length; i++)
	{
		const json_value &Client = Clients[i];
		const json_value &ClientName = Client["name"];
		const json_value &Clan = Client["clan"];
		const json_value &Country = Client["country"];
		const json_value &Score = Client["score"];
		const json_value &IsPlayer = Client["is_player"];
		const json_value &IsAfk = Client["afk"];
		Error = false;
		Error = Error || ClientName.type != json_string || str_has_cc(ClientName);
		Error = Error || Clan.type != json_string || str_has_cc(Clan);
		Error = Error || Country.type != json_integer;
		Error = Error || Score.type != json_integer;
		Error = Error || IsPlayer.type != json_boolean;
		if(Error)
		{
			return true;
		}
		if(i < SERVERINFO_MAX_CLIENTS)
		{
			CClient *pClient = &pOut->m_aClients[i];
			str_copy(pClient->m_aName, ClientName);
			str_copy(pClient->m_aClan, Clan);
			pClient->m_Country = json_int_get(&Country);
			if(!in_range(pClient->m_Country, CountryCode::MINIMUM, CountryCode::MAXIMUM))
			{
				pClient->m_Country = CountryCode::DEFAULT;
			}
			pClient->m_Score = json_int_get(&Score);
			pClient->m_IsPlayer = IsPlayer;

			pClient->m_IsAfk = false;
			if(IsAfk.type == json_boolean)
				pClient->m_IsAfk = IsAfk;

			// check if a skin is also available
			bool HasSkin = false;
			const json_value &SkinObj = Client["skin"];
			if(SkinObj.type == json_object)
			{
				const json_value &SkinName = SkinObj["name"];
				const json_value &SkinBodyColor = SkinObj["color_body"];
				const json_value &SkinFeetColor = SkinObj["color_feet"];
				// 0.6 skin
				if(SkinName.type == json_string && !str_has_cc(SkinName.u.string.ptr))
				{
					HasSkin = true;
					str_copy(pClient->m_aSkin, SkinName.u.string.ptr);
					// if skin json value existed, then always at least default to "default"
					if(pClient->m_aSkin[0] == '\0')
						str_copy(pClient->m_aSkin, "default");
					// if skin also has custom colors, add them
					if(SkinBodyColor.type == json_integer && SkinFeetColor.type == json_integer)
					{
						pClient->m_CustomSkinColors = true;
						pClient->m_CustomSkinColorBody = SkinBodyColor.u.integer;
						pClient->m_CustomSkinColorFeet = SkinFeetColor.u.integer;
					}
					// else set custom colors off
					else
					{
						pClient->m_CustomSkinColors = false;
					}
				}
				// 0.7 skin
				else if(SkinObj[SKIN_PART_NAMES[protocol7::SKINPART_BODY]].type == json_object)
				{
					for(int Part = 0; Part < protocol7::NUM_SKINPARTS; Part++)
					{
						const json_value &SkinPartObj = SkinObj[SKIN_PART_NAMES[Part]];
						if(SkinPartObj.type == json_object)
						{
							const json_value &SkinPartName = SkinPartObj["name"];
							const json_value &SkinPartColor = SkinPartObj["color"];
							if(SkinPartName.type == json_string && !str_has_cc(SkinPartName.u.string.ptr))
							{
								HasSkin = true;
								str_copy(pClient->m_aaSkin7[Part], SkinPartName.u.string.ptr);
								if(pClient->m_aaSkin7[Part][0] == '\0' && Part != protocol7::SKINPART_MARKING && Part != protocol7::SKINPART_DECORATION)
									str_copy(pClient->m_aaSkin7[Part], "standard");
							}
							else
							{
								HasSkin = false;
							}
							if(SkinPartColor.type == json_integer)
							{
								pClient->m_aUseCustomSkinColor7[Part] = true;
								pClient->m_aCustomSkinColor7[Part] = SkinPartColor.u.integer;
							}
							else
							{
								pClient->m_aUseCustomSkinColor7[Part] = false;
							}
						}
						else
						{
							HasSkin = false;
						}
					}
				}
			}

			// else make it null terminated
			if(!HasSkin)
			{
				pClient->m_aSkin[0] = '\0';
				pClient->m_aaSkin7[protocol7::SKINPART_BODY][0] = '\0';
			}
		}

		pOut->m_NumClients++;
		if((bool)IsPlayer)
		{
			pOut->m_NumPlayers++;
		}
	}
	return false;
}

bool CServerInfo2::operator==(const CServerInfo2 &Other) const
{
	bool Unequal;
	Unequal = false;
	Unequal = Unequal || m_MaxClients != Other.m_MaxClients;
	Unequal = Unequal || m_NumClients != Other.m_NumClients;
	Unequal = Unequal || m_MaxPlayers != Other.m_MaxPlayers;
	Unequal = Unequal || m_NumPlayers != Other.m_NumPlayers;
	Unequal = Unequal || m_ClientScoreKind != Other.m_ClientScoreKind;
	Unequal = Unequal || m_Passworded != Other.m_Passworded;
	Unequal = Unequal || str_comp(m_aGameType, Other.m_aGameType) != 0;
	Unequal = Unequal || str_comp(m_aName, Other.m_aName) != 0;
	Unequal = Unequal || str_comp(m_aMapName, Other.m_aMapName) != 0;
	Unequal = Unequal || str_comp(m_aVersion, Other.m_aVersion) != 0;
	Unequal = Unequal || m_RequiresLogin != Other.m_RequiresLogin;
	Unequal = Unequal || m_QuicCertificateSha256 != Other.m_QuicCertificateSha256;
	Unequal = Unequal || m_QuicNextCertificateSha256 != Other.m_QuicNextCertificateSha256;
	Unequal = Unequal || m_QuicIdentityFingerprint != Other.m_QuicIdentityFingerprint;
	Unequal = Unequal || m_QuicPort != Other.m_QuicPort;
	Unequal = Unequal || m_QuicCapabilities != Other.m_QuicCapabilities;
	Unequal = Unequal || m_QuicSharedPort != Other.m_QuicSharedPort;
	Unequal = Unequal || m_HasQuicNextCertificateSha256 != Other.m_HasQuicNextCertificateSha256;
	Unequal = Unequal || m_HasQuicIdentityFingerprint != Other.m_HasQuicIdentityFingerprint;
	Unequal = Unequal || m_QuicTrust != Other.m_QuicTrust;
	Unequal = Unequal || m_WebTransport != Other.m_WebTransport;
	Unequal = Unequal || m_WebTransportCertificateMode != Other.m_WebTransportCertificateMode;
	Unequal = Unequal || str_comp(m_aWebTransportPath, Other.m_aWebTransportPath) != 0;
	Unequal = Unequal || str_comp(m_aWebTransportUrl, Other.m_aWebTransportUrl) != 0;
	Unequal = Unequal || str_comp(m_aModernHostname, Other.m_aModernHostname) != 0;
	if(Unequal)
	{
		return false;
	}
	for(int i = 0; i < std::min(m_NumClients, (int)SERVERINFO_MAX_CLIENTS); i++)
	{
		Unequal = false;
		Unequal = Unequal || str_comp(m_aClients[i].m_aName, Other.m_aClients[i].m_aName) != 0;
		Unequal = Unequal || str_comp(m_aClients[i].m_aClan, Other.m_aClients[i].m_aClan) != 0;
		Unequal = Unequal || m_aClients[i].m_Country != Other.m_aClients[i].m_Country;
		Unequal = Unequal || m_aClients[i].m_Score != Other.m_aClients[i].m_Score;
		Unequal = Unequal || m_aClients[i].m_IsPlayer != Other.m_aClients[i].m_IsPlayer;
		Unequal = Unequal || m_aClients[i].m_IsAfk != Other.m_aClients[i].m_IsAfk;
		if(Unequal)
		{
			return false;
		}
	}
	return true;
}

CServerInfo2::operator CServerInfo() const
{
	CServerInfo Result = {0};
	Result.m_MaxClients = m_MaxClients;
	Result.m_NumClients = m_NumClients;
	Result.m_MaxPlayers = m_MaxPlayers;
	Result.m_NumPlayers = m_NumPlayers;
	Result.m_ClientScoreKind = m_ClientScoreKind;
	Result.m_RequiresLogin = m_RequiresLogin;
	Result.m_QuicCertificateSha256 = m_QuicCertificateSha256;
	Result.m_QuicNextCertificateSha256 = m_QuicNextCertificateSha256;
	Result.m_QuicIdentityFingerprint = m_QuicIdentityFingerprint;
	Result.m_QuicPort = m_QuicPort;
	Result.m_QuicCapabilities = m_QuicCapabilities;
	Result.m_QuicSharedPort = m_QuicSharedPort;
	Result.m_HasQuicNextCertificateSha256 = m_HasQuicNextCertificateSha256;
	Result.m_HasQuicIdentityFingerprint = m_HasQuicIdentityFingerprint;
	Result.m_QuicTrust = m_QuicTrust;
	Result.m_WebTransport = m_WebTransport;
	Result.m_WebTransportCertificateMode = m_WebTransportCertificateMode;
	str_copy(Result.m_aWebTransportPath, m_aWebTransportPath);
	str_copy(Result.m_aWebTransportUrl, m_aWebTransportUrl);
	str_copy(Result.m_aModernHostname, m_aModernHostname);
	Result.m_Flags = m_Passworded ? SERVER_FLAG_PASSWORD : 0;
	str_copy(Result.m_aGameType, m_aGameType);
	str_copy(Result.m_aName, m_aName);
	str_copy(Result.m_aMap, m_aMapName);
	str_copy(Result.m_aVersion, m_aVersion);

	Result.m_vClients.resize(std::min(m_NumClients, (int)SERVERINFO_MAX_CLIENTS));
	for(size_t i = 0; i < Result.m_vClients.size(); i++)
	{
		str_copy(Result.m_vClients[i].m_aName, m_aClients[i].m_aName);
		str_copy(Result.m_vClients[i].m_aClan, m_aClients[i].m_aClan);
		Result.m_vClients[i].m_Country = m_aClients[i].m_Country;
		Result.m_vClients[i].m_Score = m_aClients[i].m_Score;
		Result.m_vClients[i].m_Player = m_aClients[i].m_IsPlayer;
		Result.m_vClients[i].m_Afk = m_aClients[i].m_IsAfk;

		// 0.6 skin
		str_copy(Result.m_vClients[i].m_aSkin, m_aClients[i].m_aSkin);
		Result.m_vClients[i].m_CustomSkinColors = m_aClients[i].m_CustomSkinColors;
		Result.m_vClients[i].m_CustomSkinColorBody = m_aClients[i].m_CustomSkinColorBody;
		Result.m_vClients[i].m_CustomSkinColorFeet = m_aClients[i].m_CustomSkinColorFeet;
		// 0.7 skin
		for(int Part = 0; Part < protocol7::NUM_SKINPARTS; Part++)
		{
			str_copy(Result.m_vClients[i].m_aaSkin7[Part], m_aClients[i].m_aaSkin7[Part]);
			Result.m_vClients[i].m_aUseCustomSkinColor7[Part] = m_aClients[i].m_aUseCustomSkinColor7[Part];
			Result.m_vClients[i].m_aCustomSkinColor7[Part] = m_aClients[i].m_aCustomSkinColor7[Part];
		}
	}

	Result.m_Latency = -1;

	return Result;
}
