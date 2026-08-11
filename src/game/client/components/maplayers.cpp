/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "maplayers.h"

#include <game/client/gameclient.h>
#include <game/localization.h>

CMapLayers::CMapLayers(ERenderType Type, bool OnlineOnly)
{
	m_pLayers = nullptr;
	m_pImages = nullptr;
	m_Type = Type;
	m_OnlineOnly = OnlineOnly;

	// static parameters for ingame rendering
	m_Params.m_RenderType = m_Type;
	m_Params.m_RenderInvalidTiles = false;
	m_Params.m_TileAndQuadBuffering = true;
	m_Params.m_RenderTileBorder = true;
}

void CMapLayers::OnInit()
{
	m_MapRenderer.Clear();
}

void CMapLayers::Unload()
{
	m_MapRenderer.Clear();
	m_EnvEvaluator = CEnvelopeState();
	m_pLayers = nullptr;
	m_pImages = nullptr;
}

void CMapLayers::OnMapLoad()
{
	dbg_assert(m_pLayers != nullptr && m_pImages != nullptr, "map layers must be explicitly bound before loading");
	Load(m_pLayers, m_pImages);
}

void CMapLayers::Load(CLayers *pLayers, IMapImages *pImages)
{
	dbg_assert(pLayers != nullptr && pImages != nullptr, "map layers require layers and images");
	Unload();
	m_pLayers = pLayers;
	m_pImages = pImages;

	FRenderUploadCallback FRenderCallback = [&](const char *pTitle, const char *pMessage, int IncreaseCounter) { GameClient()->m_Menus.RenderLoading(pTitle, pMessage, IncreaseCounter); };
	auto FRenderCallbackOptional = std::make_optional<FRenderUploadCallback>(FRenderCallback);

	const char *pLoadingTitle = Localize("Loading map");
	const char *pLoadingMessage = Localize("Uploading map data to GPU");
	GameClient()->m_Menus.RenderLoading(pLoadingTitle, pLoadingMessage, 0);

	// can't do that in CMapLayers::OnInit, because some of this interfaces are not available yet
	m_MapRenderer.OnInit(Graphics(), TextRender(), RenderMap());

	m_EnvEvaluator = CEnvelopeState(m_pLayers->Map(), m_OnlineOnly);
	m_EnvEvaluator.OnInterfacesInit(GameClient());
	m_MapRenderer.Load(m_Type, m_pLayers, m_pImages, &m_EnvEvaluator, FRenderCallbackOptional);
}

void CMapLayers::OnRender(const CRenderContext &Context)
{
	if(m_OnlineOnly && !Context.m_Time.m_IsGameActive)
		return;

	Render(Context.m_View.CameraPosition(), Context.m_View.Zoom());
}

void CMapLayers::Render(vec2 Center, float Zoom)
{
	// dynamic parameters for ingame rendering
	m_Params.m_EntityOverlayVal = m_Type == RENDERTYPE_FULL_DESIGN ? 0 : g_Config.m_ClOverlayEntities;
	m_Params.m_Center = Center;
	m_Params.m_Zoom = Zoom;
	m_Params.m_RenderText = g_Config.m_ClTextEntities;
	m_Params.m_DebugRenderGroupClips = g_Config.m_DbgRenderGroupClips;
	m_Params.m_DebugRenderQuadClips = g_Config.m_DbgRenderQuadClips;
	m_Params.m_DebugRenderClusterClips = g_Config.m_DbgRenderClusterClips;
	m_Params.m_DebugRenderTileClips = g_Config.m_DbgRenderTileClips;

	m_MapRenderer.Render(m_Params);
}
