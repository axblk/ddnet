/* (c) Rajh, Redix and Sushi. */

#ifndef GAME_CLIENT_COMPONENTS_GHOST_H
#define GAME_CLIENT_COMPONENTS_GHOST_H

#include <engine/client/asset_loader.h>
#include <engine/client/ghost.h>
#include <engine/console.h>
#include <engine/map.h>
#include <engine/shared/jobs.h>

#include <generated/protocol.h>

#include <game/client/component.h>
#include <game/client/render.h>
#include <game/ghost_data.h>

struct CNetObj_Character;
class CGameState;
class CGameTickInfo;
class CPresentationContext;

class CGhost : public CComponent
{
private:
	enum
	{
		MAX_ACTIVE_GHOSTS = 256,
	};

	class CGhostPath
	{
		int m_ChunkSize;
		int m_NumItems;

		std::vector<CGhostCharacter *> m_vpChunks;

	public:
		CGhostPath() { Reset(); }
		~CGhostPath() { Reset(); }
		CGhostPath(const CGhostPath &Other) = delete;
		CGhostPath &operator=(const CGhostPath &Other) = delete;

		CGhostPath(CGhostPath &&Other) noexcept;
		CGhostPath &operator=(CGhostPath &&Other) noexcept;

		void Reset(int ChunkSize = 25 * 60); // one minute with default snap rate
		void SetSize(int Items);
		int Size() const { return m_NumItems; }

		void Add(const CGhostCharacter &Char);
		CGhostCharacter *Get(int Index);
		const CGhostCharacter *Get(int Index) const;
		int FindFirstAtOrAfterTick(int Tick) const;
	};

	// Reads and parses a ghost file
	class CGhostLoadJob : public CAssetJob
	{
		std::unique_ptr<CGhostLoader> m_pGhostLoader;
		char m_aMapName[MAX_MAP_LENGTH];
		SHA256_DIGEST m_MapSha256;
		unsigned m_MapCrc;

		CGhostSkin m_Skin;
		CGhostPath m_Path;
		int m_StartTick = -1;
		char m_aPlayer[MAX_NAME_LENGTH] = {};

		bool Process() override;

	public:
		CGhostLoadJob(std::unique_ptr<CGhostLoader> pGhostLoader, class IStorage *pStorage, const char *pFilename, const char *pMapName, const SHA256_DIGEST &MapSha256, unsigned MapCrc);

		const CGhostSkin &Skin() const { return m_Skin; }
		CGhostPath &GhostPath() { return m_Path; }
		int StartTick() const { return m_StartTick; }
		const char *Player() const { return m_aPlayer; }
	};

	class CGhostItem
	{
	public:
		std::shared_ptr<CManagedTeeRenderInfo> m_pManagedTeeRenderInfo;
		CTypedAssetResource<CGhostLoadJob> m_LoadResource;
		CGhostSkin m_Skin;
		CGhostPath m_Path;
		int m_StartTick;
		char m_aPlayer[MAX_NAME_LENGTH];

		CGhostItem() { Reset(); }

		bool Empty() const { return m_Path.Size() == 0 && !m_LoadResource; }
		bool Ready() const { return m_Path.Size() != 0; }
		void Reset()
		{
			m_LoadResource.Reset();
			m_pManagedTeeRenderInfo = nullptr;
			m_Path.Reset();
			m_StartTick = -1;
		}
	};

	static const char *ms_pGhostDir;

	class IGhostLoader *m_pGhostLoader;
	class IGhostRecorder *m_pGhostRecorder;

	CGhostItem m_aActiveGhosts[MAX_ACTIVE_GHOSTS];
	CGhostItem m_CurGhost;

	char m_aTmpFilename[IO_MAX_PATH_LENGTH];

	int m_NewRenderTick = -1;
	int m_StartRenderTick = -1;
	int m_LastDeathTick = -1;
	bool m_Recording = false;
	bool m_Rendering = false;
	bool m_RenderingStartedByServer = false;

	static void SetGhostSkinData(CGhostSkin *pSkin, const char *pSkinName, int UseCustomColor, int ColorBody, int ColorFeet);

	void GetPath(char *pBuf, int Size, const char *pPlayerName, int Time = -1) const;

