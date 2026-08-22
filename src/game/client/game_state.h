#ifndef GAME_CLIENT_GAME_STATE_H
#define GAME_CLIENT_GAME_STATE_H

#include <base/color.h>
#include <base/vmath.h>

#include <engine/client/stream.h>
#include <engine/shared/snapshot.h>

#include <generated/protocol.h>
#include <generated/protocol7.h>

#include <game/client/prediction/gameworld.h>
#include <game/gamecore.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class IClient;
class CMapContext;
class CParticles;
class CSessionId;

class CGameInfo
{
public:
	bool m_FlagStartsRace = false;
	bool m_TimeScore = false;
	bool m_UnlimitedAmmo = false;
	bool m_DDRaceRecordMessage = false;
	bool m_RaceRecordMessage = false;
	bool m_RaceSounds = false;

	bool m_AllowEyeWheel = false;
	bool m_AllowHookColl = false;
	bool m_AllowZoom = false;

	bool m_BugDDRaceGhost = false;
	bool m_BugDDRaceInput = false;
	bool m_BugFNGLaserRange = false;
	bool m_BugVanillaBounce = false;

	bool m_PredictFNG = false;
	bool m_PredictDDRace = false;
	bool m_PredictDDRaceTiles = false;
	bool m_PredictVanilla = false;

	bool m_EntitiesDDNet = false;
	bool m_EntitiesDDRace = false;
	bool m_EntitiesRace = false;
	bool m_EntitiesFNG = false;
	bool m_EntitiesVanilla = false;
	bool m_EntitiesBW = false;
	bool m_EntitiesFDDrace = false;

	bool m_Race = false;
	bool m_Pvp = false;

	bool m_DontMaskEntities = false;
	bool m_AllowXSkins = false;

	bool m_HudHealthArmor = false;
	bool m_HudAmmo = false;
	bool m_HudDDRace = false;

	bool m_NoWeakHookAndBounce = false;
	bool m_NoSkinChangeForFrozen = false;

	bool m_DDRaceTeam = false;

	bool m_PredictEvents = false;

	bool m_Supports128Teams = false;

	// zero if the server does not send them
	int m_MinTeamSize = 0;
	int m_MaxTeamSize = 0;
};

class CGameTickInfo
{
public:
	int m_PrevGameTick = 0;
	int m_GameTick = 0;
	int m_PredGameTick = 0;
	int m_PredictionTick = 0;
	float m_IntraGameTick = 0.0f;
	float m_IntraGameTickSincePrev = 0.0f;
	float m_PredIntraGameTick = 0.0f;
	float m_GameTickTime = 0.0f;
	float m_FrameTimeAverage = 0.0f;
	int m_GameTickSpeed = 0;
	int m_PredictionTime = 0;
	int64_t m_PresentationTime = 0;
	int64_t m_PresentationTimeFrequency = 1;
	float m_AnimationPlaybackSpeed = 0.0f;
	bool m_IsGameActive = false;
	bool m_IsDemoPlayback = false;
	bool m_IsDemoPlaybackPaused = false;
	bool m_ConnectionProblems = false;
};

class CGameStateId
{
	uint64_t m_Value = 0;

public:
	CGameStateId() = default;
	explicit CGameStateId(uint64_t Value) :
		m_Value(Value)
	{
	}

	bool IsValid() const { return m_Value != 0; }
	uint64_t Value() const { return m_Value; }
	bool operator==(const CGameStateId &Other) const { return m_Value == Other.m_Value; }
};

class CGameState
{
public:
	enum
	{
		SERVERMODE_PURE = 0,
		SERVERMODE_MOD,
		SERVERMODE_PUREMOD,
	};

	class CParticle
	{
	public:
		void SetDefault()
		{
			m_Pos = vec2(0, 0);
			m_Vel = vec2(0, 0);
			m_LifeSpan = 0;
			m_StartSize = 32;
			m_EndSize = 32;
			m_UseAlphaFading = false;
			m_StartAlpha = 1;
			m_EndAlpha = 1;
			m_Rot = 0;
			m_Rotspeed = 0;
			m_Gravity = 0;
			m_Friction = 0;
			m_Color = ColorRGBA(1, 1, 1, 1);
			m_Collides = true;
			m_OwnerClientId = -1;
		}

