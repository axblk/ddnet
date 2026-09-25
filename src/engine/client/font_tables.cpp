/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "font_tables.h"

namespace
{
	constexpr uint32_t Tag(const char *pTag)
	{
		return (uint32_t)(uint8_t)pTag[0] << 24 | (uint32_t)(uint8_t)pTag[1] << 16 | (uint32_t)(uint8_t)pTag[2] << 8 | (uint32_t)(uint8_t)pTag[3];
	}

	// Big-endian reads that answer 0 past the end, so that a broken table
	// maps nothing instead of reading what is not its own.
	class CReader
	{
		const uint8_t *m_pData;
		size_t m_Size;

	public:
		CReader(const uint8_t *pData, size_t Size) :
			m_pData(pData), m_Size(Size) {}

		bool Has(size_t Offset, size_t Length) const { return Offset <= m_Size && Length <= m_Size - Offset; }
		uint16_t U16(size_t Offset) const { return Has(Offset, 2) ? (uint16_t)(m_pData[Offset] << 8 | m_pData[Offset + 1]) : 0; }
		int16_t S16(size_t Offset) const { return (int16_t)U16(Offset); }
		uint32_t U32(size_t Offset) const { return Has(Offset, 4) ? (uint32_t)U16(Offset) << 16 | U16(Offset + 2) : 0; }
		uint8_t U8(size_t Offset) const { return Has(Offset, 1) ? m_pData[Offset] : 0; }
	};

	// Name IDs, see the OpenType `name` table
	enum
	{
		NAME_FAMILY = 1,
		NAME_SUBFAMILY = 2,
		NAME_TYPOGRAPHIC_FAMILY = 16,
		NAME_TYPOGRAPHIC_SUBFAMILY = 17,
		NAME_WWS_FAMILY = 21,
		NAME_WWS_SUBFAMILY = 22,
	};

	// The ASCII of a name as FreeType makes it (`tt_face_get_name`): UTF-16
	// or 8 bit, anything outside printable ASCII becomes '?'.
	std::string NameText(const CReader &Reader, size_t Offset, size_t Length, bool Utf16)
	{
		std::string Text;
		const size_t Step = Utf16 ? 2 : 1;
		for(size_t i = 0; i + Step <= Length; i += Step)
		{
			const unsigned Code = Utf16 ? Reader.U16(Offset + i) : Reader.U8(Offset + i);
			if(Code == 0)
				break;
			Text += Code < 32 || Code > 127 ? '?' : (char)Code;
		}
		return Text;
	}

	// The Unicode charmaps FreeType makes from a `cmap` table (`sfnt_find_encoding`).
	bool IsUnicode(unsigned PlatformId, unsigned EncodingId)
	{
		return PlatformId == 0 || (PlatformId == 3 && (EncodingId == 1 || EncodingId == 10));
	}

	bool IsUcs4(unsigned PlatformId, unsigned EncodingId)
	{
		return (PlatformId == 3 && EncodingId == 10) || (PlatformId == 0 && EncodingId == 4);
	}
} // namespace

bool CFontTables::IsCollection(const uint8_t *pData, size_t Size)
{
	return CReader(pData, Size).U32(0) == Tag("ttcf");
}

bool CFontTables::Read(const uint8_t *pData, size_t Size)
{
	m_pData = pData;
	m_Size = Size;
	const CReader Reader(pData, Size);
	const unsigned NumTables = Reader.U16(4);
	size_t NameOffset = 0, NameLength = 0, CmapOffset = 0, CmapLength = 0;
	m_NumGlyphs = 0;
	for(unsigned i = 0; i < NumTables; ++i)
	{
		const size_t Record = 12 + i * 16;
		const uint32_t TableTag = Reader.U32(Record);
		const size_t Offset = Reader.U32(Record + 8);
		const size_t Length = Reader.U32(Record + 12);
		if(!Reader.Has(Offset, Length))
			continue;
		if(TableTag == Tag("name"))
		{
			NameOffset = Offset;
			NameLength = Length;
		}
		else if(TableTag == Tag("cmap"))
		{
			CmapOffset = Offset;
			CmapLength = Length;
		}
		else if(TableTag == Tag("maxp"))
		{
			m_NumGlyphs = Reader.U16(Offset + 4);
		}
	}
	m_FamilyName.clear();
	m_StyleName.clear();
	return NameLength > 0 && CmapLength > 0 && ReadNames(NameOffset, NameLength) && ReadCmap(CmapOffset, CmapLength);
}

