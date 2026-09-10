#include "ebpf_key.h"

#include "siphash.h"

#include <base/fs.h>
#include <base/io.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>

namespace
{

	constexpr char KEY_MAGIC[] = "DDNXDPK1";
	constexpr int KEY_MAGIC_SIZE = 8;
	constexpr int KEY_EPOCHS = 4;
	constexpr uint32_t KEY_VERSION = 1;

	// Mirrors `struct key_file` in src/xdp/ddnet_xdp.c.
	struct CKeyFileEpoch
	{
		uint64_t m_K0;
		uint64_t m_K1;
		uint8_t m_Valid;
		uint8_t m_aPad[7];
	};

	struct CKeyFile
	{
		char m_aMagic[KEY_MAGIC_SIZE];
		uint32_t m_Version;
		uint32_t m_Current;
		CKeyFileEpoch m_aEpochs[KEY_EPOCHS];
	};

	/**
	 * Zero when the file cannot be stated, which makes the next attempt read it again.
	 */
	int64_t ModifiedTime(const char *pPath)
	{
		time_t Created, Modified;
		if(fs_file_time(pPath, &Created, &Modified) != 0)
			return 0;
		return (int64_t)Modified;
	}

} // namespace

bool CEbpfKey::Load(const char *pPath)
{
	unsigned char aPrevious[MATERIAL_SIZE];
	mem_copy(aPrevious, m_aMaterial, sizeof(aPrevious));
	const bool WasLoaded = m_Loaded;
	m_Loaded = false;
	if(pPath == nullptr || pPath[0] == '\0')
		return false;

	IOHANDLE File = io_open(pPath, IOFLAG_READ);
	if(!File)
	{
		log_info("ebpf", "no key at '%s', tokens stay in the previous format and the filter will not arm this port", pPath);
		return false;
	}

	CKeyFile KeyFile;
	const unsigned Read = io_read(File, &KeyFile, sizeof(KeyFile));
	io_close(File);
	if(Read != sizeof(KeyFile))
	{
		log_error("ebpf", "ERROR: '%s' is too short to be a key file", pPath);
		return false;
	}
	if(mem_comp(KeyFile.m_aMagic, KEY_MAGIC, KEY_MAGIC_SIZE) != 0)
	{
		log_error("ebpf", "ERROR: '%s' is not a key file", pPath);
		return false;
	}
	if(KeyFile.m_Version != KEY_VERSION)
	{
		log_error("ebpf", "ERROR: '%s' is version %u, this build understands %u", pPath, KeyFile.m_Version, KEY_VERSION);
		return false;
	}
	if(KeyFile.m_Current >= KEY_EPOCHS || !KeyFile.m_aEpochs[KeyFile.m_Current].m_Valid)
	{
		log_error("ebpf", "ERROR: '%s' names epoch %u, which holds no key", pPath, KeyFile.m_Current);
		return false;
	}

	m_K0 = KeyFile.m_aEpochs[KeyFile.m_Current].m_K0;
	m_K1 = KeyFile.m_aEpochs[KeyFile.m_Current].m_K1;
	m_aMaterial[0] = (unsigned char)KeyFile.m_Current;
	for(int Byte = 0; Byte < 8; Byte++)
	{
		m_aMaterial[1 + Byte] = (unsigned char)(m_K0 >> (8 * Byte));
		m_aMaterial[9 + Byte] = (unsigned char)(m_K1 >> (8 * Byte));
	}
	m_ModifiedTime = ModifiedTime(pPath);
	m_Loaded = true;
	if(!WasLoaded || mem_comp(aPrevious, m_aMaterial, sizeof(m_aMaterial)) != 0)
	{
		m_Generation += 1;
	}
	log_info("ebpf", "using key epoch %u from '%s'", KeyFile.m_Current, pPath);
	return true;
}

bool CEbpfKey::Reload(const char *pPath)
{
	if(pPath == nullptr || pPath[0] == '\0')
		return false;
	const int64_t Modified = ModifiedTime(pPath);
	if(Modified == m_ModifiedTime)
		return false;
	const unsigned long long OldK0 = m_K0;
	const unsigned long long OldK1 = m_K1;
	const bool WasLoaded = m_Loaded;
	if(!Load(pPath))
	{
		// Keep deriving with what was read last. A file that is briefly unreadable
		// must not silently take the server out of the filter's sight.
		m_Loaded = WasLoaded;
		m_K0 = OldK0;
		m_K1 = OldK1;
		return false;
	}
	return m_K0 != OldK0 || m_K1 != OldK1;
}

unsigned int CEbpfKey::Token(const NETADDR &Addr) const
{
	// The canonical input, byte for byte what the filter builds:
	// family (1) || address (4 or 16) || port (2, big endian)
	unsigned char aInput[1 + 16 + 2];
	const bool Ipv4 = (Addr.type & NETTYPE_IPV4) != 0;
	const int AddressSize = Ipv4 ? 4 : 16;

	aInput[0] = Ipv4 ? 4 : 6;
	mem_copy(&aInput[1], Addr.ip, AddressSize);
	aInput[1 + AddressSize] = (unsigned char)(Addr.port >> 8);
	aInput[2 + AddressSize] = (unsigned char)Addr.port;

	const siphash_key Key = {m_K0, m_K1};
	return (unsigned int)siphash24(&Key, aInput, 3 + AddressSize);
}
