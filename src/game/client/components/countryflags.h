/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_COUNTRYFLAGS_H
#define GAME_CLIENT_COMPONENTS_COUNTRYFLAGS_H

#include <engine/client/asset_loader.h>
#include <engine/graphics.h>
#include <engine/shared/protocol.h>

#include <game/client/component.h>

#include <cstddef>
#include <vector>

class CCountryFlags : public CComponent
{
public:
	class CCountryFlag
	{
		enum class EState
		{
			UNLOADED,
			PENDING,
			LOADING,
			LOADED,
			ERROR,
		};

		mutable EState m_State = EState::UNLOADED;
		CImageResource m_LoadResource;

		void RequestLoad() const;

		friend class CCountryFlags;

	public:
		/**
		 * Country code in ISO 3166-1 numeric.
		 */
		int m_CountryCode = CountryCode::DEFAULT;
		char m_aCountryCodeString[8] = {};
		IGraphics::CTextureHandle m_Texture;

		bool operator<(const CCountryFlag &Other) const;
	};

	int Sizeof() const override { return sizeof(*this); }
	void OnInit() override;
	void OnUpdate() override;
	void OnShutdown() override;

	size_t Num() const;
	const CCountryFlag &GetByCountryCode(int CountryCode) const;
	const CCountryFlag &GetByIndex(size_t Index) const;
	void Render(const CCountryFlag &Flag, ColorRGBA Color, float x, float y, float w, float h);
	void Render(int CountryCode, ColorRGBA Color, float x, float y, float w, float h);
	bool StartupAssetsLoaded() const;

private:
	std::vector<CCountryFlag> m_vCountryFlags;
	size_t m_aCountryCodeToIndexTable[CountryCode::MAXIMUM - CountryCode::MINIMUM + 1];

	int m_FlagsQuadContainerIndex = -1;
	uint64_t m_Generation = 0;

	static bool ValidateCountryCodeString(const char *pString);
	void LoadCountryflagsIndexfile();
	void StartPendingLoads();
	void FinishLoads();
};
#endif
