#include "server_identity.h"

bool FormatServerIdentityHex(char *pBuffer, int BufferSize, const unsigned char *pData, size_t DataSize)
{
	static constexpr char HEX[] = "0123456789abcdef";
	if(BufferSize <= 0 || DataSize > static_cast<size_t>((BufferSize - 1) / 2))
		return false;
	for(size_t i = 0; i < DataSize; i++)
	{
		pBuffer[2 * i] = HEX[pData[i] >> 4];
		pBuffer[2 * i + 1] = HEX[pData[i] & 0xf];
	}
	pBuffer[2 * DataSize] = '\0';
	return true;
}