		vec2 m_Pos;
		vec2 m_Vel;
		int m_Spr;
		float m_LifeSpan;
		float m_StartSize;
		float m_EndSize;
		bool m_UseAlphaFading;
		float m_StartAlpha;
		float m_EndAlpha;
		float m_Rot;
		float m_Rotspeed;
		float m_Gravity;
		float m_Friction;
		ColorRGBA m_Color;
		bool m_Collides;
		int m_OwnerClientId;
		float m_Life;
		int m_PrevPart;
		int m_NextPart;
	};

	class CParticleSystemState
	{
		friend class CParticles;

	public:
		static constexpr int MAX_PARTICLES = 1024 * 8;
		static constexpr int NUM_GROUPS = 5;

	private:
		std::vector<CParticle> m_vParticles;
		int m_FirstFree = -1;
		std::array<int, NUM_GROUPS> m_aFirstPart = {};
		int m_NumParticles = 0;
		float m_FrictionFraction = 0.0f;
		int64_t m_LastRenderTime = 0;
		bool m_TimeInitialized = false;

	public:
		CParticleSystemState() { Reset(); }
		void Reset();
		bool Add(int Group, const CParticle &Particle, float TimePassed = 0.0f);
		int NumParticles() const { return m_NumParticles; }
	};

	class CEffectClockState
	{
	public:
		bool m_Add5hz = false;
		int64_t m_LastUpdate5hz = 0;
		bool m_Add50hz = false;
		int64_t m_LastUpdate50hz = 0;
		bool m_Add100hz = false;
		int64_t m_LastUpdate100hz = 0;
		int64_t m_SkidSoundTimer = 0;

		void Reset();
		void Update(int64_t Now, int64_t TimeFrequency, float Speed);
		bool TrySkidSound(int64_t Now, int64_t TimeFrequency);
	};

	class CSceneClockState
	{
	public:
		float m_AnimationTime = 0.0f;
		float m_GameTickTime = 0.0f;
		float m_PredIntraTick = 0.0f;
		int64_t m_LastUpdateTime = 0;
		bool m_Initialized = false;

		void Reset();
		void Update(int64_t Now, int64_t TimeFrequency, float Speed, float GameTickTime, float PredIntraTick);
	};

	class CDamageIndicatorState
	{
	public:
		class CItem
		{
		public:
			vec2 m_Pos;
			vec2 m_Dir;
			float m_RemainingLife;
			float m_StartAngle;
			ColorRGBA m_Color;
			int m_OwnerClientId;
		};

	private:
		static constexpr int MAX_ITEMS = 64;
		std::array<CItem, MAX_ITEMS> m_aItems;
		int m_NumItems = 0;
		int64_t m_LastUpdateTime = 0;
		bool m_TimeInitialized = false;

	public:
		void Reset();
		void Create(vec2 Pos, vec2 Dir, int OwnerClientId, float Alpha, float StartAngle);
		void Update(float DeltaTime);
		void Advance(int64_t Now, int64_t TimeFrequency, float Speed);
		int NumItems() const { return m_NumItems; }
		const CItem &Item(int Index) const { return m_aItems[Index]; }
	};

	enum class EMouseInputType
	{
		ABSOLUTE,
		RELATIVE,
		AUTOMATED,
	};

	class CInputState
	{
	public:
		vec2 m_MousePos = vec2(0.0f, 0.0f);
		vec2 m_MousePosOnAction = vec2(0.0f, 0.0f);
		vec2 m_TargetPos = vec2(0.0f, 0.0f);
		EMouseInputType m_MouseInputType = EMouseInputType::ABSOLUTE;
		CNetObj_PlayerInput m_InputData = {};
		CNetObj_PlayerInput m_LastData = {};
		int m_InputDirectionLeft = 0;
		int m_InputDirectionRight = 0;
		int m_ShowHookColl = 0;
		std::array<int, NUM_WEAPONS> m_aAmmoCount = {};
		int64_t m_LastSendTime = 0;

		void Reset();
		void ReleaseGameplay();
		bool ApplyStrokedCommand(const char *pCommand, int Stroke, bool BlockGameplayPress);
	};

	class CRaceMessageState
	{
	public:
		float m_CheckpointDiff = 0.0f;
		float m_FinishDiff = 0.0f;
		int m_DDRaceTime = 0;
		int m_FinishReceivedTick = 0;
		int m_CheckpointReceivedTick = 0;
		bool m_ShowFinish = false;

