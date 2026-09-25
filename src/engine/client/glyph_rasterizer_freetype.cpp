/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "glyph_rasterizer.h"

#include <base/dbg.h>
#include <base/log.h>
#include <base/mem.h>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace
{
	class CFreeTypeFace final : public CFontFace
	{
	public:
		FT_Face m_Face;

		explicit CFreeTypeFace(FT_Face Face) :
			m_Face(Face) {}
		~CFreeTypeFace() override { FT_Done_Face(m_Face); }

		CFreeTypeFace(const CFreeTypeFace &) = delete;
		CFreeTypeFace &operator=(const CFreeTypeFace &) = delete;

		const char *FamilyName() const override { return m_Face->family_name; }
		const char *StyleName() const override { return m_Face->style_name; }

		unsigned GlyphIndex(int Character) const override
		{
			if(!m_Face->charmap)
				return 0;
			return FT_Get_Char_Index(m_Face, (FT_ULong)Character);
		}
	};

	FT_Face FaceOf(const CFontFace *pFace)
	{
		FT_Face Face = static_cast<const CFreeTypeFace *>(pFace)->m_Face;
		dbg_assert(Face != nullptr, "Glyph without a face");
		return Face;
	}

	class CFreeTypeRasterizer final : public IGlyphRasterizer
	{
		FT_Library m_Library = nullptr;
		// The faces point into the bytes of their files.
		std::vector<std::vector<uint8_t>> m_vFontData;
		std::vector<std::unique_ptr<CFreeTypeFace>> m_vpFaces;

		// The glyph whose bitmap is in its face's glyph slot.
		FT_Face m_LoadedFace = nullptr;
		unsigned m_LoadedGlyphIndex = 0;
		int m_LoadedFontSize = 0;

		bool Load(FT_Face Face, unsigned GlyphIndex, int Character, int FontSize)
		{
			m_LoadedFace = nullptr;
			FT_Set_Pixel_Sizes(Face, 0, FontSize);
			if(FT_Load_Glyph(Face, GlyphIndex, FT_LOAD_RENDER | FT_LOAD_NO_BITMAP))
			{
				log_debug("textrender", "Error loading glyph. Chr=%d GlyphIndex=%u", Character, GlyphIndex);
				return false;
			}
			const FT_Bitmap *pBitmap = &Face->glyph->bitmap;
			if(pBitmap->pixel_mode != FT_PIXEL_MODE_GRAY)
			{
				log_debug("textrender", "Error loading glyph, unsupported pixel mode. Chr=%d GlyphIndex=%u PixelMode=%d", Character, GlyphIndex, pBitmap->pixel_mode);
				return false;
			}
			m_LoadedFace = Face;
			m_LoadedGlyphIndex = GlyphIndex;
			m_LoadedFontSize = FontSize;
			return true;
		}

	public:
		CFreeTypeRasterizer()
		{
			FT_Init_FreeType(&m_Library);
		}

		~CFreeTypeRasterizer() override
		{
			m_vpFaces.clear();
			if(m_Library != nullptr)
				FT_Done_FreeType(m_Library);
		}

		CFreeTypeRasterizer(const CFreeTypeRasterizer &) = delete;
		CFreeTypeRasterizer &operator=(const CFreeTypeRasterizer &) = delete;

		void LogVersion() const override
		{
			int LMajor, LMinor, LPatch;
			FT_Library_Version(m_Library, &LMajor, &LMinor, &LPatch);
			log_info("textrender", "Freetype version %d.%d.%d (compiled = %d.%d.%d)", LMajor, LMinor, LPatch, FREETYPE_MAJOR, FREETYPE_MINOR, FREETYPE_PATCH);
		}

		bool LoadFaces(std::vector<uint8_t> &&vData, const char *pPath, std::vector<CFontFace *> &vpFaces) override
		{
			m_vFontData.push_back(std::move(vData));
			const FT_Byte *pFontData = m_vFontData.back().data();
			const FT_Long FontDataLength = (FT_Long)m_vFontData.back().size();

			// Face -1 returns the number of faces in the collection
			FT_Face FtFace;
			const FT_Error CollectionLoadError = FT_New_Memory_Face(m_Library, pFontData, FontDataLength, -1, &FtFace);
			if(CollectionLoadError)
			{
				log_error("textrender", "Failed to load font file '%s': %s", pPath, FT_Error_String(CollectionLoadError));
				return false;
			}
			const FT_Long NumFaces = FtFace->num_faces;
			FT_Done_Face(FtFace);

			bool Any = false;
			for(FT_Long FaceIndex = 0; FaceIndex < NumFaces; ++FaceIndex)
			{
				const FT_Error FaceLoadError = FT_New_Memory_Face(m_Library, pFontData, FontDataLength, FaceIndex, &FtFace);
				if(FaceLoadError)
				{
					log_error("textrender", "Failed to load font face %ld from font file '%s': %s", FaceIndex, pPath, FT_Error_String(FaceLoadError));
					FT_Done_Face(FtFace);
					continue;
				}
				m_vpFaces.push_back(std::make_unique<CFreeTypeFace>(FtFace));
				vpFaces.push_back(m_vpFaces.back().get());
				Any = true;
				log_debug("textrender", "Loaded font face %ld '%s %s' from font file '%s'", FaceIndex, FtFace->family_name, FtFace->style_name, pPath);
			}
			if(!Any)
			{
				log_error("textrender", "Failed to load font file '%s': no font faces could be loaded", pPath);
			}
			return Any;
		}

		const CFontFace *SystemFace() const override
		{
			return nullptr;
		}

		bool Measure(const CFontFace *pFace, unsigned GlyphIndex, int Character, int FontSize, CGlyphMetrics &Metrics) override
		{
			FT_Face Face = FaceOf(pFace);
			if(!Load(Face, GlyphIndex, Character, FontSize))
				return false;
			FT_GlyphSlot Slot = Face->glyph;
			Metrics.m_Width = Slot->bitmap.width;
			Metrics.m_Height = Slot->bitmap.rows;
			Metrics.m_OffsetX = Slot->metrics.horiBearingX >> 6;
			Metrics.m_OffsetY = -((Slot->metrics.height >> 6) - (Slot->metrics.horiBearingY >> 6));
			Metrics.m_AdvanceX = Slot->advance.x >> 6;
			return true;
		}

		bool RasterizesInBatches() const override
		{
			return false;
		}

		bool Rasterize(std::span<const CGlyphRaster> vGlyphs) override
		{
			bool Success = true;
			for(const CGlyphRaster &Glyph : vGlyphs)
			{
				FT_Face Face = FaceOf(Glyph.m_pFace);
				// The bitmap of the glyph measured last is still in its slot.
				if((m_LoadedFace != Face || m_LoadedGlyphIndex != Glyph.m_GlyphIndex || m_LoadedFontSize != Glyph.m_FontSize) &&
					!Load(Face, Glyph.m_GlyphIndex, Glyph.m_Character, Glyph.m_FontSize))
				{
					Success = false;
					continue;
				}
				const FT_Bitmap *pBitmap = &Face->glyph->bitmap;
				if((int)pBitmap->width != Glyph.m_Metrics.m_Width || (int)pBitmap->rows != Glyph.m_Metrics.m_Height)
				{
					Success = false;
					continue;
				}
				for(unsigned py = 0; py < pBitmap->rows; ++py)
				{
					mem_copy(&Glyph.m_pPixels[py * pBitmap->width], &pBitmap->buffer[py * pBitmap->width], pBitmap->width);
				}
			}
			return Success;
		}

		vec2 Kerning(const CFontFace *pFace, int FontSize, int Left, int Right) override
		{
			FT_Face Face = FaceOf(pFace);
			FT_Vector Kerning = {0, 0};
			FT_Set_Pixel_Sizes(Face, 0, FontSize);
			FT_Get_Kerning(Face, Left, Right, FT_KERNING_DEFAULT, &Kerning);
			m_LoadedFace = nullptr;
			return vec2(Kerning.x >> 6, Kerning.y >> 6);
		}

		bool InkBox(const CFontFace *pFace, int Character, int FontWidth, int FontHeight, int &Width, int &BearingX) override
		{
			FT_Face Face = FaceOf(pFace);
			m_LoadedFace = nullptr;
			FT_Set_Pixel_Sizes(Face, FontWidth, FontHeight);
#if FREETYPE_MAJOR >= 2 && FREETYPE_MINOR >= 7 && (FREETYPE_MINOR > 7 || FREETYPE_PATCH >= 1)
			const FT_Int32 FTFlags = FT_LOAD_BITMAP_METRICS_ONLY | FT_LOAD_NO_BITMAP;
#else
			const FT_Int32 FTFlags = FT_LOAD_RENDER | FT_LOAD_NO_BITMAP;
#endif
			if(FT_Load_Char(Face, Character, FTFlags))
			{
				log_debug("textrender", "Error loading glyph. Chr=%d", Character);
				return false;
			}
			Width = Face->glyph->metrics.width >> 6;
			BearingX = Face->glyph->metrics.horiBearingX >> 6;
			return true;
		}
	};
} // namespace

std::unique_ptr<IGlyphRasterizer> CreateGlyphRasterizer()
{
	return std::make_unique<CFreeTypeRasterizer>();
}