bool CFontTables::ReadNames(size_t Offset, size_t Length)
{
	const CReader Reader(m_pData + Offset, Length);
	const unsigned Count = Reader.U16(2);
	const size_t Strings = Reader.U16(4);

	// One name as `tt_face_get_name` picks it: English from Windows, else
	// from the Macintosh, else from the Unicode platform.
	const auto &&Name = [&](unsigned NameId, std::string &Text) {
		int Windows = -1, AppleEnglish = -1, AppleRoman = -1, Unicode = -1;
		bool WindowsEnglish = false;
		for(unsigned i = 0; i < Count; ++i)
		{
			const size_t Record = 6 + i * 12;
			if(Reader.U16(Record + 6) != NameId || Reader.U16(Record + 8) == 0)
				continue;
			const unsigned PlatformId = Reader.U16(Record);
			const unsigned EncodingId = Reader.U16(Record + 2);
			const unsigned LanguageId = Reader.U16(Record + 4);
			if(PlatformId == 0 || PlatformId == 2)
				Unicode = i;
			else if(PlatformId == 1)
			{
				if(LanguageId == 0)
					AppleEnglish = i;
				else if(EncodingId == 0)
					AppleRoman = i;
			}
			else if(PlatformId == 3 && (Windows == -1 || (LanguageId & 0x3ff) == 0x009) && (EncodingId == 0 || EncodingId == 1 || EncodingId == 10))
			{
				WindowsEnglish = (LanguageId & 0x3ff) == 0x009;
				Windows = i;
			}
		}
		const int Apple = AppleEnglish >= 0 ? AppleEnglish : AppleRoman;
		int Found;
		bool Utf16;
		if(Windows >= 0 && !(Apple >= 0 && !WindowsEnglish))
		{
			Found = Windows;
			Utf16 = true;
		}
		else if(Apple >= 0)
		{
			Found = Apple;
			Utf16 = false;
		}
		else if(Unicode >= 0)
		{
			Found = Unicode;
			Utf16 = true;
		}
		else
			return false;
		const size_t Record = 6 + Found * 12;
		Text = NameText(Reader, Strings + Reader.U16(Record + 10), Reader.U16(Record + 8), Utf16);
		return true;
	};

	// Which names FreeType takes as family and style (`sfnt_load_face`)
	const CReader Font(m_pData, m_Size);
	bool Wws = false;
	for(unsigned i = 0; i < Font.U16(4); ++i)
	{
		const size_t Record = 12 + i * 16;
		if(Font.U32(Record) == Tag("OS/2"))
		{
			const size_t Os2 = Font.U32(Record + 8);
			Wws = Font.U16(Os2) != 0xffff && (Font.U16(Os2 + 62) & 256) != 0;
		}
	}
	if(Wws)
	{
		if(!Name(NAME_TYPOGRAPHIC_FAMILY, m_FamilyName))
			Name(NAME_FAMILY, m_FamilyName);
		if(!Name(NAME_TYPOGRAPHIC_SUBFAMILY, m_StyleName))
			Name(NAME_SUBFAMILY, m_StyleName);
	}
	else
	{
		if(!Name(NAME_WWS_FAMILY, m_FamilyName) && !Name(NAME_TYPOGRAPHIC_FAMILY, m_FamilyName))
			Name(NAME_FAMILY, m_FamilyName);
		if(!Name(NAME_WWS_SUBFAMILY, m_StyleName) && !Name(NAME_TYPOGRAPHIC_SUBFAMILY, m_StyleName))
			Name(NAME_SUBFAMILY, m_StyleName);
	}
	return !m_FamilyName.empty();
}

