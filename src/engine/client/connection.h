/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_CONNECTION_H
#define ENGINE_CLIENT_CONNECTION_H

#include "graph.h"
#include "smooth_time.h"

#include <engine/client.h>
#include <engine/shared/protocol.h>
#include <engine/shared/snapshot.h>

#include <cstdint>

class CConnection
{
public:
	struct CInput
	{
		int m_aData[MAX_INPUT_SIZE] = {};
		int m_Tick = -1;
		int64_t m_PredictedTime = 0;
		int64_t m_PredictionMargin = 0;
		int64_t m_Time = 0;
	};

	uint64_t m_SnapshotParts = 0;
	int m_AckGameTick = -1;
	int m_CurrentRecvTick = 0;
	int m_RconAuthed = 0;
	char m_aTimeoutCode[32] = "";
	bool m_DidPostConnect = false;

	CSmoothTime m_GameTime;
	CSmoothTime m_PredictedTime;
	CInput m_aInputs[200];
	int m_CurrentInput = 0;
	CGraph m_InputtimeMarginGraph;
	CGraph m_GametimeMarginGraph;

	CSnapshotStorage m_SnapshotStorage;
	CSnapshotStorage::CHolder *m_apSnapshots[IClient::NUM_SNAPSHOT_TYPES] = {};
	int m_ReceivedSnapshots = 0;
	char m_aSnapshotIncomingData[CSnapshot::MAX_SIZE] = {};
	int m_SnapshotIncomingDataSize = 0;
	int m_SnapCrcErrors = 0;

	int m_PrevGameTick = 0;
	int m_CurGameTick = 0;
	float m_GameIntraTick = 0.0f;
	float m_GameTickTime = 0.0f;
	float m_GameIntraTickSincePrev = 0.0f;
	int m_PredTick = 0;
	float m_PredIntraTick = 0.0f;

	CConnection() :
		m_InputtimeMarginGraph(128, 2, true),
		m_GametimeMarginGraph(128, 2, true)
	{
		m_SnapshotStorage.Init();
		m_GameTime.Init(0);
		m_PredictedTime.Init(0);
	}

	void ResetInput()
	{
		for(CInput &Input : m_aInputs)
			Input.m_Tick = -1;
		m_CurrentInput = 0;
	}

	void ResetSnapshots()
	{
		for(auto &pSnapshot : m_apSnapshots)
			pSnapshot = nullptr;
		m_SnapshotStorage.PurgeAll();
		m_ReceivedSnapshots = 0;
		m_SnapshotParts = 0;
		m_SnapshotIncomingDataSize = 0;
		m_SnapCrcErrors = 0;
	}

	void ResetTiming()
	{
		m_AckGameTick = -1;
		m_CurrentRecvTick = 0;
		m_PrevGameTick = 0;
		m_CurGameTick = 0;
		m_GameIntraTick = 0.0f;
		m_GameTickTime = 0.0f;
		m_GameIntraTickSincePrev = 0.0f;
		m_PredTick = 0;
		m_PredIntraTick = 0.0f;
		m_GameTime.Init(0);
		m_PredictedTime.Init(0);
	}

	int UpdateTiming(int64_t GameNow, int64_t PredNow, int TickSpeed, int64_t Frequency)
	{
		const int64_t CurTickStart = m_CurGameTick * Frequency / TickSpeed;
		const int64_t PrevTickStart = m_PrevGameTick * Frequency / TickSpeed;
		const int PrevPredTick = static_cast<int>(PredNow * TickSpeed / Frequency);
		const int NewPredTick = PrevPredTick + 1;

		m_GameIntraTick = static_cast<float>(GameNow - PrevTickStart) / static_cast<float>(CurTickStart - PrevTickStart);
		m_GameTickTime = static_cast<float>(GameNow - PrevTickStart) / static_cast<float>(Frequency);
		m_GameIntraTickSincePrev = static_cast<float>(GameNow - PrevTickStart) / static_cast<float>(Frequency / TickSpeed);
		const int64_t CurPredTickStart = NewPredTick * Frequency / TickSpeed;
		const int64_t PrevPredTickStart = PrevPredTick * Frequency / TickSpeed;
		m_PredIntraTick = static_cast<float>(PredNow - PrevPredTickStart) / static_cast<float>(CurPredTickStart - PrevPredTickStart);
		return NewPredTick;
	}

	void ResetGameplay()
	{
		ResetInput();
		ResetSnapshots();
		ResetTiming();
	}
};

#endif
