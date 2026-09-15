#ifndef GAME_MAP_DOCUMENT_IMAGES_H
#define GAME_MAP_DOCUMENT_IMAGES_H

#include <engine/client/asset_loader.h>
#include <engine/graphics.h>

#include <game/map/document/map_state.h>
#include <game/map/render_interfaces.h>

#include <string>
#include <vector>

class IStorage;

/**
 * How a version of the map samples each of its images.
 *
 * A tileset is sampled as one texture per tile, which the graphics card wants
 * as an array of layers; a quad samples the picture as a whole. An image that
 * is both is loaded as both, and one that nothing samples is not loaded at all
 * - a map that names twenty images and draws with three should cost three.
 */
enum
{
	IMAGEUSE_TILES = 1,
	IMAGEUSE_QUADS = 2,
};

/** One entry per image of the version, `0` for one that nothing draws with. */
std::vector<unsigned char> ImageUsage(const map_document::CMapState &Map);

/**
 * The pictures of a map document, on the graphics card.
 *
 * This is the same job `CToolMapImages` does for a map that was read from a
 * file, with the two differences that a document brings: its pixels are
 * already unpacked, because a version holds what it holds rather than a file to
 * read again, and the version changes while somebody is looking at it. So
 * nothing here waits: `Use` says what the version names, `Update` puts on the
 * graphics card whatever has arrived since, and a layer whose picture is not
 * there yet is drawn without one. An editor that stopped to wait would be a
 * page that stopped answering.
 *
 * An image keeps its texture across versions as long as what it is made of
 * stays the same - the pixels for an embedded image, the name for an external
 * one - so renaming an image, or changing anything else about the map, uploads
 * nothing.
 */
class CDocumentImages final : public IMapImages
{
public:
	/**
	 * @param pGraphics Where the pictures are put, and what outlives them.
	 * @param pStorage Where an external picture is read from.
	 * @param pAssetLoader Who reads and decodes it, on a thread of its own.
	 * @param pShared Where the pictures that do not come out of the map are
	 * asked for - the entities sheet, the speedup arrow, the overlays. They
	 * belong to `data/` rather than to the map, so whoever brings `data/`
	 * brings them; `nullptr` for a program that draws no entity overlay.
	 * @param pLogContext What a failed picture is reported under.
	 */
	CDocumentImages(IGraphics *pGraphics, IStorage *pStorage, CAssetLoader *pAssetLoader, IMapImages *pShared, const char *pLogContext);
	~CDocumentImages() override;

	CDocumentImages(const CDocumentImages &) = delete;
	CDocumentImages &operator=(const CDocumentImages &) = delete;

	/**
	 * Says which pictures this version draws with, asks for the ones that are
	 * new and gives up the ones that nothing names any more.
	 *
	 * Cheap enough to call after every change: what it does when nothing about
	 * the images changed is compare a handful of pointers.
	 */
	void Use(const map_document::CMapState &Map);

	/** Puts on the graphics card what has arrived. Once a frame. */
	void Update();

	/** Whether something is still on its way. */
	bool Loading() const { return !m_vLoading.empty(); }

	IGraphics::CTextureHandle Get(int Index) const override;
	int Num() const override { return (int)m_vImages.size(); }

	IGraphics::CTextureHandle GetEntities(EMapImageEntityLayerType EntityLayerType) override;
	IGraphics::CTextureHandle GetSpeedupArrow() override;
	IGraphics::CTextureHandle GetTuneColors() override;
	IGraphics::CTextureHandle GetOverlayBottom() override;
	IGraphics::CTextureHandle GetOverlayTop() override;
	IGraphics::CTextureHandle GetOverlayCenter() override;

private:
	/**
	 * What an image is made of, as far as a texture is concerned: two images
	 * that answer this the same way can share one.
	 */
	class CSource
	{
	public:
		/** The pixels of an embedded image, as the node they are. */
		const void *m_pData = nullptr;
		/** The file an external image is read from, empty for an embedded one. */
		std::string m_Path;
		int m_Flags = 0;

		bool operator==(const CSource &Other) const
		{
			return m_pData == Other.m_pData && m_Path == Other.m_Path && m_Flags == Other.m_Flags;
		}
		bool operator!=(const CSource &Other) const { return !(*this == Other); }
	};

	class CImage
	{
	public:
		CSource m_Source;
		IGraphics::CTextureHandle m_Texture;
	};

	class CLoading
	{
	public:
		size_t m_Index = 0;
		CSource m_Source;
		CImageResource m_Resource;
	};

	void Release(IGraphics::CTextureHandle *pTexture);
	/** What the version says this image is made of, or nothing to draw. */
	static CSource SourceOf(const map_document::CImage &Image, unsigned char Usage);

	IGraphics *m_pGraphics;
	IStorage *m_pStorage;
	CAssetLoader *m_pAssetLoader;
	IMapImages *m_pShared;
	const char *m_pLogContext;

	std::vector<CImage> m_vImages;
	/**
	 * What has been asked for and has not arrived yet. A picture that arrives
	 * for an image the map has changed since is dropped rather than uploaded,
	 * which is what the source in here is compared for.
	 */
	std::vector<CLoading> m_vLoading;
};

#endif // GAME_MAP_DOCUMENT_IMAGES_H