bool CFontTables::ReadCmap(size_t Offset, size_t Length)
{
	const CReader Reader(m_pData + Offset, Length);
	const unsigned Count = Reader.U16(2);
	// FreeType takes the last UCS-4 map, else the last Unicode map
	// (`find_unicode_charmap`). Variation sequences (format 14) map nothing.
	int Found = -1;
	for(int Pass = 0; Pass < 2 && Found < 0; ++Pass)
	{
		for(int i = (int)Count - 1; i >= 0; --i)
		{
			const size_t Record = 4 + i * 8;
			const unsigned PlatformId = Reader.U16(Record);
			const unsigned EncodingId = Reader.U16(Record + 2);
			const size_t Subtable = Reader.U32(Record + 4);
			const int Format = Reader.U16(Subtable);
			if(!Reader.Has(Subtable, 4) || Format == 14 || !IsUnicode(PlatformId, EncodingId) || (Pass == 0 && !IsUcs4(PlatformId, EncodingId)))
				continue;
			if(Format != 0 && Format != 4 && Format != 6 && Format != 10 && Format != 12 && Format != 13)
				continue;
			Found = i;
			m_CmapOffset = Offset + Subtable;
			m_CmapFormat = Format;
			break;
		}
	}
	return Found >= 0;
}

unsigned CFontTables::GlyphIndex(int Character) const
{
	if(m_CmapFormat < 0 || Character < 0)
		return 0;
	const CReader Reader(m_pData + m_CmapOffset, m_Size - m_CmapOffset);
	const uint32_t Code = Character;
	unsigned Glyph = 0;
	switch(m_CmapFormat)
	{
	case 0:
		Glyph = Code < 256 ? Reader.U8(6 + Code) : 0;
		break;
	case 4:
	{
		if(Code > 0xffff)
			break;
		const unsigned SegCount = Reader.U16(6) / 2;
		const size_t Ends = 14, Starts = 16 + SegCount * 2, Deltas = Starts + SegCount * 2, RangeOffsets = Deltas + SegCount * 2;
		// The first segment that ends at or after the character
		unsigned Low = 0, High = SegCount;
		while(Low < High)
		{
			const unsigned Middle = (Low + High) / 2;
			if(Reader.U16(Ends + Middle * 2) < Code)
				Low = Middle + 1;
			else
				High = Middle;
		}
		if(Low >= SegCount)
			break;
		const unsigned Start = Reader.U16(Starts + Low * 2);
		if(Code < Start)
			break;
		const unsigned Delta = Reader.U16(Deltas + Low * 2);
		const unsigned RangeOffset = Reader.U16(RangeOffsets + Low * 2);
		if(RangeOffset == 0)
			Glyph = (Code + Delta) & 0xffff;
		else
		{
			Glyph = Reader.U16(RangeOffsets + Low * 2 + RangeOffset + (Code - Start) * 2);
			if(Glyph != 0)
				Glyph = (Glyph + Delta) & 0xffff;
		}
		break;
	}
	case 6:
	{
		const uint32_t First = Reader.U16(6);
		const uint32_t Entries = Reader.U16(8);
		if(Code >= First && Code - First < Entries)
			Glyph = Reader.U16(10 + (Code - First) * 2);
		break;
	}
	case 10:
	{
		const uint32_t First = Reader.U32(12);
		const uint32_t Entries = Reader.U32(16);
		if(Code >= First && Code - First < Entries)
			Glyph = Reader.U16(20 + (size_t)(Code - First) * 2);
		break;
	}
	case 12:
	case 13:
	{
		const uint32_t Groups = Reader.U32(12);
		uint32_t Low = 0, High = Groups;
		while(Low < High)
		{
			const uint32_t Middle = Low + (High - Low) / 2;
			const size_t Group = 16 + (size_t)Middle * 12;
			if(Reader.U32(Group + 4) < Code)
				Low = Middle + 1;
			else
				High = Middle;
		}
		if(Low >= Groups)
			break;
		const size_t Group = 16 + (size_t)Low * 12;
		const uint32_t StartCode = Reader.U32(Group);
		if(Code >= StartCode)
			Glyph = m_CmapFormat == 12 ? Reader.U32(Group + 8) + (Code - StartCode) : Reader.U32(Group + 8);
		break;
	}
	default:
		break;
	}
	// FreeType answers no glyph for one the font does not have (`FT_Get_Char_Index`)
	return Glyph < m_NumGlyphs ? Glyph : 0;
}
