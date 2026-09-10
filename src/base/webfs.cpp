#include "webfs.h"

#include <algorithm>
#include <string_view>

namespace
{
	// Kept in step with `scripts/generate_web_data.py`.
	constexpr std::string_view FORMAT_HEADER = "ddnet-web-data 1";
	constexpr size_t HASH_LENGTH = 16;

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

	bool ParseHash(std::string_view Text)
	{
		if(Text.size() != HASH_LENGTH)
			return false;
		return std::all_of(Text.begin(), Text.end(), [](const char Character) {
			return (Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f');
		});
	}
} // namespace

bool CWebDataIndex::Parse(const char *pData, size_t Size)
{
	m_Entries.clear();

	std::string_view Rest(pData, Size);
	const auto &&NextLine = [&Rest]() -> std::string_view {
		const size_t End = std::min(Rest.find('\n'), Rest.size());
		const std::string_view Line = Rest.substr(0, End);
		Rest = Rest.substr(std::min(End + 1, Rest.size()));
		// Written with Unix line endings, but a checkout may have turned them.
		return Line.empty() || Line.back() != '\r' ? Line : Line.substr(0, Line.size() - 1);
	};

	if(NextLine() != FORMAT_HEADER)
	{
		m_Entries.clear();
		return false;
	}

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
			// A file name may hold spaces, so the two fields behind it are
			// taken off the end and whatever is left is the path.
			const size_t HashStart = Line.rfind(' ');
			if(HashStart == std::string_view::npos)
			{
				m_Entries.clear();
				return false;
			}
			const size_t SizeStart = Line.rfind(' ', HashStart - 1);
			if(SizeStart == std::string_view::npos || SizeStart < 2)
			{
				m_Entries.clear();
				return false;
			}
			if(!ParseSize(Line.substr(SizeStart + 1, HashStart - SizeStart - 1), &Entry.m_Size) ||
				!ParseHash(Line.substr(HashStart + 1)))
			{
				m_Entries.clear();
				return false;
			}
			Entry.m_Hash = Line.substr(HashStart + 1);
			Path = Line.substr(2, SizeStart - 2);
		}
		else
		{
			m_Entries.clear();
			return false;
		}

		if(!PathIsSane(Path))
		{
			m_Entries.clear();
			return false;
		}
		Entry.m_Path = Path;
		if(!m_Entries.emplace(Entry.m_Path, std::move(Entry)).second)
		{
			m_Entries.clear();
			return false;
		}
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
