#include "image.h"

#include <engine/gfx/image_manipulation.h>
#include <engine/graphics.h>
#include <engine/storage.h>

#include <game/editor/editor.h>
#include <game/mapitems.h>

CEditorImage::CEditorImage(CEditorMap *pMap) :
	CMapObject(pMap),
	m_Automapper(pMap)
{
	m_Texture.Invalidate();
}

CEditorImage::~CEditorImage()
{
	Graphics()->UnloadTexture(&m_Texture);
}

void CEditorImage::OnAttach(CEditorMap *pMap)
{
	CMapObject::OnAttach(pMap);
	m_Automapper.OnAttach(pMap);
}

void CEditorImage::AnalyseTileFlags()
{
	std::fill(std::begin(m_aTileFlags), std::end(m_aTileFlags), 0);

	size_t TileWidth = m_Width / 16;
	size_t TileHeight = m_Height / 16;
	if(TileWidth == TileHeight && m_Format == CImageInfo::FORMAT_RGBA)
	{
		int TileId = 0;
		for(size_t ty = 0; ty < 16; ty++)
			for(size_t tx = 0; tx < 16; tx++, TileId++)
			{
				bool Opaque = true;
				for(size_t x = 0; x < TileWidth; x++)
					for(size_t y = 0; y < TileHeight; y++)
					{
						size_t p = (ty * TileWidth + y) * m_Width + tx * TileWidth + x;
						if(m_pData[p * 4 + 3] < 250)
						{
							Opaque = false;
							break;
						}
					}

				if(Opaque)
					m_aTileFlags[TileId] |= TILEFLAG_OPAQUE;
			}
	}
}

void CEditorImage::Upload(int LoadFlags, bool GiveUpData)
{
	if(m_Width % 16 != 0 || m_Height % 16 != 0)
		LoadFlags &= ~IGraphics::TEXLOAD_LAYERED;
	Graphics()->UnloadTexture(&m_Texture);
	m_TextureFlags = LoadFlags;
	if(!GiveUpData)
	{
		m_Texture = Graphics()->LoadTextureRaw(*this, LoadFlags, m_aName);
		return;
	}
	const size_t Width = m_Width;
	const size_t Height = m_Height;
	const CImageInfo::EImageFormat Format = m_Format;
	m_Texture = Graphics()->LoadTextureRawMove(*this, LoadFlags, m_aName);
	m_Width = Width;
	m_Height = Height;
	m_Format = Format;
}

IGraphics::CTextureHandle CEditorImage::Texture(bool Layered)
{
	const bool Missing = Layered ? (m_TextureFlags & IGraphics::TEXLOAD_LAYERED) == 0 : (m_TextureFlags & IGraphics::TEXLOAD_NO_2D_TEXTURE) != 0;
	if(!Missing || (Layered && (m_Width % 16 != 0 || m_Height % 16 != 0)))
		return m_Texture;

	const bool GiveUpData = m_pData == nullptr;
	if(GiveUpData)
	{
		char aBuf[IO_MAX_PATH_LENGTH];
		str_format(aBuf, sizeof(aBuf), "mapres/%s.png", m_aName);
		if(!Graphics()->LoadPng(*this, aBuf, IStorage::TYPE_ALL))
		{
			// The render path asks every frame, so a file that is not there
			// is not looked for again.
			m_TextureFlags = Layered ? (m_TextureFlags | IGraphics::TEXLOAD_LAYERED) : (m_TextureFlags & ~IGraphics::TEXLOAD_NO_2D_TEXTURE);
			return m_Texture;
		}
		ConvertToRgba(*this);
	}
	Upload(Layered ? (m_TextureFlags | IGraphics::TEXLOAD_LAYERED) : (m_TextureFlags & ~IGraphics::TEXLOAD_NO_2D_TEXTURE), GiveUpData);
	return m_Texture;
}

void CEditorImage::Free()
{
	Graphics()->UnloadTexture(&m_Texture);
	m_TextureFlags = 0;
	m_Automapper.Unload();
	CImageInfo::Free();
}

CEditorImage &CEditorImage::operator=(CImageInfo &&Other)
{
	CImageInfo *pThis = this;
	*pThis = std::move(Other);
	return *this;
}
