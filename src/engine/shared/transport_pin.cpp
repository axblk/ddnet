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
	if(pFragment == pHost || pFragment[1] == '\0')
		return false;
	if(const char *pValue = str_startswith(pFragment, "#cert-sha256="))
	{
		pPin->m_Trust = EModernTransportTrust::CERTIFICATE_HASH;
		return ParseCertificateHashes(pValue, pPin);
	}
	if(str_comp(pFragment, "#webpki") == 0)
	{
		pPin->m_Trust = EModernTransportTrust::WEBPKI;
		return true;
	}
	const char *pValue = str_startswith(pFragment, "#identity-sha256=");
	if(!*pWebTransport && pValue)
	{
		pPin->m_Trust = EModernTransportTrust::IDENTITY;
		return sha256_from_str(&pPin->m_Fingerprint, pValue) == 0;
	}
	return false;
}