		void Reset();
		void ApplyDDRaceTime(int Time, int Check, bool Finish, int Tick);
		void ApplyLegacyRecord(int Time, int Check, int Tick);
	};

	class CRuntimeState
	{
	public:
		int m_ServerMode = SERVERMODE_PURE;
		int m_LastNewPredictedTick = -1;
		int m_CheckInfo = -1;
		int m_LocalTuneZone = -1;
		bool m_ReceivedTuning = false;
		int m_ExpectingTuningForZone = -1;
		int m_ExpectingTuningSince = 0;
		CTuningParams m_CurrentTuning = CTuningParams::DEFAULT;
		int m_NextChangeInfo = -1;
		bool m_DDRaceMsgSent = false;
		int m_ShowOthers = -1;
		int m_EnableSpectatorCount = -1;
		int m_SwitchStateTeam = -1;
		float m_PlayerRecord = -1.0f;
		int m_LegacyPredictedTick = -1;
		int m_LastRoundStartTick = -1;
		int m_LastRaceTick = -1;
		std::array<int, MAX_CLIENTS> m_aStrongHookLastUpdateTick = {};
		CCharOrder m_CharOrder;
		std::array<int, 2> m_aFlagDropTick = {};
		int m_LastFlagCarrierRed = -4;
		int m_LastFlagCarrierBlue = -4;
		std::array<vec2, MAX_CLIENTS> m_aLastPredictedPosition = {};
		std::array<bool, MAX_CLIENTS> m_aLastPredictedActive = {};
		bool m_GameOver = false;
		bool m_GamePaused = false;
		bool m_ReceivedDDNetPlayer = false;
		bool m_ReceivedDDNetPlayerFinishTimes = false;
		bool m_ReceivedDDNetPlayerFinishTimesMillis = false;
		CInputState m_Input;

		void Reset();
	};

	class CClientSnapshot
	{
	public:
		bool m_Active = false;
		bool m_HasPlayerInfo = false;
		bool m_HasPrevPlayerInfo = false;
		bool m_HasClientInfo = false;
		bool m_HasCharacter = false;
		bool m_HasPrevCharacter = false;
		bool m_HasExtendedCharacter = false;
		bool m_HasPrevExtendedCharacter = false;
		bool m_HasDDNetPlayer = false;
		bool m_HasSpecChar = false;
		CNetObj_PlayerInfo m_PlayerInfo = {};
		CNetObj_ClientInfo m_ClientInfo = {};
		CNetObj_Character m_Character = {};
		CNetObj_Character m_PrevCharacter = {};
		CNetObj_DDNetCharacter m_ExtendedCharacter = {};
		int m_PrevExtendedTargetX = 0;
		int m_PrevExtendedTargetY = 0;
		CNetObj_DDNetPlayer m_DDNetPlayer = {};
		CNetObj_SpecChar m_SpecChar = {};
	};

	class CRenderedClient
	{
	public:
		bool m_Active = false;
		CNetObj_Character m_Prev = {};
		CNetObj_Character m_Cur = {};
		vec2 m_Position = vec2(0.0f, 0.0f);
		bool m_IsPredicted = false;
		bool m_IsPredictedLocal = false;
	};

	class CClientPredictionHistory
	{
	public:
		int64_t m_aSmoothStart[2] = {};
		int64_t m_aSmoothLen[2] = {};
		vec2 m_aPredPos[200] = {};
		int m_aPredTick[200] = {};
	};

	class CClientEmoticonState
	{
	public:
		int m_Emoticon = 0;
		float m_StartFraction = 0.0f;
		int m_StartTick = -1;

		void Reset()
		{
			*this = {};
			m_StartTick = -1;
		}
	};

	class CClientIdentityState
	{
	public:
		bool m_Active = false;
		CNetObj_ClientInfo m_ClientInfo = {};

		void Reset() { *this = {}; }
	};

	class CPredictedClient
	{
	public:
		bool m_HasPrev = false;
		bool m_HasCurrent = false;
		CCharacterCore m_Prev;
		CCharacterCore m_Current;
	};

	class CProtocol7ClientState
	{
	public:
		bool m_Active = false;
		char m_aaSkinPartNames[protocol7::NUM_SKINPARTS][protocol7::MAX_SKIN_LENGTH] = {};
		int m_aUseCustomColors[protocol7::NUM_SKINPARTS] = {};
		int m_aSkinPartColors[protocol7::NUM_SKINPARTS] = {};
		int m_PlayerFlags = 0;

