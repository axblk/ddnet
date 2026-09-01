#include "test.h"

#include <base/fs.h>
#include <base/io.h>
#include <base/mem.h>
#include <base/net.h>
#include <base/str.h>

#include <engine/shared/ebpf_key.h>

#include <gtest/gtest.h>

namespace
{

	// Mirrors `struct key_file` in src/xdp/ddnet_xdp.c, which is what the filter service
	// writes and what this test has to be able to read.
	struct CKeyFile
	{
		char m_aMagic[8];
		uint32_t m_Version;
		uint32_t m_Current;
		struct
		{
			uint64_t m_K0;
			uint64_t m_K1;
			uint8_t m_Valid;
			uint8_t m_aPad[7];
		} m_aEpochs[4];
	};

	constexpr uint64_t TEST_K0 = 0x0123456789abcdefULL;
	constexpr uint64_t TEST_K1 = 0xfedcba9876543210ULL;

	CKeyFile ValidKeyFile()
	{
		CKeyFile KeyFile;
		mem_zero(&KeyFile, sizeof(KeyFile));
		mem_copy(KeyFile.m_aMagic, "DDNXDPK1", sizeof(KeyFile.m_aMagic));
		KeyFile.m_Version = 1;
		KeyFile.m_Current = 2;
		KeyFile.m_aEpochs[2].m_K0 = TEST_K0;
		KeyFile.m_aEpochs[2].m_K1 = TEST_K1;
		KeyFile.m_aEpochs[2].m_Valid = 1;
		return KeyFile;
	}

	class EbpfKey : public ::testing::Test // NOLINT(readability-identifier-naming)
	{
	protected:
		CTestInfo m_Info;
		char m_aPath[IO_MAX_PATH_LENGTH];

		void SetUp() override
		{
			str_format(m_aPath, sizeof(m_aPath), "%s-key", m_Info.m_aFilename);
		}

		void TearDown() override
		{
			if(fs_is_file(m_aPath))
			{
				EXPECT_FALSE(fs_remove(m_aPath));
			}
		}

		void Write(const CKeyFile &KeyFile, size_t Size = sizeof(CKeyFile))
		{
			IOHANDLE File = io_open(m_aPath, IOFLAG_WRITE);
			ASSERT_TRUE(File);
			io_write(File, &KeyFile, Size);
			io_close(File);
		}

		NETADDR Address(const char *pAddress)
		{
			NETADDR Addr;
			EXPECT_EQ(net_addr_from_str(&Addr, pAddress), 0);
			return Addr;
		}
	};

} // namespace

TEST_F(EbpfKey, MissingFileLeavesTheDerivationAlone)
{
	CEbpfKey Key;
	EXPECT_FALSE(Key.Load("this-file-does-not-exist"));
	EXPECT_FALSE(Key.IsLoaded());
	EXPECT_FALSE(Key.Load(""));
}

TEST_F(EbpfKey, RejectsWhatItCannotUse)
{
	CEbpfKey Key;

	CKeyFile Truncated = ValidKeyFile();
	Write(Truncated, sizeof(CKeyFile) - 1);
	EXPECT_FALSE(Key.Load(m_aPath));

	CKeyFile WrongMagic = ValidKeyFile();
	WrongMagic.m_aMagic[0] = 'X';
	Write(WrongMagic);
	EXPECT_FALSE(Key.Load(m_aPath));

	CKeyFile WrongVersion = ValidKeyFile();
	WrongVersion.m_Version = 2;
	Write(WrongVersion);
	EXPECT_FALSE(Key.Load(m_aPath));

	// A file that names an epoch holding no key would otherwise derive tokens from
	// zeroes, which every other host would derive as well.
	CKeyFile EmptyEpoch = ValidKeyFile();
	EmptyEpoch.m_aEpochs[2].m_Valid = 0;
	Write(EmptyEpoch);
	EXPECT_FALSE(Key.Load(m_aPath));

	CKeyFile OutOfRange = ValidKeyFile();
	OutOfRange.m_Current = 4;
	Write(OutOfRange);
	EXPECT_FALSE(Key.Load(m_aPath));
}

