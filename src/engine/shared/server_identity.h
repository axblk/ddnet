#ifndef ENGINE_SHARED_SERVER_IDENTITY_H
#define ENGINE_SHARED_SERVER_IDENTITY_H

#include <array>
#include <cstddef>

static constexpr size_t SERVER_IDENTITY_PUBLIC_KEY_SIZE = 32;
static constexpr size_t SERVER_IDENTITY_SIGNATURE_SIZE = 64;

struct CServerIdentityBinding
{
	std::array<unsigned char, SERVER_IDENTITY_PUBLIC_KEY_SIZE> m_PublicKey;
	std::array<unsigned char, SERVER_IDENTITY_SIGNATURE_SIZE> m_CertificateSignature;
	std::array<unsigned char, SERVER_IDENTITY_SIGNATURE_SIZE> m_NextCertificateSignature;
	bool m_HasNextCertificateSignature;

	bool operator==(const CServerIdentityBinding &Other) const = default;
};

bool FormatServerIdentityHex(char *pBuffer, int BufferSize, const unsigned char *pData, size_t DataSize);

#endif // ENGINE_SHARED_SERVER_IDENTITY_H
