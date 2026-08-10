#ifndef GAME_CLIENT_GAME_STATE_H
#define GAME_CLIENT_GAME_STATE_H

#include <engine/client/stream.h>

#include <generated/protocol.h>

#include <game/client/prediction/gameworld.h>
#include <game/gamecore.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class IClient;
class CMapContext;

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
	class CClientSnapshot
	{
	public:
		bool m_Active = false;
		bool m_HasPlayerInfo = false;
		bool m_HasClientInfo = false;
		bool m_HasCharacter = false;
		bool m_HasPrevCharacter = false;
		bool m_HasExtendedCharacter = false;
		CNetObj_PlayerInfo m_PlayerInfo = {};
		CNetObj_ClientInfo m_ClientInfo = {};
		CNetObj_Character m_Character = {};
		CNetObj_Character m_PrevCharacter = {};
		CNetObj_DDNetCharacter m_ExtendedCharacter = {};
	};

	class CPredictedClient
	{
	public:
		bool m_HasPrev = false;
		bool m_HasCurrent = false;
		CCharacterCore m_Prev;
		CCharacterCore m_Current;
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
	std::vector<CEntitySnapshot> m_vEntities;
	bool m_HasGameInfo = false;
	CNetObj_GameInfo m_GameInfo = {};
	bool m_HasSpectatorInfo = false;
	CNetObj_SpectatorInfo m_SpectatorInfo = {};
	CTeamsCore m_Teams;
	CGameWorld m_GameWorld;
	CGameWorld m_PredictedWorld;
	CGameWorld m_PrevPredictedWorld;
	bool m_PredictionInitialized = false;

	void RebuildGameWorld();

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
	void Predict(const IClient &Client, int Conn);
	void PredictTo(int TargetTick, const std::function<const CNetObj_PlayerInput *(int)> &InputAt);
	void MarkPredicted(int Tick) { m_PredictionTick = Tick; }

	CGameStateId Id() const { return m_Id; }
	CStreamId StreamId() const { return m_StreamId; }
	int LocalClientId() const { return m_LocalClientId; }
	int SnapshotTick() const { return m_SnapshotTick; }
	int SnapshotItems() const { return m_SnapshotItems; }
	int PredictionTick() const { return m_PredictionTick; }
	const CTuningParams &Tuning(int TuneZone = 0) const { return m_aTuning[TuneZone]; }
	const CClientSnapshot &Client(int ClientId) const { return m_aClients[ClientId]; }
	const CPredictedClient &PredictedClient(int ClientId) const { return m_aPredictedClients[ClientId]; }
	const CTeamsCore &Teams() const { return m_Teams; }
	const CGameWorld &GameWorld() const { return m_GameWorld; }
	const CGameWorld &PredictedWorld() const { return m_PredictedWorld; }
	const CGameWorld &PrevPredictedWorld() const { return m_PrevPredictedWorld; }
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
