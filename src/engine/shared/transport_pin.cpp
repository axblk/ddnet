#include "transport_pin.h"

#include <base/math.h>
#include <base/net.h>
#include <base/str.h>

bool IsModernTransportUrl(const char *pUrl)
{
	return str_startswith(pUrl, "ddnet+quic://") || str_startswith(pUrl, "tw-0.7+quic://") ||
	       str_startswith(pUrl, "ddnet+wt://") || str_startswith(pUrl, "tw-0.7+wt://");
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
	if(pHostname[0] == '\0')
		return false;
	// A buffer too small to hold the whole URL cuts the path off the end, and the
	// path has to read exactly "/ddnet", so a truncated URL fails the reading below.
	str_format(pBuffer, BufferSize, "https://%s:%d/ddnet", pHostname, Port);
	return ValidateWebTransportUrl(pBuffer, Port);
}

bool ParseCertificateHashes(const char *pValue, CModernTransportPin *pPin)
{
	const char *pSeparator = str_find(pValue, ",");
	if(!pSeparator)
		return sha256_from_str(&pPin->m_Fingerprint, pValue) == 0;
	if(pSeparator - pValue != SHA256_DIGEST_LENGTH * 2 || str_find(pSeparator + 1, ","))
		return false;
	char aFingerprint[SHA256_MAXSTRSIZE];
	str_truncate(aFingerprint, sizeof(aFingerprint), pValue, SHA256_DIGEST_LENGTH * 2);
	pPin->m_HasNextFingerprint = true;
	return sha256_from_str(&pPin->m_Fingerprint, aFingerprint) == 0 && sha256_from_str(&pPin->m_NextFingerprint, pSeparator + 1) == 0 && pPin->m_Fingerprint != pPin->m_NextFingerprint;
}

void FormatCertificateHashes(char *pBuffer, int BufferSize, const CModernTransportPin &Pin)
{
	sha256_str(Pin.m_Fingerprint, pBuffer, BufferSize);
	if(!Pin.m_HasNextFingerprint)
		return;
	char aNextFingerprint[SHA256_MAXSTRSIZE];
	sha256_str(Pin.m_NextFingerprint, aNextFingerprint, sizeof(aNextFingerprint));
	str_append(pBuffer, ",", BufferSize);
	str_append(pBuffer, aNextFingerprint, BufferSize);
}

bool FormatModernTransportFragment(char *pBuffer, int BufferSize, bool WebTransport, const CModernTransportPin &Pin)
{
	pBuffer[0] = '\0';
	switch(Pin.m_Trust)
	{
	case EModernTransportTrust::TOFU:
		return !WebTransport;
	case EModernTransportTrust::WEBPKI:
		if(!WebTransport)
			str_copy(pBuffer, "webpki", BufferSize);
		return true;
	case EModernTransportTrust::CERTIFICATE_HASH:
	{
		char aHashes[2 * SHA256_MAXSTRSIZE];
		FormatCertificateHashes(aHashes, sizeof(aHashes), Pin);
		str_format(pBuffer, BufferSize, "cert-sha256=%s", aHashes);
		return true;
	}
	case EModernTransportTrust::SPKI_HASH:
	{
		if(WebTransport)
			return false;
		char aFingerprint[SHA256_MAXSTRSIZE];
		sha256_str(Pin.m_Fingerprint, aFingerprint, sizeof(aFingerprint));
		str_format(pBuffer, BufferSize, "spki-sha256=%s", aFingerprint);
		return true;
	}
	default:
		return false;
	}
}

bool FormatModernTransportUrl(char *pBuffer, int BufferSize, bool WebTransport, const NETADDR &Address, const char *pHostname, const CModernTransportPin &Pin)
{
	char aFragment[2 * SHA256_MAXSTRSIZE + 16];
	if(!FormatModernTransportFragment(aFragment, sizeof(aFragment), WebTransport, Pin))
		return false;
	const bool Sixup = (Address.type & NETTYPE_TW7) != 0;
	const char *pScheme = WebTransport ? (Sixup ? "tw-0.7+wt://" : "ddnet+wt://") : (Sixup ? "tw-0.7+quic://" : "ddnet+quic://");
	char aHost[300];
	if(pHostname[0] != '\0')
		str_format(aHost, sizeof(aHost), "%s:%d", pHostname, Address.port);
	else
	{
		// The scheme says what the address is, the type only says how to write it.
		NETADDR HostAddress = Address;
		HostAddress.type &= NETTYPE_IPV4 | NETTYPE_IPV6;
		if(HostAddress.type == 0)
			return false;
		net_addr_str(&HostAddress, aHost, sizeof(aHost), true);
	}
	const int FragmentLength = aFragment[0] != '\0' ? 1 + str_length(aFragment) : 0;
	if(str_length(pScheme) + str_length(aHost) + FragmentLength >= BufferSize)
		return false;
	str_format(pBuffer, BufferSize, "%s%s%s%s", pScheme, aHost, aFragment[0] != '\0' ? "#" : "", aFragment);
	return true;
}

// The pins a fragment names: `#webpki`, and where they are allowed,
// `#cert-sha256=` and `#spki-sha256=`.
static bool ParsePinFragment(const char *pFragment, bool CertificateHashes, bool Spki, CModernTransportPin *pPin)
{
	if(pFragment[0] != '#' || pFragment[1] == '\0')
		return false;
	if(const char *pValue = str_startswith(pFragment, "#cert-sha256="); pValue && CertificateHashes)
	{
		pPin->m_Trust = EModernTransportTrust::CERTIFICATE_HASH;
		return ParseCertificateHashes(pValue, pPin);
	}
	if(str_comp(pFragment, "#webpki") == 0)
	{
		pPin->m_Trust = EModernTransportTrust::WEBPKI;
		return true;
	}
	if(const char *pValue = str_startswith(pFragment, "#spki-sha256="); pValue && Spki)
	{
		pPin->m_Trust = EModernTransportTrust::SPKI_HASH;
		return sha256_from_str(&pPin->m_Fingerprint, pValue) == 0;
	}
	return false;
}

bool ParseModernTransportUrl(const char *pUrl, bool *pWebTransport, CModernTransportPin *pPin)
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

	*pPin = {};
	const char *pFragment = str_find(pHost, "#");
	const char *pPath = str_find(pHost, "/");
	const char *pQuery = str_find(pHost, "?");
	const char *pUserInfo = str_find(pHost, "@");
	if(pHost[0] == '\0' || (pPath && (!pFragment || pPath < pFragment)) || (pQuery && (!pFragment || pQuery < pFragment)) || (pUserInfo && (!pFragment || pUserInfo < pFragment)))
		return false;
	if(!pFragment)
	{
		pPin->m_Trust = *pWebTransport ? EModernTransportTrust::WEBPKI : EModernTransportTrust::TOFU;
		return true;
	}
	if(pFragment == pHost)
		return false;
	return ParsePinFragment(pFragment, true, !*pWebTransport, pPin);
}

bool ParseWebsocketPin(const char *pFragment, CModernTransportPin *pPin)
{
	*pPin = {};
	if(pFragment[0] == '\0')
	{
		pPin->m_Trust = EModernTransportTrust::TOFU;
		return true;
	}
	return ParsePinFragment(pFragment, false, true, pPin);
}