		void Reset();
	};

	class CSnapState
	{
	public:
		const CNetObj_Character *m_pLocalCharacter;
		const CNetObj_Character *m_pLocalPrevCharacter;
		const CNetObj_PlayerInfo *m_pLocalInfo;
		const CNetObj_SpectatorInfo *m_pSpectatorInfo;
		const CNetObj_SpectatorInfo *m_pPrevSpectatorInfo;
		const CNetObj_SpectatorCount *m_pSpectatorCount;
		int m_NumFlags;
		const CNetObj_Flag *m_apFlags[CSnapshot::MAX_ITEMS];
		const CNetObj_Flag *m_apPrevFlags[CSnapshot::MAX_ITEMS];
		const CNetObj_GameInfo *m_pGameInfoObj;
		const CNetObj_GameData *m_pGameDataObj;
		const CNetObj_GameData *m_pPrevGameDataObj;

		const CNetObj_PlayerInfo *m_apPlayerInfos[MAX_CLIENTS];
		const CNetObj_PlayerInfo *m_apPrevPlayerInfos[MAX_CLIENTS];

		const CNetObj_PlayerInfo *m_apInfoByScore[MAX_CLIENTS];
		const CNetObj_PlayerInfo *m_apInfoByName[MAX_CLIENTS];
		const CNetObj_PlayerInfo *m_apInfoByDDTeamScore[MAX_CLIENTS];
		const CNetObj_PlayerInfo *m_apInfoByDDTeamName[MAX_CLIENTS];

		int m_LocalClientId;
		int m_NumPlayers;
		int m_aTeamSize[2];
		int m_HighestClientId;

		class CSpectateInfo
		{
		public:
			bool m_Active;
			int m_SpectatorId;
			bool m_UsePosition;
			vec2 m_Position;

			bool m_HasCameraInfo;
			float m_Zoom;
			int m_Deadzone;
			int m_FollowFactor;
		};
		CSpectateInfo m_SpecInfo;

		class CCharacterInfo
		{
		public:
			bool m_Active;

			// snapshots
			CNetObj_Character m_Prev;
			CNetObj_Character m_Cur;

			CNetObj_DDNetCharacter m_ExtendedData;
			const CNetObj_DDNetCharacter *m_pPrevExtendedData;
			bool m_HasExtendedData;
			bool m_HasExtendedDisplayInfo;
		};
		CCharacterInfo m_aCharacters[MAX_CLIENTS];
	};

	class CEntitySnapshot
	{
	public:
		int m_Id = -1;
		int m_Type = -1;
		std::vector<unsigned char> m_vData;
		std::vector<unsigned char> m_vPrevData;
		bool m_HasEntityEx = false;
		CNetObj_EntityEx m_EntityEx = {};
	};

private:
	CGameStateId m_Id;
	CStreamId m_StreamId;
	int m_LocalClientId = -1;
	int m_SnapshotTick = 0;
	int m_SnapshotItems = 0;
	int m_PredictionTick = 0;
	std::array<CTuningParams, TuneZone::NUM> m_aTuning;
	std::array<CClientSnapshot, MAX_CLIENTS> m_aClients;
	std::vector<CClientIdentityState> m_vClientIdentities;
	std::vector<CClientEmoticonState> m_vClientEmoticons;
	std::vector<CRenderedClient> m_vRenderedClients;
	std::vector<CClientPredictionHistory> m_vClientPredictionHistory;
	std::array<CPredictedClient, MAX_CLIENTS> m_aPredictedClients;
	std::vector<CNetObj_Character> m_vSnappedCharacters;
	std::vector<CNetObj_Character> m_vEvolvedCharacters;
	std::array<CProtocol7ClientState, MAX_CLIENTS> m_aProtocol7Clients;
	std::vector<CEntitySnapshot> m_vEntities;
	bool m_HasGameInfo = false;
	CNetObj_GameInfo m_GameInfo = {};
	bool m_HasSpectatorInfo = false;
	CNetObj_SpectatorInfo m_SpectatorInfo = {};
	bool m_HasSpectatorCount = false;
	CNetObj_SpectatorCount m_SpectatorCount = {};
	CGameInfo m_CoreGameInfo;
	CTeamsCore m_Teams;
	CSnapState m_Snap = {};
	CGameWorld m_GameWorld;
	CGameWorld m_PredictedWorld;
	CGameWorld m_PrevPredictedWorld;
	bool m_PredictionInitialized = false;
	bool m_FullyPredicted = false;
	CRuntimeState m_Runtime;
	CEffectClockState m_EffectClock;
	CSceneClockState m_SceneClock;
	CParticleSystemState m_Particles;
	CDamageIndicatorState m_DamageIndicators;
	CRaceMessageState m_RaceMessages;

