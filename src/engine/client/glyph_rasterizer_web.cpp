/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "font_tables.h"
#include "glyph_rasterizer.h"

#include <base/log.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

// The browser's side, in glyph_rasterizer_web.js. They run on the thread that
// calls them, whose fonts (`document.fonts` or the worker's `self.fonts`) the
// faces are added to.
extern "C" {
int ddnet_glyphs_add_face(int FaceId, const uint8_t *pData, int Size);
void ddnet_glyphs_measure(int FaceId, int FontSize, int Character, float *pBox);
float ddnet_glyphs_kerning(int FaceId, int FontSize, int Left, int Right);
void ddnet_glyphs_draw(const void *pGlyphs, int Count);
}

namespace
{
	// The face the system's fonts stand for
	constexpr int SYSTEM_FACE_ID = -1;

	class CWebFontFace final : public CFontFace
	{
	public:
		int m_Id;
		std::vector<uint8_t> m_vData;
		CFontTables m_Tables;

		CWebFontFace(int Id, std::vector<uint8_t> &&vData) :
			m_Id(Id), m_vData(std::move(vData)) {}

		const char *FamilyName() const override { return m_Tables.FamilyName(); }
		const char *StyleName() const override { return m_Tables.StyleName(); }
		unsigned GlyphIndex(int Character) const override { return m_Tables.GlyphIndex(Character); }
	};

	// Covers everything: the browser picks a font of the system for a
	// character, or draws the box of a missing one.
	class CWebSystemFace final : public CFontFace
	{
	public:
		const char *FamilyName() const override { return "system"; }
		const char *StyleName() const override { return ""; }
		unsigned GlyphIndex(int Character) const override { return 1; }
	};

	// What `ddnet_glyphs_draw` reads, one per glyph.
	struct SWebGlyph
	{
		int32_t m_FaceId;
		int32_t m_FontSize;
		int32_t m_Character;
		int32_t m_OffsetX;
		int32_t m_OffsetY;
		int32_t m_Width;
		int32_t m_Height;
		uint8_t *m_pPixels;
	};

	// The box of a glyph, from what `measureText` says about its ink:
	// distances from the pen, left and up positive.
	class CInk
	{
	public:
		float m_Advance;
		float m_Left;
		float m_Right;
		float m_Ascent;
		float m_Descent;
	};

	// Face, size and code point (21 bits) in one key. The sizes of text
	// drawn into textures are not limited to those of the atlas.
	uint64_t GlyphKey(int FaceId, int FontSize, int Character)
	{
		return (uint64_t)(FaceId + 1) << 48 | (uint64_t)(FontSize & 0x7ffffff) << 21 | (uint64_t)(Character & 0x1fffff);
	}

	class CWebGlyphRasterizer final : public IGlyphRasterizer
	{
		std::vector<std::unique_ptr<CWebFontFace>> m_vpFaces;
		CWebSystemFace m_SystemFace;
		std::unordered_map<uint64_t, float> m_Kerning;
		// What was measured and drawn, for the text that is drawn into
		// textures a character at a time and not kept in the atlas: the
		// numbers of the entities, the same few digits over and over.
		std::unordered_map<uint64_t, CInk> m_Measured;
		std::unordered_map<uint64_t, std::vector<uint8_t>> m_Drawn;
		static constexpr size_t MAX_REMEMBERED = 4096;

		CInk MeasureInk(int FaceId, int FontSize, int Character)
		{
			const uint64_t Key = GlyphKey(FaceId, FontSize, Character);
			if(const auto It = m_Measured.find(Key); It != m_Measured.end())
				return It->second;
			float aBox[5];
			ddnet_glyphs_measure(FaceId, FontSize, Character, aBox);
			if(m_Measured.size() >= MAX_REMEMBERED)
				m_Measured.clear();
			return m_Measured.emplace(Key, CInk{aBox[0], aBox[1], aBox[2], aBox[3], aBox[4]}).first->second;
		}

		int FaceId(const CFontFace *pFace) const
		{
			return pFace == &m_SystemFace ? SYSTEM_FACE_ID : static_cast<const CWebFontFace *>(pFace)->m_Id;
		}

	public:
		void LogVersion() const override
		{
			log_info("textrender", "Glyphs drawn by the browser (OffscreenCanvas)");
		}

		bool LoadFaces(std::vector<uint8_t> &&vData, const char *pPath, std::vector<CFontFace *> &vpFaces) override
		{
			if(CFontTables::IsCollection(vData.data(), vData.size()))
			{
				log_warn("textrender", "Font collection '%s' left to the fonts of the system", pPath);
				return false;
			}
			auto pFace = std::make_unique<CWebFontFace>((int)m_vpFaces.size(), std::move(vData));
			if(!pFace->m_Tables.Read(pFace->m_vData.data(), pFace->m_vData.size()))
			{
				log_error("textrender", "Failed to load font file '%s': no names or no Unicode character map", pPath);
				return false;
			}
			if(!ddnet_glyphs_add_face(pFace->m_Id, pFace->m_vData.data(), (int)pFace->m_vData.size()))
			{
				log_error("textrender", "Failed to load font file '%s': the browser did not take it", pPath);
				return false;
			}
			log_debug("textrender", "Loaded font face 0 '%s %s' from font file '%s'", pFace->FamilyName(), pFace->StyleName(), pPath);
			vpFaces.push_back(pFace.get());
			m_vpFaces.push_back(std::move(pFace));
			return true;
		}

