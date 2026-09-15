#include <base/log.h>
#include <base/str.h>

#include <engine/image.h>
#include <engine/storage.h>

#include <game/map/document_images.h>

#include <algorithm>
#include <utility>
#include <variant>

using namespace map_document;

namespace
{
	// One map's worth of pictures is one owner and one generation: a document
	// is one map, and which of its pictures are still wanted is decided here
	// rather than by the loader.
	constexpr int ASSET_OWNER = 0;
	constexpr uint64_t ASSET_GENERATION = 1;

	constexpr LOG_COLOR WARNING_LOG_COLOR = LOG_COLOR{255, 255, 0};
} // namespace

std::vector<unsigned char> ImageUsage(const CMapState &Map)
{
	std::vector<unsigned char> vUsage(Map.NumImages(), 0);
	const auto Mark = [&vUsage](int Image, unsigned char Use) {
		if(Image >= 0 && (size_t)Image < vUsage.size())
			vUsage[Image] |= Use;
	};
	for(size_t Group = 0; Group < Map.NumGroups(); ++Group)
	{
		for(size_t Layer = 0; Layer < Map.NumLayers(Group); ++Layer)
		{
			const CLayer *pLayer = Map.Layer(Group, Layer);
			if(std::holds_alternative<CTileLayer>(*pLayer))
				Mark(std::get<CTileLayer>(*pLayer).m_Image, IMAGEUSE_TILES);
			else if(std::holds_alternative<CQuadLayer>(*pLayer))
				Mark(std::get<CQuadLayer>(*pLayer).m_Image, IMAGEUSE_QUADS);
		}
	}
	return vUsage;
}

CDocumentImages::CDocumentImages(IGraphics *pGraphics, IStorage *pStorage, CAssetLoader *pAssetLoader, IMapImages *pShared, const char *pLogContext) :
	m_pGraphics(pGraphics), m_pStorage(pStorage), m_pAssetLoader(pAssetLoader), m_pShared(pShared), m_pLogContext(pLogContext)
{
}

CDocumentImages::~CDocumentImages()
{
	for(CImage &Image : m_vImages)
		Release(&Image.m_Texture);
}

void CDocumentImages::Release(IGraphics::CTextureHandle *pTexture)
{
	if(pTexture->IsValid())
		m_pGraphics->UnloadTexture(pTexture);
}

CDocumentImages::CSource CDocumentImages::SourceOf(const map_document::CImage &Image, unsigned char Usage)
{
	CSource Source;
	if(Usage == 0)
		return Source; // nothing draws with it, so there is nothing to load
	Source.m_Flags = ((Usage & IMAGEUSE_TILES) != 0 ? IGraphics::TEXLOAD_LAYERED : 0) |
			 ((Usage & IMAGEUSE_QUADS) != 0 ? 0 : IGraphics::TEXLOAD_NO_2D_TEXTURE);
	if(Image.m_External)
	{
		if(Image.m_Name.empty())
			return CSource();
		// The 0.7 maps that carry a second picture under `<name>_0.7.png` are
		// read as what they name; which file that is, is a question about the
		// file the map came out of rather than about the version being edited.
		char aPath[IO_MAX_PATH_LENGTH];
		str_format(aPath, sizeof(aPath), "mapres/%s.png", Image.m_Name.c_str());
		Source.m_Path = aPath;
		return Source;
	}
	const size_t Pixels = (size_t)std::max(Image.m_Width, 0) * (size_t)std::max(Image.m_Height, 0);
	if(Pixels == 0 || Image.m_Data.Size() < Pixels * CImageInfo::PixelSize(CImageInfo::FORMAT_RGBA))
		return CSource();
	Source.m_pData = Image.m_Data.Id();
	return Source;
}