	void RebuildGameWorld();
	void UpdateWorldConfigFromSnapshot();
	void EvolveCharacter(CNetObj_Character &Character, int Tick);

public:
	CGameState() { Reset(); }
	CGameState(CGameStateId Id, CStreamId StreamId);

	void Reset();
	void InitPrediction(CMapContext &MapContext);
	void ApplySnapshot(const IClient &Client, CSessionId SessionId, CStreamId StreamId);
	void ApplySnapshotData(int Tick, int NumItems, std::array<CClientSnapshot, MAX_CLIENTS> aClients, const CNetObj_GameInfo *pGameInfo = nullptr, std::vector<CEntitySnapshot> vEntities = {});
	void ApplySnapshotMetadata(int Tick, int NumItems, int LocalClientId);
	void ApplyEmoticon(int ClientId, int Emoticon, int Tick, float StartFraction);
	void ApplyTuning(const CTuningParams &Tuning, int TuneZone = 0);
	void SetTeam(int ClientId, int Team);
	void SetDDrace16(bool DDrace16) { m_Teams.m_IsDDRace16 = DDrace16; }
	void SetCoreGameInfo(const CGameInfo &GameInfo);
	void Predict(const IClient &Client, CSessionId SessionId, CStreamId StreamId);
	void PredictTo(int TargetTick, const std::function<const CNetObj_PlayerInput *(int)> &InputAt);
	void UpdateRenderedClient(int ClientId, bool UsePredicted, bool PredictedLocal, float IntraGameTick, float PredIntraGameTick);
	void MarkPredicted(int Tick) { m_PredictionTick = Tick; }
	/**
	 * Whether somebody else runs the full DDNet prediction in these worlds.
	 *
	 * That prediction keeps the game world across snapshots and reconciles it
	 * with each new one, so rebuilding the world from the snapshot would throw
	 * away exactly what it computed. A state nobody drives falls back to the
	 * simple prediction here, which builds its world fresh every snapshot.
	 */
	void SetFullyPredicted(bool FullyPredicted) { m_FullyPredicted = FullyPredicted; }
	bool IsFullyPredicted() const { return m_FullyPredicted; }
	void ClearPredictedClients() { m_aPredictedClients = {}; }