		const CFontFace *SystemFace() const override
		{
			return &m_SystemFace;
		}

		bool Measure(const CFontFace *pFace, unsigned GlyphIndex, int Character, int FontSize, CGlyphMetrics &Metrics) override
		{
			const CInk Ink = MeasureInk(FaceId(pFace), FontSize, Character);
			// The whole pixels the ink touches
			const int Left = (int)std::floor(-Ink.m_Left);
			const int Right = (int)std::ceil(Ink.m_Right);
			const int Top = (int)std::ceil(Ink.m_Ascent);
			const int Bottom = -(int)std::ceil(Ink.m_Descent);
			if(Right > Left && Top > Bottom)
			{
				Metrics.m_Width = Right - Left;
				Metrics.m_Height = Top - Bottom;
				Metrics.m_OffsetX = Left;
				Metrics.m_OffsetY = Bottom;
			}
			else
			{
				Metrics = {};
			}
			Metrics.m_AdvanceX = (int)std::round(Ink.m_Advance);
			return true;
		}

		bool RasterizesInBatches() const override
		{
			// Every canvas read back costs, however few pixels it has
			return true;
		}

		bool Rasterize(std::span<const CGlyphRaster> vGlyphs) override
		{
			std::vector<SWebGlyph> vWebGlyphs;
			vWebGlyphs.reserve(vGlyphs.size());
			for(const CGlyphRaster &Glyph : vGlyphs)
			{
				if(Glyph.m_Metrics.m_Width <= 0 || Glyph.m_Metrics.m_Height <= 0)
					continue;
				const size_t Size = (size_t)Glyph.m_Metrics.m_Width * Glyph.m_Metrics.m_Height;
				const int Id = FaceId(Glyph.m_pFace);
				if(const auto It = m_Drawn.find(GlyphKey(Id, Glyph.m_FontSize, Glyph.m_Character)); It != m_Drawn.end() && It->second.size() == Size)
				{
					std::copy(It->second.begin(), It->second.end(), Glyph.m_pPixels);
					continue;
				}
				vWebGlyphs.push_back({Id, Glyph.m_FontSize, Glyph.m_Character, Glyph.m_Metrics.m_OffsetX, Glyph.m_Metrics.m_OffsetY, Glyph.m_Metrics.m_Width, Glyph.m_Metrics.m_Height, Glyph.m_pPixels});
			}
			if(vWebGlyphs.empty())
				return true;
			ddnet_glyphs_draw(vWebGlyphs.data(), (int)vWebGlyphs.size());
			if(m_Drawn.size() + vWebGlyphs.size() > MAX_REMEMBERED)
				m_Drawn.clear();
			for(const SWebGlyph &Glyph : vWebGlyphs)
				m_Drawn[GlyphKey(Glyph.m_FaceId, Glyph.m_FontSize, Glyph.m_Character)].assign(Glyph.m_pPixels, Glyph.m_pPixels + (size_t)Glyph.m_Width * Glyph.m_Height);
			return true;
		}

		vec2 Kerning(const CFontFace *pFace, int FontSize, int Left, int Right) override
		{
			const int Id = FaceId(pFace);
			// Face, size (at most 128) and two code points (21 bits each) in one key
			const uint64_t Key = (uint64_t)(Id + 1) << 56 | (uint64_t)(FontSize & 0xff) << 48 | (uint64_t)(Left & 0x1fffff) << 21 | (uint64_t)(Right & 0x1fffff);
			auto It = m_Kerning.find(Key);
			if(It == m_Kerning.end())
				It = m_Kerning.emplace(Key, std::round(ddnet_glyphs_kerning(Id, FontSize, Left, Right))).first;
			return vec2(It->second, 0.0f);
		}

		bool InkBox(const CFontFace *pFace, int Character, int FontWidth, int FontHeight, int &Width, int &BearingX) override
		{
			const CInk Ink = MeasureInk(FaceId(pFace), FontHeight, Character);
			// A font that is wider than high is the same font stretched
			const float Stretch = FontWidth > 0 ? (float)FontWidth / FontHeight : 1.0f;
			const int Left = (int)std::floor(-Ink.m_Left * Stretch);
			const int Right = (int)std::ceil(Ink.m_Right * Stretch);
			Width = std::max(Right - Left, 0);
			BearingX = Left;
			return true;
		}
	};
} // namespace

std::unique_ptr<IGlyphRasterizer> CreateGlyphRasterizer()
{
	return std::make_unique<CWebGlyphRasterizer>();
}
