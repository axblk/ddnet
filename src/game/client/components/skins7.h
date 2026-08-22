/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_SKINS7_H
#define GAME_CLIENT_COMPONENTS_SKINS7_H

#include <base/color.h>
#include <base/vmath.h>

#include <engine/client/asset_loader.h>
#include <engine/client/enums.h>
#include <engine/graphics.h>

#include <generated/protocol.h>
#include <generated/protocol7.h>

#include <game/client/component.h>
#include <game/client/render.h>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

class CSkins7 : public CComponent
{
public:
	enum
	{
		SKINFLAG_SPECIAL = 1 << 0,
		SKINFLAG_STANDARD = 1 << 1,

		NUM_COLOR_COMPONENTS = 4,

		HAT_NUM = 2,
		HAT_OFFSET_SIDE = 2,
	};

	typedef std::function<void()> TSkinLoadedCallback;

	class CSkinPart
	{
		enum class EState
		{
			UNLOADED,
			PENDING,
			LOADING,
			LOADED,
			ERROR,
		};

		CSkins7 *m_pSkins7 = nullptr;
		int m_StorageType = 0;
		bool m_AlwaysLoaded = false;
		mutable EState m_State = EState::UNLOADED;
		mutable std::optional<std::chrono::nanoseconds> m_FirstLoadRequest;
		mutable std::optional<std::chrono::nanoseconds> m_LastLoadRequest;
		CImageResource m_LoadResource;
		class CLoadData;
		std::shared_ptr<CLoadData> m_pLoadData;

		void RequestLoad() const;

		friend class CSkins7;

	public:
		int m_Type = 0;
		int m_Flags = 0;
		char m_aName[24] = {};
		IGraphics::CTextureHandle m_OriginalTexture;
		IGraphics::CTextureHandle m_ColorableTexture;
		ColorRGBA m_BloodColor = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);

		void ApplyTo(CTeeRenderInfo::CSixup &SixupRenderInfo) const;

		bool operator<(const CSkinPart &Other) const;
	};

	class CSkin
	{
	public:
		int m_Flags;
		char m_aName[24];
		const CSkinPart *m_apParts[protocol7::NUM_SKINPARTS];
		int m_aUseCustomColors[protocol7::NUM_SKINPARTS];
		unsigned m_aPartColors[protocol7::NUM_SKINPARTS];

		bool operator<(const CSkin &Other) const;
		bool operator==(const CSkin &Other) const;
	};

	static const char *const ms_apSkinPartNames[protocol7::NUM_SKINPARTS];
	static const char *const ms_apSkinPartNamesLocalized[protocol7::NUM_SKINPARTS];
	static const char *const ms_apColorComponents[NUM_COLOR_COMPONENTS];

	static std::array<char *, 2> ms_apSkinNameVariables;
	static std::array<std::array<char *, protocol7::NUM_SKINPARTS>, 2> ms_apSkinVariables;
	static std::array<std::array<int *, protocol7::NUM_SKINPARTS>, 2> ms_apUCCVariables; // use custom color
	static std::array<std::array<unsigned *, protocol7::NUM_SKINPARTS>, 2> ms_apColorVariables;

	int Sizeof() const override { return sizeof(*this); }
	void OnInit() override;
	void OnUpdate() override;
	void OnShutdown() override;

	void Refresh(TSkinLoadedCallback &&SkinLoadedCallback);
	std::chrono::nanoseconds LastRefreshTime() const { return m_LastRefreshTime; }
	bool StartupAssetsLoaded() const;

	const std::vector<CSkin> &GetSkins() const;
	const std::vector<CSkinPart> &GetSkinParts(int Part) const;
	const CSkinPart *FindSkinPartOrNullptr(int Part, const char *pName, bool AllowSpecialPart) const;
	const CSkinPart *FindDefaultSkinPart(int Part) const;
	const CSkinPart *FindSkinPart(int Part, const char *pName, bool AllowSpecialPart) const;
	void RandomizeSkin(int Dummy) const;

	ColorRGBA GetColor(int Value, bool UseAlpha) const;
	void ApplyColorTo(CTeeRenderInfo::CSixup &SixupRenderInfo, bool UseCustomColors, int Value, int Part) const;
	ColorRGBA GetTeamColor(int UseCustomColors, int PartColor, int Team, int Part) const;

	// returns true if everything was valid and nothing changed
	bool ValidateSkinParts(char *apPartNames[protocol7::NUM_SKINPARTS], int *pUseCustomColors, int *pPartColors, int GameFlags) const;

	bool SaveSkinfile(const char *pName, int Dummy);
	bool RemoveSkin(const CSkin *pSkin);

	IGraphics::CTextureHandle XmasHatTexture() const { return m_XmasHatTexture; }
	IGraphics::CTextureHandle BotDecorationTexture() const { return m_BotTexture; }

	static bool IsSpecialSkin(const char *pName);

private:
	std::chrono::nanoseconds m_LastRefreshTime;

	std::vector<CSkinPart> m_avSkinParts[protocol7::NUM_SKINPARTS];
	CSkinPart m_aPlaceholderSkinParts[protocol7::NUM_SKINPARTS];
	std::vector<CSkin> m_vSkins;

	IGraphics::CTextureHandle m_XmasHatTexture;
	IGraphics::CTextureHandle m_BotTexture;
	CImageResource m_XmasHatResource;
	CImageResource m_BotResource;
	uint64_t m_Generation = 0;
	std::optional<std::chrono::nanoseconds> m_PartUpdateTime;

	static int SkinPartScan(const char *pName, int IsDir, int DirType, void *pUser);
	bool RegisterSkinPart(int PartType, const char *pName, int DirType);
	static int SkinScan(const char *pName, int IsDir, int DirType, void *pUser);
	bool LoadSkin(const char *pName, int DirType);
	const CSkinPart *FindSkinPartWithoutRequest(int Part, const char *pName, bool AllowSpecialPart) const;
	const CSkinPart *FindDefaultSkinPartWithoutRequest(int Part) const;
	void StartPendingLoads();
	void FinishLoads();
	void UnloadUnusedParts();
	void StartSpecialLoads();
	void FinishSpecialLoad(CImageResource &Resource, IGraphics::CTextureHandle &Texture, const char *pDescription);
	void UnloadSkinPart(CSkinPart &SkinPart);

	void InitPlaceholderSkinParts();

	void AddSkinFromConfigVariables(const char *pName, int Dummy);
};

#endif