TEST_F(EbpfKey, MaterialCarriesTheEpochAndTheKey)
{
	CEbpfKey Key;
	Write(ValidKeyFile());
	ASSERT_TRUE(Key.Load(m_aPath));

	const unsigned char *pMaterial = Key.Material();
	EXPECT_EQ(pMaterial[0], 2);
	for(int Byte = 0; Byte < 8; Byte++)
	{
		EXPECT_EQ(pMaterial[1 + Byte], (unsigned char)(TEST_K0 >> (8 * Byte)));
		EXPECT_EQ(pMaterial[9 + Byte], (unsigned char)(TEST_K1 >> (8 * Byte)));
	}
}

// The filter recomputes this number from the packet alone. If the two derivations
// disagree by one byte, every verified packet is dropped, so the expected values are
// pinned here. They were produced with the C implementation in src/xdp/siphash.h,
// which its own test checks against the vectors from the SipHash specification.
TEST_F(EbpfKey, DerivesTheTokensTheFilterExpects)
{
	CEbpfKey Key;
	Write(ValidKeyFile());
	ASSERT_TRUE(Key.Load(m_aPath));

	EXPECT_EQ(Key.Token(Address("192.0.2.1:8303")), 0x361cc765u);
	EXPECT_EQ(Key.Token(Address("[2001:db8::1]:8303")), 0xfe18cc3cu);

	// The port is part of the derivation. Without it every client behind one address
	// would share a token, which is what the previous SHA256 derivation did.
	EXPECT_EQ(Key.Token(Address("192.0.2.1:8304")), 0x28ab18e6u);
	EXPECT_NE(Key.Token(Address("192.0.2.1:8303")), Key.Token(Address("192.0.2.1:8304")));

	// The global token, which `CNetServer` derives over the null address and the
	// filter checks in `global_token_valid`. The filter builds an IPv4 input of
	// seven bytes for it, so the server has to say IPv4 as well - a NETADDR left
	// at zero has no family and would be hashed over nineteen.
	EXPECT_EQ(Key.Token(Address("0.0.0.0:0")), 0x719ad4b3u);
}

TEST_F(EbpfKey, PicksUpARotationAndSurvivesAnUnreadableFile)
{
	CEbpfKey Key;
	Write(ValidKeyFile());
	ASSERT_TRUE(Key.Load(m_aPath));
	const unsigned int Before = Key.Token(Address("192.0.2.1:8303"));
	const unsigned long long FirstGeneration = Key.Generation();
	EXPECT_GT(FirstGeneration, 0u);

	CKeyFile Rotated = ValidKeyFile();
	Rotated.m_Current = 3;
	Rotated.m_aEpochs[3].m_K0 = 42;
	Rotated.m_aEpochs[3].m_K1 = 43;
	Rotated.m_aEpochs[3].m_Valid = 1;
	// The check is on the modification time, which has a resolution of a second.
	EXPECT_FALSE(fs_remove(m_aPath));
	Write(Rotated);
	Key.Load(m_aPath);
	EXPECT_NE(Key.Token(Address("192.0.2.1:8303")), Before);
	EXPECT_EQ(Key.Material()[0], 3);
	// What tells the QUIC endpoint that the connection IDs it stamps are tagged
	// with a key the filter no longer knows.
	EXPECT_EQ(Key.Generation(), FirstGeneration + 1);

	// A file that briefly cannot be read must not take the server out of the
	// filter's sight, so the last key stays in use.
	const unsigned int After = Key.Token(Address("192.0.2.1:8303"));
	EXPECT_FALSE(fs_remove(m_aPath));
	EXPECT_FALSE(Key.Reload(m_aPath));
	EXPECT_TRUE(Key.IsLoaded());
	EXPECT_EQ(Key.Token(Address("192.0.2.1:8303")), After);
	EXPECT_EQ(Key.Generation(), FirstGeneration + 1) << "a failed read is not a rotation";
}
