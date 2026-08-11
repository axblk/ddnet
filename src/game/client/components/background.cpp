#include "background.h"

#include <base/str.h>

#include <engine/map.h>
#include <engine/shared/config.h>

#include <game/client/components/mapimages.h>
#include <game/client/components/maplayers.h>
#include <game/client/gameclient.h>
#include <game/layers.h>
#include <game/localization.h>

CBackground::CBackground(ERenderType MapType, bool OnlineOnly) :
	CMapLayers(MapType, OnlineOnly)
{
	m_pLayers = new CLayers;
	m_pBackgroundLayers = m_pLayers;
	m_pImages = nullptr;
	m_pBackgroundImages = nullptr;
	m_Loaded = false;
	m_UseCurrentMap = false;
	m_aMapName[0] = '\0';
}

CBackground::~CBackground()
{
	delete m_pBackgroundLayers;
	delete m_pBackgroundImages;
}

void CBackground::OnInit()
{
	m_pBackgroundMap = CreateMap();
	m_pMap = m_pBackgroundMap.get();
	if(g_Config.m_ClBackgroundEntities[0] != '\0' && str_comp(g_Config.m_ClBackgroundEntities, CURRENT_MAP))
		LoadBackground();
}

void CBackground::OnInterfacesInit(CGameClient *pClient)
{
	CComponentInterfaces::OnInterfacesInit(pClient);
	m_pBackgroundImages = new CMapRenderImages(pClient->m_MapImages);
	m_pBackgroundImages->OnInterfacesInit(pClient);
	m_pImages = m_pBackgroundImages;
}

void CBackground::LoadBackground()
{
	CMapLayers::Unload();
	m_pBackgroundImages->Unload();
	if(m_Loaded && m_pMap == m_pBackgroundMap.get())
		m_pMap->Unload();

	m_Loaded = false;
	m_UseCurrentMap = false;
	m_pMap = m_pBackgroundMap.get();
	m_pLayers = m_pBackgroundLayers;
	m_pImages = m_pBackgroundImages;

	str_copy(m_aMapName, g_Config.m_ClBackgroundEntities);
	if(g_Config.m_ClBackgroundEntities[0] != '\0')
	{
		char aBuf[IO_MAX_PATH_LENGTH];
		str_format(aBuf, sizeof(aBuf), "maps/%s%s", g_Config.m_ClBackgroundEntities, str_endswith(g_Config.m_ClBackgroundEntities, ".map") ? "" : ".map");
		if(str_comp(g_Config.m_ClBackgroundEntities, CURRENT_MAP) == 0)
		{
			m_UseCurrentMap = true;
			return;
		}
		if(m_pMap->Load(g_Config.m_ClBackgroundEntities, Storage(), aBuf, IStorage::TYPE_ALL))
		{
			m_pLayers->Init(m_pMap, true, true);
			m_Loaded = true;
		}

		if(m_Loaded)
		{
			m_pBackgroundImages->Load(m_pLayers, m_pMap, Client()->IsSixup());
			CMapLayers::Load(m_pLayers, m_pBackgroundImages);
		}
	}
}

void CBackground::OnShutdown()
{
	CMapLayers::Unload();
	if(m_pBackgroundImages != nullptr)
		m_pBackgroundImages->Unload();
}

void CBackground::OnMapLoad()
{
	if(str_comp(g_Config.m_ClBackgroundEntities, CURRENT_MAP) == 0 || str_comp(g_Config.m_ClBackgroundEntities, m_aMapName))
	{
		LoadBackground();
	}
}

void CBackground::OnRender(const CRenderContext &Context)
{
	if(!m_Loaded || m_UseCurrentMap)
		return;

	if(g_Config.m_ClOverlayEntities != 100)
		return;

	m_pBackgroundImages->SetGameInfo(Context.m_State.CoreGameInfo());
	CMapLayers::OnRender(Context);
}
