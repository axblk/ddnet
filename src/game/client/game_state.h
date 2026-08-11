#ifndef GAME_CLIENT_GAME_STATE_H
#define GAME_CLIENT_GAME_STATE_H

#include <base/color.h>
#include <base/vmath.h>

#include <engine/client/stream.h>

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
};

class CGameTickInfo
{
public:
	int m_PrevGameTick = 0;
	int m_GameTick = 0;
	int m_PredGameTick = 0;
	float m_IntraGameTick = 0.0f;
	float m_IntraGameTickSincePrev = 0.0f;
	float m_PredIntraGameTick = 0.0f;
	float m_GameTickTime = 0.0f;
	int m_GameTickSpeed = 0;
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
		};

	private:
		static constexpr int MAX_ITEMS = 64;
		std::array<CItem, MAX_ITEMS> m_aItems;
		int m_NumItems = 0;
		float m_LastLocalTime = 0.0f;
		bool m_TimeInitialized = false;

	public:
		void Reset();
		void Create(vec2 Pos, vec2 Dir, float Alpha, float StartAngle);
		void Update(float DeltaTime);
		void Advance(float LocalTime, float Speed);
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
		bool m_HasClientInfo = false;
		bool m_HasCharacter = false;
		bool m_HasPrevCharacter = false;
		bool m_HasExtendedCharacter = false;
		bool m_HasDDNetPlayer = false;
		bool m_HasSpecChar = false;
		CNetObj_PlayerInfo m_PlayerInfo = {};
		CNetObj_ClientInfo m_ClientInfo = {};
		CNetObj_Character m_Character = {};
		CNetObj_Character m_PrevCharacter = {};
		CNetObj_DDNetCharacter m_ExtendedCharacter = {};
		CNetObj_DDNetPlayer m_DDNetPlayer = {};
		CNetObj_SpecChar m_SpecChar = {};
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

	class CEntitySnapshot
	{
	public:
		int m_Id = -1;
		int m_Type = -1;
		std::vector<unsigned char> m_vData;
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
	std::array<CPredictedClient, MAX_CLIENTS> m_aPredictedClients;
	std::array<CProtocol7ClientState, MAX_CLIENTS> m_aProtocol7Clients;
	std::vector<CEntitySnapshot> m_vEntities;
	bool m_HasGameInfo = false;
	CNetObj_GameInfo m_GameInfo = {};
	bool m_HasSpectatorInfo = false;
	CNetObj_SpectatorInfo m_SpectatorInfo = {};
	CGameInfo m_CoreGameInfo;
	CTeamsCore m_Teams;
	CGameWorld m_GameWorld;
	CGameWorld m_PredictedWorld;
	CGameWorld m_PrevPredictedWorld;
	bool m_PredictionInitialized = false;
	CRuntimeState m_Runtime;
	CEffectClockState m_EffectClock;
	CSceneClockState m_SceneClock;
	CParticleSystemState m_Particles;
	CDamageIndicatorState m_DamageIndicators;
	CRaceMessageState m_RaceMessages;

	void RebuildGameWorld();
	void UpdateWorldConfigFromSnapshot();

public:
	CGameState() = default;
	CGameState(CGameStateId Id, CStreamId StreamId);

	void Reset();
	void InitPrediction(CMapContext &MapContext);
	void ApplySnapshot(const IClient &Client, int Conn);
	void ApplySnapshotData(int Tick, int NumItems, std::array<CClientSnapshot, MAX_CLIENTS> aClients, const CNetObj_GameInfo *pGameInfo = nullptr, std::vector<CEntitySnapshot> vEntities = {});
	void ApplySnapshotMetadata(int Tick, int NumItems, int LocalClientId);
	void ApplyTuning(const CTuningParams &Tuning, int TuneZone = 0);
	void SetTeam(int ClientId, int Team);
	void SetDDrace16(bool DDrace16) { m_Teams.m_IsDDRace16 = DDrace16; }
	void SetCoreGameInfo(const CGameInfo &GameInfo);
	void Predict(const IClient &Client, int Conn);
	void PredictTo(int TargetTick, const std::function<const CNetObj_PlayerInput *(int)> &InputAt);
	void MarkPredicted(int Tick) { m_PredictionTick = Tick; }

	CGameStateId Id() const { return m_Id; }
	CStreamId StreamId() const { return m_StreamId; }
	int LocalClientId() const { return m_LocalClientId; }
	void SetLocalClientId(int LocalClientId) { m_LocalClientId = LocalClientId; }
	int SnapshotTick() const { return m_SnapshotTick; }
	int SnapshotItems() const { return m_SnapshotItems; }
	int PredictionTick() const { return m_PredictionTick; }
	const CTuningParams &Tuning(int TuneZone = 0) const { return m_aTuning[TuneZone]; }
	const CClientSnapshot &Client(int ClientId) const { return m_aClients[ClientId]; }
	const CNetObj_DDNetCharacter *ExtendedCharacter(int ClientId) const;
	const CPredictedClient &PredictedClient(int ClientId) const { return m_aPredictedClients[ClientId]; }
	CProtocol7ClientState &Protocol7Client(int ClientId) { return m_aProtocol7Clients[ClientId]; }
	const CProtocol7ClientState &Protocol7Client(int ClientId) const { return m_aProtocol7Clients[ClientId]; }
	const CTeamsCore &Teams() const { return m_Teams; }
	const CGameInfo &CoreGameInfo() const { return m_CoreGameInfo; }
	const CGameWorld &GameWorld() const { return m_GameWorld; }
	const CGameWorld &PredictedWorld() const { return m_PredictedWorld; }
	const CGameWorld &PrevPredictedWorld() const { return m_PrevPredictedWorld; }
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