void CDocumentImages::Use(const CMapState &Map)
{
	const std::vector<unsigned char> vUsage = ImageUsage(Map);
	std::vector<CImage> vNext(Map.NumImages());
	for(size_t i = 0; i < vNext.size(); ++i)
		vNext[i].m_Source = SourceOf(*Map.Image(i), vUsage[i]);

	// A picture that is made of the same thing as one we already have is that
	// picture: the texture is handed over rather than uploaded again, which is
	// what makes calling this after every change cost nothing.
	for(CImage &Wanted : vNext)
	{
		if(Wanted.m_Source == CSource())
			continue;
		for(CImage &Held : m_vImages)
		{
			if(Held.m_Texture.IsValid() && Held.m_Source == Wanted.m_Source)
			{
				Wanted.m_Texture = Held.m_Texture;
				Held.m_Texture = IGraphics::CTextureHandle();
				break;
			}
		}
	}
	for(CImage &Held : m_vImages)
		Release(&Held.m_Texture);
	m_vImages = std::move(vNext);

	// What was asked for and is still wanted stays on its way; the rest is
	// dropped when it lands.
	m_vLoading.erase(std::remove_if(m_vLoading.begin(), m_vLoading.end(), [this](const CLoading &Loading) {
		return Loading.m_Index >= m_vImages.size() || m_vImages[Loading.m_Index].m_Source != Loading.m_Source;
	}),
		m_vLoading.end());

	for(size_t i = 0; i < m_vImages.size(); ++i)
	{
		const CSource &Source = m_vImages[i].m_Source;
		if(m_vImages[i].m_Texture.IsValid() || Source == CSource())
			continue;
		if(std::any_of(m_vLoading.begin(), m_vLoading.end(), [i](const CLoading &Loading) { return Loading.m_Index == i; }))
			continue;

		if(Source.m_pData != nullptr)
		{
			// The pixels are already unpacked - the version holds them - so
			// there is nothing to decode on a job thread and the upload is the
			// whole of it. The image does not own what it points at, and the
			// upload copies before it returns.
			const map_document::CImage &Held = *Map.Image(i);
			CImageInfo Info;
			Info.m_Width = (size_t)Held.m_Width;
			Info.m_Height = (size_t)Held.m_Height;
			Info.m_Format = CImageInfo::FORMAT_RGBA;
			Info.m_pData = const_cast<uint8_t *>(Held.m_Data.All().data());
			char aName[IO_MAX_PATH_LENGTH];
			str_format(aName, sizeof(aName), "embedded: %s", Held.m_Name.c_str());
			m_vImages[i].m_Texture = m_pGraphics->LoadTextureRaw(Info, Source.m_Flags, aName);
			Info.m_pData = nullptr;
			continue;
		}

		CLoading Loading;
		Loading.m_Index = i;
		Loading.m_Source = Source;
		Loading.m_Resource = m_pAssetLoader->LoadImageFile(m_pStorage, Source.m_Path.c_str(), IStorage::TYPE_ALL, ASSET_OWNER, ASSET_GENERATION);
		m_vLoading.push_back(std::move(Loading));
	}
}

void CDocumentImages::Update()
{
	if(m_vLoading.empty())
		return;
	m_pAssetLoader->Update();
	for(auto It = m_vLoading.begin(); It != m_vLoading.end();)
	{
		if(!It->m_Resource.IsFinished())
		{
			++It;
			continue;
		}
		const bool StillWanted = It->m_Index < m_vImages.size() && m_vImages[It->m_Index].m_Source == It->m_Source;
		if(StillWanted && It->m_Resource.IsReady(ASSET_GENERATION))
		{
			CImageInfo Image = It->m_Resource.TakeImage();
			m_vImages[It->m_Index].m_Texture = m_pGraphics->LoadTextureRawMove(Image, It->m_Source.m_Flags, It->m_Resource.Path());
		}
		else if(StillWanted)
		{
			log_warn_color(WARNING_LOG_COLOR, m_pLogContext, "Failed to load map image '%s'.", It->m_Resource.Path());
		}
		It = m_vLoading.erase(It);
	}
}

IGraphics::CTextureHandle CDocumentImages::Get(int Index) const
{
	if(Index >= 0 && (size_t)Index < m_vImages.size())
		return m_vImages[Index].m_Texture;
	return IGraphics::CTextureHandle();
}

IGraphics::CTextureHandle CDocumentImages::GetEntities(EMapImageEntityLayerType EntityLayerType)
{
	return m_pShared != nullptr ? m_pShared->GetEntities(EntityLayerType) : IGraphics::CTextureHandle();
}

IGraphics::CTextureHandle CDocumentImages::GetSpeedupArrow()
{
	return m_pShared != nullptr ? m_pShared->GetSpeedupArrow() : IGraphics::CTextureHandle();
}

IGraphics::CTextureHandle CDocumentImages::GetTuneColors()
{
	return m_pShared != nullptr ? m_pShared->GetTuneColors() : IGraphics::CTextureHandle();
}

IGraphics::CTextureHandle CDocumentImages::GetOverlayBottom()
{
	return m_pShared != nullptr ? m_pShared->GetOverlayBottom() : IGraphics::CTextureHandle();
}

IGraphics::CTextureHandle CDocumentImages::GetOverlayTop()
{
	return m_pShared != nullptr ? m_pShared->GetOverlayTop() : IGraphics::CTextureHandle();
}

IGraphics::CTextureHandle CDocumentImages::GetOverlayCenter()
{
	return m_pShared != nullptr ? m_pShared->GetOverlayCenter() : IGraphics::CTextureHandle();
}
