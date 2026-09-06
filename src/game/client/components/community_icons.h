#ifndef GAME_CLIENT_COMPONENTS_COMMUNITY_ICONS_H
#define GAME_CLIENT_COMPONENTS_COMMUNITY_ICONS_H

#include <base/hash.h>

#include <engine/client/asset_loader.h>
#include <engine/graphics.h>
#include <engine/serverbrowser.h>

#include <game/client/component.h>
#include <game/client/ui_rect.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class CCommunityIcon
{
	friend class CCommunityIcons;

private:
	char m_aCommunityId[CServerInfo::MAX_COMMUNITY_ID_LENGTH];
	SHA256_DIGEST m_Sha256;
	IGraphics::CTextureHandle m_OrgTexture;
	IGraphics::CTextureHandle m_GreyTexture;
};

class CCommunityIcons : public CComponentInterfaces
{
public:
	const CCommunityIcon *Find(const char *pCommunityId);
	void Render(const CCommunityIcon *pIcon, CUIRect Rect, bool Active);
	void Load();
	void Update();
	void Shutdown();

private:
	class CCommunityIconLoadResult
	{
	public:
		SHA256_DIGEST m_Sha256;
		CImageInfo m_ImageInfoGrayscale;
	};

	class CCommunityIconLoad
	{
	public:
		char m_aCommunityId[CServerInfo::MAX_COMMUNITY_ID_LENGTH] = {};
		uint64_t m_Generation = 0;
		/**
		 * Whether the icon is being downloaded instead of being loaded from an
		 * existing file, so the same icon is not downloaded twice.
		 */
		bool m_Download = false;
		CImageResource m_Resource;
		std::shared_ptr<CCommunityIconLoadResult> m_pResult;
	};

	std::vector<CCommunityIcon> m_vCommunityIcons;
	std::deque<CCommunityIconLoad> m_CommunityIconLoads;
	std::optional<SHA256_DIGEST> m_CommunityIconsInfoSha256;
	uint64_t m_Generation = 1;
	static int FileScan(const char *pName, int IsDir, int DirType, void *pUser);
	std::function<bool(CImageInfo &)> IconPostprocess(const char *pPath, int StorageType, const std::shared_ptr<CCommunityIconLoadResult> &pResult);
	void StartLoad(const char *pCommunityId, int StorageType);
	void StartDownload(const char *pCommunityId, const char *pUrl, const SHA256_DIGEST &Sha256);
	void LoadFinish(const char *pCommunityId, CImageInfo &Info, CImageInfo &InfoGrayscale, const SHA256_DIGEST &Sha256);
};

#endif