	void AddInfos(const CNetObj_Character *pChar, const CNetObj_DDNetCharacter *pDDnetChar);
	int GetSlot() const;

	void CheckStart();
	void CheckStartLocal(bool Predicted);
	void TryRenderStart(int Tick, bool ServerControl);

	void StartRecord(int Tick);
	void StopRecord(int Time = -1);
	void StartRender(int Tick);
	void StopRender();

	void UpdateTeeRenderInfo(CGhostItem &Ghost);
	template<typename F>
	void ForEachGhostFrame(const CGameState &State, const CGameTickInfo &Time, F &&Function) const;

	static void ConGPlay(IConsole::IResult *pResult, void *pUserData);

public:
	bool m_AllowRestart;

	int Sizeof() const override { return sizeof(*this); }

	void UpdatePresentation(const CPresentationContext &Context);
	void OnRender(const CRenderContext &Context) override;
	void OnConsoleInit() override;
	void OnReset() override;
	void OnMessage(int MsgType, void *pRawMsg) override;
	void OnMapLoad() override;
	void OnShutdown() override;
	void OnNewSnapshot() override;
	void OnUpdate() override;

	void OnNewPredictedSnapshot();

	int FreeSlots() const;
	int Load(const char *pFilename);
	void Unload(int Slot);
	void UnloadAll();

	// A ghost of the current map on disk, as the ghost page of the menus
	// lists it. The player's own best one is loaded with the map.
	struct CGhostListItem
	{
		char m_aFilename[IO_MAX_PATH_LENGTH];
		char m_aPlayer[MAX_NAME_LENGTH];

		bool m_Failed;
		int m_Time;
		int m_Slot;
		bool m_Own;
		time_t m_Date;

		CGhostListItem() :
			m_Failed(false), m_Slot(-1), m_Own(false) { m_aFilename[0] = 0; }

		bool operator<(const CGhostListItem &Other) const { return m_Time < Other.m_Time; }

		bool Active() const { return m_Slot != -1; }
		bool HasFile() const { return m_aFilename[0]; }
	};

	enum
	{
		GHOST_SORT_NONE = -1,
		GHOST_SORT_NAME,
		GHOST_SORT_TIME,
		GHOST_SORT_DATE,
	};

	std::vector<CGhostListItem> &Ghostlist() { return m_vGhosts; }
	bool GhostlistScanning() const { return m_pGhostlistScanJob != nullptr; }
	void GhostlistPopulate();
	CGhostListItem *GetOwnGhost();
	void DeleteGhostItem(int Index);
	void SortGhostlist();

	void SaveGhost(CGhostListItem *pItem);

	const char *GetGhostDir() const { return ms_pGhostDir; }

	class IGhostLoader *GhostLoader() const { return m_pGhostLoader; }
	class IGhostRecorder *GhostRecorder() const { return m_pGhostRecorder; }

private:
	// Reads the headers of the ghosts of the current map
	class CGhostlistScanJob : public IJob
	{
		IStorage *m_pStorage;
		std::unique_ptr<CGhostLoader> m_pGhostLoader;
		char m_aGhostDir[IO_MAX_PATH_LENGTH];
		char m_aMapName[MAX_MAP_LENGTH];
		SHA256_DIGEST m_MapSha256;
		unsigned m_MapCrc;
		std::vector<CGhostListItem> m_vGhosts;

		static int FetchCallback(const CFsFileInfo *pInfo, int IsDir, int StorageType, void *pUser);
		void Run() override;

	public:
		CGhostlistScanJob(IStorage *pStorage, std::unique_ptr<CGhostLoader> pGhostLoader, const char *pGhostDir, const char *pMapName, const SHA256_DIGEST &MapSha256, unsigned MapCrc);

		std::vector<CGhostListItem> &Ghosts() { return m_vGhosts; }
	};

	std::vector<CGhostListItem> m_vGhosts;
	std::shared_ptr<CGhostlistScanJob> m_pGhostlistScanJob;

	void UpdateGhostlistScan();
	void UpdateOwnGhost(CGhostListItem Item);
	void OnGhostLoadFailed(int Slot);
};

#endif
