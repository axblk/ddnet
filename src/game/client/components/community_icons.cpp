#include "community_icons.h"

#include <base/log.h>

#include <engine/client/asset_loader.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/http.h>
#include <engine/storage.h>

#include <game/client/gameclient.h>

#include <functional>
#include <memory>
#include <string>

namespace
{
	constexpr int ASSET_OWNER_COMMUNITY_ICONS = 4;
}

int CCommunityIcons::FileScan(const char *pName, int IsDir, int DirType, void *pUser)
{
	const char *pExtension = ".png";
	CCommunityIcons *pSelf = static_cast<CCommunityIcons *>(pUser);
	if(IsDir || !str_endswith(pName, pExtension) || str_length(pName) - str_length(pExtension) >= (int)CServerInfo::MAX_COMMUNITY_ID_LENGTH)
		return 0;

	char aCommunityId[CServerInfo::MAX_COMMUNITY_ID_LENGTH];
	str_truncate(aCommunityId, sizeof(aCommunityId), pName, str_length(pName) - str_length(pExtension));

	pSelf->StartLoad(aCommunityId, DirType);
	return 0;
}

const CCommunityIcon *CCommunityIcons::Find(const char *pCommunityId)
{
	auto Icon = std::find_if(m_vCommunityIcons.begin(), m_vCommunityIcons.end(), [pCommunityId](const CCommunityIcon &Element) {
		return str_comp(Element.m_aCommunityId, pCommunityId) == 0;
	});
	return Icon == m_vCommunityIcons.end() ? nullptr : &(*Icon);
}

std::function<bool(CImageInfo &)> CCommunityIcons::IconPostprocess(const char *pPath, int StorageType, const std::shared_ptr<CCommunityIconLoadResult> &pResult)
{
	IStorage *pStorage = Storage();
	const std::string Path(pPath);
	return [pResult, pStorage, Path, StorageType](CImageInfo &Info) {
		if(Info.m_Format != CImageInfo::FORMAT_RGBA)
			return false;
		if(!pStorage->CalculateHashes(Path.c_str(), StorageType, &pResult->m_Sha256))
			return false;
		pResult->m_ImageInfoGrayscale = Info.DeepCopy();
		ConvertToGrayscale(pResult->m_ImageInfoGrayscale);
		return true;
	};
}

void CCommunityIcons::StartLoad(const char *pCommunityId, int StorageType)
{
	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "communityicons/%s.png", pCommunityId);
	auto pResult = std::make_shared<CCommunityIconLoadResult>();
	CImageResource Resource = GameClient()->AssetLoader().LoadImageFile(Storage(), aPath, StorageType, ASSET_OWNER_COMMUNITY_ICONS, m_Generation, IconPostprocess(aPath, StorageType, pResult));

	CCommunityIconLoad Load;
	str_copy(Load.m_aCommunityId, pCommunityId);
	Load.m_Generation = m_Generation;
	Load.m_Resource = std::move(Resource);
	Load.m_pResult = std::move(pResult);
	m_CommunityIconLoads.push_back(std::move(Load));
}

void CCommunityIcons::StartDownload(const char *pCommunityId, const char *pUrl, const SHA256_DIGEST &Sha256)
{
	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "communityicons/%s.png", pCommunityId);
	std::shared_ptr<IHttpRequest> pRequest = CreateHttpRequest(pUrl);
	pRequest->WriteToFile(Storage(), aPath, IStorage::TYPE_SAVE);
	pRequest->ExpectSha256(Sha256);
	pRequest->Timeout(CTimeout{0, 0, 0, 0});
	pRequest->LogProgress(HTTPLOG::FAILURE);

	// The icon is only written to the file, so it is always loaded from there
	auto pResult = std::make_shared<CCommunityIconLoadResult>();
	CImageResource Resource = GameClient()->AssetLoader().LoadImageHttp(Http(), std::move(pRequest), CHttpAssetDestination(Storage(), aPath, IStorage::TYPE_SAVE, false), aPath, ASSET_OWNER_COMMUNITY_ICONS, m_Generation, IconPostprocess(aPath, IStorage::TYPE_SAVE, pResult));

	CCommunityIconLoad Load;
	str_copy(Load.m_aCommunityId, pCommunityId);
	Load.m_Generation = m_Generation;
	Load.m_Download = true;
	Load.m_Resource = std::move(Resource);
	Load.m_pResult = std::move(pResult);
	m_CommunityIconLoads.push_back(std::move(Load));
}

