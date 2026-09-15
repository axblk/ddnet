#ifndef GAME_EDITOR_MAPITEMS_IMAGE_H
#define GAME_EDITOR_MAPITEMS_IMAGE_H

#include <base/types.h>

#include <engine/graphics.h>

#include <game/editor/auto_map.h>
#include <game/editor/map_object.h>

class CEditorImage : public CImageInfo, public CMapObject
{
public:
	explicit CEditorImage(CEditorMap *pMap);
	~CEditorImage() override;
	void OnAttach(CEditorMap *pMap) override;

	void AnalyseTileFlags();
	void Free();

	/**
	 * Puts the image on the GPU. A tile layer samples it by tile index, a quad
	 * layer and the editor's own previews as a plain image, and every variant
	 * that is asked for costs its own copy, so only the asked ones are there.
	 * Hands the pixels over rather than copying them when they are not needed
	 * afterwards; the dimensions survive that, the editor still reads them.
	 */
	void Upload(int LoadFlags, bool GiveUpData);

	/**
	 * The image on the GPU, uploading the wanted variant if it is not there
	 * yet. That reads the file again for an external image, which keeps no
	 * pixels, so it is for a layer that was just given this image.
	 */
	IGraphics::CTextureHandle Texture(bool Layered);

	CEditorImage &operator=(CImageInfo &&Other);

	IGraphics::CTextureHandle m_Texture;
	int m_TextureFlags = 0;
	int m_External = 0;
	char m_aName[IO_MAX_PATH_LENGTH] = "";
	unsigned char m_aTileFlags[256];

	CAutomapper m_Automapper;
};

#endif
