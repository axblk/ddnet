#include "web_data_index.h"

#include <base/str.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace
{
	// Kept in step with `scripts/generate_web_data.py`.
	constexpr std::string_view FORMAT_HEADER = "ddnet-web-data 2";
	constexpr size_t HASH_LENGTH = 16;
	constexpr size_t CRC_LENGTH = 8;

	bool PathIsSane(std::string_view Path)
	{
		// The index names files that are fetched and opened, so a path that walks
		// out of the data directory or that no file can have is refused here
		// rather than everywhere it is used.
		if(Path.empty() || Path.front() == '/' || Path.back() == '/')
			return false;
		if(Path.find('\\') != std::string_view::npos || Path.find("//") != std::string_view::npos)
			return false;
		for(const char Character : Path)
		{
			if((unsigned char)Character < 0x20)
				return false;
		}
		size_t Start = 0;
		while(Start <= Path.size())
		{
			const size_t End = std::min(Path.find('/', Start), Path.size());
			const std::string_view Part = Path.substr(Start, End - Start);
			if(Part == "." || Part == "..")
				return false;
			Start = End + 1;
		}
		return true;
	}

	bool ParseSize(std::string_view Text, int64_t *pSize)
	{
		if(Text.empty() || Text.size() > 19)
			return false;
		int64_t Size = 0;
		for(const char Character : Text)
		{
			if(Character < '0' || Character > '9')
				return false;
			Size = Size * 10 + (Character - '0');
		}
		*pSize = Size;
		return true;
	}

	bool IsHex(std::string_view Text, size_t Length)
	{
		if(Text.size() != Length)
			return false;
		return std::all_of(Text.begin(), Text.end(), [](const char Character) {
			return (Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f');
		});
	}

	bool ParseHash(std::string_view Text)
	{
		return IsHex(Text, HASH_LENGTH);
	}

	bool ParseSha256(std::string_view Text, SHA256_DIGEST *pSha256)
	{
		if(!IsHex(Text, SHA256_MAXSTRSIZE - 1))
			return false;
		const std::string Digits(Text);
		return sha256_from_str(pSha256, Digits.c_str()) == 0;
	}

	bool ParseCrc(std::string_view Text, unsigned *pCrc)
	{
		if(!IsHex(Text, CRC_LENGTH))
			return false;
		unsigned char aBytes[4];
		const std::string Digits(Text);
		if(str_hex_decode(aBytes, sizeof(aBytes), Digits.c_str()) != 0)
			return false;
		*pCrc = ((unsigned)aBytes[0] << 24) | ((unsigned)aBytes[1] << 16) | ((unsigned)aBytes[2] << 8) | (unsigned)aBytes[3];
		return true;
	}

	// A file name may hold spaces, so two fields behind it are taken off the
	// end and whatever is left is the path.
	bool SplitTwoFields(std::string_view Line, std::string_view *pPath, std::string_view *pFirst, std::string_view *pSecond)
	{
		const size_t SecondStart = Line.rfind(' ');
		if(SecondStart == std::string_view::npos || SecondStart == 0)
			return false;
		const size_t FirstStart = Line.rfind(' ', SecondStart - 1);
		if(FirstStart == std::string_view::npos || FirstStart < 2)
			return false;
		*pPath = Line.substr(2, FirstStart - 2);
		*pFirst = Line.substr(FirstStart + 1, SecondStart - FirstStart - 1);
		*pSecond = Line.substr(SecondStart + 1);
		return true;
	}
} // namespace

bool CWebDataIndex::Parse(const char *pData, size_t Size)
{
	m_Entries.clear();
	const auto &&Fail = [this]() {
		m_Entries.clear();
		return false;
	};

	std::string_view Rest(pData, Size);
	const auto &&NextLine = [&Rest]() -> std::string_view {
		const size_t End = std::min(Rest.find('\n'), Rest.size());
		const std::string_view Line = Rest.substr(0, End);
		Rest = Rest.substr(std::min(End + 1, Rest.size()));
		// Written with Unix line endings, but a checkout may have turned them.
		return Line.empty() || Line.back() != '\r' ? Line : Line.substr(0, Line.size() - 1);
	};

	if(NextLine() != FORMAT_HEADER)
		return Fail();

	while(!Rest.empty())
	{
		const std::string_view Line = NextLine();
		if(Line.empty())
			continue;

		CEntry Entry;
		std::string_view Path;
		if(Line.substr(0, 2) == "d ")
		{
			Entry.m_IsDirectory = true;
			Path = Line.substr(2);
		}
		else if(Line.substr(0, 2) == "f ")
		{
			std::string_view SizeField, Hash;
			if(!SplitTwoFields(Line, &Path, &SizeField, &Hash) || !ParseSize(SizeField, &Entry.m_Size) || !ParseHash(Hash))
				return Fail();
			Entry.m_Hash = Hash;
		}
		else if(Line.substr(0, 2) == "m ")
		{
			// The digests of a map, behind the map's file.
			std::string_view Sha256, Crc;
			SHA256_DIGEST MapSha256;
			unsigned MapCrc;
			if(!SplitTwoFields(Line, &Path, &Sha256, &Crc) || !ParseSha256(Sha256, &MapSha256) || !ParseCrc(Crc, &MapCrc))
				return Fail();
			const auto File = m_Entries.find(std::string(Path));
			if(File == m_Entries.end() || File->second.m_IsDirectory || File->second.m_Sha256.has_value())
				return Fail();
			File->second.m_Sha256 = MapSha256;
			File->second.m_Crc = MapCrc;
			continue;
		}
		else
		{
			return Fail();
		}

		if(!PathIsSane(Path))
			return Fail();
		Entry.m_Path = Path;
		if(!m_Entries.emplace(Entry.m_Path, std::move(Entry)).second)
			return Fail();
	}

	return true;
}

const CWebDataIndex::CEntry *CWebDataIndex::Find(const char *pPath) const
{
	const auto It = m_Entries.find(pPath);
	return It == m_Entries.end() ? nullptr : &It->second;
}

bool CWebDataIndex::IsFile(const char *pPath) const
{
	const CEntry *pEntry = Find(pPath);
	return pEntry != nullptr && !pEntry->m_IsDirectory;
}

bool CWebDataIndex::IsDirectory(const char *pPath) const
{
	if(pPath[0] == '\0')
		return true;
	const CEntry *pEntry = Find(pPath);
	return pEntry != nullptr && pEntry->m_IsDirectory;
}

void CWebDataIndex::List(const char *pPath, const std::function<void(const CEntry &)> &pfnCallback) const
{
	std::string Prefix = pPath;
	if(!Prefix.empty())
		Prefix += '/';
	// Everything in a directory sorts directly behind it, because the
	// directory with its separator is a prefix of every path below it.
	for(auto It = m_Entries.lower_bound(Prefix); It != m_Entries.end(); ++It)
	{
		const std::string &Path = It->first;
		if(Path.compare(0, Prefix.size(), Prefix) != 0)
			break;
		if(Path.find('/', Prefix.size()) != std::string::npos)
			continue;
		pfnCallback(It->second);
	}
}