	CGameStateId Id() const { return m_Id; }
	CStreamId StreamId() const { return m_StreamId; }
	int LocalClientId() const { return m_LocalClientId; }
	void SetLocalClientId(int LocalClientId) { m_LocalClientId = LocalClientId; }
	int SnapshotTick() const { return m_SnapshotTick; }
	int SnapshotItems() const { return m_SnapshotItems; }
	int PredictionTick() const { return m_PredictionTick; }
	const CTuningParams &Tuning(int TuneZone = 0) const { return m_aTuning[TuneZone]; }
	const CClientSnapshot &Client(int ClientId) const { return m_aClients[ClientId]; }
	const CClientIdentityState &ClientIdentity(int ClientId) const { return m_vClientIdentities[ClientId]; }
	void ApplyClientIdentity(int ClientId, const CNetObj_ClientInfo &ClientInfo)
	{
		m_vClientIdentities[ClientId].m_Active = true;
		m_vClientIdentities[ClientId].m_ClientInfo = ClientInfo;
	}
	const CClientEmoticonState &ClientEmoticon(int ClientId) const { return m_vClientEmoticons[ClientId]; }
	CRenderedClient &RenderedClient(int ClientId) { return m_vRenderedClients[ClientId]; }
	const CRenderedClient &RenderedClient(int ClientId) const { return m_vRenderedClients[ClientId]; }
	CClientPredictionHistory &PredictionHistory(int ClientId) { return m_vClientPredictionHistory[ClientId]; }
	const CClientPredictionHistory &PredictionHistory(int ClientId) const { return m_vClientPredictionHistory[ClientId]; }
	const CNetObj_DDNetCharacter *ExtendedCharacter(int ClientId) const;
	CPredictedClient &PredictedClient(int ClientId) { return m_aPredictedClients[ClientId]; }
	const CPredictedClient &PredictedClient(int ClientId) const { return m_aPredictedClients[ClientId]; }
	CProtocol7ClientState &Protocol7Client(int ClientId) { return m_aProtocol7Clients[ClientId]; }
	const CProtocol7ClientState &Protocol7Client(int ClientId) const { return m_aProtocol7Clients[ClientId]; }
	const CTeamsCore &Teams() const { return m_Teams; }
	const CGameInfo &CoreGameInfo() const { return m_CoreGameInfo; }
	CSnapState &Snap() { return m_Snap; }
	const CSnapState &Snap() const { return m_Snap; }
	CGameWorld &GameWorld() { return m_GameWorld; }
	const CGameWorld &GameWorld() const { return m_GameWorld; }
	CGameWorld &PredictedWorld() { return m_PredictedWorld; }
	const CGameWorld &PredictedWorld() const { return m_PredictedWorld; }
	CGameWorld &PrevPredictedWorld() { return m_PrevPredictedWorld; }
	const CGameWorld &PrevPredictedWorld() const { return m_PrevPredictedWorld; }
	const std::vector<CEntitySnapshot> &Entities() const { return m_vEntities; }
	const CNetObj_GameData *GameData() const;
	CRuntimeState &Runtime() { return m_Runtime; }
	const CRuntimeState &Runtime() const { return m_Runtime; }
	CEffectClockState &EffectClock() { return m_EffectClock; }
	const CEffectClockState &EffectClock() const { return m_EffectClock; }
	CSceneClockState &SceneClock() { return m_SceneClock; }
	const CSceneClockState &SceneClock() const { return m_SceneClock; }
	CParticleSystemState &Particles() { return m_Particles; }
	const CParticleSystemState &Particles() const { return m_Particles; }
	CDamageIndicatorState &DamageIndicators() { return m_DamageIndicators; }
	const CDamageIndicatorState &DamageIndicators() const { return m_DamageIndicators; }
	CRaceMessageState &RaceMessages() { return m_RaceMessages; }
	const CRaceMessageState &RaceMessages() const { return m_RaceMessages; }
	CInputState &Input() { return m_Runtime.m_Input; }
	const CInputState &Input() const { return m_Runtime.m_Input; }
	bool HasGameWorldCharacter(int ClientId) const;
	CCharacterCore GameWorldCharacterCore(int ClientId) const;
	bool IsOtherTeamFromLocalPlayer(int ClientId) const;
	uint64_t SnapshotDigest() const;
	uint64_t PredictionDigest() const;
	bool HasGameInfo() const { return m_HasGameInfo; }
	const CNetObj_GameInfo &GameInfo() const { return m_GameInfo; }
	void ApplySpectatorInfo(const CNetObj_SpectatorInfo &SpectatorInfo)
	{
		m_HasSpectatorInfo = true;
		m_SpectatorInfo = SpectatorInfo;
	}
	bool HasSpectatorInfo() const { return m_HasSpectatorInfo; }
	const CNetObj_SpectatorInfo &SpectatorInfo() const { return m_SpectatorInfo; }
	void ApplySpectatorCount(const CNetObj_SpectatorCount &SpectatorCount)
	{
		m_HasSpectatorCount = true;
		m_SpectatorCount = SpectatorCount;
	}
	bool HasSpectatorCount() const { return m_HasSpectatorCount; }
	const CNetObj_SpectatorCount &SpectatorCount() const { return m_SpectatorCount; }
};

class CGameStateManager
{
	uint64_t m_NextId = 1;
	std::vector<std::unique_ptr<CGameState>> m_vpStates;

public:
	CGameStateId Create(CStreamId StreamId);
	CGameState *Find(CGameStateId Id);
	const CGameState *Find(CGameStateId Id) const;
	CGameState *FindByStream(CStreamId StreamId);
	const CGameState *FindByStream(CStreamId StreamId) const;
	bool Destroy(CGameStateId Id);
	std::vector<std::unique_ptr<CGameState>> &States() { return m_vpStates; }
	const std::vector<std::unique_ptr<CGameState>> &States() const { return m_vpStates; }
	size_t NumStates() const { return m_vpStates.size(); }
};

#endif // GAME_CLIENT_GAME_STATE_H
