#include "transport_pin.h"

#include <base/str.h>

bool ParseModernTransportFragment(const char *pFragment, bool WebTransport, CModernTransportPin *pPin)
{
	*pPin = {};
	const char *pValue;
	if(pFragment[0] == '\0')
	{
		pPin->m_Trust = WebTransport ? EModernTransportTrust::WEBPKI : EModernTransportTrust::TOFU;
		return true;
	}
	if(str_comp(pFragment, "webpki") == 0)
	{
		pPin->m_Trust = EModernTransportTrust::WEBPKI;
		return true;
	}
	if((pValue = str_startswith(pFragment, "spki-sha256=")) != nullptr)
	{
		pPin->m_Trust = EModernTransportTrust::SPKI_HASH;
		return !WebTransport && sha256_from_str(&pPin->m_Fingerprint, pValue) == 0;
	}
	if((pValue = str_startswith(pFragment, "cert-sha256=")) != nullptr)
	{
		pPin->m_Trust = EModernTransportTrust::CERTIFICATE_HASH;
		return WebTransport && ParseCertificateHashes(pValue, pPin);
	}
	pPin->m_Trust = EModernTransportTrust::INVALID;
	return false;
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