void CCommunityIcons::LoadFinish(const char *pCommunityId, CImageInfo &Info, CImageInfo &InfoGrayscale, const SHA256_DIGEST &Sha256)
{
	CCommunityIcon CommunityIcon;
	str_copy(CommunityIcon.m_aCommunityId, pCommunityId);
	CommunityIcon.m_Sha256 = Sha256;
	CommunityIcon.m_OrgTexture = Graphics()->LoadTextureRawMove(Info, 0, pCommunityId);
	CommunityIcon.m_GreyTexture = Graphics()->LoadTextureRawMove(InfoGrayscale, 0, pCommunityId);
	if(!CommunityIcon.m_OrgTexture.IsValid() || !CommunityIcon.m_GreyTexture.IsValid())
	{
		Graphics()->UnloadTexture(&CommunityIcon.m_OrgTexture);
		Graphics()->UnloadTexture(&CommunityIcon.m_GreyTexture);
		log_error("menus/browser", "Failed to create textures for community icon '%s'", pCommunityId);
		return;
	}

	auto ExistingIcon = std::find_if(m_vCommunityIcons.begin(), m_vCommunityIcons.end(), [pCommunityId](const CCommunityIcon &Element) {
		return str_comp(Element.m_aCommunityId, pCommunityId) == 0;
	});
	if(ExistingIcon == m_vCommunityIcons.end())
	{
		m_vCommunityIcons.push_back(CommunityIcon);
	}
	else
	{
		Graphics()->UnloadTexture(&ExistingIcon->m_OrgTexture);
		Graphics()->UnloadTexture(&ExistingIcon->m_GreyTexture);
		*ExistingIcon = CommunityIcon;
	}

	log_trace("menus/browser", "Loaded community icon '%s'", pCommunityId);
}

void CCommunityIcons::Render(const CCommunityIcon *pIcon, CUIRect Rect, bool Active)
{
	Rect.VMargin(Rect.w / 2.0f - Rect.h, &Rect);

	Graphics()->WrapClamp();
	Graphics()->TextureSet(Active ? pIcon->m_OrgTexture : pIcon->m_GreyTexture);
	Graphics()->QuadsBegin();
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, Active ? 1.0f : 0.5f);
	IGraphics::CQuadItem QuadItem(Rect.x, Rect.y, Rect.w, Rect.h);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	Graphics()->QuadsEnd();
	Graphics()->WrapNormal();
}

void CCommunityIcons::Load()
{
	++m_Generation;
	GameClient()->AssetLoader().AbortOwnerBeforeGeneration(ASSET_OWNER_COMMUNITY_ICONS, m_Generation);
	m_CommunityIconLoads.clear();
	Storage()->ListDirectory(IStorage::TYPE_ALL, "communityicons", FileScan, this);
}

void CCommunityIcons::Shutdown()
{
	++m_Generation;
	GameClient()->AssetLoader().AbortOwnerBeforeGeneration(ASSET_OWNER_COMMUNITY_ICONS, m_Generation);
	m_CommunityIconLoads.clear();
}

void CCommunityIcons::Update()
{
	// Finish one icon per update, so the textures are not all created at once
	if(!m_CommunityIconLoads.empty())
	{
		CCommunityIconLoad &Load = m_CommunityIconLoads.front();
		CImageResource &Resource = Load.m_Resource;
		if(Resource.IsFinished())
		{
			if(Load.m_Generation == m_Generation && Resource.IsReady(m_Generation))
			{
				CImageInfo Info = Resource.TakeImage();
				LoadFinish(Load.m_aCommunityId, Info, Load.m_pResult->m_ImageInfoGrayscale, Load.m_pResult->m_Sha256);
			}
			else if(Load.m_Generation == m_Generation && Resource.IsFailed(m_Generation) &&
				Resource.HttpSource() != EHttpAssetSource::NONE)
			{
				log_error("menus/browser", "Failed to load community icon from '%s'", Resource.Path());
			}
			m_CommunityIconLoads.pop_front();
		}
	}

	// Rescan for changed communities only when necessary
	if(!ServerBrowser()->DDNetInfoAvailable() || m_CommunityIconsInfoSha256 == ServerBrowser()->DDNetInfoSha256().value())
		return;
	m_CommunityIconsInfoSha256 = ServerBrowser()->DDNetInfoSha256();

	// Remove icons for removed communities
	auto RemovalIterator = m_vCommunityIcons.begin();
	while(RemovalIterator != m_vCommunityIcons.end())
	{
		if(ServerBrowser()->Community(RemovalIterator->m_aCommunityId) == nullptr)
		{
			Graphics()->UnloadTexture(&RemovalIterator->m_OrgTexture);
			Graphics()->UnloadTexture(&RemovalIterator->m_GreyTexture);
			RemovalIterator = m_vCommunityIcons.erase(RemovalIterator);
		}
		else
		{
			++RemovalIterator;
		}
	}

	// Find added and updated community icons
	for(const auto &Community : ServerBrowser()->Communities())
	{
		if(!Community.IconSha256().has_value())
			continue;
		auto ExistingIcon = std::find_if(m_vCommunityIcons.begin(), m_vCommunityIcons.end(), [Community](const auto &Element) {
			return str_comp(Element.m_aCommunityId, Community.Id()) == 0;
		});
		auto ExistingDownload = std::find_if(m_CommunityIconLoads.begin(), m_CommunityIconLoads.end(), [Community](const auto &Element) {
			return Element.m_Download && str_comp(Element.m_aCommunityId, Community.Id()) == 0;
		});
		if(ExistingDownload == m_CommunityIconLoads.end() && (ExistingIcon == m_vCommunityIcons.end() || ExistingIcon->m_Sha256 != Community.IconSha256().value()))
		{
			StartDownload(Community.Id(), Community.IconUrl(), Community.IconSha256().value());
		}
	}
}
