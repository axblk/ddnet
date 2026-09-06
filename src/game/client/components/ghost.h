/* (c) Rajh, Redix and Sushi. */

#ifndef GAME_CLIENT_COMPONENTS_GHOST_H
#define GAME_CLIENT_COMPONENTS_GHOST_H

#include <engine/client/ghost.h>
#include <engine/shared/jobs.h>

#include <generated/protocol.h>

#include <game/client/component.h>
#include <game/client/components/menus.h>
#include <game/client/render.h>

struct CNetObj_Character;
class CGameState;
class CGameTickInfo;
class CPresentationContext;

enum
{
	GHOSTDATA_TYPE_SKIN = 0,
	GHOSTDATA_TYPE_CHARACTER_NO_TICK,
	GHOSTDATA_TYPE_CHARACTER,
	GHOSTDATA_TYPE_START_TICK
};

struct CGhostSkin
{
	int m_aSkin[6];
	int m_UseCustomColor;
	int m_ColorBody;
	int m_ColorFeet;
};

struct CGhostCharacter_NoTick
{
	int m_X;
	int m_Y;
	int m_VelX;
	int m_VelY;
	int m_Angle;
	int m_Direction;
	int m_Weapon;
	int m_HookState;
	int m_HookX;
	int m_HookY;
	int m_AttackTick;
};

struct CGhostCharacter : public CGhostCharacter_NoTick
{
	int m_Tick;
};

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

	/**
	 * Reads a whole ghost file and decompresses it chunk by chunk.
	 *
	 * The decompression is the expensive part, so the job parses the file
	 * instead of only reading it. It owns its own ghost loader and touches
	 * nothing else.
	 */
	class CGhostLoadJob : public IJob
	{
		std::unique_ptr<CGhostLoader> m_pGhostLoader;
		char m_aFilename[IO_MAX_PATH_LENGTH];
		char m_aMapName[MAX_MAP_LENGTH];
		SHA256_DIGEST m_MapSha256;
		unsigned m_MapCrc;

		CGhostSkin m_Skin;
		CGhostPath m_Path;
		int m_StartTick = -1;
		char m_aPlayer[MAX_NAME_LENGTH] = {};
		bool m_Success = false;

		void Run() override;

	public:
		CGhostLoadJob(std::unique_ptr<CGhostLoader> pGhostLoader, const char *pFilename, const char *pMapName, const SHA256_DIGEST &MapSha256, unsigned MapCrc);

		bool Success() const { return m_Success; }
		const CGhostSkin &Skin() const { return m_Skin; }
		CGhostPath &Path() { return m_Path; }
		int StartTick() const { return m_StartTick; }
		const char *Player() const { return m_aPlayer; }
	};

	class CGhostItem
	{
	public:
		std::shared_ptr<CManagedTeeRenderInfo> m_pManagedTeeRenderInfo;
		std::shared_ptr<CGhostLoadJob> m_pLoadJob;
		CGhostSkin m_Skin;
		CGhostPath m_Path;
		int m_StartTick;
		char m_aPlayer[MAX_NAME_LENGTH];

		CGhostItem() { Reset(); }

		// A slot that is still loading is taken, but has nothing to render yet.
		bool Empty() const { return m_Path.Size() == 0 && m_pLoadJob == nullptr; }
		bool Ready() const { return m_Path.Size() != 0; }
		void Reset()
		{
			if(m_pLoadJob)
			{
				m_pLoadJob->Abort();
				m_pLoadJob = nullptr;
			}
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
	static void GetGhostCharacter(CGhostCharacter *pGhostChar, const CNetObj_Character *pChar, const CNetObj_DDNetCharacter *pDDnetChar);
	static void GetNetObjCharacter(CNetObj_Character *pChar, const CGhostCharacter *pGhostChar);

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

	void SaveGhost(CMenus::CGhostItem *pItem);

	const char *GetGhostDir() const { return ms_pGhostDir; }

	class IGhostLoader *GhostLoader() const { return m_pGhostLoader; }
	class IGhostRecorder *GhostRecorder() const { return m_pGhostRecorder; }
};

#endif
