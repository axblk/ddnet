#include "test.h"

#include <base/hash.h>
#include <base/io.h>
#include <base/logger.h>
#include <base/mem.h>
#include <base/types.h>

#include <engine/engine.h>
#include <engine/http.h>
#include <engine/kernel.h>
#include <engine/message.h>
#include <engine/server/databases/connection.h>
#include <engine/server/databases/connection_pool.h>
#include <engine/server/register.h>
#include <engine/server/server.h>
#include <engine/server/server_logger.h>
#include <engine/shared/assertion_logger.h>
#include <engine/shared/config.h>
#include <engine/shared/datafile.h>
#include <engine/shared/jsonwriter.h>
#include <engine/shared/protocol_ex.h>
#include <engine/storage.h>

#include <generated/protocol.h>
#include <generated/protocol7.h>

#include <game/mapitems.h>
#include <game/match_report.h>
#include <game/server/entities/character.h>
#include <game/server/entities/dragger.h>
#include <game/server/entities/dragger_beam.h>
#include <game/server/entities/laser.h>
#include <game/server/entities/pickup.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/gamemodes/ddnet.h>
#include <game/server/gamemodes/ddrace.h>
#include <game/server/gamemodes/ddrace_character.h>
#include <game/server/gamemodes/ddrace_player.h>
#include <game/server/gameworld.h>
#include <game/server/interactions.h>
#include <game/server/mode/game_mode_registry.h>
#include <game/server/modes/catch16/groups.h>
#include <game/server/modes/fng/fng.h>
#include <game/server/modes/vanilla/ctf.h>
#include <game/server/modes/vanilla/dead_spectators.h>
#include <game/server/modes/vanilla/dm.h>
#include <game/server/modes/vanilla/flag.h>
#include <game/server/modes/vanilla/tdm.h>
#include <game/server/modes/zcatch/rules.h>
#include <game/server/player.h>
#include <game/server/save.h>
#include <game/server/score.h>
#include <game/server/teams.h>
#include <game/version.h>

#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <thread>

bool IsInterrupted()
{
	return false;
}

#if defined(CONF_PLATFORM_ANDROID)
std::vector<std::string> FetchAndroidServerCommandQueue()
{
	return {};
}
#endif

class CTestGameControllerDDRace : public CGameControllerDDRace
{
public:
	using CGameControllerDDRace::CGameControllerDDRace;

	int TestGameInfoFlags() const { return GameInfoFlags(SERVER_DEMO_CLIENT); }
	int TestGameInfoFlags2() const { return GameInfoFlags2(SERVER_DEMO_CLIENT); }
};

class CTestGameControllerDDNet : public CGameControllerDDNet
{
public:
	using CGameControllerDDNet::CGameControllerDDNet;

	int TestGameInfoFlags() const { return GameInfoFlags(SERVER_DEMO_CLIENT); }
	int TestGameInfoFlags2() const { return GameInfoFlags2(SERVER_DEMO_CLIENT); }
};

// keeps the match report messages instead of sending them
class CTestServer final : public CServer
{
public:
	class CMessage
	{
	public:
		int m_MsgId;
		int m_Flags;
		int m_ClientId;
		std::vector<unsigned char> m_vData;
	};
	std::vector<CMessage> m_vMatchReportMessages;
	// what clients are told about the map
	std::vector<CMessage> m_vMapMessages;

	void AdvanceTick(int Ticks) { m_CurrentGameTick += Ticks; }

	int SendMsg(CMsgPacker *pMsg, int Flags, int ClientId) override
	{
		if(pMsg->m_System && (pMsg->m_MsgId == NETMSG_MAP_DETAILS || pMsg->m_MsgId == NETMSG_MAP_CHANGE))
		{
			m_vMapMessages.push_back({pMsg->m_MsgId, Flags, ClientId, {pMsg->Data(), pMsg->Data() + pMsg->Size()}});
			return 0;
		}
		if(pMsg->m_System || (pMsg->m_MsgId != NETMSG_MATCH_REPORT_START && pMsg->m_MsgId != NETMSG_MATCH_REPORT_CHUNK))
			return CServer::SendMsg(pMsg, Flags, ClientId);
		m_vMatchReportMessages.push_back({pMsg->m_MsgId, Flags, ClientId, {pMsg->Data(), pMsg->Data() + pMsg->Size()}});
		return 0;
	}
};

// a match report as the client receives it
class CReceivedMatchReport
{
public:
	CMatchReport m_Report;
	bool m_Live;
	bool m_PersistOnDisconnect;
	int m_LocalParticipantId;
	int m_NumChunks = 0;

	std::optional<int64_t> Metric(int ParticipantId, const char *pMetricId) const { return m_Report.Metric(EMatchSubjectKind::PARTICIPANT, ParticipantId, pMetricId); }
	const CMatchParticipant *Participant(const char *pName) const
	{
		for(const CMatchParticipant &Participant : m_Report.m_vParticipants)
			if(Participant.m_DisplayName == pName)
				return &Participant;
		return nullptr;
	}
};

class GameWorld : public ::testing::Test // NOLINT(readability-identifier-naming)
{
public:
	IGameServer *m_pGameServer = nullptr;
	CTestServer *m_pServer = nullptr;
	std::unique_ptr<IKernel> m_pKernel;
	CTestInfo m_TestInfo;
	std::unique_ptr<IStorage> m_pStorage;
	CConfig m_ConfigBackup;

	CGameContext *GameServer() // NOLINT(readability-make-member-function-const)
	{
		return (CGameContext *)m_pGameServer;
	}

	IGameController *GameController() // NOLINT(readability-make-member-function-const)
	{
		return GameServer()->GameHost().Controller();
	}

	template<typename TCharacter = CCharacter>
	TCharacter *SpawnPlayer(int ClientId, vec2 Pos, int Team = TEAM_GAME)
	{
		CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, Team, false, -1);
		return pPlayer ? dynamic_cast<TCharacter *>(pPlayer->ForceSpawn(Pos)) : nullptr;
	}

	void DeletePlayers()
	{
		for(CPlayer *&pPlayer : GameServer()->m_apPlayers)
		{
			delete pPlayer;
			pPlayer = nullptr;
		}
	}

	void SelectGameMode(const char *pName)
	{
		for(const CPlayer *pPlayer : GameServer()->m_apPlayers)
			ASSERT_EQ(pPlayer, nullptr) << "cannot switch modes with live players";
		GameServer()->GameHost().Shutdown();
		ASSERT_TRUE(GameServer()->GameHost().Select(pName));
		GameServer()->GameHost().Init(m_pServer->DbPool());
	}

	template<typename TController>
	TController &SelectController(const char *pName)
	{
		// characters keep a reference to the mode that made them
		for(const CPlayer *pPlayer : GameServer()->m_apPlayers)
			EXPECT_EQ(pPlayer, nullptr) << "cannot switch modes with live players";
		GameServer()->GameHost().Shutdown();
		auto pController = std::make_unique<TController>(GameServices(), *FindGameMode(pName));
		TController &Controller = *pController;
		GameServer()->GameHost().Select(std::move(pController));
		GameServer()->GameHost().Init(m_pServer->DbPool());
		return Controller;
	}

	// a player whose client is in the game, so that it gets its match report
	CPlayer *JoinPlayer(int ClientId, int Team, const char *pName)
	{
		m_pServer->m_aClients[ClientId].m_State = CServer::CClient::STATE_INGAME;
		str_copy(m_pServer->m_aClients[ClientId].m_aName, pName);
		CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, Team, false, -1);
		GameController()->OnPlayerConnect(pPlayer);
		// the report counts players from the tick they are seen in
		GameController()->Tick();
		return pPlayer;
	}

	void LeavePlayer(int ClientId)
	{
		GameController()->OnPlayerDisconnect(GameServer()->m_apPlayers[ClientId], "test");
		delete GameServer()->m_apPlayers[ClientId];
		GameServer()->m_apPlayers[ClientId] = nullptr;
		m_pServer->m_aClients[ClientId].m_State = CServer::CClient::STATE_EMPTY;
	}

	// ForceSpawn replaces the character without removing it, the server only spawns players that have none
	CCharacter *Respawn(CPlayer *pPlayer, vec2 Pos)
	{
		if(CCharacter *pAlive = pPlayer->GetCharacter())
		{
			pAlive->Reset();
			delete pAlive;
		}
		else
		{
			// a dead character is removed by the player's tick
			pPlayer->Tick();
		}
		return pPlayer->ForceSpawn(Pos);
	}

	/**
	 * The reports the client received, put together the way the client does it.
	 *
	 * The chunks of a large report are spread over ticks, so the server runs until
	 * nothing is left to send.
	 */
	std::vector<CReceivedMatchReport> ReceivedMatchReports(int ClientId)
	{
		for(int i = 0; i < 20; i++)
		{
			m_pServer->AdvanceTick(1);
			GameController()->Tick();
		}
		std::vector<CReceivedMatchReport> vReports;
		std::string Payload;
		int Size = 0;
		for(const CTestServer::CMessage &Message : m_pServer->m_vMatchReportMessages)
		{
			if(Message.m_ClientId != ClientId)
				continue;
			EXPECT_EQ(Message.m_Flags, MSGFLAG_VITAL | MSGFLAG_NORECORD);
			CUnpacker Unpacker;
			Unpacker.Reset(Message.m_vData.data(), Message.m_vData.size());
			const void *pMatchId = Unpacker.GetRaw(sizeof(CUuid));
			CUuid MatchId = UUID_ZEROED;
			if(pMatchId)
				mem_copy(&MatchId, pMatchId, sizeof(MatchId));
			if(Message.m_MsgId == NETMSG_MATCH_REPORT_START)
			{
				CReceivedMatchReport &Received = vReports.emplace_back();
				Received.m_Report.m_MatchId = MatchId;
				Received.m_Live = Unpacker.GetInt();
				Received.m_PersistOnDisconnect = Unpacker.GetInt();
				Received.m_LocalParticipantId = Unpacker.GetInt();
				Size = Unpacker.GetInt();
				Payload.clear();
			}
			else
			{
				EXPECT_FALSE(vReports.empty());
				if(vReports.empty())
					break;
				CReceivedMatchReport &Received = vReports.back();
				EXPECT_EQ(MatchId, Received.m_Report.m_MatchId);
				EXPECT_EQ(Unpacker.GetInt(), Received.m_NumChunks);
				const int ChunkSize = Unpacker.GetInt();
				const void *pChunk = Unpacker.GetRaw(ChunkSize);
				EXPECT_NE(pChunk, nullptr);
				EXPECT_LE(ChunkSize, MatchReportLimits::MAX_CHUNK_SIZE);
				if(pChunk)
					Payload.append((const char *)pChunk, ChunkSize);
				Received.m_NumChunks++;
				if((int)Payload.size() == Size)
				{
					std::string Error;
					EXPECT_TRUE(MatchReportFromPacked(Payload.data(), Payload.size(), Received.m_Report, &Error)) << Error;
					EXPECT_EQ(Received.m_Report.m_MatchId, MatchId);
					EXPECT_TRUE(MatchReportValidate(Received.m_Report, &Error)) << Error;
				}
			}
			EXPECT_FALSE(Unpacker.Error());
		}
		m_pServer->m_vMatchReportMessages.erase(std::remove_if(m_pServer->m_vMatchReportMessages.begin(), m_pServer->m_vMatchReportMessages.end(), [ClientId](const CTestServer::CMessage &Message) { return Message.m_ClientId == ClientId; }), m_pServer->m_vMatchReportMessages.end());
		return vReports;
	}

	CReceivedMatchReport ReceivedMatchReport(int ClientId)
	{
		std::vector<CReceivedMatchReport> vReports = ReceivedMatchReports(ClientId);
		EXPECT_EQ(vReports.size(), 1u);
		return vReports.empty() ? CReceivedMatchReport{} : vReports.back();
	}

	CGameServices &GameServices() // NOLINT(readability-make-member-function-const)
	{
		return GameServer()->GameHost().Services();
	}

	CGameControllerDDRace *RaceControllerOrNull() // NOLINT(readability-make-member-function-const)
	{
		return dynamic_cast<CGameControllerDDRace *>(GameController());
	}

	CGameControllerDDRace &RaceController() // NOLINT(readability-make-member-function-const)
	{
		return *RaceControllerOrNull();
	}

	CGameTeams &RaceTeams() // NOLINT(readability-make-member-function-const)
	{
		return RaceController().RaceTeams();
	}

	CScore &RaceScore() // NOLINT(readability-make-member-function-const)
	{
		return RaceController().RaceScore();
	}

	GameWorld()
	{
		m_ConfigBackup = g_Config;

		auto *pServer = new CTestServer();
		m_pServer = pServer;

		m_pKernel = std::unique_ptr<IKernel>(IKernel::Create());
		m_pKernel->RegisterInterface(m_pServer);

		IEngine *pEngine = CreateTestEngine(GAME_NAME);
		m_pKernel->RegisterInterface(pEngine);

		m_TestInfo.m_DeleteTestStorageFilesOnSuccess = true;
		m_pStorage = m_TestInfo.CreateTestStorage();
		EXPECT_NE(m_pStorage, nullptr);
		m_pKernel->RegisterInterface(m_pStorage.get(), false);

		IConsole *pConsole = CreateConsole(CFGFLAG_SERVER | CFGFLAG_ECON).release();
		m_pKernel->RegisterInterface(pConsole);

		IConfigManager *pConfigManager = CreateConfigManager();
		m_pKernel->RegisterInterface(pConfigManager);

		IEngineHttp *pEngineHttp = CreateEngineHttp();
		m_pKernel->RegisterInterface(pEngineHttp); // IEngineHttp
		m_pKernel->RegisterInterface(static_cast<IHttp *>(pEngineHttp), false);

		IEngineAntibot *pEngineAntibot = CreateEngineAntibot();
		m_pKernel->RegisterInterface(pEngineAntibot);
		m_pKernel->RegisterInterface(static_cast<IAntibot *>(pEngineAntibot), false);

		m_pGameServer = CreateGameServer();
		m_pKernel->RegisterInterface(m_pGameServer);

		pEngine->Init();
		pConsole->Init();
		pConfigManager->Init();

		m_pServer->RegisterCommands();

		EXPECT_NE(m_pServer->LoadMap("coverage"), 0);

		m_pServer->m_RunServer = CServer::RUNNING;

		m_pServer->m_AuthManager.Init();

		{
			int Size = GameServer()->PersistentClientDataSize();
			for(auto &Client : m_pServer->m_aClients)
			{
				Client.Reset();
				Client.m_HasPersistentData = false;
				Client.m_pPersistentData = malloc(Size);
			}
		}
		m_pServer->m_pPersistentData = malloc(GameServer()->PersistentDataSize());
		EXPECT_NE(m_pServer->LoadMap("coverage"), 0);

		EXPECT_TRUE(pEngineHttp->Init(std::chrono::seconds{2})) << "Failed to initialize the HTTP client";

		pServer->m_NetServer.SetCallbacks(
			CServer::NewClientCallback,
			CServer::NewClientNoAuthCallback,
			CServer::ClientRejoinCallback,
			CServer::DelClientCallback, pServer);

		pServer->m_Econ.Init(pServer->Config(), pServer->Console(), &pServer->m_ServerBan);

		pServer->m_Fifo.Init(pServer->Console(), pServer->Config()->m_SvInputFifo, CFGFLAG_SERVER);
		m_pServer->Antibot()->Init();
		GameServer()->OnInit(nullptr);
		pServer->ReadAnnouncementsFile();
		pServer->InitMaplist();
	}

	~GameWorld() override
	{
		m_pServer->m_Econ.Shutdown();
		m_pServer->m_Fifo.Shutdown();
		m_pGameServer->OnShutdown(nullptr);
		m_pServer->DbPool()->OnShutdown();

		g_Config = m_ConfigBackup;
	}
};

TEST_F(GameWorld, DebugDummiesConnectAndDrop)
{
	g_Config.m_DbgDummies = 2;
	m_pServer->UpdateDebugDummies(false);

	const int FirstDummy = m_pServer->MaxClients() - 1;
	const int SecondDummy = m_pServer->MaxClients() - 2;
	EXPECT_TRUE(m_pServer->ClientIngame(FirstDummy));
	EXPECT_TRUE(m_pServer->ClientIngame(SecondDummy));

	g_Config.m_DbgDummies = 1;
	m_pServer->UpdateDebugDummies(false);

	EXPECT_TRUE(m_pServer->ClientIngame(FirstDummy));
	EXPECT_FALSE(m_pServer->ClientIngame(SecondDummy));
}

namespace
{
	class CTestVanillaTDM final : public CGameControllerVanillaTDM
	{
	public:
		using CGameControllerVanillaTDM::CGameControllerVanillaTDM;
		using CGameControllerVanillaTeamplay::UpdateTeamBalance;

		void SetTeamScores(int Red, int Blue)
		{
			m_aTeamScores[TEAM_RED] = Red;
			m_aTeamScores[TEAM_BLUE] = Blue;
		}
	};

	class CTestVanillaCTF final : public CGameControllerVanillaCTF
	{
	public:
		using CGameControllerVanillaCTF::CGameControllerVanillaCTF;
		using CGameControllerVanillaTeamplay::UpdateTeamBalance;
		using IGameController::AddMatchMetric;

		void SetTeamScores(int Red, int Blue)
		{
			m_aTeamScores[TEAM_RED] = Red;
			m_aTeamScores[TEAM_BLUE] = Blue;
		}

		void BeginSuddenDeath()
		{
			Match().BeginSuddenDeath();
		}

		void StartRoundAt(int Tick)
		{
			Match().StartRound(Tick);
		}

		bool IsSuddenDeath() const
		{
			return Match().IsSuddenDeath();
		}
	};
}

// A map change as the server does it between two ticks
static void ChangeMap(GameWorld *pWorld, const char *pMapName)
{
	ASSERT_NE(pWorld->m_pServer->LoadMap(pMapName), 0) << pMapName;
	pWorld->m_pGameServer->OnShutdown(pWorld->m_pServer->m_pPersistentData);
	pWorld->m_pKernel->ReregisterInterface(pWorld->m_pGameServer);
	pWorld->m_pGameServer->OnInit(pWorld->m_pServer->m_pPersistentData);
}

// Until the map is converted for the clients who need it
static void WaitForMapConversion(GameWorld *pWorld)
{
	for(int i = 0; i < 30000 && !pWorld->m_pServer->UpdateMapConversion(); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	ASSERT_TRUE(pWorld->m_pServer->UpdateMapConversion()) << "the map was not converted in 30 seconds";
}

// The size a client was told about in the last map change it got
static std::optional<int> ToldMapSize(CTestServer *pServer, int ClientId)
{
	for(auto It = pServer->m_vMapMessages.rbegin(); It != pServer->m_vMapMessages.rend(); ++It)
	{
		if(It->m_ClientId != ClientId || It->m_MsgId != NETMSG_MAP_CHANGE)
			continue;
		CUnpacker Unpacker;
		Unpacker.Reset(It->m_vData.data(), It->m_vData.size());
		Unpacker.GetString();
		Unpacker.GetInt();
		const int Size = Unpacker.GetInt();
		if(Unpacker.Error())
			return std::nullopt;
		return Size;
	}
	return std::nullopt;
}

static void WriteMap(GameWorld *pWorld, const char *pFrom, const char *pTo)
{
	void *pData;
	unsigned Size;
	ASSERT_TRUE(pWorld->m_pStorage->ReadFile(pFrom, IStorage::TYPE_ALL, &pData, &Size)) << pFrom;
	pWorld->m_pStorage->CreateFolder("maps", IStorage::TYPE_SAVE);
	IOHANDLE File = pWorld->m_pStorage->OpenFile(pTo, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	ASSERT_TRUE(File) << pTo;
	EXPECT_EQ(io_write(File, pData, Size), Size);
	io_close(File);
	free(pData);
}

TEST_F(GameWorld, SixupMapIsAMatterOfTheMap)
{
	// coverage has no version for 0.7, and is not converted with
	// sv_map_convert off, which leaves 0.7 on for the next map
	str_copy(g_Config.m_SvMapConvert, "off");
	ChangeMap(this, "coverage");
	EXPECT_FALSE(m_pServer->m_SixupMapAvailable);
	EXPECT_EQ(m_pServer->m_aCurrentMapSize[CServer::MAP_TYPE_SIXUP], 0u);
	EXPECT_EQ(g_Config.m_SvSixup, 1);

	// Tutorial has one in maps7/
	ChangeMap(this, "Tutorial");
	EXPECT_TRUE(m_pServer->m_SixupMapAvailable);
	void *pData;
	unsigned Size;
	ASSERT_TRUE(m_pStorage->ReadFile("maps7/Tutorial.map", IStorage::TYPE_ALL, &pData, &Size));
	EXPECT_EQ(m_pServer->m_aCurrentMapSize[CServer::MAP_TYPE_SIXUP], Size);
	EXPECT_EQ(mem_comp(m_pServer->CurrentMapData(CServer::MAP_TYPE_SIXUP), pData, Size), 0);
	EXPECT_NE(sha256_comp(m_pServer->m_aCurrentMapSha256[CServer::MAP_TYPE_SIXUP], m_pServer->m_aCurrentMapSha256[CServer::MAP_TYPE_SIX]), 0);
	free(pData);

	ChangeMap(this, "coverage");
	EXPECT_FALSE(m_pServer->m_SixupMapAvailable);
	EXPECT_EQ(m_pServer->m_aCurrentMapSize[CServer::MAP_TYPE_SIXUP], 0u);
	EXPECT_EQ(g_Config.m_SvSixup, 1);
}

TEST_F(GameWorld, SixupMapIsTheMapThatTeeworlds07Wrote)
{
	// ctf1 as Teeworlds 0.7 ships it, as a map of its own without a version in maps7/
	void *pData;
	unsigned Size;
	ASSERT_TRUE(m_pStorage->ReadFile("maps7/ctf1.map", IStorage::TYPE_ALL, &pData, &Size));
	m_pStorage->CreateFolder("maps", IStorage::TYPE_SAVE);
	IOHANDLE File = m_pStorage->OpenFile("maps/ctf1_07.map", IOFLAG_WRITE, IStorage::TYPE_SAVE);
	ASSERT_TRUE(File);
	EXPECT_EQ(io_write(File, pData, Size), Size);
	io_close(File);
	free(pData);

	// Without converting, DDNet clients get it as it is, too
	str_copy(g_Config.m_SvMapConvert, "off");
	ChangeMap(this, "ctf1_07");
	EXPECT_TRUE(m_pServer->m_SixupMapAvailable);
	EXPECT_EQ(m_pServer->m_aCurrentMapSize[CServer::MAP_TYPE_SIXUP], Size);
	EXPECT_EQ(m_pServer->m_aCurrentMapSize[CServer::MAP_TYPE_SIX], Size);
	EXPECT_EQ(m_pServer->CurrentMapData(CServer::MAP_TYPE_SIXUP), m_pServer->CurrentMapData(CServer::MAP_TYPE_SIX));
	EXPECT_EQ(m_pServer->m_aCurrentMapCrc[CServer::MAP_TYPE_SIXUP], m_pServer->m_aCurrentMapCrc[CServer::MAP_TYPE_SIX]);
	EXPECT_EQ(sha256_comp(m_pServer->m_aCurrentMapSha256[CServer::MAP_TYPE_SIXUP], m_pServer->m_aCurrentMapSha256[CServer::MAP_TYPE_SIX]), 0);
}

TEST_F(GameWorld, DDNetClientsGetTheTeeworlds07MapConverted)
{
	// ctf5 as Teeworlds 0.7 ships it, which uses tiles DDNet has elsewhere
	WriteMap(this, "test/maps07/ctf5.map", "maps/ctf5_07.map");
	str_copy(g_Config.m_SvMapsBaseUrl, "https://maps.example/");
	ChangeMap(this, "ctf5_07");
	const IMap *pMap = GameServer()->Map();
	EXPECT_TRUE(m_pServer->m_aMapConverting[CServer::MAP_TYPE_SIX]);
	EXPECT_FALSE(m_pServer->m_aMapConverting[CServer::MAP_TYPE_SIXUP]);
	WaitForMapConversion(this);
	EXPECT_FALSE(m_pServer->m_aMapConverting[CServer::MAP_TYPE_SIX]);

	// 0.7 clients get the map
	EXPECT_TRUE(m_pServer->m_SixupMapAvailable);
	EXPECT_EQ(m_pServer->CurrentMapData(CServer::MAP_TYPE_SIXUP), pMap->MapData());
	EXPECT_EQ(m_pServer->m_aCurrentMapSize[CServer::MAP_TYPE_SIXUP], (unsigned)pMap->Size());
	EXPECT_EQ(sha256_comp(m_pServer->m_aCurrentMapSha256[CServer::MAP_TYPE_SIXUP], pMap->Sha256()), 0);

	// DDNet clients a conversion, which is what its size, crc and sha256 are of
	const unsigned Size = m_pServer->m_aCurrentMapSize[CServer::MAP_TYPE_SIX];
	const unsigned char *pData = m_pServer->CurrentMapData(CServer::MAP_TYPE_SIX);
	ASSERT_NE(pData, pMap->MapData());
	EXPECT_NE(sha256_comp(m_pServer->m_aCurrentMapSha256[CServer::MAP_TYPE_SIX], pMap->Sha256()), 0);
	EXPECT_EQ(sha256_comp(m_pServer->m_aCurrentMapSha256[CServer::MAP_TYPE_SIX], sha256(pData, Size)), 0);
	EXPECT_EQ(m_pServer->m_aCurrentMapCrc[CServer::MAP_TYPE_SIX], crc32(0, pData, Size));
	std::vector<uint8_t> vData(pData, pData + Size);
	CDataFileReader Reader;
	ASSERT_TRUE(Reader.OpenFromMemory("ctf5_07", vData, "memory"));
	EXPECT_FALSE(IsTeeworlds07Map(Reader));

	// Never the URL of the map before it was converted
	char aSha256[SHA256_MAXSTRSIZE];
	sha256_str(pMap->Sha256(), aSha256, sizeof(aSha256));
	EXPECT_EQ(str_find(m_pServer->m_aMapDownloadUrl, aSha256), nullptr) << m_pServer->m_aMapDownloadUrl;
	sha256_str(m_pServer->m_aCurrentMapSha256[CServer::MAP_TYPE_SIX], aSha256, sizeof(aSha256));
	EXPECT_NE(str_find(m_pServer->m_aMapDownloadUrl, aSha256), nullptr) << m_pServer->m_aMapDownloadUrl;
	EXPECT_TRUE(str_startswith(m_pServer->m_aMapDownloadUrl, "https://maps.example/ctf5_07_"));

	// The map itself stays what the game knows it by
	EXPECT_EQ(sha256_comp(pMap->Sha256(), sha256(pMap->MapData(), pMap->Size())), 0);
}

TEST_F(GameWorld, SixupClientsWaitForTheConversion)
{
	// ctf5 has no version in maps7/, 0.7 clients get a conversion
	ChangeMap(this, "ctf5");
	EXPECT_TRUE(m_pServer->m_SixupMapAvailable);
	EXPECT_TRUE(m_pServer->m_aMapConverting[CServer::MAP_TYPE_SIXUP]);
	EXPECT_FALSE(m_pServer->m_aMapConverting[CServer::MAP_TYPE_SIX]);

	CServer::CClient &Sixup = m_pServer->m_aClients[0];
	CServer::CClient &DDNet = m_pServer->m_aClients[1];
	Sixup.m_State = CServer::CClient::STATE_CONNECTING;
	Sixup.m_Sixup = true;
	DDNet.m_State = CServer::CClient::STATE_CONNECTING;
	DDNet.m_Sixup = false;
	m_pServer->m_vMapMessages.clear();
	m_pServer->SendMap(0);
	m_pServer->SendMap(1);

	// Only the one who needs the conversion waits, and is told nothing,
	// not even a size of 0
	EXPECT_EQ(ToldMapSize(m_pServer, 0), std::nullopt);
	EXPECT_TRUE(Sixup.m_WaitingForMap);
	EXPECT_EQ(ToldMapSize(m_pServer, 1), GameServer()->Map()->Size());
	EXPECT_FALSE(DDNet.m_WaitingForMap);

	WaitForMapConversion(this);
	EXPECT_FALSE(Sixup.m_WaitingForMap);
	const unsigned Size = m_pServer->m_aCurrentMapSize[CServer::MAP_TYPE_SIXUP];
	EXPECT_GT(Size, 0u);
	EXPECT_EQ(ToldMapSize(m_pServer, 0), (int)Size);
	const unsigned char *pData = m_pServer->CurrentMapData(CServer::MAP_TYPE_SIXUP);
	EXPECT_EQ(sha256_comp(m_pServer->m_aCurrentMapSha256[CServer::MAP_TYPE_SIXUP], sha256(pData, Size)), 0);
	std::vector<uint8_t> vData(pData, pData + Size);
	CDataFileReader Reader;
	ASSERT_TRUE(Reader.OpenFromMemory("ctf5", vData, "memory"));
	EXPECT_TRUE(IsTeeworlds07Map(Reader));
	// under a name of its own, as Teeworlds 0.7 comes with another ctf5
	EXPECT_STREQ(m_pServer->m_aSixupMapName, "ctf5_ddnet");
	// DDNet clients were not bothered again
	EXPECT_EQ(std::count_if(m_pServer->m_vMapMessages.begin(), m_pServer->m_vMapMessages.end(), [](const CTestServer::CMessage &Message) { return Message.m_ClientId == 1; }), 2);

	// Loading the same map again takes the conversion there is
	ChangeMap(this, "ctf5");
	EXPECT_FALSE(m_pServer->m_aMapConverting[CServer::MAP_TYPE_SIXUP]);
	EXPECT_EQ(m_pServer->m_aCurrentMapSize[CServer::MAP_TYPE_SIXUP], Size);

	Sixup.m_State = CServer::CClient::STATE_EMPTY;
	Sixup.m_Sixup = false;
	DDNet.m_State = CServer::CClient::STATE_EMPTY;
}

TEST_F(GameWorld, MapThatLooksTheSameIsNotConverted)
{
	// dm6 looks the same in both versions; under this name there is no version in maps7/
	WriteMap(this, "maps/dm6.map", "maps/dm6_copy.map");
	ChangeMap(this, "dm6_copy");
	// which is found out without a conversion
	EXPECT_EQ(m_pServer->m_pMapConversionJob, nullptr);
	EXPECT_FALSE(m_pServer->m_aMapConverting[CServer::MAP_TYPE_SIXUP]);
	EXPECT_TRUE(m_pServer->m_SixupMapAvailable);
	EXPECT_EQ(m_pServer->CurrentMapData(CServer::MAP_TYPE_SIXUP), GameServer()->Map()->MapData());
	EXPECT_EQ(m_pServer->m_aCurrentMapSize[CServer::MAP_TYPE_SIXUP], m_pServer->m_aCurrentMapSize[CServer::MAP_TYPE_SIX]);
	EXPECT_EQ(m_pServer->m_aCurrentMapCrc[CServer::MAP_TYPE_SIXUP], m_pServer->m_aCurrentMapCrc[CServer::MAP_TYPE_SIX]);
	EXPECT_EQ(sha256_comp(m_pServer->m_aCurrentMapSha256[CServer::MAP_TYPE_SIXUP], m_pServer->m_aCurrentMapSha256[CServer::MAP_TYPE_SIX]), 0);
}

TEST_F(GameWorld, MapChangeStopsTheConversion)
{
	ChangeMap(this, "ctf5");
	EXPECT_TRUE(m_pServer->m_aMapConverting[CServer::MAP_TYPE_SIXUP]);
	ChangeMap(this, "Tutorial");
	EXPECT_FALSE(m_pServer->m_aMapConverting[CServer::MAP_TYPE_SIXUP]);
	EXPECT_EQ(m_pServer->m_pMapConversionJob, nullptr);
	WaitForMapConversion(this);
	for(int i = 0; i < 30000 && !m_pServer->m_vpStoppedMapConversions.empty(); i++)
	{
		m_pServer->UpdateMapConversion();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	EXPECT_TRUE(m_pServer->m_vpStoppedMapConversions.empty());

	// What the stopped conversion gave is not served
	void *pData;
	unsigned Size;
	ASSERT_TRUE(m_pStorage->ReadFile("maps7/Tutorial.map", IStorage::TYPE_ALL, &pData, &Size));
	EXPECT_EQ(m_pServer->m_aCurrentMapSize[CServer::MAP_TYPE_SIXUP], Size);
	EXPECT_EQ(mem_comp(m_pServer->CurrentMapData(CServer::MAP_TYPE_SIXUP), pData, Size), 0);
	free(pData);
	EXPECT_EQ(m_pServer->m_apMapConversion[CServer::MAP_TYPE_SIXUP], nullptr);
	EXPECT_STREQ(m_pServer->m_aSixupMapName, "Tutorial");
}

TEST_F(GameWorld, ClosestCharacter)
{
	CNetObj_PlayerInput Input = {};
	CCharacter *pChr1 = new CCharacter(&GameServer()->m_World, Input);
	pChr1->m_Pos = vec2(0, 0);
	GameServer()->m_World.InsertEntity(pChr1);

	CCharacter *pChr2 = new CCharacter(&GameServer()->m_World, Input);
	pChr2->m_Pos = vec2(10, 10);
	GameServer()->m_World.InsertEntity(pChr2);

	CCharacter *pClosest = GameServer()->m_World.ClosestCharacter(vec2(1, 1), 20, nullptr);
	EXPECT_EQ(pClosest, pChr1);
}

TEST_F(GameWorld, IntersectEntity)
{
	CNetObj_PlayerInput Input = {};
	CCharacter *pChrLeft = new CCharacter(&GameServer()->m_World, Input);
	pChrLeft->m_Pos = vec2(15, 10);
	GameServer()->m_World.InsertEntity(pChrLeft);

	CCharacter *pChrRight = new CCharacter(&GameServer()->m_World, Input);
	pChrRight->m_Pos = vec2(16, 10);
	GameServer()->m_World.InsertEntity(pChrRight);

	float Radius = 5.0f;
	vec2 IntersectAt;
	CCharacter *pIntersectedChar;

	// both tees are exactly on the line
	// if we go intersect left to right we find the left one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(10, 10), // intersect from
		vec2(20, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrLeft);

	// if we intersect right to left we find the right one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrRight);

	// but not if we ignore the right one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		pChrRight, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrLeft);

	// or we force find the left one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		pChrLeft /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrLeft);

	// pNotThis == pThisOnly => nullptr

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		pChrLeft, // pNotThis
		-1, // CollideWith
		pChrLeft /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, nullptr);

	// the tee closer to the start of the intersection line
	// will not be matched if it is further than Radius away
	// from the line

	vec2 CloserToFromButTooFarFromLine = vec2(11, 11 + Radius + pChrLeft->GetProximityRadius());
	pChrLeft->SetPosition(CloserToFromButTooFarFromLine);
	pChrLeft->m_Pos = CloserToFromButTooFarFromLine;

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(10, 10), // intersect from
		vec2(20, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrRight);
}

TEST_F(GameWorld, BasicTick)
{
	int ClientId = 0;
	bool Afk = true;
	int LastWhisperTo = -1;
	const int StartTeam = GameController()->GetAutoTeam(ClientId);
	GameServer()->CreatePlayer(ClientId, StartTeam, Afk, LastWhisperTo);

	GameServer()->OnTick();
}

TEST_F(GameWorld, CharacterEmote)
{
	int ClientId = 0;
	bool Afk = true;
	int LastWhisperTo = -1;
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, Afk, LastWhisperTo);
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	pPlayer->ForceSpawn(vec2(0, 0));
	CCharacter *pChr = pPlayer->GetCharacter();
	ASSERT_NE(pChr, nullptr);

	// afk
	pPlayer->SetAfk(true);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_BLINK);

	// not afk
	pPlayer->SetAfk(false);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_NORMAL);

	// frozen
	pChr->Freeze(10);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_BLINK);

	// frozen and paused
	pPlayer->Pause(CPlayer::PAUSE_PAUSED, true);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_NORMAL);

	// ninja jetpack
	pPlayer->Pause(CPlayer::PAUSE_NONE, true);
	pChr->Unfreeze();
	static_cast<CPlayerDDRace *>(pPlayer)->m_NinjaJetpack = true;
	pChr->SetJetpack(true);
	pChr->SetActiveWeapon(WEAPON_GUN);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_HAPPY);

	// /emote angry 3 chat command
	pChr->SetEmote(EMOTE_ANGRY, GameServer()->Server()->Tick() + GameServer()->Server()->TickSpeed() * 3);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_ANGRY);

	// /emote angry 3 chat command and frozen
	pChr->Freeze(10);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_ANGRY);
}

TEST(Tunings, OutOfRangeBecomesIntMin)
{
	const float IntMin = std::numeric_limits<int>::min() / 100.0f;
	CTuneParam Param;
	EXPECT_EQ((float)(Param = 555555555555555.0f), IntMin);
	EXPECT_EQ((float)(Param = -555555555555555.0f), IntMin);
	EXPECT_EQ((float)(Param = std::numeric_limits<float>::quiet_NaN()), IntMin);
	EXPECT_EQ((float)(Param = 0.5f), 0.5f);
}

TEST_F(GameWorld, VanillaWeaponFire)
{
	constexpr int ClientId = 0;
	SelectGameMode("dm");
	auto &Controller = *dynamic_cast<CGameControllerVanillaDM *>(GameController());
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	pPlayer->ForceSpawn(vec2(0, 0));
	CCharacter *pCharacter = pPlayer->GetCharacter();
	ASSERT_NE(pCharacter, nullptr);
	const CTuningParams Tuning = *GameServer()->GlobalTuning();
	auto CountProjectiles = [this]() {
		int Count = 0;
		for(CEntity *pEntity = GameServer()->m_World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEntity; pEntity = pEntity->TypeNext())
			Count++;
		return Count;
	};

	pCharacter->SetWeaponAmmo(WEAPON_GUN, 10);
	CWeaponFireContext Context = {pCharacter, WEAPON_GUN, vec2(1, 0), vec2(1, 0), pCharacter->m_Pos, &Tuning};
	const int BeforeGun = CountProjectiles();
	pCharacter->SetActiveWeapon(WEAPON_GUN);
	CNetObj_PlayerInput Input = {};
	Input.m_TargetX = 1;
	pCharacter->OnDirectInput(&Input);
	Input.m_Fire = 1;
	pCharacter->OnDirectInput(&Input);
	EXPECT_EQ(pCharacter->GetWeaponAmmo(WEAPON_GUN), 9);
	EXPECT_EQ(CountProjectiles(), BeforeGun + 1);
	pCharacter->SetWeaponAmmo(WEAPON_SHOTGUN, 10);
	Context.m_Weapon = WEAPON_SHOTGUN;
	const int BeforeShotgun = CountProjectiles();
	const CWeaponFireResult ShotgunResult = Controller.OnCharacterFireWeapon(Context);
	EXPECT_TRUE(ShotgunResult.m_Fired);
	EXPECT_TRUE(ShotgunResult.m_ConsumeAmmo);
	EXPECT_EQ(CountProjectiles(), BeforeShotgun + 5);

	pCharacter->SetWeaponAmmo(WEAPON_GUN, 0);
	Context.m_Weapon = WEAPON_GUN;
	const int BeforeNoAmmo = CountProjectiles();
	const CWeaponFireResult NoAmmoResult = Controller.OnCharacterFireWeapon(Context);
	EXPECT_FALSE(NoAmmoResult.m_Fired);
	EXPECT_GT(NoAmmoResult.m_ReloadTicks, 0);
	EXPECT_EQ(CountProjectiles(), BeforeNoAmmo);
}

TEST(MatchLifecycle, PreservesRoundTransitions)
{
	CMatchLifecycle Match(10);
	EXPECT_TRUE(Match.IsRunning());
	Match.SetWarmupTicks(2);
	EXPECT_FALSE(Match.EndRound(11));
	EXPECT_FALSE(Match.TickWarmup());
	EXPECT_TRUE(Match.TickWarmup());

	Match.StartRound(20);
	Match.BeginSuddenDeath();
	EXPECT_TRUE(Match.IsSuddenDeath());
	EXPECT_TRUE(Match.EndRound(30));
	EXPECT_FALSE(Match.EndRound(31));
	EXPECT_TRUE(Match.IsGameOver());
	EXPECT_FALSE(Match.IsSuddenDeath());
	EXPECT_FALSE(Match.ShouldRestartRound(40, 10));
	EXPECT_TRUE(Match.ShouldRestartRound(41, 10));
	Match.StartRound(41);
	Match.AdvanceRound();
	EXPECT_EQ(Match.RoundStartTick(), 41);
	EXPECT_EQ(Match.RoundCount(), 1);
}

TEST(MatchLifecycle, WaitsForPlayersUntilTheModeStartsTheMatch)
{
	CMatchLifecycle Match(10);
	Match.WaitForPlayers();
	EXPECT_TRUE(Match.IsWarmup());
	EXPECT_TRUE(Match.IsWaitingForPlayers());
	EXPECT_FALSE(Match.IsRunning());
	EXPECT_EQ(Match.WarmupTicks(), 0);
	// only the mode ends it
	for(int i = 0; i < 100; i++)
		EXPECT_FALSE(Match.TickWarmup());
	EXPECT_FALSE(Match.EndRound(20));
	Match.SetWarmupTicks(0);
	EXPECT_TRUE(Match.IsRunning());
	Match.SetRoundStartTick(30);
	EXPECT_EQ(Match.RoundStartTick(), 30);
}

TEST_F(GameWorld, MatchReportTracksJoinsLeaversAndTeams)
{
	SelectGameMode("tdm");
	const int RoundStartTick = m_pServer->Tick();
	JoinPlayer(1, TEAM_RED, "stays");
	m_pServer->AdvanceTick(10);
	CPlayer *pFirst = JoinPlayer(0, TEAM_RED, "leaves");
	const uint32_t FirstUniqueClientId = pFirst->GetUniqueCid();
	GameController()->DoTeamChange(pFirst, TEAM_BLUE, false);
	m_pServer->AdvanceTick(20);
	LeavePlayer(0);

	// the slot is taken again by somebody else
	m_pServer->AdvanceTick(5);
	CPlayer *pSecond = JoinPlayer(0, TEAM_RED, "comes");
	ASSERT_NE(pSecond->GetUniqueCid(), FirstUniqueClientId);
	m_pServer->AdvanceTick(15);
	GameController()->EndRound();

	const CReceivedMatchReport Received = ReceivedMatchReport(0);
	EXPECT_FALSE(Received.m_Live);
	EXPECT_FALSE(Received.m_PersistOnDisconnect);
	const CMatchReport &Report = Received.m_Report;
	EXPECT_EQ(Report.m_ModeId, "tdm");
	EXPECT_EQ(Report.m_RoundStartTick, RoundStartTick);
	EXPECT_EQ(Report.m_DurationTicks, 50);
	EXPECT_EQ(Report.m_Termination, EMatchTermination::COMPLETED);
	ASSERT_EQ(Report.m_vParticipants.size(), 3u);
	const CMatchParticipant *pLeft = Received.Participant("leaves");
	const CMatchParticipant *pCame = Received.Participant("comes");
	ASSERT_NE(pLeft, nullptr);
	ASSERT_NE(pCame, nullptr);
	EXPECT_EQ(Received.m_LocalParticipantId, pCame->m_ParticipantId);
	EXPECT_EQ(pLeft->m_JoinedTick, 10);
	EXPECT_EQ(pLeft->m_LeftTick, 30);
	EXPECT_EQ(pLeft->m_TeamId, TEAM_BLUE);
	EXPECT_EQ(pCame->m_JoinedTick, 35);
	EXPECT_FALSE(pCame->m_LeftTick.has_value());
	EXPECT_EQ(pCame->m_TeamId, TEAM_RED);
	EXPECT_EQ(Received.Metric(pLeft->m_ParticipantId, "playtime_ticks"), 20);
	EXPECT_EQ(Received.Metric(pCame->m_ParticipantId, "playtime_ticks"), 15);
	EXPECT_EQ(Received.Metric(pCame->m_ParticipantId, "score"), 0);
	// a draw between the teams, and everybody shares the result of their team
	ASSERT_NE(Report.Standing(EMatchSubjectKind::TEAM, TEAM_BLUE), nullptr);
	EXPECT_EQ(Report.Standing(EMatchSubjectKind::TEAM, TEAM_BLUE)->m_Outcome, EMatchOutcome::DRAW);
	EXPECT_EQ(Report.Standing(EMatchSubjectKind::TEAM, TEAM_RED)->m_Rank, 1);
	ASSERT_NE(Report.Standing(EMatchSubjectKind::PARTICIPANT, pLeft->m_ParticipantId), nullptr);
	EXPECT_EQ(Report.Standing(EMatchSubjectKind::PARTICIPANT, pLeft->m_ParticipantId)->m_Outcome, EMatchOutcome::DRAW);
	EXPECT_EQ(Report.m_vStandings.size(), 5u);

	// the one who stayed from the start gets the same report
	EXPECT_EQ(ReceivedMatchReport(1).m_Report.m_MatchId, Report.m_MatchId);
}

TEST_F(GameWorld, MatchReportSkipsWarmup)
{
	g_Config.m_SvWarmup = 10;
	SelectGameMode("dm");
	JoinPlayer(0, TEAM_GAME, "player");
	GameController()->EndRound();
	EXPECT_TRUE(ReceivedMatchReports(0).empty());

	GameController()->DoWarmup(0);
	GameController()->StartRound();
	GameController()->EndRound();
	EXPECT_EQ(ReceivedMatchReport(0).m_Report.m_Termination, EMatchTermination::COMPLETED);
}

TEST_F(GameWorld, MatchReportTracksKillsDeathsAndSuicides)
{
	SelectGameMode("dm");
	CPlayer *pKiller = JoinPlayer(0, TEAM_GAME, "killer");
	CPlayer *pVictim = JoinPlayer(1, TEAM_GAME, "victim");
	CCharacter *pVictimCharacter = pVictim->ForceSpawn(vec2(64.0f, 96.0f));
	ASSERT_NE(pVictimCharacter, nullptr);
	GameController()->OnCharacterDeath({pVictimCharacter, pKiller, pKiller->GetCid(), WEAPON_GUN, false});
	CCharacter *pKillerCharacter = pKiller->ForceSpawn(vec2(64.0f, 96.0f));
	ASSERT_NE(pKillerCharacter, nullptr);
	GameController()->OnCharacterDeath({pKillerCharacter, pKiller, pKiller->GetCid(), WEAPON_SELF, false});
	GameController()->EndRound();

	const CReceivedMatchReport Received = ReceivedMatchReport(0);
	const int KillerId = Received.Participant("killer")->m_ParticipantId;
	const int VictimId = Received.Participant("victim")->m_ParticipantId;
	EXPECT_EQ(Received.Metric(KillerId, "kills"), 1);
	EXPECT_EQ(Received.Metric(KillerId, "weapon_1_kills"), 1);
	EXPECT_EQ(Received.Metric(KillerId, "deaths"), 1);
	EXPECT_EQ(Received.Metric(KillerId, "suicides"), 1);
	EXPECT_EQ(Received.Metric(VictimId, "deaths"), 1);
	EXPECT_EQ(Received.Metric(VictimId, "weapon_1_deaths"), 1);
	// only what happened is counted
	EXPECT_FALSE(Received.Metric(VictimId, "kills").has_value());
	EXPECT_FALSE(Received.Metric(VictimId, "suicides").has_value());
}

TEST_F(GameWorld, MatchReportTracksWeaponCombat)
{
	SelectGameMode("dm");
	CPlayer *pAttacker = JoinPlayer(0, TEAM_GAME, "attacker");
	CPlayer *pVictim = JoinPlayer(1, TEAM_GAME, "victim");
	CCharacter *pAttackerCharacter = pAttacker->ForceSpawn(vec2(0.0f, 0.0f));
	CCharacter *pVictimCharacter = pVictim->ForceSpawn(vec2(64.0f, 0.0f));
	ASSERT_NE(pAttackerCharacter, nullptr);
	ASSERT_NE(pVictimCharacter, nullptr);

	pAttackerCharacter->SetActiveWeapon(WEAPON_GUN);
	pAttackerCharacter->SetWeaponAmmo(WEAPON_GUN, 10);
	CNetObj_PlayerInput Input = {};
	Input.m_TargetX = 1;
	pAttackerCharacter->OnDirectInput(&Input);
	Input.m_Fire = 1;
	pAttackerCharacter->OnDirectInput(&Input);
	pVictimCharacter->SetHealth(10);
	EXPECT_TRUE(pVictimCharacter->TakeDamage(vec2(0.0f, 0.0f), 3, pAttacker->GetCid(), WEAPON_GUN));
	GameController()->EndRound();

	const CReceivedMatchReport Received = ReceivedMatchReport(0);
	const int AttackerId = Received.Participant("attacker")->m_ParticipantId;
	const int VictimId = Received.Participant("victim")->m_ParticipantId;
	EXPECT_EQ(Received.Metric(AttackerId, "shots"), 1);
	EXPECT_EQ(Received.Metric(AttackerId, "hits"), 1);
	EXPECT_EQ(Received.Metric(AttackerId, "damage_done"), 3);
	EXPECT_EQ(Received.Metric(AttackerId, "weapon_1_shots"), 1);
	EXPECT_EQ(Received.Metric(AttackerId, "weapon_1_hits"), 1);
	EXPECT_EQ(Received.Metric(AttackerId, "weapon_1_damage_done"), 3);
	EXPECT_EQ(Received.Metric(VictimId, "damage_taken"), 3);
	EXPECT_EQ(Received.Metric(VictimId, "weapon_1_damage_taken"), 3);
}

TEST_F(GameWorld, MatchReportEndsOnceAndARestartEndsItWithoutResult)
{
	SelectGameMode("dm");
	JoinPlayer(0, TEAM_GAME, "player");
	GameController()->EndRound();
	GameController()->EndRound();
	const CReceivedMatchReport Completed = ReceivedMatchReport(0);
	EXPECT_EQ(Completed.m_Report.m_Termination, EMatchTermination::COMPLETED);

	// the round after the finished one starts, then is restarted in the middle
	GameController()->StartRound();
	EXPECT_TRUE(ReceivedMatchReports(0).empty());
	GameController()->StartRound();
	const CReceivedMatchReport Restarted = ReceivedMatchReport(0);
	EXPECT_EQ(Restarted.m_Report.m_Termination, EMatchTermination::ADMIN_ENDED);
	EXPECT_NE(Restarted.m_Report.m_MatchId, Completed.m_Report.m_MatchId);
	ASSERT_EQ(Restarted.m_Report.m_vStandings.size(), 1u);
	EXPECT_EQ(Restarted.m_Report.m_vStandings[0].m_Outcome, EMatchOutcome::DNF);

	GameController()->AbortMatchReport();
	const CReceivedMatchReport Aborted = ReceivedMatchReport(0);
	EXPECT_EQ(Aborted.m_Report.m_Termination, EMatchTermination::ADMIN_ENDED);
	GameController()->AbortMatchReport();
	EXPECT_TRUE(ReceivedMatchReports(0).empty());
}

namespace
{
	class CNamedWinnerController : public CGameControllerVanillaDM
	{
	public:
		using CGameControllerVanillaDM::CGameControllerVanillaDM;
		using CGameControllerVanillaDM::SetMatchWinners;
		using CGameControllerVanillaDM::VanillaPlayer;
	};
}

TEST_F(GameWorld, MatchReportRanksTheWinnerTheModeNames)
{
	CNamedWinnerController &Controller = SelectController<CNamedWinnerController>("dm");
	CPlayer *pSurvivor = JoinPlayer(0, TEAM_GAME, "survivor");
	JoinPlayer(1, TEAM_GAME, "scorer");
	JoinPlayer(2, TEAM_GAME, "other");
	Controller.VanillaPlayer(0)->m_Score = 1;
	Controller.VanillaPlayer(1)->m_Score = 5;
	Controller.VanillaPlayer(2)->m_Score = 3;
	Controller.SetMatchWinners({pSurvivor});
	GameController()->EndRound();

	const CReceivedMatchReport Received = ReceivedMatchReport(0);
	const auto Standing = [&](const char *pName) {
		const int Id = Received.Participant(pName)->m_ParticipantId;
		for(const CMatchStanding &Candidate : Received.m_Report.m_vStandings)
			if(Candidate.m_SubjectKind == EMatchSubjectKind::PARTICIPANT && Candidate.m_SubjectId == Id)
				return Candidate;
		return CMatchStanding{};
	};
	EXPECT_EQ(Standing("survivor").m_Rank, 1);
	EXPECT_EQ(Standing("survivor").m_Outcome, EMatchOutcome::WIN);
	EXPECT_EQ(Standing("scorer").m_Rank, 2);
	EXPECT_EQ(Standing("scorer").m_Outcome, EMatchOutcome::LOSS);
	EXPECT_EQ(Standing("other").m_Rank, 3);

	// the next match ranks by score again
	GameController()->StartRound();
	Controller.VanillaPlayer(1)->m_Score = 5;
	GameController()->EndRound();
	const CReceivedMatchReport Next = ReceivedMatchReport(0);
	const int ScorerId = Next.Participant("scorer")->m_ParticipantId;
	for(const CMatchStanding &Candidate : Next.m_Report.m_vStandings)
	{
		if(Candidate.m_SubjectId == ScorerId)
		{
			EXPECT_EQ(Candidate.m_Outcome, EMatchOutcome::WIN);
		}
	}
}

TEST_F(GameWorld, MatchReportKeepsTheParticipantsThatFit)
{
	SelectGameMode("dm");
	JoinPlayer(1, TEAM_GAME, "first");
	for(int i = 0; i < MatchReportLimits::MAX_PARTICIPANTS; ++i)
	{
		JoinPlayer(0, TEAM_GAME, "passing");
		LeavePlayer(0);
	}
	GameController()->EndRound();
	EXPECT_EQ(ReceivedMatchReport(1).m_Report.m_vParticipants.size(), (size_t)MatchReportLimits::MAX_PARTICIPANTS);
}

TEST_F(GameWorld, MatchReportIsSentInChunksOverTicks)
{
	auto &Controller = SelectController<CTestVanillaCTF>("ctf");
	CPlayer *pPlayer = JoinPlayer(0, TEAM_RED, "player");
	// metric ids of a mod, enough of them for a report that does not fit into one tick
	static const std::vector<std::string> s_vMetricIds = [] {
		std::vector<std::string> vIds;
		vIds.reserve(1000);
		for(int i = 0; i < 1000; i++)
			vIds.push_back("mod_metric_" + std::to_string(i) + "_with_a_long_name");
		return vIds;
	}();
	for(const std::string &MetricId : s_vMetricIds)
		Controller.AddMatchMetric(pPlayer, MetricId.c_str(), 1);
	Controller.EndRound();

	// the start and the first chunks go out with the end of the round
	const size_t Announced = m_pServer->m_vMatchReportMessages.size();
	EXPECT_EQ(Announced, 9u);
	const CReceivedMatchReport Received = ReceivedMatchReport(0);
	EXPECT_GT(Received.m_NumChunks, 8);
	EXPECT_EQ(Received.Metric(Received.m_LocalParticipantId, "mod_metric_999_with_a_long_name"), 1);
}

TEST_F(GameWorld, MatchLiveStatsAreRateLimitedAndShareTheMatchId)
{
	SelectGameMode("dm");
	CPlayer *pAttacker = JoinPlayer(0, TEAM_GAME, "live-player");
	CPlayer *pVictim = JoinPlayer(1, TEAM_GAME, "other");
	CCharacter *pAttackerCharacter = pAttacker->ForceSpawn(vec2(0.0f, 0.0f));
	CCharacter *pVictimCharacter = pVictim->ForceSpawn(vec2(64.0f, 0.0f));
	ASSERT_NE(pAttackerCharacter, nullptr);
	ASSERT_NE(pVictimCharacter, nullptr);
	pVictimCharacter->SetHealth(10);
	ASSERT_TRUE(pVictimCharacter->TakeDamage(vec2(), 3, 0, WEAPON_GUN));

	GameController()->SendLiveStats(0);
	GameController()->SendLiveStats(0);
	const CReceivedMatchReport Live = ReceivedMatchReport(0);
	EXPECT_TRUE(Live.m_Live);
	EXPECT_TRUE(Live.m_PersistOnDisconnect);
	EXPECT_EQ(Live.m_Report.m_Termination, EMatchTermination::ABORTED);
	// everyone is in it, the one who asked is marked
	ASSERT_EQ(Live.m_Report.m_vParticipants.size(), 2u);
	EXPECT_EQ(Live.m_LocalParticipantId, Live.Participant("live-player")->m_ParticipantId);
	EXPECT_EQ(Live.Metric(Live.m_LocalParticipantId, "weapon_1_damage_done"), 3);

	m_pServer->AdvanceTick(m_pServer->TickSpeed() * 2);
	GameController()->SendLiveStats(0);
	GameController()->EndRound();
	// the final report is not replaced by a live one
	GameController()->SendLiveStats(0);
	const std::vector<CReceivedMatchReport> vReports = ReceivedMatchReports(0);
	ASSERT_EQ(vReports.size(), 2u);
	EXPECT_TRUE(vReports[0].m_Live);
	EXPECT_FALSE(vReports[1].m_Live);
	EXPECT_EQ(vReports[1].m_Report.m_MatchId, Live.m_Report.m_MatchId);
}

TEST_F(GameWorld, RaceLiveStatsUseTheLoadedPlayerData)
{
	SelectGameMode("ddnet");
	CPlayer *pPlayer = JoinPlayer(0, TEAM_GAME, "race-player");
	m_pServer->AdvanceTick(200);
	auto *pCharacter = static_cast<CCharacterDDRace *>(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pCharacter, nullptr);
	pCharacter->m_DDRaceState = ERaceState::STARTED;
	pCharacter->m_StartTime = m_pServer->Tick() - 100;
	pCharacter->m_LastTimeCp = 4;
	CPlayerData *pData = RaceScore().PlayerData(0);
	pData->m_BestTime = 60.5f;
	pData->m_MapRank = 7;
	pData->m_MapFinishes = 12;
	pData->m_SessionFinishes = 2;
	pData->m_LastFinishTime = 61.25f;
	RaceScore().SetCurrentRecord(50.0f);

	// what comes from the database is left out until it is there
	GameController()->SendLiveStats(0);
	const CReceivedMatchReport Loading = ReceivedMatchReport(0);
	EXPECT_FALSE(Loading.Metric(0, "personal_best_ticks").has_value());
	EXPECT_FALSE(Loading.Metric(0, "map_rank").has_value());
	EXPECT_EQ(Loading.Metric(0, "session_finishes"), 2);

	pData->m_PlayerDataLoaded = true;
	m_pServer->AdvanceTick(m_pServer->TickSpeed() * 2);
	GameController()->SendLiveStats(0);
	const CReceivedMatchReport Live = ReceivedMatchReport(0);
	EXPECT_TRUE(Live.m_Live);
	EXPECT_FALSE(Live.m_PersistOnDisconnect);
	EXPECT_EQ(Live.m_Report.m_ModeId, "ddnet");
	EXPECT_EQ(Live.m_Report.m_MatchId, Loading.m_Report.m_MatchId);
	EXPECT_EQ(Live.m_LocalParticipantId, 0);
	ASSERT_EQ(Live.m_Report.m_vStandings.size(), 1u);
	EXPECT_EQ(Live.m_Report.m_vStandings[0].m_Rank, 7);
	EXPECT_EQ(Live.Metric(0, "personal_best_ticks"), 3025);
	EXPECT_EQ(Live.Metric(0, "map_best_ticks"), 2500);
	EXPECT_EQ(Live.Metric(0, "map_rank"), 7);
	EXPECT_EQ(Live.Metric(0, "map_finishes"), 12);
	EXPECT_EQ(Live.Metric(0, "last_finish_ticks"), 3063);
	EXPECT_EQ(Live.Metric(0, "current_run_ticks"), 100 + m_pServer->TickSpeed() * 2 + 20);
	EXPECT_EQ(Live.Metric(0, "current_checkpoint"), 5);

	// a race has no rounds to report
	GameController()->EndRound();
	EXPECT_TRUE(ReceivedMatchReports(0).empty());
}

TEST_F(GameWorld, MapEntitySetsAreExplicit)
{
	auto CountEntities = [this](int Type) {
		int Count = 0;
		for(CEntity *pEntity = GameServer()->m_World.FindFirst(Type); pEntity; pEntity = pEntity->TypeNext())
			Count++;
		return Count;
	};

	CGameControllerVanillaDM VanillaController(GameServices(), *FindGameMode("dm"));
	const int BeforeVanilla = CountEntities(CGameWorld::ENTTYPE_PROJECTILE);
	EXPECT_FALSE(VanillaController.OnEntity({ENTITY_CRAZY_SHOTGUN, 10, 10, LAYER_GAME, 0, true, 0}));
	EXPECT_EQ(CountEntities(CGameWorld::ENTTYPE_PROJECTILE), BeforeVanilla);
	EXPECT_TRUE(VanillaController.OnEntity({ENTITY_SPAWN, 10, 10, LAYER_GAME, 0, true, 0}));
	EXPECT_TRUE(VanillaController.OnEntity({ENTITY_SPAWN, 10, 10, LAYER_GAME, 0, false, 0}));
	const int BeforePickups = CountEntities(CGameWorld::ENTTYPE_PICKUP);
	EXPECT_TRUE(VanillaController.OnEntity({ENTITY_HEALTH_1, 10, 10, LAYER_GAME, 0, true, 0}));
	EXPECT_EQ(CountEntities(CGameWorld::ENTTYPE_PICKUP), BeforePickups + 1);
	EXPECT_EQ(static_cast<CPickup *>(GameServer()->m_World.FindFirst(CGameWorld::ENTTYPE_PICKUP))->Type(), POWERUP_HEALTH);

	CGameControllerDDNet DDNetController(GameServices(), *FindGameMode("ddnet"));
	EXPECT_TRUE(DDNetController.OnEntity({ENTITY_CRAZY_SHOTGUN, 10, 10, LAYER_GAME, 0, true, 0}));
	EXPECT_EQ(CountEntities(CGameWorld::ENTTYPE_PROJECTILE), BeforeVanilla + 1);
	EXPECT_TRUE(DDNetController.OnEntity({ENTITY_HEALTH_1, 10, 10, LAYER_GAME, 0, true, 0}));
	EXPECT_EQ(static_cast<CPickup *>(GameServer()->m_World.FindFirst(CGameWorld::ENTTYPE_PICKUP))->Type(), POWERUP_FREEZE);

	CGameControllerDDRace ModController(GameServices(), *FindGameMode("mod"));
	EXPECT_TRUE(ModController.OnEntity({ENTITY_CRAZY_SHOTGUN, 10, 10, LAYER_GAME, 0, true, 0}));
	EXPECT_EQ(CountEntities(CGameWorld::ENTTYPE_PROJECTILE), BeforeVanilla + 2);
}

TEST_F(GameWorld, DDRacePhysicsFlagsDoNotLeakRaceMetadata)
{
	const CTestGameControllerDDRace ModController(GameServices(), *FindGameMode("mod"));
	const CTestGameControllerDDNet DDNetController(GameServices(), *FindGameMode("ddnet"));

	const int PhysicsFlags = GAMEINFOFLAG_PREDICT_DDRACE |
				 GAMEINFOFLAG_PREDICT_DDRACE_TILES |
				 GAMEINFOFLAG_ENTITIES_DDNET |
				 GAMEINFOFLAG_ENTITIES_DDRACE;
	const int RaceFlags = GAMEINFOFLAG_TIMESCORE |
			      GAMEINFOFLAG_GAMETYPE_RACE |
			      GAMEINFOFLAG_GAMETYPE_DDRACE |
			      GAMEINFOFLAG_GAMETYPE_DDNET |
			      GAMEINFOFLAG_RACE_RECORD_MESSAGE |
			      GAMEINFOFLAG_ENTITIES_RACE |
			      GAMEINFOFLAG_RACE;

	EXPECT_EQ(ModController.TestGameInfoFlags() & PhysicsFlags, PhysicsFlags);
	EXPECT_EQ(ModController.TestGameInfoFlags() & RaceFlags, 0);
	EXPECT_EQ(DDNetController.TestGameInfoFlags() & (PhysicsFlags | RaceFlags), PhysicsFlags | RaceFlags);
	EXPECT_EQ(ModController.TestGameInfoFlags2(), DDNetController.TestGameInfoFlags2());
}

TEST_F(GameWorld, DDRaceMapSettingsAreModeOwned)
{
	ASSERT_GE(GameServer()->Collision()->GetWidth() * GameServer()->Collision()->GetHeight(), 6);
	CGameControllerDDRace Controller(GameServices(), *FindGameMode("mod"));
	const int aMapSettings[] = {TILE_OLDLASER, TILE_NPC, TILE_EHOOK, TILE_NOHIT, TILE_NPH};
	for(int i = 0; i < 5; i++)
		GameServer()->Collision()->SetCollisionAt((i + 1) * 32.0f, 0.0f, aMapSettings[i]);

	g_Config.m_SvOldLaser = 0;
	g_Config.m_SvEndlessDrag = 0;
	g_Config.m_SvHit = 1;
	Controller.OnReset();

	EXPECT_EQ(g_Config.m_SvOldLaser, 1);
	EXPECT_EQ(g_Config.m_SvEndlessDrag, 1);
	EXPECT_EQ(g_Config.m_SvHit, 0);
	EXPECT_EQ(GameServer()->GlobalTuning()->m_PlayerCollision, 0);
	EXPECT_EQ(GameServer()->GlobalTuning()->m_PlayerHooking, 0);
}

TEST_F(GameWorld, CharacterTickPhasesAreModeOwned)
{
	vec2 SpawnPos;
	bool FoundSpawn = false;
	for(int y = 1; y < GameServer()->Collision()->GetHeight() - 1 && !FoundSpawn; y++)
	{
		for(int x = 1; x < GameServer()->Collision()->GetWidth() - 1; x++)
		{
			const vec2 Candidate(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
			if(!GameServer()->Collision()->TestBox(Candidate, CCharacterCore::PhysicalSizeVec2()))
			{
				SpawnPos = Candidate;
				FoundSpawn = true;
				break;
			}
		}
	}
	ASSERT_TRUE(FoundSpawn);

	g_Config.m_SvNoWeakHook = 0;

	SelectGameMode("dm");
	CCharacter *pVanillaCharacter = SpawnPlayer(0, SpawnPos);
	EXPECT_NE(pVanillaCharacter, nullptr);
	if(!pVanillaCharacter)
	{
		delete GameServer()->m_apPlayers[0];
		GameServer()->m_apPlayers[0] = nullptr;
		return;
	}
	EXPECT_EQ(dynamic_cast<CCharacterDDRace *>(pVanillaCharacter), nullptr);
	EXPECT_TRUE(pVanillaCharacter->Freeze(2));
	const int VanillaFreezeBefore = pVanillaCharacter->m_FreezeTime;
	pVanillaCharacter->Tick();
	EXPECT_EQ(pVanillaCharacter->m_FreezeTime, VanillaFreezeBefore);

	const vec2 ClippedPos(-10000.0f, -10000.0f);
	pVanillaCharacter->SetPosition(ClippedPos);
	pVanillaCharacter->m_Pos = ClippedPos;
	pVanillaCharacter->Tick();
	EXPECT_FALSE(pVanillaCharacter->IsAlive());
	delete GameServer()->m_apPlayers[0];
	GameServer()->m_apPlayers[0] = nullptr;

	SelectGameMode("ddnet");
	CCharacter *pRaceCharacter = SpawnPlayer(0, SpawnPos);
	EXPECT_NE(pRaceCharacter, nullptr);
	if(!pRaceCharacter)
	{
		return;
	}
	EXPECT_NE(dynamic_cast<CCharacterDDRace *>(pRaceCharacter), nullptr);
	EXPECT_TRUE(pRaceCharacter->Freeze(2));
	const int RaceFreezeBefore = pRaceCharacter->m_FreezeTime;
	pRaceCharacter->Tick();
	EXPECT_EQ(pRaceCharacter->m_FreezeTime, RaceFreezeBefore - 1);

	delete GameServer()->m_apPlayers[0];
	GameServer()->m_apPlayers[0] = nullptr;
	SelectGameMode("mod");
	CCharacter *pModCharacter = SpawnPlayer(0, SpawnPos);
	ASSERT_NE(dynamic_cast<CCharacterDDRace *>(pModCharacter), nullptr);
	EXPECT_TRUE(pModCharacter->Freeze(2));
	const int ModFreezeBefore = pModCharacter->m_FreezeTime;
	pModCharacter->Tick();
	EXPECT_EQ(pModCharacter->m_FreezeTime, ModFreezeBefore - 1);
}

TEST_F(GameWorld, WhetherAHitThawsIsModeOwned)
{
	// DDRace: the laser and the hammer thaw
	SelectGameMode("ddnet");
	CCharacter *pRaceVictim = SpawnPlayer(0, vec2(64.0f, 96.0f));
	ASSERT_NE(SpawnPlayer(1, vec2(128.0f, 96.0f)), nullptr);
	ASSERT_NE(pRaceVictim, nullptr);
	EXPECT_TRUE(pRaceVictim->Freeze(5));
	pRaceVictim->TakeDamage(vec2(), 0, 1, WEAPON_LASER);
	EXPECT_EQ(pRaceVictim->m_FreezeTime, 0);
	EXPECT_TRUE(pRaceVictim->Freeze(5));
	pRaceVictim->TakeDamage(vec2(), 0, 1, WEAPON_HAMMER);
	EXPECT_EQ(pRaceVictim->m_FreezeTime, 0);
	EXPECT_TRUE(pRaceVictim->Freeze(5));
	pRaceVictim->TakeDamage(vec2(), 0, 1, WEAPON_GRENADE);
	EXPECT_GT(pRaceVictim->m_FreezeTime, 0);
	DeletePlayers();

	// the vanilla modes know no freeze of their own and leave it be
	SelectGameMode("dm");
	CCharacter *pVanillaVictim = SpawnPlayer(0, vec2(64.0f, 96.0f));
	ASSERT_NE(SpawnPlayer(1, vec2(128.0f, 96.0f)), nullptr);
	ASSERT_NE(pVanillaVictim, nullptr);
	EXPECT_TRUE(pVanillaVictim->Freeze(5));
	pVanillaVictim->TakeDamage(vec2(), 1, 1, WEAPON_HAMMER);
	EXPECT_GT(pVanillaVictim->m_FreezeTime, 0);
	pVanillaVictim->TakeDamage(vec2(), 1, 1, WEAPON_LASER);
	EXPECT_GT(pVanillaVictim->m_FreezeTime, 0);
}

TEST_F(GameWorld, FreezeWithoutTheDDRaceCharacter)
{
	SelectGameMode("dm");
	CCharacter *pCharacter = SpawnPlayer(0, vec2(64.0f, 96.0f));
	ASSERT_NE(pCharacter, nullptr);
	CNetObj_PlayerInput Input = {};
	Input.m_Direction = 1;
	Input.m_Hook = 1;
	Input.m_TargetX = 1;
	pCharacter->OnPredictedInput(&Input);
	EXPECT_FALSE(pCharacter->TickFreeze());

	ASSERT_TRUE(pCharacter->Freeze(1));
	int FrozenTicks = 0;
	while(pCharacter->TickFreeze())
		FrozenTicks++;
	EXPECT_EQ(FrozenTicks, SERVER_TICK_SPEED - 2);
	EXPECT_EQ(pCharacter->m_FreezeTime, 0);

	// a DDNet client learns about the freeze from the ninja, the vanilla modes send no DDNet character
	ASSERT_TRUE(pCharacter->Freeze(1));
	m_pServer->m_aClients[0].m_State = CServer::CClient::STATE_INGAME;
	m_pServer->SetClientDDNetVersion(0, VERSION_DDNET_NEW_HUD);
	GameServer()->m_PlayerMapping.InitPlayerMap(0);
	m_pServer->m_SnapshotBuilder.Init(false);
	pCharacter->Snap(0);
	CSnapshotBuffer Buffer;
	m_pServer->m_SnapshotBuilder.Finish(&Buffer);
	int TranslatedId = 0;
	ASSERT_TRUE(m_pServer->Translate(TranslatedId, 0));
	const auto *pSnapped = static_cast<const CNetObj_Character *>(Buffer.AsSnapshot()->FindItem(NETOBJTYPE_CHARACTER, TranslatedId));
	ASSERT_NE(pSnapped, nullptr);
	EXPECT_EQ(pSnapped->m_Weapon, WEAPON_NINJA);
}

TEST_F(GameWorld, CharacterSpawnInitializationIsModeOwned)
{
	vec2 TunePosition;
	int MapTuneZone = 0;
	for(int y = 0; y < GameServer()->Collision()->GetHeight() && MapTuneZone == 0; y++)
	{
		for(int x = 0; x < GameServer()->Collision()->GetWidth(); x++)
		{
			MapTuneZone = GameServer()->Collision()->IsTune(y * GameServer()->Collision()->GetWidth() + x);
			if(MapTuneZone > 0)
			{
				TunePosition = vec2(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
				break;
			}
		}
	}
	ASSERT_GT(MapTuneZone, 0);
	const float GlobalGravity = GameServer()->TuningList()[0].m_Gravity;
	const float ZoneGravity = GlobalGravity + 1.0f;
	GameServer()->TuningList()[MapTuneZone].m_Gravity = ZoneGravity;

	g_Config.m_SvEndlessDrag = 1;
	g_Config.m_SvTeam = SV_TEAM_FORCED_SOLO;

	SelectGameMode("dm");
	GameServer()->TuningList()[MapTuneZone].m_Gravity = ZoneGravity;
	EXPECT_EQ(GameController()->TuningZoneAt(TunePosition), 0);
	CPlayer *pVanillaPlayer = GameServer()->CreatePlayer(0, TEAM_GAME, false, -1);
	CCharacter *pVanillaCharacter = pVanillaPlayer->ForceSpawn(TunePosition);
	EXPECT_NE(pVanillaCharacter, nullptr);
	if(!pVanillaCharacter)
	{
		delete GameServer()->m_apPlayers[0];
		GameServer()->m_apPlayers[0] = nullptr;
		return;
	}
	EXPECT_FALSE(pVanillaCharacter->GetCore().m_EndlessHook);
	EXPECT_FALSE(pVanillaCharacter->GetCore().m_Solo);
	EXPECT_EQ(pVanillaCharacter->TuningZone(), 0);
	EXPECT_FLOAT_EQ(pVanillaCharacter->GetCore().m_Tuning.m_Gravity, GlobalGravity);
	m_pServer->m_aClients[0].m_State = CServer::CClient::STATE_INGAME;
	pVanillaPlayer->Tick();
	EXPECT_EQ(pVanillaPlayer->m_TuneZone, 0);
	auto *pVanillaProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, 0, TunePosition, vec2(1.0f, 0.0f), 10, false, false, -1, vec2(1.0f, 0.0f));
	EXPECT_EQ(pVanillaProjectile->NetInfo(SERVER_DEMO_CLIENT).m_TuneZone, 0);
	delete pVanillaProjectile;
	pVanillaCharacter->SetInvincible(true);
	EXPECT_TRUE(pVanillaCharacter->GetCore().m_Invincible);
	EXPECT_FALSE(pVanillaCharacter->GetCore().m_Super);
	pVanillaCharacter->PreTick();
	EXPECT_TRUE(pVanillaCharacter->IsAlive());
	EXPECT_EQ(dynamic_cast<CCharacterDDRace *>(pVanillaCharacter), nullptr);
	delete GameServer()->m_apPlayers[0];
	GameServer()->m_apPlayers[0] = nullptr;
	m_pServer->m_aClients[0].m_State = CServer::CClient::STATE_EMPTY;

	SelectGameMode("ddnet");
	g_Config.m_SvEndlessDrag = 1;
	g_Config.m_SvTeam = SV_TEAM_FORCED_SOLO;
	GameServer()->TuningList()[MapTuneZone].m_Gravity = ZoneGravity;
	EXPECT_EQ(GameController()->TuningZoneAt(TunePosition), MapTuneZone);
	CCharacter *pDDNetCharacter = SpawnPlayer(1, TunePosition);
	EXPECT_NE(pDDNetCharacter, nullptr);
	if(!pDDNetCharacter)
		return;
	EXPECT_TRUE(pDDNetCharacter->GetCore().m_EndlessHook);
	EXPECT_TRUE(pDDNetCharacter->GetCore().m_Solo);
	CCharacterDDRace *pDDRaceCharacter = dynamic_cast<CCharacterDDRace *>(pDDNetCharacter);
	ASSERT_NE(pDDRaceCharacter, nullptr);
	EXPECT_EQ(pDDRaceCharacter->TuningZone(), MapTuneZone);
	EXPECT_FLOAT_EQ(pDDRaceCharacter->GetCore().m_Tuning.m_Gravity, ZoneGravity);
	EXPECT_EQ(pDDRaceCharacter->m_TuneZoneOld, -1);
	m_pServer->m_aClients[1].m_State = CServer::CClient::STATE_INGAME;
	GameServer()->m_apPlayers[1]->Tick();
	EXPECT_EQ(GameServer()->m_apPlayers[1]->m_TuneZone, MapTuneZone);
	auto *pDDNetProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, 1, TunePosition, vec2(1.0f, 0.0f), 10, false, false, -1, vec2(1.0f, 0.0f));
	EXPECT_EQ(pDDNetProjectile->NetInfo(SERVER_DEMO_CLIENT).m_TuneZone, MapTuneZone);
	delete pDDNetProjectile;
	EXPECT_TRUE(pDDRaceCharacter->HasRaceTeams());
	pDDNetCharacter->Pause(true);
	pDDRaceCharacter->m_StartTime = GameServer()->Server()->Tick() + 1;
	pDDNetCharacter->PreTick();
	EXPECT_FALSE(pDDNetCharacter->IsAlive());
}

TEST_F(GameWorld, PhysicsRulesIsModeOwned)
{
	g_Config.m_SvTeam = SV_TEAM_FORCED_SOLO;

	SelectGameMode("dm");
	EXPECT_FALSE(GameServer()->m_World.m_Core.m_PhysicsRules.m_DDNetMovement);
	GameServer()->CreatePlayer(0, TEAM_GAME, false, -1);
	GameServer()->CreatePlayer(1, TEAM_GAME, false, -1);
	ASSERT_NE(GameServer()->m_apPlayers[0]->ForceSpawn(vec2(64.0f, 96.0f)), nullptr);
	ASSERT_NE(GameServer()->m_apPlayers[1]->ForceSpawn(vec2(192.0f, 96.0f)), nullptr);
	EXPECT_TRUE(GameController()->TeamsCore().SameTeam(0, 1));
	EXPECT_TRUE(GameController()->TeamsCore().CanCollide(0, 1));
	DeletePlayers();

	SelectGameMode("ddnet");
	g_Config.m_SvTeam = SV_TEAM_FORCED_SOLO;
	RaceTeams().Reset();
	EXPECT_TRUE(GameServer()->m_World.m_Core.m_PhysicsRules.m_DDNetMovement);
	GameServer()->CreatePlayer(0, TEAM_GAME, false, -1);
	GameServer()->CreatePlayer(1, TEAM_GAME, false, -1);
	ASSERT_NE(GameServer()->m_apPlayers[0]->ForceSpawn(vec2(64.0f, 96.0f)), nullptr);
	ASSERT_NE(GameServer()->m_apPlayers[1]->ForceSpawn(vec2(192.0f, 96.0f)), nullptr);
	EXPECT_FALSE(GameController()->TeamsCore().SameTeam(0, 1));
	EXPECT_FALSE(GameController()->TeamsCore().CanCollide(0, 1));
}

TEST_F(GameWorld, PhysicsRulesGatesDDNetCoreFlags)
{
	auto HookedPlayer = [&](const CPhysicsRules &Rules) {
		CWorldCore World;
		World.m_PhysicsRules = Rules;
		CTeamsCore Teams;
		CCharacterCore Hooker;
		CCharacterCore Target;
		Hooker.Reset();
		Target.Reset();
		Hooker.Init(&World, GameServer()->Collision(), &Teams);
		Target.Init(&World, GameServer()->Collision(), &Teams);
		Hooker.m_Id = 0;
		Target.m_Id = 1;
		World.m_apCharacters[0] = &Hooker;
		World.m_apCharacters[1] = &Target;
		Hooker.m_Pos = vec2(64.0f, 96.0f);
		Target.m_Pos = vec2(120.0f, 96.0f);
		Hooker.m_HookPos = vec2(80.0f, 96.0f);
		Hooker.m_HookDir = vec2(1.0f, 0.0f);
		Hooker.m_HookState = HOOK_FLYING;
		Hooker.m_HookHitDisabled = true;
		Hooker.m_Input.m_TargetX = 1;
		Hooker.m_Input.m_TargetY = 0;
		Hooker.Tick(false, false);
		return Hooker.HookedPlayer();
	};

	EXPECT_EQ(HookedPlayer(CPhysicsRules()), 1);
	EXPECT_EQ(HookedPlayer(CPhysicsRules::DDNetFromConfig()), -1);
}

TEST_F(GameWorld, PhysicsRulesGatesOldHookTeleport)
{
	int TeleNumber = 0;
	vec2 TelePosition;
	for(int Index = 0; Index < GameServer()->Collision()->GetWidth() * GameServer()->Collision()->GetHeight(); ++Index)
	{
		TeleNumber = GameServer()->Collision()->IsTeleport(Index);
		if(TeleNumber > 0 && !GameServer()->Collision()->TeleOuts(TeleNumber - 1).empty())
		{
			TelePosition = GameServer()->Collision()->GetPos(Index);
			break;
		}
	}
	ASSERT_GT(TeleNumber, 0);

	auto HookUsesTeleport = [&](CPhysicsRules Rules) {
		CWorldCore World;
		Rules.m_TeleportHookOld = true;
		World.m_PhysicsRules = Rules;
		CTeamsCore Teams;
		CCharacterCore Core;
		Core.Reset();
		Core.Init(&World, GameServer()->Collision(), &Teams);
		Core.m_Pos = TelePosition - vec2(64.0f, 0.0f);
		Core.m_HookPos = TelePosition - vec2(32.0f, 0.0f);
		Core.m_HookDir = vec2(1.0f, 0.0f);
		Core.m_HookState = HOOK_FLYING;
		Core.m_Input.m_TargetX = 1;
		Core.m_Input.m_TargetY = 0;
		Core.Tick(false, false);
		return Core.m_NewHook;
	};

	EXPECT_FALSE(HookUsesTeleport(CPhysicsRules()));
	EXPECT_TRUE(HookUsesTeleport(CPhysicsRules::DDNetFromConfig()));
}

TEST_F(GameWorld, PhysicsRulesFollowServerConfig)
{
	// vanilla physics ignore the DDNet settings; selecting a mode loads the game settings
	SelectGameMode("dm");
	g_Config.m_SvHit = 0;
	g_Config.m_SvNoWeakHook = 1;
	GameServer()->m_World.Tick();
	EXPECT_TRUE(GameServer()->m_World.m_Core.m_PhysicsRules.m_WeaponsHitOthers);
	EXPECT_TRUE(GameServer()->m_World.m_Core.m_PhysicsRules.m_WeakHook);

	SelectGameMode("ddnet");
	g_Config.m_SvHit = 0;
	g_Config.m_SvNoWeakHook = 1;
	GameServer()->m_World.Tick();
	EXPECT_FALSE(GameServer()->m_World.m_Core.m_PhysicsRules.m_WeaponsHitOthers);
	EXPECT_FALSE(GameServer()->m_World.m_Core.m_PhysicsRules.m_WeakHook);

	// Changing a setting takes effect without restarting the round.
	g_Config.m_SvHit = 1;
	GameServer()->m_World.Tick();
	EXPECT_TRUE(GameServer()->m_World.m_Core.m_PhysicsRules.m_WeaponsHitOthers);
}

TEST_F(GameWorld, PhysicsRulesGatesStopperGround)
{
	vec2 StopperPosition;
	bool Found = false;
	for(int Index = 0; Index < GameServer()->Collision()->GetWidth() * GameServer()->Collision()->GetHeight(); ++Index)
	{
		const vec2 Position = GameServer()->Collision()->GetPos(Index) - vec2(0.0f, 18.0f);
		if(!GameServer()->Collision()->IsOnGround(Position, CCharacterCore::PhysicalSize()) && (GameServer()->Collision()->GetMoveRestrictions(Position + vec2(0.0f, 18.0f), 0.0f) & CANTMOVE_DOWN))
		{
			StopperPosition = Position;
			Found = true;
			break;
		}
	}
	ASSERT_TRUE(Found);

	SelectGameMode("dm");
	CCharacter *pVanillaCharacter = SpawnPlayer(0, StopperPosition);
	ASSERT_NE(pVanillaCharacter, nullptr);
	EXPECT_FALSE(pVanillaCharacter->IsGrounded());
	DeletePlayers();

	SelectGameMode("ddnet");
	CCharacter *pDDNetCharacter = SpawnPlayer(0, StopperPosition);
	ASSERT_NE(pDDNetCharacter, nullptr);
	EXPECT_TRUE(pDDNetCharacter->IsGrounded());
}

TEST_F(GameWorld, DDRaceStartWarningIsCharacterOwned)
{
	CCharacterDDRace *pCharacter = SpawnPlayer<CCharacterDDRace>(0, vec2(64.0f, 96.0f));
	ASSERT_NE(pCharacter, nullptr);

	EXPECT_TRUE(pCharacter->TryStartWarning());
	EXPECT_FALSE(pCharacter->TryStartWarning());
}

TEST_F(GameWorld, DDRaceSaveUsesPlayerNinjaJetpack)
{
	CCharacterDDRace *pCharacter = SpawnPlayer<CCharacterDDRace>(0, vec2(64.0f, 96.0f));
	ASSERT_NE(pCharacter, nullptr);
	CPlayer *pPlayer = pCharacter->GetPlayer();

	static_cast<CPlayerDDRace *>(pPlayer)->m_NinjaJetpack = true;
	CSaveTee SavedTee;
	SavedTee.Save(pCharacter, false);
	static_cast<CPlayerDDRace *>(pPlayer)->m_NinjaJetpack = false;
	ASSERT_TRUE(SavedTee.Load(pCharacter));
	EXPECT_TRUE(static_cast<CPlayerDDRace *>(pPlayer)->m_NinjaJetpack);
}

TEST_F(GameWorld, DDRaceSaveIsBlockedByDraggerBeam)
{
	constexpr int ClientId = 0;
	constexpr int Team = 1;
	ASSERT_NE(SpawnPlayer(ClientId, vec2(64.0f, 96.0f)), nullptr);
	RaceTeams().SetForceCharacterTeam(ClientId, Team);
	RaceTeams().ChangeTeamState(Team, ETeamState::STARTED);

	auto *pDragger = new CDragger(&GameServer()->m_World, vec2(64.0f, 64.0f), 1.0f, false);
	new CDraggerBeam(&GameServer()->m_World, pDragger, pDragger->GetPos(), 1.0f, false, ClientId, 0, 0);
	CSaveTeam SavedTeam; // NOLINT(clang-analyzer-unix.Malloc)
	EXPECT_EQ(SavedTeam.Save(GameServices(), &RaceTeams(), Team, true, false), ESaveResult::DRAGGER_ACTIVE);
}

TEST_F(GameWorld, DDRaceRescueStateIsCharacterOwned)
{
	constexpr int ClientId = 0;
	CCharacterDDRace *pCharacter = SpawnPlayer<CCharacterDDRace>(ClientId, vec2(64.0f, 96.0f));
	ASSERT_NE(pCharacter, nullptr);

	g_Config.m_SvRescue = 1;
	g_Config.m_SvRescueDelay = 0;

	const vec2 AutoPosition = pCharacter->m_Pos;
	const bool AutoRescueSet = pCharacter->TrySetRescue(RESCUEMODE_AUTO);
	pCharacter->m_Pos = vec2(72.0f, 96.0f);
	pCharacter->SetPosition(pCharacter->m_Pos);
	const vec2 ManualPosition = pCharacter->m_Pos;
	const bool ManualRescueSet = pCharacter->TrySetRescue(RESCUEMODE_MANUAL);

	if(!AutoRescueSet || !ManualRescueSet)
	{
		FAIL() << "Failed to establish deterministic rescue positions";
	}

	auto &PlayerState = RaceTeams().PlayerState(ClientId);
	PlayerState.m_RescueMode = RESCUEMODE_AUTO;
	pCharacter->m_Pos = vec2(128.0f, 128.0f);
	pCharacter->SetPosition(pCharacter->m_Pos);
	CCharacterCore Core = pCharacter->GetCore();
	Core.m_Vel = vec2(4.0f, 5.0f);
	Core.m_HookState = HOOK_GRABBED;
	pCharacter->SetCore(Core);
	pCharacter->m_StartTime = 1234;
	pCharacter->m_DDRaceState = ERaceState::STARTED;
	EXPECT_TRUE(pCharacter->Rescue());
	EXPECT_EQ(pCharacter->m_Pos, AutoPosition);
	EXPECT_EQ(pCharacter->GetCore().m_Vel, vec2(0.0f, 0.0f));
	EXPECT_EQ(pCharacter->GetCore().m_HookState, HOOK_IDLE);
	EXPECT_EQ(pCharacter->m_StartTime, 1234);
	EXPECT_EQ(pCharacter->m_DDRaceState, ERaceState::STARTED);

	PlayerState.m_RescueMode = RESCUEMODE_MANUAL;
	pCharacter->m_Pos = vec2(160.0f, 128.0f);
	pCharacter->SetPosition(pCharacter->m_Pos);
	EXPECT_TRUE(pCharacter->Rescue());
	EXPECT_EQ(pCharacter->m_Pos, ManualPosition);
	g_Config.m_SvRescueDelay = 1;
	const vec2 CooldownPosition(192.0f, 128.0f);
	pCharacter->m_Pos = CooldownPosition;
	pCharacter->SetPosition(pCharacter->m_Pos);
	EXPECT_FALSE(pCharacter->Rescue());
	EXPECT_EQ(pCharacter->m_Pos, CooldownPosition);
}

TEST_F(GameWorld, DDRaceSuperStateIsCharacterOwned)
{
	constexpr int ClientId = 0;
	CCharacterDDRace *pCharacter = SpawnPlayer<CCharacterDDRace>(ClientId, vec2(64.0f, 96.0f));
	ASSERT_NE(pCharacter, nullptr);

	const int OriginalTeam = pCharacter->Team();
	pCharacter->SetInvincible(true);
	pCharacter->SetSuper(true);
	EXPECT_TRUE(pCharacter->IsSuper());
	EXPECT_FALSE(pCharacter->GetCore().m_Invincible);
	EXPECT_EQ(pCharacter->Team(), TEAM_SUPER);
	EXPECT_EQ(pCharacter->TeamBeforeSuper(), OriginalTeam);
	EXPECT_EQ(pCharacter->m_DDRaceState, ERaceState::CHEATED);

	pCharacter->SetInvincible(true);
	EXPECT_FALSE(pCharacter->IsSuper());
	EXPECT_TRUE(pCharacter->GetCore().m_Invincible);
	EXPECT_EQ(pCharacter->Team(), OriginalTeam);
}

TEST_F(GameWorld, CharacterDeathTransactionIsModeOwned)
{
	constexpr int VictimId = 0;
	constexpr int OtherId = 1;
	CPlayer *pVictimPlayer = GameServer()->CreatePlayer(VictimId, TEAM_GAME, false, -1);
	CPlayer *pOtherPlayer = GameServer()->CreatePlayer(OtherId, TEAM_GAME, false, -1);
	ASSERT_NE(pVictimPlayer, nullptr);
	ASSERT_NE(pOtherPlayer, nullptr);
	CCharacterDDRace *pVictim = dynamic_cast<CCharacterDDRace *>(pVictimPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pVictim, nullptr);
	g_Config.m_SvRescue = 1;
	const bool RescueSet = pVictim->TrySetRescue(RESCUEMODE_AUTO);
	ASSERT_TRUE(RescueSet);
	const vec2 RescuePosition = pVictim->GetLastRescueTeeRef().GetPos();

	auto &VictimState = RaceTeams().PlayerState(VictimId);
	auto &OtherState = RaceTeams().PlayerState(OtherId);
	VictimState.m_SwapTargetClientId = OtherId;
	OtherState.m_SwapTargetClientId = VictimId;
	pVictim->Die(VictimId, WEAPON_SELF, false);

	EXPECT_FALSE(pVictim->IsAlive());
	EXPECT_EQ(GameServer()->m_World.m_Core.m_apCharacters[VictimId], nullptr);
	ASSERT_TRUE(VictimState.m_LastDeath.has_value());
	EXPECT_EQ(VictimState.m_LastDeath->GetPos(), RescuePosition);
	EXPECT_EQ(VictimState.m_SwapTargetClientId, -1);
	EXPECT_EQ(OtherState.m_SwapTargetClientId, -1);
}

TEST_F(GameWorld, PlayerAutoRespawnPolicyIsModeOwned)
{
	GameServer()->CreatePlayer(0, TEAM_GAME, false, -1);
	CPlayer *pPlayer = GameServer()->m_apPlayers[0];
	ASSERT_NE(pPlayer, nullptr);
	pPlayer->m_PreviousDieTick = 100;
	pPlayer->m_DieTick = 101;

	CGameControllerVanillaDM VanillaController(GameServices(), *FindGameMode("dm"));
	EXPECT_EQ(VanillaController.PlayerAutoRespawnTick(pPlayer), pPlayer->m_DieTick + 2);

	const int DDNetRespawnTick = pPlayer->m_PreviousDieTick + GameServer()->Server()->TickSpeed() * 3 + 2;
	CGameControllerDDNet DDNetController(GameServices(), *FindGameMode("ddnet"));
	EXPECT_EQ(DDNetController.PlayerAutoRespawnTick(pPlayer), DDNetRespawnTick);

	CGameControllerDDRace ModController(GameServices(), *FindGameMode("mod"));
	EXPECT_EQ(ModController.PlayerAutoRespawnTick(pPlayer), DDNetRespawnTick);
}

TEST_F(GameWorld, PlayerSetTeamOperationIsModeOwned)
{
	constexpr int ClientId = 0;
	CCharacterDDRace *pCharacter = SpawnPlayer<CCharacterDDRace>(ClientId, vec2(64.0f, 96.0f));
	ASSERT_NE(pCharacter, nullptr);
	CPlayer *pPlayer = pCharacter->GetPlayer();

	CGameControllerVanillaDM VanillaController(GameServices(), *FindGameMode("dm"));
	CGameControllerDDNet DDNetController(GameServices(), *FindGameMode("ddnet"));
	CGameControllerDDRace ModController(GameServices(), *FindGameMode("mod"));

	g_Config.m_SvKillProtection = 1;
	g_Config.m_SvSpamprotection = 0;

	pCharacter->m_DDRaceState = ERaceState::STARTED;
	pCharacter->m_StartTime = GameServer()->Server()->Tick() - GameServer()->Server()->TickSpeed() * 60;
	VanillaController.OnPlayerSetTeam(ClientId, TEAM_SPECTATORS);
	EXPECT_EQ(pPlayer->GetTeam(), TEAM_SPECTATORS);

	pPlayer->SetTeam(TEAM_GAME);
	pCharacter = dynamic_cast<CCharacterDDRace *>(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	if(!pCharacter)
	{
		FAIL() << "failed to respawn test character";
		return;
	}
	pCharacter->m_DDRaceState = ERaceState::STARTED;
	pCharacter->m_StartTime = GameServer()->Server()->Tick() - GameServer()->Server()->TickSpeed() * 60;
	DDNetController.OnPlayerSetTeam(ClientId, TEAM_SPECTATORS);
	EXPECT_EQ(pPlayer->GetTeam(), TEAM_GAME);

	ModController.OnPlayerSetTeam(ClientId, TEAM_SPECTATORS);
	EXPECT_EQ(pPlayer->GetTeam(), TEAM_GAME);
}

TEST_F(GameWorld, PlayerKillOperationIsModeOwned)
{
	constexpr int ClientId = 0;
	g_Config.m_SvKillProtection = 1;
	g_Config.m_SvKillDelay = 0;

	SelectGameMode("dm");
	CCharacter *pVanillaCharacter = SpawnPlayer(ClientId, vec2(64.0f, 96.0f));
	ASSERT_NE(pVanillaCharacter, nullptr);
	CPlayer *pPlayer = pVanillaCharacter->GetPlayer();
	GameController()->OnPlayerKill(ClientId);
	EXPECT_EQ(pPlayer->GetCharacter(), nullptr);

	delete GameServer()->m_apPlayers[ClientId];
	GameServer()->m_apPlayers[ClientId] = nullptr;
	SelectGameMode("mod");
	pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);
	CCharacterDDRace *pRaceCharacter = dynamic_cast<CCharacterDDRace *>(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pRaceCharacter, nullptr);
	pRaceCharacter->m_DDRaceState = ERaceState::STARTED;
	pRaceCharacter->m_StartTime = GameServer()->Server()->Tick() - GameServer()->Server()->TickSpeed() * 60;
	GameController()->OnPlayerKill(ClientId);
	EXPECT_EQ(pPlayer->GetCharacter(), pRaceCharacter);
	EXPECT_TRUE(pRaceCharacter->IsAlive());
}

namespace
{
	// a mode whose kill key only counts the presses
	class CKillKeyController : public CGameControllerVanillaDM
	{
	public:
		using CGameControllerVanillaDM::CGameControllerVanillaDM;
		int m_Presses = 0;
		void OnPlayerKillKey(CPlayer *pPlayer) override { m_Presses++; }
	};
}

TEST_F(GameWorld, KillKeyMeaningIsModeOwned)
{
	CKillKeyController &Controller = SelectController<CKillKeyController>("dm");
	CCharacter *pCharacter = SpawnPlayer(0, vec2(64.0f, 96.0f));
	ASSERT_NE(pCharacter, nullptr);
	CPlayer *pPlayer = pCharacter->GetPlayer();
	GameController()->OnPlayerKill(0);
	GameController()->OnPlayerKill(0);
	EXPECT_EQ(Controller.m_Presses, 2);
	EXPECT_EQ(pPlayer->GetCharacter(), pCharacter);

	// only while the game runs and for the living
	GameController()->SetGamePaused(true);
	GameController()->OnPlayerKill(0);
	GameController()->SetGamePaused(false);
	pPlayer->KillCharacter();
	GameController()->OnPlayerKill(0);
	EXPECT_EQ(Controller.m_Presses, 2);
}

TEST_F(GameWorld, TargetVoteOperationsAreModeOwned)
{
	constexpr int CallerId = 0;
	constexpr int TargetId = 1;
	CGameControllerVanillaDM VanillaController(GameServices(), *FindGameMode("dm"));
	CGameControllerDDRace ModController(GameServices(), *FindGameMode("mod"));

	g_Config.m_SvVoteKickMin = 0;
	g_Config.m_SvVoteKickBantime = 0;
	g_Config.m_SvPauseable = 0;
	g_Config.m_SvVotePause = 0;
	g_Config.m_SvVoteSpectateRejoindelay = 3;

	CPlayer *pCaller = GameServer()->CreatePlayer(CallerId, TEAM_GAME, false, -1);
	CPlayer *pTarget = GameServer()->CreatePlayer(TargetId, TEAM_GAME, false, -1);
	ASSERT_NE(pCaller, nullptr);
	ASSERT_NE(pTarget, nullptr);

	VanillaController.OnPlayerCallKickVote(CallerId, TargetId, "test");
	EXPECT_EQ(GameServer()->m_VoteType, CGameContext::VOTE_TYPE_KICK);
	EXPECT_EQ(GameServer()->m_VoteVictim, TargetId);
	EXPECT_STREQ(GameServer()->m_aVoteCommand, "kick 1 Kicked by vote");
	GameServer()->EndVote();

	VanillaController.OnPlayerCallSpectateVote(CallerId, TargetId, "test");
	EXPECT_EQ(GameServer()->m_VoteType, CGameContext::VOTE_TYPE_SPECTATE);
	EXPECT_EQ(GameServer()->m_VoteVictim, TargetId);
	EXPECT_STREQ(GameServer()->m_aVoteCommand, "set_team 1 -1 3");
	GameServer()->EndVote();

	CCharacter *pCallerCharacter = pCaller->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pTargetCharacter = pTarget->ForceSpawn(vec2(96.0f, 96.0f));
	ASSERT_NE(pCallerCharacter, nullptr);
	ASSERT_NE(pTargetCharacter, nullptr);
	ModController.RaceTeams().SetForceCharacterTeam(CallerId, 1);
	ModController.RaceTeams().SetForceCharacterTeam(TargetId, 1);

	ModController.OnPlayerCallKickVote(CallerId, TargetId, "test");
	EXPECT_EQ(GameServer()->m_VoteType, CGameContext::VOTE_TYPE_KICK);
	EXPECT_EQ(GameServer()->m_VoteVictim, TargetId);
	EXPECT_STREQ(GameServer()->m_aVoteCommand, "uninvite 1 1; set_team_ddr 1 0");
	GameServer()->EndVote();

	ModController.OnPlayerCallSpectateVote(CallerId, TargetId, "test");
	EXPECT_EQ(GameServer()->m_VoteType, CGameContext::VOTE_TYPE_SPECTATE);
	EXPECT_EQ(GameServer()->m_VoteVictim, TargetId);
	EXPECT_STREQ(GameServer()->m_aVoteCommand, "uninvite 1 1; set_team 1 -1 3");
	GameServer()->EndVote();
}

TEST_F(GameWorld, TargetVoteAudienceIsModeOwned)
{
	constexpr int CreatorId = 0;
	constexpr int VoterId = 1;
	constexpr int SpectatorId = 2;
	CGameControllerVanillaDM VanillaController(GameServices(), *FindGameMode("dm"));
	CGameControllerDDRace ModController(GameServices(), *FindGameMode("mod"));

	CPlayer *pCreator = GameServer()->CreatePlayer(CreatorId, TEAM_GAME, false, -1);
	CPlayer *pVoter = GameServer()->CreatePlayer(VoterId, TEAM_GAME, false, -1);
	CPlayer *pSpectator = GameServer()->CreatePlayer(SpectatorId, TEAM_SPECTATORS, false, -1);
	ASSERT_NE(pCreator, nullptr);
	ASSERT_NE(pVoter, nullptr);
	ASSERT_NE(pSpectator, nullptr);
	CCharacter *pCreatorCharacter = pCreator->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pVoterCharacter = pVoter->ForceSpawn(vec2(96.0f, 96.0f));
	ASSERT_NE(pCreatorCharacter, nullptr);
	ASSERT_NE(pVoterCharacter, nullptr);

	ModController.RaceTeams().SetForceCharacterTeam(CreatorId, 1);
	ModController.RaceTeams().SetForceCharacterTeam(VoterId, 2);

	EXPECT_TRUE(VanillaController.CanPlayerVoteOnTargetVote(CreatorId, VoterId));
	EXPECT_FALSE(VanillaController.CanPlayerVoteOnTargetVote(CreatorId, SpectatorId));
	EXPECT_FALSE(ModController.CanPlayerVoteOnTargetVote(CreatorId, VoterId));
	EXPECT_FALSE(ModController.CanPlayerVoteOnTargetVote(CreatorId, SpectatorId));

	ModController.RaceTeams().SetForceCharacterTeam(VoterId, 1);
	EXPECT_TRUE(ModController.CanPlayerVoteOnTargetVote(CreatorId, VoterId));
}

TEST_F(GameWorld, PlayerVetoActivityIsModeOwned)
{
	constexpr int ClientId = 0;
	CCharacterDDRace *pCharacter = SpawnPlayer<CCharacterDDRace>(ClientId, vec2(64.0f, 96.0f));
	ASSERT_NE(pCharacter, nullptr);
	CPlayer *pPlayer = pCharacter->GetPlayer();

	CGameControllerVanillaDM VanillaController(GameServices(), *FindGameMode("dm"));
	CGameControllerDDRace ModController(GameServices(), *FindGameMode("mod"));

	const int Now = GameServer()->Server()->Tick();
	pPlayer->m_JoinTick = Now - GameServer()->Server()->TickSpeed() * 5 * 60;
	pCharacter->m_DDRaceState = ERaceState::STARTED;
	pCharacter->m_StartTime = Now - GameServer()->Server()->TickSpeed() * 30 * 60;

	EXPECT_EQ(VanillaController.PlayerVetoActivityStartTick(ClientId), pPlayer->m_JoinTick);
	EXPECT_EQ(ModController.PlayerVetoActivityStartTick(ClientId), pCharacter->m_StartTime);

	pCharacter->m_DDRaceState = ERaceState::NONE;
	EXPECT_EQ(ModController.PlayerVetoActivityStartTick(ClientId), pPlayer->m_JoinTick);
}

TEST_F(GameWorld, PlayerVisibilityPolicyIsModeOwned)
{
	constexpr int TargetId = 0;
	constexpr int ViewerId = 1;
	SelectGameMode("mod");
	auto &ModController = *dynamic_cast<CGameControllerDDRace *>(GameController());
	GameServer()->CreatePlayer(TargetId, TEAM_GAME, false, -1);
	GameServer()->CreatePlayer(ViewerId, TEAM_GAME, false, -1);
	CCharacterDDRace *pTarget = dynamic_cast<CCharacterDDRace *>(GameServer()->m_apPlayers[TargetId]->ForceSpawn(vec2(64.0f, 96.0f)));
	CCharacterDDRace *pViewer = dynamic_cast<CCharacterDDRace *>(GameServer()->m_apPlayers[ViewerId]->ForceSpawn(vec2(96.0f, 96.0f)));
	ASSERT_NE(pTarget, nullptr);
	ASSERT_NE(pViewer, nullptr);

	g_Config.m_SvShowOthers = 1;
	g_Config.m_SvShowOthersDefault = SHOW_OTHERS_ONLY_TEAM;

	ModController.RaceTeams().SetForceCharacterTeam(TargetId, 1);
	ModController.RaceTeams().SetForceCharacterTeam(ViewerId, 2);

	g_Config.m_SvShowOthersDefault = SHOW_OTHERS_OFF;
	ModController.RaceTeams().PlayerState(ViewerId).m_ShowOthers = SHOW_OTHERS_OFF;
	EXPECT_FALSE(pTarget->CanSnapCharacter(ViewerId));
	ModController.OnPlayerShowOthers(ViewerId, SHOW_OTHERS_ON);
	EXPECT_TRUE(pTarget->CanSnapCharacter(ViewerId));
}

TEST_F(GameWorld, EntityInteractionPolicyIsModeOwned)
{
	constexpr int OwnerId = 0;
	constexpr int ViewerId = 1;
	constexpr int SpectatorId = 2;
	SelectGameMode("mod");
	auto &ModController = *dynamic_cast<CGameControllerDDRace *>(GameController());
	CPlayer *pOwner = GameServer()->CreatePlayer(OwnerId, TEAM_GAME, false, -1);
	ASSERT_NE(pOwner, nullptr);
	CPlayer *pViewer = GameServer()->CreatePlayer(ViewerId, TEAM_GAME, false, -1);
	CPlayer *pSpectator = GameServer()->CreatePlayer(SpectatorId, TEAM_SPECTATORS, false, -1);
	ASSERT_NE(pViewer, nullptr);
	ASSERT_NE(pSpectator, nullptr);
	CCharacter *pOwnerCharacter = pOwner->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pViewerCharacter = pViewer->ForceSpawn(vec2(96.0f, 96.0f));
	ASSERT_NE(pOwnerCharacter, nullptr);
	ASSERT_NE(pViewerCharacter, nullptr);

	CInteractions Interaction;
	Interaction.Init(OwnerId, pOwner->GetUniqueCid());
	Interaction.FillOwnerConnected(1, false, false, false);

	ModController.RaceTeams().SetForceCharacterTeam(OwnerId, 1);
	ModController.RaceTeams().SetForceCharacterTeam(ViewerId, 2);
	ModController.RaceTeams().SetForceCharacterTeam(SpectatorId, 2);
	ModController.RaceTeams().PlayerState(ViewerId).m_ShowOthers = SHOW_OTHERS_OFF;

	EXPECT_FALSE(ModController.CanCharacterHitCharacter(pOwnerCharacter, pViewerCharacter));
	EXPECT_FALSE(Interaction.CanSee(GameServer(), ViewerId));
	EXPECT_FALSE(Interaction.CanHit(GameServer(), ViewerId));
	EXPECT_FALSE(Interaction.CanSeeMask(GameServer()).test(ViewerId));
	EXPECT_TRUE(Interaction.CanSee(GameServer(), OwnerId));
	auto *pProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, OwnerId, vec2(64.0f, 96.0f), vec2(1.0f, 0.0f), 10, false, false, -1, vec2(1.0f, 0.0f));
	EXPECT_FALSE(pProjectile->CanCollide(ViewerId));
	auto *pOwnerlessProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, -1, vec2(64.0f, 96.0f), vec2(1.0f, 0.0f), 10, false, false, -1, vec2(1.0f, 0.0f));
	EXPECT_FALSE(pOwnerlessProjectile->CanCollide(ViewerId));

	ModController.RaceTeams().PlayerState(ViewerId).m_ShowOthers = SHOW_OTHERS_ONLY_TEAM;
	EXPECT_FALSE(Interaction.CanSee(GameServer(), ViewerId));
	ModController.RaceTeams().SetForceCharacterTeam(ViewerId, 1);
	EXPECT_TRUE(ModController.CanCharacterHitCharacter(pOwnerCharacter, pViewerCharacter));
	EXPECT_TRUE(Interaction.CanSee(GameServer(), ViewerId));
	EXPECT_TRUE(pProjectile->CanCollide(ViewerId));
	EXPECT_FALSE(pOwnerlessProjectile->CanCollide(ViewerId));
	ModController.RaceTeams().SetForceCharacterTeam(ViewerId, TEAM_FLOCK);
	EXPECT_TRUE(pOwnerlessProjectile->CanCollide(ViewerId));
	ModController.RaceTeams().SetForceCharacterTeam(ViewerId, 1);
	ModController.RaceTeams().PlayerState(ViewerId).m_ShowOthers = SHOW_OTHERS_OFF;
	pViewerCharacter->SetSolo(true);
	EXPECT_FALSE(Interaction.CanSee(GameServer(), ViewerId));
	pViewerCharacter->SetSolo(false);

	ModController.RaceTeams().PlayerState(SpectatorId).m_SpecTeam = true;
	EXPECT_FALSE(Interaction.CanSee(GameServer(), SpectatorId));
	pSpectator->SetSpectatorId(OwnerId);
	EXPECT_TRUE(Interaction.CanSee(GameServer(), SpectatorId));
	pSpectator->SetSpectatorId(ViewerId);
	EXPECT_TRUE(Interaction.CanSee(GameServer(), SpectatorId));
	pViewer->KillCharacter();
	EXPECT_FALSE(Interaction.CanSee(GameServer(), SpectatorId));

	CInteractions NoHitOthers;
	NoHitOthers.Init(OwnerId, pOwner->GetUniqueCid());
	NoHitOthers.FillOwnerConnected(0, false, true, false);
	EXPECT_TRUE(NoHitOthers.CanHit(GameServer(), OwnerId));
	EXPECT_FALSE(NoHitOthers.CanHit(GameServer(), ViewerId));
	CInteractions NoHitSelf;
	NoHitSelf.Init(OwnerId, pOwner->GetUniqueCid());
	NoHitSelf.FillOwnerConnected(0, false, false, true);
	EXPECT_FALSE(NoHitSelf.CanHit(GameServer(), OwnerId));
	EXPECT_TRUE(NoHitSelf.CanHit(GameServer(), ViewerId));

	CGameControllerVanillaDM VanillaController(GameServices(), *FindGameMode("dm"));
	EXPECT_TRUE(VanillaController.CanCharacterHitCharacter(pOwnerCharacter, pOwnerCharacter));
	EXPECT_TRUE(VanillaController.CanSeeInteraction(Interaction, ViewerId));
	EXPECT_TRUE(VanillaController.CanHitInteraction(Interaction, ViewerId));

	delete pProjectile;
	delete pOwnerlessProjectile;
}

TEST_F(GameWorld, PlayerTeamGroupIsModeOwned)
{
	constexpr int RedOne = 0;
	constexpr int RedTwo = 1;
	constexpr int Blue = 2;
	SelectGameMode("tdm");
	auto &TdmController = *dynamic_cast<CGameControllerVanillaTDM *>(GameController());

	CPlayer *pRedOne = GameServer()->CreatePlayer(RedOne, TEAM_RED, false, -1);
	CPlayer *pRedTwo = GameServer()->CreatePlayer(RedTwo, TEAM_RED, false, -1);
	CPlayer *pBlue = GameServer()->CreatePlayer(Blue, TEAM_BLUE, false, -1);
	ASSERT_NE(pRedOne, nullptr);
	ASSERT_NE(pRedTwo, nullptr);
	ASSERT_NE(pBlue, nullptr);
	CCharacter *pRedOneCharacter = pRedOne->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pRedTwoCharacter = pRedTwo->ForceSpawn(vec2(96.0f, 96.0f));
	CCharacter *pBlueCharacter = pBlue->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pRedOneCharacter, nullptr);
	ASSERT_NE(pRedTwoCharacter, nullptr);
	ASSERT_NE(pBlueCharacter, nullptr);

	EXPECT_EQ(TdmController.PlayerTeamGroup(RedOne), TEAM_RED);
	EXPECT_EQ(TdmController.PlayerTeamGroup(RedTwo), TEAM_RED);
	EXPECT_EQ(TdmController.PlayerTeamGroup(Blue), TEAM_BLUE);
	CJsonStringWriter TdmServerInfoWriter;
	TdmServerInfoWriter.BeginObject();
	GameServer()->OnUpdatePlayerServerInfo(&TdmServerInfoWriter, Blue);
	TdmServerInfoWriter.EndObject();
	const std::string TdmServerInfo = TdmServerInfoWriter.GetOutputString();
	EXPECT_NE(TdmServerInfo.find("\"team\": 1"), std::string::npos);

	DeletePlayers();
	SelectGameMode("mod");
	auto &ModController = *dynamic_cast<CGameControllerDDRace *>(GameController());
	pRedOne = GameServer()->CreatePlayer(RedOne, TEAM_GAME, false, -1);
	pRedTwo = GameServer()->CreatePlayer(RedTwo, TEAM_GAME, false, -1);
	pBlue = GameServer()->CreatePlayer(Blue, TEAM_GAME, false, -1);
	ASSERT_NE(pRedOne->ForceSpawn(vec2(64.0f, 96.0f)), nullptr);
	ASSERT_NE(pRedTwo->ForceSpawn(vec2(96.0f, 96.0f)), nullptr);
	ASSERT_NE(pBlue->ForceSpawn(vec2(128.0f, 96.0f)), nullptr);
	ModController.RaceTeams().SetForceCharacterTeam(RedOne, 1);
	ModController.RaceTeams().SetForceCharacterTeam(RedTwo, 1);
	ModController.RaceTeams().SetForceCharacterTeam(Blue, 2);
	EXPECT_EQ(ModController.PlayerTeamGroup(RedOne), 1);
	EXPECT_EQ(ModController.PlayerTeamGroup(RedTwo), 1);
	EXPECT_EQ(ModController.PlayerTeamGroup(Blue), 2);
	CJsonStringWriter RaceServerInfoWriter;
	RaceServerInfoWriter.BeginObject();
	GameServer()->OnUpdatePlayerServerInfo(&RaceServerInfoWriter, Blue);
	RaceServerInfoWriter.EndObject();
	const std::string RaceServerInfo = RaceServerInfoWriter.GetOutputString();
	EXPECT_NE(RaceServerInfo.find("\"team\": 2"), std::string::npos);
}

TEST_F(GameWorld, WorldEventAudienceIsModeOwned)
{
	constexpr int Red = 0;
	constexpr int Blue = 1;
	SelectGameMode("tdm");

	CPlayer *pRed = GameServer()->CreatePlayer(Red, TEAM_RED, false, -1);
	CPlayer *pBlue = GameServer()->CreatePlayer(Blue, TEAM_BLUE, false, -1);
	ASSERT_NE(pRed, nullptr);
	ASSERT_NE(pBlue, nullptr);
	CCharacter *pRedCharacter = pRed->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pBlueCharacter = pBlue->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pRedCharacter, nullptr);
	ASSERT_NE(pBlueCharacter, nullptr);

	EXPECT_TRUE(pRedCharacter->TeamMask().test(Red));
	EXPECT_TRUE(pRedCharacter->TeamMask().test(Blue));

	DeletePlayers();
	SelectGameMode("mod");
	auto &ModController = *dynamic_cast<CGameControllerDDRace *>(GameController());
	pRed = GameServer()->CreatePlayer(Red, TEAM_GAME, false, -1);
	pBlue = GameServer()->CreatePlayer(Blue, TEAM_GAME, false, -1);
	pRedCharacter = pRed->ForceSpawn(vec2(64.0f, 96.0f));
	pBlueCharacter = pBlue->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pRedCharacter, nullptr);
	ASSERT_NE(pBlueCharacter, nullptr);
	ModController.RaceTeams().SetForceCharacterTeam(Red, 1);
	ModController.RaceTeams().SetForceCharacterTeam(Blue, 2);
	ModController.RaceTeams().PlayerState(Blue).m_ShowOthers = SHOW_OTHERS_OFF;

	EXPECT_TRUE(pRedCharacter->TeamMask().test(Red));
	EXPECT_FALSE(pRedCharacter->TeamMask().test(Blue));
}

TEST_F(GameWorld, PreInputAudienceIsModeOwned)
{
	constexpr int SenderId = 0;
	constexpr int AllyId = 1;
	constexpr int OpponentId = 2;
	SelectGameMode("tdm");

	CPlayer *pSender = GameServer()->CreatePlayer(SenderId, TEAM_RED, false, -1);
	CPlayer *pAlly = GameServer()->CreatePlayer(AllyId, TEAM_RED, false, -1);
	CPlayer *pOpponent = GameServer()->CreatePlayer(OpponentId, TEAM_BLUE, false, -1);
	ASSERT_NE(pSender, nullptr);
	ASSERT_NE(pAlly, nullptr);
	ASSERT_NE(pOpponent, nullptr);
	CCharacter *pSenderCharacter = pSender->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pAllyCharacter = pAlly->ForceSpawn(vec2(96.0f, 96.0f));
	CCharacter *pOpponentCharacter = pOpponent->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pSenderCharacter, nullptr);
	ASSERT_NE(pAllyCharacter, nullptr);
	ASSERT_NE(pOpponentCharacter, nullptr);

	m_pServer->m_aClients[AllyId].m_State = CServer::CClient::STATE_INGAME;
	m_pServer->m_aClients[OpponentId].m_State = CServer::CClient::STATE_INGAME;
	m_pServer->SetClientDDNetVersion(AllyId, VERSION_DDNET_PREINPUT);
	m_pServer->SetClientDDNetVersion(OpponentId, VERSION_DDNET_PREINPUT);
	bool aTdmClients[MAX_CLIENTS] = {};
	GameServer()->PreInputClients(SenderId, aTdmClients);
	EXPECT_TRUE(aTdmClients[AllyId]);
	EXPECT_TRUE(aTdmClients[OpponentId]);

	DeletePlayers();
	SelectGameMode("mod");
	auto &ModController = *dynamic_cast<CGameControllerDDRace *>(GameController());
	pSender = GameServer()->CreatePlayer(SenderId, TEAM_GAME, false, -1);
	pAlly = GameServer()->CreatePlayer(AllyId, TEAM_GAME, false, -1);
	pOpponent = GameServer()->CreatePlayer(OpponentId, TEAM_GAME, false, -1);
	ASSERT_NE(pSender->ForceSpawn(vec2(64.0f, 96.0f)), nullptr);
	ASSERT_NE(pAlly->ForceSpawn(vec2(96.0f, 96.0f)), nullptr);
	ASSERT_NE(pOpponent->ForceSpawn(vec2(128.0f, 96.0f)), nullptr);
	ModController.RaceTeams().SetForceCharacterTeam(SenderId, 1);
	ModController.RaceTeams().SetForceCharacterTeam(AllyId, 1);
	ModController.RaceTeams().SetForceCharacterTeam(OpponentId, 2);
	ModController.RaceTeams().PlayerState(AllyId).m_ShowOthers = SHOW_OTHERS_ON;
	ModController.RaceTeams().PlayerState(OpponentId).m_ShowOthers = SHOW_OTHERS_ON;

	bool aRaceClients[MAX_CLIENTS] = {};
	GameServer()->PreInputClients(SenderId, aRaceClients);
	EXPECT_TRUE(aRaceClients[AllyId]);
	EXPECT_FALSE(aRaceClients[OpponentId]);
}

TEST_F(GameWorld, PlayerSnapshotContributionsAreModeOwned)
{
	constexpr int ClientId = 1;
	SelectGameMode("dm");
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);

	m_pServer->m_aClients[ClientId].m_State = CServer::CClient::STATE_INGAME;
	m_pServer->SetClientDDNetVersion(ClientId, VERSION_DDNET_128_PLAYERS - 1);
	GameServer()->m_PlayerMapping.InitPlayerMap(ClientId);
	int TranslatedId = ClientId;
	ASSERT_TRUE(m_pServer->Translate(TranslatedId, ClientId));
	ASSERT_NE(TranslatedId, ClientId);

	m_pServer->m_SnapshotBuilder.Init();
	pPlayer->Snap(ClientId);
	CSnapshotBuffer VanillaBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&VanillaBuffer);
	const CSnapshot *pVanillaSnapshot = VanillaBuffer.AsSnapshot();
	EXPECT_NE(pVanillaSnapshot->FindItem(NETOBJTYPE_CLIENTINFO, TranslatedId), nullptr);
	EXPECT_NE(pVanillaSnapshot->FindItem(NETOBJTYPE_PLAYERINFO, TranslatedId), nullptr);
	EXPECT_EQ(pVanillaSnapshot->FindItem(NETOBJTYPE_DDNETPLAYER, TranslatedId), nullptr);
	EXPECT_EQ(pVanillaSnapshot->FindItem(NETOBJTYPE_DDNETSPECTATORINFO, TranslatedId), nullptr);

	DeletePlayers();
	SelectGameMode("mod");
	pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);
	GameServer()->m_PlayerMapping.InitPlayerMap(ClientId);
	TranslatedId = ClientId;
	ASSERT_TRUE(m_pServer->Translate(TranslatedId, ClientId));
	ASSERT_NE(TranslatedId, ClientId);
	m_pServer->m_SnapshotBuilder.Init();
	pPlayer->Snap(ClientId);
	CSnapshotBuffer RaceBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&RaceBuffer);
	const CSnapshot *pRaceSnapshot = RaceBuffer.AsSnapshot();
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_CLIENTINFO, TranslatedId), nullptr);
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_PLAYERINFO, TranslatedId), nullptr);
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_DDNETPLAYER, TranslatedId), nullptr);
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_DDNETSPECTATORINFO, TranslatedId), nullptr);
}

TEST_F(GameWorld, ModeShowsOtherColorsAndKeepsThePlayersOwn)
{
	CPlayer *pPlayer = JoinPlayer(0, TEAM_GAME, "painted");
	pPlayer->SetTeeInfos("pinky", true, 0x112233, 0x445566);
	pPlayer->SetTeeInfoOverride(CTeeInfoOverride::Colors(0xA0FF00));
	EXPECT_EQ(pPlayer->TeeInfos().m_ColorBody, 0xA0FF00);
	EXPECT_EQ(pPlayer->OwnTeeInfos().m_ColorBody, 0x112233);

	// what the player changes meanwhile stays theirs, under the colour of the mode
	pPlayer->SetTeeInfos("default", true, 0x778899, 0x445566);
	EXPECT_STREQ(pPlayer->TeeInfos().m_aSkinName, "default");
	EXPECT_EQ(pPlayer->TeeInfos().m_ColorBody, 0xA0FF00);

	GameServer()->m_PlayerMapping.InitPlayerMap(0);
	int TranslatedId = 0;
	ASSERT_TRUE(m_pServer->Translate(TranslatedId, 0));
	m_pServer->m_SnapshotBuilder.Init(false);
	pPlayer->Snap(0);
	CSnapshotBuffer Buffer;
	m_pServer->m_SnapshotBuilder.Finish(&Buffer);
	const auto *pClientInfo = static_cast<const CNetObj_ClientInfo *>(Buffer.AsSnapshot()->FindItem(NETOBJTYPE_CLIENTINFO, TranslatedId));
	ASSERT_NE(pClientInfo, nullptr);
	EXPECT_EQ(pClientInfo->m_ColorBody, 0xA0FF00);

	pPlayer->SetTeeInfoOverride(std::nullopt);
	EXPECT_EQ(pPlayer->TeeInfos().m_ColorBody, 0x778899);
}

TEST_F(GameWorld, CharacterSnapshotContributionsAreModeOwned)
{
	constexpr int ClientId = 0;
	SelectGameMode("dm");
	m_pServer->m_aClients[ClientId].m_State = CServer::CClient::STATE_INGAME;

	CPlayer *pVanillaPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	GameServer()->m_PlayerMapping.InitPlayerMap(ClientId);
	CCharacter *pVanillaCharacter = pVanillaPlayer->ForceSpawn(vec2(64.0f, 96.0f));
	EXPECT_NE(pVanillaCharacter, nullptr);
	if(!pVanillaCharacter)
	{
		delete GameServer()->m_apPlayers[ClientId];
		GameServer()->m_apPlayers[ClientId] = nullptr;
		return;
	}
	EXPECT_EQ(dynamic_cast<CCharacterDDRace *>(pVanillaCharacter), nullptr);
	m_pServer->m_SnapshotBuilder.Init();
	pVanillaCharacter->Snap(ClientId);
	CSnapshotBuffer VanillaBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&VanillaBuffer);
	const CSnapshot *pVanillaSnapshot = VanillaBuffer.AsSnapshot();
	EXPECT_NE(pVanillaSnapshot->FindItem(NETOBJTYPE_CHARACTER, ClientId), nullptr);
	EXPECT_EQ(pVanillaSnapshot->FindItem(NETOBJTYPE_DDNETCHARACTER, ClientId), nullptr);

	delete GameServer()->m_apPlayers[ClientId];
	GameServer()->m_apPlayers[ClientId] = nullptr;
	SelectGameMode("ddnet");
	CPlayer *pRacePlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	GameServer()->m_PlayerMapping.InitPlayerMap(ClientId);
	CCharacter *pRaceCharacter = pRacePlayer->ForceSpawn(vec2(64.0f, 96.0f));
	ASSERT_NE(pRaceCharacter, nullptr);
	EXPECT_NE(dynamic_cast<CCharacterDDRace *>(pRaceCharacter), nullptr);
	m_pServer->m_SnapshotBuilder.Init();
	pRaceCharacter->Snap(ClientId);
	CSnapshotBuffer RaceBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&RaceBuffer);
	const CSnapshot *pRaceSnapshot = RaceBuffer.AsSnapshot();
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_CHARACTER, ClientId), nullptr);
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_DDNETCHARACTER, ClientId), nullptr);
}

TEST_F(GameWorld, EntitySnapshotFormatsAreModeOwned)
{
	constexpr int ClientId = 0;
	constexpr int LaserId = 100;
	constexpr int PickupId = 101;
	SelectGameMode("dm");
	ASSERT_NE(GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1), nullptr);
	auto *pProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, -1, vec2(64.0f, 96.0f), vec2(1.0f, 0.0f), 100, false, false, -1, vec2(1.0f, 0.0f));
	ASSERT_TRUE(pProjectile->GetId() >= 0); // NOLINT(clang-analyzer-unix.Malloc)
	const int ProjectileId = pProjectile->GetId();

	m_pServer->m_aClients[ClientId].m_State = CServer::CClient::STATE_INGAME;
	m_pServer->SetClientDDNetVersion(ClientId, VERSION_DDNET_ENTITY_NETOBJS);
	const CSnapContext SnapContext(VERSION_DDNET_ENTITY_NETOBJS, false, SERVER_DEMO_CLIENT);

	m_pServer->m_SnapshotBuilder.Init();
	GameServer()->SnapLaserObject(SnapContext, LaserId, vec2(32.0f, 32.0f), vec2(64.0f, 32.0f), 1);
	GameServer()->SnapPickup(SnapContext, PickupId, vec2(32.0f, 64.0f), POWERUP_WEAPON, WEAPON_SHOTGUN, 0, 0);
	pProjectile->Snap(ClientId);
	CSnapshotBuffer VanillaBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&VanillaBuffer);
	const CSnapshot *pVanillaSnapshot = VanillaBuffer.AsSnapshot();
	EXPECT_NE(pVanillaSnapshot->FindItem(NETOBJTYPE_LASER, LaserId), nullptr);
	EXPECT_NE(pVanillaSnapshot->FindItem(NETOBJTYPE_PICKUP, PickupId), nullptr);
	EXPECT_NE(pVanillaSnapshot->FindItem(NETOBJTYPE_PROJECTILE, ProjectileId), nullptr);
	EXPECT_EQ(pVanillaSnapshot->FindItem(NETOBJTYPE_DDNETLASER, LaserId), nullptr);
	EXPECT_EQ(pVanillaSnapshot->FindItem(NETOBJTYPE_DDNETPICKUP, PickupId), nullptr);
	EXPECT_EQ(pVanillaSnapshot->FindItem(NETOBJTYPE_DDNETPROJECTILE, ProjectileId), nullptr);

	m_pServer->SetClientDDNetVersion(ClientId, VERSION_DDNET_MSG_LEGACY);
	m_pServer->m_SnapshotBuilder.Init();
	pProjectile->Snap(ClientId);
	CSnapshotBuffer VanillaLegacyBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&VanillaLegacyBuffer);
	const CSnapshot *pVanillaLegacySnapshot = VanillaLegacyBuffer.AsSnapshot();
	EXPECT_NE(pVanillaLegacySnapshot->FindItem(NETOBJTYPE_PROJECTILE, ProjectileId), nullptr);
	EXPECT_EQ(pVanillaLegacySnapshot->FindItem(NETOBJTYPE_DDRACEPROJECTILE, ProjectileId), nullptr);

	delete pProjectile;
	DeletePlayers();
	SelectGameMode("mod");
	ASSERT_NE(GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1), nullptr);
	pProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, -1, vec2(64.0f, 96.0f), vec2(1.0f, 0.0f), 100, false, false, -1, vec2(1.0f, 0.0f));
	ASSERT_TRUE(pProjectile->GetId() >= 0);
	const int RaceProjectileId = pProjectile->GetId();
	m_pServer->SetClientDDNetVersion(ClientId, VERSION_DDNET_ENTITY_NETOBJS);
	m_pServer->m_SnapshotBuilder.Init();
	GameServer()->SnapLaserObject(SnapContext, LaserId, vec2(32.0f, 32.0f), vec2(64.0f, 32.0f), 1);
	GameServer()->SnapPickup(SnapContext, PickupId, vec2(32.0f, 64.0f), POWERUP_WEAPON, WEAPON_SHOTGUN, 0, 0);
	pProjectile->Snap(ClientId);
	CSnapshotBuffer RaceBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&RaceBuffer);
	const CSnapshot *pRaceSnapshot = RaceBuffer.AsSnapshot();
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_DDNETLASER, LaserId), nullptr);
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_DDNETPICKUP, PickupId), nullptr);
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_DDNETPROJECTILE, RaceProjectileId), nullptr);

	m_pServer->SetClientDDNetVersion(ClientId, VERSION_DDNET_MSG_LEGACY);
	m_pServer->m_SnapshotBuilder.Init();
	pProjectile->Snap(ClientId);
	CSnapshotBuffer RaceLegacyBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&RaceLegacyBuffer);
	const CSnapshot *pRaceLegacySnapshot = RaceLegacyBuffer.AsSnapshot();
	EXPECT_NE(pRaceLegacySnapshot->FindItem(NETOBJTYPE_DDRACEPROJECTILE, RaceProjectileId), nullptr);
}

TEST_F(GameWorld, HotReloadStateIsModeOwned)
{
	CCharacterDDRace *pCharacter = SpawnPlayer<CCharacterDDRace>(0, vec2(64.0f, 96.0f));
	ASSERT_NE(pCharacter, nullptr);
	EXPECT_NE(dynamic_cast<CCharacterDDRace *>(pCharacter), nullptr);

	CGameControllerVanillaDM VanillaController(GameServices(), *FindGameMode("dm"));
	EXPECT_FALSE(VanillaController.SaveStateForMapReload());
	EXPECT_EQ(GameServer()->GameHost().MapReloadState(), nullptr);

	CGameControllerDDNet DDNetController(GameServices(), *FindGameMode("ddnet"));
	auto &PlayerState = RaceTeams().PlayerState(0);
	EXPECT_FALSE(PlayerState.m_LastTeleTee.has_value());
	EXPECT_FALSE(PlayerState.m_LastDeath.has_value());
	const vec2 SavedPosition = pCharacter->m_Pos;
	std::unique_ptr<IGameModeMapReloadState> pState = DDNetController.SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));

	PlayerState.m_LastTeleTee.emplace();
	PlayerState.m_LastDeath.emplace();
	pCharacter->m_Pos = vec2(320.0f, 320.0f);
	pCharacter->SetPosition(pCharacter->m_Pos);
	DDNetController.RestoreCharacterAfterMapReload(pCharacter);
	EXPECT_EQ(pCharacter->m_Pos, SavedPosition);
	EXPECT_FALSE(PlayerState.m_LastTeleTee.has_value());
	EXPECT_FALSE(PlayerState.m_LastDeath.has_value());

	RaceTeams().SaveLastTeleport(pCharacter);
	PlayerState.m_LastDeath = PlayerState.m_LastTeleTee;
	ASSERT_TRUE(PlayerState.m_LastTeleTee.has_value());
	ASSERT_TRUE(PlayerState.m_LastDeath.has_value());
	const vec2 SavedTeleportPosition = PlayerState.m_LastTeleTee->GetPos();
	const vec2 SavedDeathPosition = PlayerState.m_LastDeath->GetPos();
	pState = DDNetController.SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));
	PlayerState.m_LastTeleTee.reset();
	PlayerState.m_LastDeath.reset();
	DDNetController.RestoreCharacterAfterMapReload(pCharacter);
	ASSERT_TRUE(PlayerState.m_LastTeleTee.has_value());
	ASSERT_TRUE(PlayerState.m_LastDeath.has_value());
	EXPECT_EQ(PlayerState.m_LastTeleTee->GetPos(), SavedTeleportPosition);
	EXPECT_EQ(PlayerState.m_LastDeath->GetPos(), SavedDeathPosition);

	pState = DDNetController.SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));
	pCharacter->m_Pos = vec2(384.0f, 384.0f);
	pCharacter->SetPosition(pCharacter->m_Pos);
	VanillaController.RestoreCharacterAfterMapReload(pCharacter);
	DDNetController.RestoreCharacterAfterMapReload(pCharacter);
	EXPECT_EQ(pCharacter->m_Pos, vec2(384.0f, 384.0f));
}

TEST_F(GameWorld, MapReloadStateSurvivesContextReconstruction)
{
	constexpr int ClientId = 0;
	CCharacterDDRace *pCharacter = SpawnPlayer<CCharacterDDRace>(ClientId, vec2(64.0f, 96.0f));
	ASSERT_NE(pCharacter, nullptr);
	RaceTeams().SaveLastTeleport(pCharacter);
	RaceTeams().PlayerState(ClientId).m_LastDeath = RaceTeams().PlayerState(ClientId).m_LastTeleTee;
	const vec2 SavedPosition = pCharacter->m_Pos;
	const vec2 SavedTeleportPosition = RaceTeams().PlayerState(ClientId).m_LastTeleTee->GetPos();

	std::unique_ptr<IGameModeMapReloadState> pState = GameController()->SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));
	GameServer()->OnShutdown(m_pServer->m_pPersistentData);
	m_pKernel->ReregisterInterface(m_pGameServer);
	GameServer()->OnInit(m_pServer->m_pPersistentData);

	ASSERT_NE(GameServer()->GameHost().MapReloadState(), nullptr);
	CCharacter *pRestoredCharacter = SpawnPlayer(ClientId, vec2(320.0f, 320.0f));
	ASSERT_NE(pRestoredCharacter, nullptr);
	EXPECT_EQ(pRestoredCharacter->m_Pos, SavedPosition);
	const auto &RestoredPlayerState = RaceTeams().PlayerState(ClientId);
	ASSERT_TRUE(RestoredPlayerState.m_LastTeleTee.has_value());
	ASSERT_TRUE(RestoredPlayerState.m_LastDeath.has_value());
	EXPECT_EQ(RestoredPlayerState.m_LastTeleTee->GetPos(), SavedTeleportPosition);
	EXPECT_EQ(RestoredPlayerState.m_LastDeath->GetPos(), SavedTeleportPosition);
}

TEST_F(GameWorld, MapReloadStateExpiresAfterOneContextHandoff)
{
	std::unique_ptr<IGameModeMapReloadState> pState = GameController()->SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));

	std::unique_ptr<IGameModeMapReloadState> pTransferredState = GameServer()->GameHost().TakeMapReloadState();
	ASSERT_NE(pTransferredState, nullptr);
	GameServer()->GameHost().RestoreMapReloadState(std::move(pTransferredState));
	ASSERT_NE(GameServer()->GameHost().MapReloadState(), nullptr);

	EXPECT_FALSE(GameServer()->GameHost().TakeMapReloadState());
	EXPECT_EQ(GameServer()->GameHost().MapReloadState(), nullptr);
}

TEST_F(GameWorld, MapReloadDisconnectKeepsSharedTeamState)
{
	constexpr int Team = 5;
	for(int ClientId = 0; ClientId < 2; ClientId++)
	{
		GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
		ASSERT_NE(GameServer()->m_apPlayers[ClientId]->ForceSpawn(vec2(64.0f + ClientId * 32.0f, 96.0f)), nullptr);
		RaceTeams().SetForceCharacterTeam(ClientId, Team);
	}
	RaceTeams().SetPractice(Team, true);

	std::unique_ptr<IGameModeMapReloadState> pState = GameController()->SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));
	m_pServer->m_aClients[0].m_State = CServer::CClient::STATE_AUTH;
	m_pServer->m_aClients[0].m_DebugDummy = true;
	GameServer()->OnClientDrop(0, "test disconnect before map reload");
	m_pServer->m_aClients[0].m_State = CServer::CClient::STATE_EMPTY;
	m_pServer->m_aClients[0].m_DebugDummy = false;
	RaceTeams().SetPractice(Team, false);

	GameController()->RestoreCharacterAfterMapReload(GameServer()->GetPlayerChar(1));
	EXPECT_TRUE(RaceTeams().IsPractice(Team));
}

TEST_F(GameWorld, PreparingMapReloadReplacesTeamState)
{
	constexpr int ClientId = 0;
	constexpr int Team = 5;
	CCharacter *pCharacter = SpawnPlayer(ClientId, vec2(64.0f, 96.0f));
	ASSERT_NE(pCharacter, nullptr);
	RaceTeams().SetForceCharacterTeam(ClientId, Team);
	RaceTeams().SetPractice(Team, true);

	std::unique_ptr<IGameModeMapReloadState> pState = GameController()->SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));
	RaceTeams().SetPractice(Team, false);
	pState = GameController()->SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));
	RaceTeams().SetPractice(Team, true);

	GameController()->RestoreCharacterAfterMapReload(pCharacter);
	EXPECT_FALSE(RaceTeams().IsPractice(Team));
}

TEST_F(GameWorld, ModeOwnedCommandsFollowControllerLifetime)
{
	IConsole *pConsole = GameServer()->Console();
	const char *const apModeChatCommands[] = {"info", "map", "mapinfo", "rank", "team", "practice", "tp", "hitothers", "save", "settings", "timer"};
	const char *const apModeAdminCommands[] = {"tele", "set_team_ddr", "save_dry", "random_map", "random_unfinished_map", "switch_open", "tune_zone", "tune_zone_dump", "tune_zone_reset", "tune_zone_enter", "tune_zone_leave"};
	auto ExpectModeCommands = [&](bool Registered) {
		for(const char *pName : apModeChatCommands)
			EXPECT_EQ(pConsole->GetCommandInfo(pName, CFGFLAG_CHAT, false) != nullptr, Registered) << pName;
		for(const char *pName : apModeAdminCommands)
			EXPECT_EQ(pConsole->GetCommandInfo(pName, CFGFLAG_SERVER, false) != nullptr, Registered) << pName;
	};
	// /pause is DDRace's own pause, and the ready mode's in the vanilla modes
	auto ExpectPauseAndReady = [&](bool Pause, bool Ready) {
		EXPECT_EQ(pConsole->GetCommandInfo("pause", CFGFLAG_CHAT, false) != nullptr, Pause);
		EXPECT_EQ(pConsole->GetCommandInfo("ready", CFGFLAG_CHAT, false) != nullptr, Ready);
	};

	ExpectModeCommands(true);
	ExpectPauseAndReady(true, false);
	EXPECT_TRUE(RaceControllerOrNull() != nullptr);
	RaceScore().SetCurrentRecord(12.5f);
	ASSERT_TRUE(RaceScore().CurrentRecord().has_value());
	EXPECT_FLOAT_EQ(RaceScore().CurrentRecord().value(), 12.5f);
	ASSERT_NE(pConsole->GetCommandInfo("help", CFGFLAG_CHAT, false), nullptr);
	ASSERT_NE(pConsole->GetCommandInfo("showall", CFGFLAG_CHAT, false), nullptr);
	ASSERT_NE(pConsole->GetCommandInfo("kill_pl", CFGFLAG_SERVER, false), nullptr);

	GameServer()->GameHost().Shutdown();
	ExpectModeCommands(false);
	ExpectPauseAndReady(false, false);
	EXPECT_FALSE(RaceControllerOrNull() != nullptr);
	EXPECT_NE(pConsole->GetCommandInfo("help", CFGFLAG_CHAT, false), nullptr);
	EXPECT_NE(pConsole->GetCommandInfo("showall", CFGFLAG_CHAT, false), nullptr);
	EXPECT_NE(pConsole->GetCommandInfo("kill_pl", CFGFLAG_SERVER, false), nullptr);

	SelectGameMode("dm");
	ExpectModeCommands(false);
	ExpectPauseAndReady(true, true);
	EXPECT_FALSE(RaceControllerOrNull() != nullptr);
	EXPECT_FALSE(RaceControllerOrNull() != nullptr);
	const int VanillaClientId = 0;
	CPlayer *pVanillaPlayer = GameServer()->CreatePlayer(VanillaClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pVanillaPlayer, nullptr);
	EXPECT_NE(dynamic_cast<CPlayerVanilla *>(pVanillaPlayer), nullptr);
	CCharacter *pVanillaCharacter = pVanillaPlayer->ForceSpawn(vec2(64.0f, 96.0f));
	ASSERT_NE(pVanillaCharacter, nullptr);
	EXPECT_EQ(dynamic_cast<CCharacterDDRace *>(pVanillaCharacter), nullptr);
	auto *pVanillaLaser = new CLaser(&GameServer()->m_World, pVanillaCharacter->m_Pos, vec2(1.0f, 0.0f), 64.0f, VanillaClientId, WEAPON_LASER);
	pVanillaLaser->Tick();
	delete pVanillaLaser;
	auto *pVanillaProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, VanillaClientId, pVanillaCharacter->m_Pos, vec2(1.0f, 0.0f), 100, false, false, -1, vec2(1.0f, 0.0f));
	EXPECT_TRUE(pVanillaProjectile->CanCollide(VanillaClientId));
	delete pVanillaProjectile;
	g_Config.m_SvPlayerDemoRecord = 1;
	EXPECT_TRUE(m_pStorage->CreateFolder("demos", IStorage::TYPE_SAVE));
	m_pServer->StartRecord(VanillaClientId);
	EXPECT_TRUE(m_pServer->IsRecording(VanillaClientId));
	pVanillaCharacter->StopRecording();
	EXPECT_FALSE(m_pServer->IsRecording(VanillaClientId));
	m_pServer->m_aClients[VanillaClientId].m_State = CServer::CClient::STATE_READY;
	m_pServer->SetClientScore(VanillaClientId, 17);
	GameController()->OnPlayerNameChanged(VanillaClientId);
	EXPECT_EQ(m_pServer->m_aClients[VanillaClientId].m_Score, 17);
	g_Config.m_SvTestingCommands = 1;
	g_Config.m_SvPracticeByDefault = 0;
	pConsole->ExecuteLine("sv_practice_by_default 1", IConsole::CLIENT_ID_UNSPECIFIED, true);
	EXPECT_EQ(g_Config.m_SvPracticeByDefault, 1);

	delete GameServer()->m_apPlayers[VanillaClientId];
	GameServer()->m_apPlayers[VanillaClientId] = nullptr;
	SelectGameMode("ddnet");
	ExpectModeCommands(true);
	ExpectPauseAndReady(true, false);
	EXPECT_TRUE(RaceControllerOrNull() != nullptr);
	EXPECT_TRUE(RaceControllerOrNull() != nullptr);
	EXPECT_FALSE(RaceScore().CurrentRecord().has_value());
	g_Config.m_SvTestingCommands = 1;
	g_Config.m_SvPracticeByDefault = 0;
	RaceTeams().Reset();
	EXPECT_FALSE(RaceTeams().PracticeByDefault());
	EXPECT_FALSE(RaceTeams().IsPractice(TEAM_FLOCK));
	pConsole->ExecuteLine("sv_practice_by_default 1", IConsole::CLIENT_ID_UNSPECIFIED, true);
	EXPECT_TRUE(RaceTeams().PracticeByDefault());
	EXPECT_TRUE(RaceTeams().IsPractice(TEAM_FLOCK));
}

TEST_F(GameWorld, DDRaceTeamCommandUsesModeOwnedState)
{
	constexpr int ClientId = 0;
	ASSERT_NE(SpawnPlayer(ClientId, vec2(64.0f, 96.0f)), nullptr);
	EXPECT_EQ(RaceTeams().m_Core.Team(ClientId), TEAM_FLOCK);

	GameServer()->Console()->ExecuteLine("team 1", ClientId);
	EXPECT_EQ(RaceTeams().m_Core.Team(ClientId), 1);
}

TEST_F(GameWorld, DDRaceSwitchLifecycleIsTeamOwned)
{
	auto &vSwitchers = GameServer()->Switchers();
	ASSERT_GT(vSwitchers.size(), 1u);
	constexpr int Switch = 1;
	constexpr int Team = 0;
	auto &Switcher = vSwitchers[Switch];
	Switcher.m_aStatus[Team] = true;
	Switcher.m_aEndTick[Team] = GameServer()->Server()->Tick();
	Switcher.m_aType[Team] = TILE_SWITCHTIMEDOPEN;
	RaceTeams().Tick();
	EXPECT_FALSE(Switcher.m_aStatus[Team]);
	EXPECT_EQ(Switcher.m_aType[Team], TILE_SWITCHCLOSE);

	Switcher.m_aEndTick[Team] = GameServer()->Server()->Tick();
	Switcher.m_aType[Team] = TILE_SWITCHTIMEDCLOSE;
	RaceTeams().Tick();
	EXPECT_TRUE(Switcher.m_aStatus[Team]);
	EXPECT_EQ(Switcher.m_aType[Team], TILE_SWITCHOPEN);
}

TEST_F(GameWorld, DDRaceRecordingPolicyIsCharacterOwned)
{
	constexpr int ClientId = 0;
	CCharacterDDRace *pCharacter = SpawnPlayer<CCharacterDDRace>(ClientId, vec2(64.0f, 96.0f));
	ASSERT_NE(pCharacter, nullptr);
	g_Config.m_SvPlayerDemoRecord = 1;
	EXPECT_TRUE(m_pStorage->CreateFolder("demos", IStorage::TYPE_SAVE));
	m_pServer->StartRecord(ClientId);
	ASSERT_TRUE(m_pServer->IsRecording(ClientId));

	CPlayerData *pData = RaceScore().PlayerData(ClientId);
	pData->m_RecordStopTick = GameServer()->Server()->Tick() + GameServer()->Server()->TickSpeed();
	pData->m_RecordFinishTime = 12.5f;
	pCharacter->StopRecording();

	EXPECT_FALSE(m_pServer->IsRecording(ClientId));
	EXPECT_EQ(pData->m_RecordStopTick, -1);
}

TEST_F(GameWorld, DDRacePlayerCommandUsesModeOwnedState)
{
	constexpr int ClientId = 0;
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);

	g_Config.m_SvShowOthers = 1;
	GameServer()->Console()->ExecuteLine("showothers 2", ClientId);
	EXPECT_EQ(RaceTeams().PlayerState(ClientId).m_ShowOthers, 2);
	GameServer()->Console()->ExecuteLine("ninjajetpack 1", ClientId);
	EXPECT_TRUE(static_cast<CPlayerDDRace *>(pPlayer)->m_NinjaJetpack);
}

TEST_F(GameWorld, DDRaceAdminAndPracticeCommandsUseModeOwnedState)
{
	constexpr int ClientId = 0;
	constexpr int Team = 1;
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);
	auto *pCharacter = static_cast<CCharacterDDRace *>(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pCharacter, nullptr);
	m_pServer->m_aClients[ClientId].m_State = CServer::CClient::STATE_READY;
	const int AuthKey = m_pServer->m_AuthManager.AddKey("gameworld-admin", "test", RoleName::ADMIN);
	ASSERT_GE(AuthKey, 0);
	m_pServer->m_aClients[ClientId].m_AuthKey = AuthKey;

	g_Config.m_SvTestingCommands = 1;
	GameServer()->Console()->ExecuteLine("super", ClientId);
	EXPECT_TRUE(pCharacter->IsSuper());
	GameServer()->Console()->ExecuteLine("unsuper", ClientId);
	EXPECT_FALSE(pCharacter->IsSuper());

	RaceTeams().SetForceCharacterTeam(ClientId, Team);
	RaceTeams().SetPractice(Team, true);
	GameServer()->Console()->ExecuteLineFlag("hitothers all", CFGFLAG_CHAT, ClientId);
	EXPECT_TRUE(pCharacter->HammerHitDisabled());
	EXPECT_TRUE(pCharacter->ShotgunHitDisabled());
	EXPECT_TRUE(pCharacter->GrenadeHitDisabled());
	EXPECT_TRUE(pCharacter->LaserHitDisabled());
}

TEST_F(GameWorld, RaceScorePlayerStateFollowsPlayerIdentity)
{
	constexpr int ClientId = 0;
	CScore *pScore = &RaceScore();
	CPlayer *pFirstPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pFirstPlayer, nullptr);
	const uint32_t FirstUniqueClientId = pFirstPlayer->GetUniqueCid();
	pScore->ResetPlayer(ClientId);

	float aTimeCp[NUM_CHECKPOINTS] = {};
	pScore->PlayerData(ClientId)->Set(12.5f, aTimeCp);
	ASSERT_TRUE(pScore->PlayerData(ClientId)->m_BestTime.has_value());
	pScore->BeginFinishEligibilityCheck(ClientId);
	EXPECT_TRUE(pScore->FinishEligibilityCheckActive(ClientId));
	pScore->SetNotEligibleForFinish(ClientId);
	EXPECT_TRUE(pScore->NotEligibleForFinish(ClientId));
	EXPECT_FALSE(pScore->FinishEligibilityCheckActive(ClientId));

	CPlayer *pSecondPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pSecondPlayer, nullptr);
	ASSERT_NE(FirstUniqueClientId, pSecondPlayer->GetUniqueCid());
	pScore->Tick();
	EXPECT_FALSE(pScore->PlayerData(ClientId)->m_BestTime.has_value());
	EXPECT_FALSE(pScore->NotEligibleForFinish(ClientId));
	EXPECT_FALSE(pScore->FinishEligibilityCheckActive(ClientId));

	pScore->PlayerData(ClientId)->Set(13.5f, aTimeCp);
	pScore->SetNotEligibleForFinish(ClientId);
	GameController()->OnPlayerNameChanged(ClientId);
	EXPECT_FALSE(pScore->PlayerData(ClientId)->m_BestTime.has_value());
	EXPECT_FALSE(pScore->NotEligibleForFinish(ClientId));

	pScore->PlayerData(ClientId)->Set(14.5f, aTimeCp);
	pScore->SetNotEligibleForFinish(ClientId);
	GameController()->OnPlayerDisconnect(pSecondPlayer, "test");
	EXPECT_FALSE(pScore->PlayerData(ClientId)->m_BestTime.has_value());
	EXPECT_FALSE(pScore->NotEligibleForFinish(ClientId));
}

TEST_F(GameWorld, RaceFinishEligibilityIsModeOwned)
{
	constexpr int ClientId = 0;
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);

	GameController()->OnPlayerEnter(pPlayer);
	EXPECT_TRUE(RaceScore().FinishEligibilityCheckActive(ClientId));
	EXPECT_FALSE(GameController()->OnPlayerChatMessage(ClientId, "hello", 0));
	EXPECT_TRUE(GameController()->OnPlayerChatMessage(ClientId, "xd sure chillerbot.png is lyfe", 0));
	EXPECT_TRUE(RaceScore().NotEligibleForFinish(ClientId));

	CGameControllerVanillaDM VanillaController(GameServices(), *FindGameMode("dm"));
	EXPECT_FALSE(VanillaController.OnPlayerChatMessage(ClientId, "xd sure chillerbot.png is lyfe", 0));
}

TEST_F(GameWorld, RaceTeamPlayerStateFollowsPlayerIdentity)
{
	constexpr int ClientId = 0;
	CPlayer *pFirstPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pFirstPlayer, nullptr);
	GameController()->OnPlayerConnect(pFirstPlayer);
	CCharacterDDRace *pFirstCharacter = dynamic_cast<CCharacterDDRace *>(pFirstPlayer->ForceSpawn(vec2(0.0f, 32.0f)));
	ASSERT_NE(pFirstCharacter, nullptr);
	EXPECT_FALSE(RaceTeams().LoadLastTeleport(pFirstCharacter));
	RaceTeams().SaveLastTeleport(pFirstCharacter);
	pFirstCharacter->SetPosition(vec2(64.0f, 64.0f));
	EXPECT_TRUE(RaceTeams().LoadLastTeleport(pFirstCharacter));
	EXPECT_EQ(pFirstCharacter->m_Pos.x, 0.0f);
	EXPECT_EQ(pFirstCharacter->m_Pos.y, 32.0f);

	auto SetState = [this] {
		auto &State = RaceTeams().PlayerState(ClientId);
		State.m_TeeStarted = true;
		State.m_TeeFinished = true;
		State.m_LastChat = 1;
		State.m_LastSwap = 2;
		State.m_LastInvited = 3;
		State.m_LastTeamChange = 4;
		State.m_VotedForPractice = true;
		State.m_SwapTargetClientId = 1;
		State.m_RescueMode = RESCUEMODE_MANUAL;
		State.m_LastTeleTee.emplace();
		State.m_LastDeath.emplace();
		State.m_ShowOthers = SHOW_OTHERS_OFF;
		State.m_SpecTeam = true;
	};
	auto ExpectReset = [this](int ShowOthers) {
		const auto &State = RaceTeams().PlayerState(ClientId);
		EXPECT_FALSE(State.m_TeeStarted);
		EXPECT_FALSE(State.m_TeeFinished);
		EXPECT_EQ(State.m_LastChat, 0);
		EXPECT_EQ(State.m_LastSwap, 0);
		EXPECT_EQ(State.m_LastInvited, 0);
		EXPECT_FALSE(State.m_LastTeamChange.has_value());
		EXPECT_FALSE(State.m_VotedForPractice);
		EXPECT_EQ(State.m_SwapTargetClientId, -1);
		EXPECT_EQ(State.m_RescueMode, RESCUEMODE_AUTO);
		EXPECT_FALSE(State.m_LastTeleTee.has_value());
		EXPECT_FALSE(State.m_LastDeath.has_value());
		EXPECT_EQ(State.m_ShowOthers, ShowOthers);
		EXPECT_FALSE(State.m_SpecTeam);
	};

	SetState();
	RaceTeams().Reset();
	const auto &RoundResetState = RaceTeams().PlayerState(ClientId);
	// A world reset restarts the run. What the player set up themselves and the
	// tees they saved for /back and /lastdeath belong to the player and stay.
	EXPECT_FALSE(RoundResetState.m_TeeStarted);
	EXPECT_FALSE(RoundResetState.m_TeeFinished);
	EXPECT_EQ(RoundResetState.m_LastChat, 0);
	EXPECT_TRUE(RoundResetState.m_LastTeleTee.has_value());
	EXPECT_TRUE(RoundResetState.m_LastDeath.has_value());
	EXPECT_EQ(RoundResetState.m_RescueMode, RESCUEMODE_MANUAL);
	EXPECT_TRUE(RoundResetState.m_LastTeamChange.has_value());
	EXPECT_EQ(RoundResetState.m_ShowOthers, SHOW_OTHERS_OFF);
	EXPECT_TRUE(RoundResetState.m_SpecTeam);

	SetState();
	CPlayer *pSecondPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pSecondPlayer, nullptr);
	GameController()->OnPlayerConnect(pSecondPlayer);
	ExpectReset(g_Config.m_SvShowOthersDefault);

	SetState();
	GameController()->OnPlayerDisconnect(pSecondPlayer, "test");
	ExpectReset(SHOW_OTHERS_ON);
}

TEST_F(GameWorld, VanillaTDMTeamDamage)
{
	SelectGameMode("tdm");
	auto &Controller = *dynamic_cast<CGameControllerVanillaTDM *>(GameController());

	GameServer()->CreatePlayer(0, TEAM_RED, false, -1);
	GameServer()->CreatePlayer(1, TEAM_RED, false, -1);
	GameServer()->CreatePlayer(2, TEAM_BLUE, false, -1);
	CCharacter *pAttacker = GameServer()->m_apPlayers[0]->ForceSpawn(vec2(-64, 0));
	CCharacter *pTeammate = GameServer()->m_apPlayers[1]->ForceSpawn(vec2(0, 0));
	CCharacter *pEnemy = GameServer()->m_apPlayers[2]->ForceSpawn(vec2(64, 0));
	pTeammate->SetHealth(10);
	pEnemy->SetHealth(10);
	g_Config.m_SvTeamdamage = 0;

	CGameDamageContext Hit;
	Hit.m_Force = vec2(2, 0);
	Hit.m_Damage = 1;
	Hit.m_From = pAttacker->GetPlayer()->GetCid();
	Hit.m_Weapon = WEAPON_GUN;
	Controller.OnCharacterTakeDamage(pTeammate, Hit);
	Controller.OnCharacterTakeDamage(pEnemy, Hit);
	const int TeammateHealth = pTeammate->GetHealth();
	const int EnemyHealth = pEnemy->GetHealth();
	const float TeammateVelocityX = pTeammate->GetCore().m_Vel.x;
	auto *pFriendlyProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, pAttacker->GetPlayer()->GetCid(), pTeammate->m_Pos, vec2(1, 0), 100, false, false, -1, vec2(1, 0));
	auto *pEnemyProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, pAttacker->GetPlayer()->GetCid(), pEnemy->m_Pos, vec2(1, 0), 100, false, false, -1, vec2(1, 0));
	Controller.DoTeamChange(pAttacker->GetPlayer(), TEAM_BLUE, false);
	const int BlueTeamAfterForceSpawn = pEnemy->GetPlayer()->GetTeam();
	const int FriendlyProjectileOwner = pFriendlyProjectile->GetOwnerId(); // NOLINT(clang-analyzer-unix.Malloc)
	const int EnemyProjectileOwner = pEnemyProjectile->GetOwnerId(); // NOLINT(clang-analyzer-unix.Malloc)

	EXPECT_EQ(TeammateHealth, 10);
	EXPECT_GT(TeammateVelocityX, 0.0f);
	EXPECT_EQ(EnemyHealth, 9);
	EXPECT_EQ(BlueTeamAfterForceSpawn, TEAM_BLUE);
	EXPECT_EQ(FriendlyProjectileOwner, -1);
	EXPECT_EQ(EnemyProjectileOwner, -1);
}

TEST_F(GameWorld, VanillaTeamBalanceRunsAfterConfiguredDelay)
{
	g_Config.m_SvTeambalanceTime = 1;
	CTestVanillaTDM Controller(GameServices(), *FindGameMode("tdm"));

	for(int ClientId = 0; ClientId < 4; ClientId++)
		GameServer()->m_apPlayers[ClientId] = Controller.CreatePlayer(ClientId + 1, ClientId, ClientId == 3 ? TEAM_BLUE : TEAM_RED);
	auto *pRedZero = static_cast<CPlayerVanilla *>(GameServer()->m_apPlayers[0]);
	auto *pRedBestFit = static_cast<CPlayerVanilla *>(GameServer()->m_apPlayers[1]);
	auto *pRedTen = static_cast<CPlayerVanilla *>(GameServer()->m_apPlayers[2]);
	auto *pBlue = static_cast<CPlayerVanilla *>(GameServer()->m_apPlayers[3]);
	pRedZero->m_Score = 0;
	pRedBestFit->m_Score = 2;
	pRedTen->m_Score = 10;
	pBlue->m_Score = 3;
	pRedBestFit->m_LastActionTick = 123;

	char aError[64];
	EXPECT_FALSE(Controller.CanJoinTeam(TEAM_RED, 4, aError, sizeof(aError)));
	EXPECT_STREQ(aError, "Teams must remain balanced");
	const int Now = GameServer()->Server()->Tick();
	Controller.UpdateTeamBalance(Now);
	Controller.UpdateTeamBalance(Now + GameServer()->Server()->TickSpeed() * 60);
	EXPECT_EQ(pRedBestFit->GetTeam(), TEAM_RED);
	Controller.UpdateTeamBalance(Now + GameServer()->Server()->TickSpeed() * 60 + 1);

	EXPECT_EQ(pRedZero->GetTeam(), TEAM_RED);
	EXPECT_EQ(pRedBestFit->GetTeam(), TEAM_BLUE);
	EXPECT_EQ(pRedTen->GetTeam(), TEAM_RED);
	EXPECT_EQ(pBlue->GetTeam(), TEAM_BLUE);
	EXPECT_EQ(pRedBestFit->m_LastActionTick, 123);
}

TEST_F(GameWorld, VanillaCTFScoreLimitUsesRawScore)
{
	auto &Controller = SelectController<CTestVanillaCTF>("ctf");
	g_Config.m_SvScorelimit = 1;
	g_Config.m_SvTimelimit = 0;

	Controller.SetTeamScores(1, 0);
	Controller.Tick();

	EXPECT_TRUE(Controller.IsGamePaused());
}

TEST_F(GameWorld, VanillaCTFTiedTimeLimitStartsSuddenDeath)
{
	auto &Controller = SelectController<CTestVanillaCTF>("ctf");
	g_Config.m_SvScorelimit = 0;
	g_Config.m_SvTimelimit = 1;

	Controller.SetTeamScores(0, 0);
	Controller.StartRoundAt(GameServer()->Server()->Tick() - GameServer()->Server()->TickSpeed() * 60);
	Controller.Tick();

	EXPECT_FALSE(Controller.IsGamePaused());
	EXPECT_TRUE(Controller.IsSuddenDeath());
}

TEST_F(GameWorld, VanillaCTFSuddenDeathIsReported)
{
	auto &Controller = SelectController<CTestVanillaCTF>("ctf");
	g_Config.m_SvScorelimit = 1;
	g_Config.m_SvTimelimit = 0;
	JoinPlayer(0, TEAM_RED, "red");

	Controller.SetTeamScores(101, 100);
	Controller.BeginSuddenDeath();
	Controller.SendLiveStats(0);
	const CReceivedMatchReport Live = ReceivedMatchReport(0);
	EXPECT_EQ(Live.m_Report.m_Termination, EMatchTermination::ABORTED);
	bool LiveSuddenDeath = false;
	for(const CMatchMetric &Metric : Live.m_Report.m_vMetrics)
		LiveSuddenDeath |= Metric.m_SubjectKind == EMatchSubjectKind::MATCH && Metric.m_MetricId == "sudden_death";
	EXPECT_TRUE(LiveSuddenDeath);

	Controller.SetTeamScores(200, 100);
	Controller.Tick();
	EXPECT_TRUE(Controller.IsGamePaused());
	const CReceivedMatchReport Final = ReceivedMatchReport(0);
	EXPECT_EQ(Final.m_Report.m_MatchId, Live.m_Report.m_MatchId);
	EXPECT_EQ(Final.m_Report.m_Termination, EMatchTermination::COMPLETED);
	bool FinalSuddenDeath = false;
	for(const CMatchMetric &Metric : Final.m_Report.m_vMetrics)
		FinalSuddenDeath |= Metric.m_SubjectKind == EMatchSubjectKind::MATCH && Metric.m_MetricId == "sudden_death";
	EXPECT_TRUE(FinalSuddenDeath);
	EXPECT_EQ(Final.m_Report.Metric(EMatchSubjectKind::TEAM, TEAM_RED, "score"), 200);
	ASSERT_NE(Final.m_Report.Standing(EMatchSubjectKind::TEAM, TEAM_RED), nullptr);
	EXPECT_EQ(Final.m_Report.Standing(EMatchSubjectKind::TEAM, TEAM_RED)->m_Outcome, EMatchOutcome::WIN);
	EXPECT_EQ(Final.m_Report.Standing(EMatchSubjectKind::PARTICIPANT, Final.m_LocalParticipantId)->m_Outcome, EMatchOutcome::WIN);
}

TEST_F(GameWorld, VanillaCTFSuddenDeathUsesCaptureScore)
{
	auto &Controller = SelectController<CTestVanillaCTF>("ctf");
	g_Config.m_SvScorelimit = 1;
	g_Config.m_SvTimelimit = 0;

	Controller.SetTeamScores(101, 100);
	Controller.BeginSuddenDeath();
	Controller.Tick();

	EXPECT_FALSE(Controller.IsGamePaused());
	EXPECT_TRUE(Controller.IsSuddenDeath());
	Controller.SetTeamScores(200, 100);
	Controller.Tick();
	EXPECT_TRUE(Controller.IsGamePaused());
}

TEST_F(GameWorld, VanillaCTFTeamBalanceDoesNotMoveFlagCarrier)
{
	auto &Controller = SelectController<CTestVanillaCTF>("ctf");
	g_Config.m_SvTeambalanceTime = 1;

	for(int ClientId = 0; ClientId < 4; ClientId++)
		ASSERT_NE(GameServer()->CreatePlayer(ClientId, ClientId == 3 ? TEAM_BLUE : TEAM_RED, false, -1), nullptr);
	auto *pRedZero = static_cast<CPlayerVanilla *>(GameServer()->m_apPlayers[0]);
	auto *pCarrier = static_cast<CPlayerVanilla *>(GameServer()->m_apPlayers[1]);
	auto *pRedTen = static_cast<CPlayerVanilla *>(GameServer()->m_apPlayers[2]);
	auto *pBlue = static_cast<CPlayerVanilla *>(GameServer()->m_apPlayers[3]);
	pRedZero->m_Score = 0;
	pCarrier->m_Score = 2;
	pRedTen->m_Score = 10;
	pBlue->m_Score = 3;
	for(int ClientId = 0; ClientId < 4; ClientId++)
		ASSERT_NE(GameServer()->m_apPlayers[ClientId]->ForceSpawn(vec2(64.0f + ClientId * 32.0f, 96.0f)), nullptr);
	ASSERT_TRUE(Controller.OnEntity({ENTITY_FLAGSTAND_BLUE, 2, 2, LAYER_GAME, 0, true, 0}));
	ASSERT_NE(Controller.Flag(TEAM_BLUE), nullptr);
	Controller.Flag(TEAM_BLUE)->Grab(pCarrier->GetCharacter());

	const int Now = GameServer()->Server()->Tick();
	Controller.UpdateTeamBalance(Now);
	Controller.UpdateTeamBalance(Now + GameServer()->Server()->TickSpeed() * 60 + 1);

	EXPECT_EQ(pRedZero->GetTeam(), TEAM_BLUE);
	EXPECT_EQ(pCarrier->GetTeam(), TEAM_RED);
	EXPECT_EQ(pRedTen->GetTeam(), TEAM_RED);
	EXPECT_EQ(pBlue->GetTeam(), TEAM_BLUE);
	EXPECT_EQ(Controller.Flag(TEAM_BLUE)->Carrier(), pCarrier->GetCharacter());
}

TEST_F(GameWorld, InstagibDMVerticalSlice)
{
	SelectGameMode("idm");

	constexpr int AttackerId = 0;
	constexpr int VictimId = 1;
	CPlayer *pAttacker = GameServer()->CreatePlayer(AttackerId, TEAM_GAME, false, -1);
	CPlayer *pVictim = GameServer()->CreatePlayer(VictimId, TEAM_GAME, false, -1);
	ASSERT_NE(pAttacker, nullptr);
	ASSERT_NE(pVictim, nullptr);
	CCharacter *pAttackerCharacter = pAttacker->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pVictimCharacter = pVictim->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pAttackerCharacter, nullptr);
	ASSERT_NE(pVictimCharacter, nullptr);
	EXPECT_FALSE(pAttackerCharacter->GetWeaponGot(WEAPON_HAMMER));
	EXPECT_FALSE(pAttackerCharacter->GetWeaponGot(WEAPON_GUN));
	EXPECT_TRUE(pAttackerCharacter->GetWeaponGot(WEAPON_LASER));
	EXPECT_EQ(pAttackerCharacter->GetWeaponAmmo(WEAPON_LASER), -1);
	EXPECT_EQ(pAttackerCharacter->GetActiveWeapon(), WEAPON_LASER);

	pAttackerCharacter->TakeDamage(vec2(), 0, AttackerId, WEAPON_LASER);
	EXPECT_TRUE(pAttackerCharacter->IsAlive());
	EXPECT_EQ(pAttackerCharacter->GetHealth(), 10);

	pVictimCharacter->SetArmor(10);
	pVictimCharacter->TakeDamage(vec2(), 0, AttackerId, WEAPON_LASER);
	EXPECT_FALSE(pVictimCharacter->IsAlive());
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pAttacker), 1);
	EXPECT_TRUE(GameController()->OnEntity({ENTITY_SPAWN, 1, 1, LAYER_GAME, 0, true, 0}));
	EXPECT_FALSE(GameController()->OnEntity({ENTITY_HEALTH_1, 1, 1, LAYER_GAME, 0, true, 0}));
	EXPECT_FALSE(GameController()->OnEntity({ENTITY_WEAPON_LASER, 1, 1, LAYER_GAME, 0, true, 0}));
}

TEST_F(GameWorld, InstagibTDMReusesTeamplay)
{
	SelectGameMode("itdm");

	constexpr int AttackerId = 0;
	constexpr int TeammateId = 1;
	constexpr int EnemyId = 2;
	CPlayer *pAttacker = GameServer()->CreatePlayer(AttackerId, TEAM_RED, false, -1);
	CPlayer *pTeammate = GameServer()->CreatePlayer(TeammateId, TEAM_RED, false, -1);
	CPlayer *pEnemy = GameServer()->CreatePlayer(EnemyId, TEAM_BLUE, false, -1);
	ASSERT_NE(pAttacker, nullptr);
	ASSERT_NE(pTeammate, nullptr);
	ASSERT_NE(pEnemy, nullptr);
	CCharacter *pAttackerCharacter = pAttacker->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pTeammateCharacter = pTeammate->ForceSpawn(vec2(96.0f, 96.0f));
	CCharacter *pEnemyCharacter = pEnemy->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pAttackerCharacter, nullptr);
	ASSERT_NE(pTeammateCharacter, nullptr);
	ASSERT_NE(pEnemyCharacter, nullptr);

	EXPECT_FALSE(pAttackerCharacter->GetWeaponGot(WEAPON_HAMMER));
	EXPECT_FALSE(pAttackerCharacter->GetWeaponGot(WEAPON_GUN));
	EXPECT_TRUE(pAttackerCharacter->GetWeaponGot(WEAPON_LASER));
	EXPECT_EQ(pAttackerCharacter->GetWeaponAmmo(WEAPON_LASER), -1);

	g_Config.m_SvTeamdamage = 0;
	pAttackerCharacter->TakeDamage(vec2(), 0, AttackerId, WEAPON_LASER);
	pTeammateCharacter->TakeDamage(vec2(2.0f, 0.0f), 0, AttackerId, WEAPON_LASER);
	pEnemyCharacter->SetArmor(10);
	pEnemyCharacter->TakeDamage(vec2(), 0, AttackerId, WEAPON_LASER);

	EXPECT_TRUE(pAttackerCharacter->IsAlive());
	EXPECT_TRUE(pTeammateCharacter->IsAlive());
	EXPECT_EQ(pTeammateCharacter->GetHealth(), 10);
	EXPECT_GT(pTeammateCharacter->GetCore().m_Vel.x, 0.0f);
	EXPECT_FALSE(pEnemyCharacter->IsAlive());
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pAttacker), 1);
	const auto *pTDM = dynamic_cast<CGameControllerVanillaTDM *>(GameController());
	ASSERT_NE(pTDM, nullptr);
	EXPECT_EQ(pTDM->TeamScore(TEAM_RED), 1);
	EXPECT_FALSE(GameController()->OnEntity({ENTITY_HEALTH_1, 1, 1, LAYER_GAME, 0, true, 0}));
}

TEST_F(GameWorld, InstagibCTFReusesFlagLifecycle)
{
	SelectGameMode("ictf");

	EXPECT_FALSE(GameController()->OnEntity({ENTITY_HEALTH_1, 1, 1, LAYER_GAME, 0, true, 0}));
	EXPECT_TRUE(GameController()->OnEntity({ENTITY_FLAGSTAND_RED, 1, 1, LAYER_GAME, 0, true, 0}));
	EXPECT_TRUE(GameController()->OnEntity({ENTITY_FLAGSTAND_BLUE, 2, 1, LAYER_GAME, 0, true, 0}));
	const auto *pCTF = dynamic_cast<CGameControllerVanillaCTF *>(GameController());
	ASSERT_NE(pCTF, nullptr);
	ASSERT_NE(pCTF->Flag(TEAM_RED), nullptr);
	ASSERT_NE(pCTF->Flag(TEAM_BLUE), nullptr);

	constexpr int AttackerId = 0;
	constexpr int VictimId = 1;
	CPlayer *pAttacker = GameServer()->CreatePlayer(AttackerId, TEAM_RED, false, -1);
	CPlayer *pVictim = GameServer()->CreatePlayer(VictimId, TEAM_BLUE, false, -1);
	ASSERT_NE(pAttacker, nullptr);
	ASSERT_NE(pVictim, nullptr);
	CCharacter *pAttackerCharacter = pAttacker->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pVictimCharacter = pVictim->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pAttackerCharacter, nullptr);
	ASSERT_NE(pVictimCharacter, nullptr);
	EXPECT_TRUE(pVictimCharacter->GetWeaponGot(WEAPON_LASER));
	EXPECT_FALSE(pVictimCharacter->GetWeaponGot(WEAPON_GUN));

	pCTF->Flag(TEAM_RED)->Grab(pVictimCharacter);
	ASSERT_EQ(pCTF->Flag(TEAM_RED)->Carrier(), pVictimCharacter);
	pVictimCharacter->SetArmor(10);
	pVictimCharacter->TakeDamage(vec2(), 0, AttackerId, WEAPON_LASER);

	EXPECT_FALSE(pVictimCharacter->IsAlive());
	EXPECT_EQ(pCTF->Flag(TEAM_RED)->Carrier(), nullptr);
	EXPECT_FALSE(pCTF->Flag(TEAM_RED)->IsAtStand());
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pAttacker), 2);
}

TEST_F(GameWorld, GrenadeInstagibDMVerticalSlice)
{
	SelectGameMode("gdm");

	constexpr int AttackerId = 0;
	constexpr int VictimId = 1;
	CPlayer *pAttacker = GameServer()->CreatePlayer(AttackerId, TEAM_GAME, false, -1);
	CPlayer *pVictim = GameServer()->CreatePlayer(VictimId, TEAM_GAME, false, -1);
	ASSERT_NE(pAttacker, nullptr);
	ASSERT_NE(pVictim, nullptr);
	CCharacter *pAttackerCharacter = pAttacker->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pVictimCharacter = pVictim->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pAttackerCharacter, nullptr);
	ASSERT_NE(pVictimCharacter, nullptr);

	EXPECT_FALSE(pAttackerCharacter->GetWeaponGot(WEAPON_HAMMER));
	EXPECT_FALSE(pAttackerCharacter->GetWeaponGot(WEAPON_GUN));
	EXPECT_TRUE(pAttackerCharacter->GetWeaponGot(WEAPON_GRENADE));
	EXPECT_EQ(pAttackerCharacter->GetWeaponAmmo(WEAPON_GRENADE), -1);
	EXPECT_EQ(pAttackerCharacter->GetActiveWeapon(), WEAPON_GRENADE);

	pAttackerCharacter->TakeDamage(vec2(2.0f, 0.0f), 5, AttackerId, WEAPON_GRENADE);
	EXPECT_TRUE(pAttackerCharacter->IsAlive());
	EXPECT_EQ(pAttackerCharacter->GetHealth(), 10);
	EXPECT_GT(pAttackerCharacter->GetCore().m_Vel.x, 0.0f);

	pVictimCharacter->TakeDamage(vec2(2.0f, 0.0f), 3, AttackerId, WEAPON_GRENADE);
	EXPECT_TRUE(pVictimCharacter->IsAlive());
	EXPECT_EQ(pVictimCharacter->GetHealth(), 10);
	EXPECT_GT(pVictimCharacter->GetCore().m_Vel.x, 0.0f);

	pVictimCharacter->SetArmor(10);
	pVictimCharacter->TakeDamage(vec2(), 4, AttackerId, WEAPON_GRENADE);
	EXPECT_FALSE(pVictimCharacter->IsAlive());
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pAttacker), 1);

	EXPECT_TRUE(GameController()->OnEntity({ENTITY_SPAWN, 1, 1, LAYER_GAME, 0, true, 0}));
	EXPECT_FALSE(GameController()->OnEntity({ENTITY_HEALTH_1, 1, 1, LAYER_GAME, 0, true, 0}));
	EXPECT_FALSE(GameController()->OnEntity({ENTITY_WEAPON_GRENADE, 1, 1, LAYER_GAME, 0, true, 0}));
}

TEST_F(GameWorld, PvPSpawnProtectionGuardsBothSides)
{
	g_Config.m_SvRespawnProtectionMs = 1000;
	SelectGameMode("idm");
	CCharacter *pAttacker = SpawnPlayer(0, vec2(64.0f, 96.0f));
	CCharacter *pVictim = SpawnPlayer(1, vec2(128.0f, 96.0f));
	ASSERT_NE(pAttacker, nullptr);
	ASSERT_NE(pVictim, nullptr);
	pVictim->TakeDamage(vec2(), 0, 0, WEAPON_LASER);
	EXPECT_TRUE(pVictim->IsAlive());

	// a victim that has been around for a while, hit by a fresh attacker
	pVictim->m_SpawnTick -= 2 * SERVER_TICK_SPEED;
	pVictim->TakeDamage(vec2(), 0, 0, WEAPON_LASER);
	EXPECT_TRUE(pVictim->IsAlive());

	pAttacker->m_SpawnTick -= 2 * SERVER_TICK_SPEED;
	pVictim->TakeDamage(vec2(), 0, 0, WEAPON_LASER);
	EXPECT_FALSE(pVictim->IsAlive());
}

TEST_F(GameWorld, PvPShotHeldDownSinceTheSpawnWaits)
{
	SelectGameMode("idm");
	CCharacter *pCharacter = SpawnPlayer(0, vec2(64.0f, 96.0f));
	ASSERT_NE(pCharacter, nullptr);
	CTuningParams Tuning;
	CWeaponFireContext Held = {pCharacter, WEAPON_LASER, vec2(1, 0), vec2(1, 0), pCharacter->m_Pos, &Tuning, false};
	EXPECT_FALSE(GameController()->OnCharacterFireWeapon(Held).m_Fired);
	CWeaponFireContext Clicked = Held;
	Clicked.m_Pressed = true;
	EXPECT_TRUE(GameController()->OnCharacterFireWeapon(Clicked).m_Fired);
	pCharacter->m_SpawnTick -= SERVER_TICK_SPEED;
	EXPECT_TRUE(GameController()->OnCharacterFireWeapon(Held).m_Fired);
}

TEST_F(GameWorld, PvPAnticamperFreezesOrKillsWhoStays)
{
	g_Config.m_SvAnticamper = 1;
	g_Config.m_SvAnticamperTime = 5;
	g_Config.m_SvAnticamperFreeze = 7;
	SelectGameMode("idm");
	CCharacter *pCamper = SpawnPlayer(0, vec2(64.0f, 96.0f));
	ASSERT_NE(pCamper, nullptr);
	for(int Tick = 0; Tick <= 5 * SERVER_TICK_SPEED; Tick++)
	{
		m_pServer->AdvanceTick(1);
		GameController()->Tick();
	}
	EXPECT_EQ(pCamper->m_FreezeTime, 7 * SERVER_TICK_SPEED);
	GameController()->TickCharacterPreCore(pCamper);
	EXPECT_EQ(pCamper->m_FreezeTime, 7 * SERVER_TICK_SPEED - 1);

	pCamper->Unfreeze();
	g_Config.m_SvAnticamperFreeze = 0;
	for(int Tick = 0; Tick <= 5 * SERVER_TICK_SPEED && pCamper->IsAlive(); Tick++)
	{
		m_pServer->AdvanceTick(1);
		GameController()->Tick();
	}
	EXPECT_FALSE(pCamper->IsAlive());
}

TEST_F(GameWorld, PvPKillingSpreeIsReported)
{
	g_Config.m_SvKillingspreeKills = 2;
	SelectGameMode("idm");
	CPlayer *pKiller = JoinPlayer(0, TEAM_GAME, "killer");
	CPlayer *pVictim = JoinPlayer(1, TEAM_GAME, "victim");
	ASSERT_NE(pKiller->ForceSpawn(vec2(64.0f, 96.0f)), nullptr);
	for(int Kill = 0; Kill < 3; Kill++)
	{
		CCharacter *pVictimCharacter = Respawn(pVictim, vec2(128.0f, 96.0f));
		ASSERT_NE(pVictimCharacter, nullptr);
		pVictimCharacter->m_SpawnTick -= SERVER_TICK_SPEED;
		pVictimCharacter->TakeDamage(vec2(), 0, 0, WEAPON_LASER);
		ASSERT_FALSE(pVictimCharacter->IsAlive());
	}
	pKiller->KillCharacter(WEAPON_SELF);
	ASSERT_NE(pKiller->ForceSpawn(vec2(64.0f, 96.0f)), nullptr);
	CCharacter *pVictimCharacter = Respawn(pVictim, vec2(128.0f, 96.0f));
	pVictimCharacter->TakeDamage(vec2(), 0, 0, WEAPON_LASER);
	GameController()->EndRound();
	const CReceivedMatchReport Received = ReceivedMatchReport(0);
	EXPECT_EQ(Received.Metric(Received.m_LocalParticipantId, "best_spree"), 3);
}

TEST_F(GameWorld, PvPZoomOnlyWithSvAllowZoom)
{
	SelectGameMode("idm");
	EXPECT_EQ(GameController()->GameInfoFlags(-1) & GAMEINFOFLAG_ALLOW_ZOOM, 0);
	g_Config.m_SvAllowZoom = 1;
	EXPECT_NE(GameController()->GameInfoFlags(-1) & GAMEINFOFLAG_ALLOW_ZOOM, 0);
}

// the events of the tick a demo would get
static int SnappedEvents(GameWorld *pWorld, int Type)
{
	pWorld->m_pServer->m_SnapshotBuilder.Init(false);
	pWorld->GameServer()->m_Events.Snap(SERVER_DEMO_CLIENT);
	CSnapshotBuffer Buffer;
	pWorld->m_pServer->m_SnapshotBuilder.Finish(&Buffer);
	int Count = 0;
	for(int Index = 0; Index < Buffer.AsSnapshot()->NumItems(); Index++)
		Count += Buffer.AsSnapshot()->GetItemType(Index) == Type;
	return Count;
}

TEST_F(GameWorld, InstagibKillsWithoutDamageIndicators)
{
	SelectGameMode("idm");
	CCharacter *pAttacker = SpawnPlayer(0, vec2(64.0f, 96.0f));
	CCharacter *pVictim = SpawnPlayer(1, vec2(128.0f, 96.0f));
	ASSERT_NE(pAttacker, nullptr);
	ASSERT_NE(pVictim, nullptr);
	GameServer()->m_Events.Clear();
	pVictim->TakeDamage(vec2(), 0, 0, WEAPON_LASER);
	EXPECT_FALSE(pVictim->IsAlive());
	EXPECT_EQ(SnappedEvents(this, NETEVENTTYPE_DAMAGEIND), 0);
	EXPECT_EQ(SnappedEvents(this, NETEVENTTYPE_DEATH), 1);
}

TEST_F(GameWorld, InstagibOnlyWallshotsKill)
{
	g_Config.m_SvOnlyWallshotKills = 1;
	SelectGameMode("idm");
	CPlayer *pAttacker = JoinPlayer(0, TEAM_GAME, "attacker");
	ASSERT_NE(pAttacker->ForceSpawn(vec2(64.0f, 96.0f)), nullptr);
	CCharacter *pVictim = SpawnPlayer(1, vec2(128.0f, 96.0f));
	ASSERT_NE(pVictim, nullptr);
	CGameDamageContext Hit;
	Hit.m_From = 0;
	Hit.m_Weapon = WEAPON_LASER;
	pVictim->TakeDamage(Hit);
	EXPECT_TRUE(pVictim->IsAlive());
	Hit.m_Bounces = 1;
	pVictim->TakeDamage(Hit);
	EXPECT_FALSE(pVictim->IsAlive());
	GameController()->EndRound();
	const CReceivedMatchReport Received = ReceivedMatchReport(0);
	EXPECT_EQ(Received.Metric(Received.m_LocalParticipantId, "wallshots"), 1);
}

TEST_F(GameWorld, InstagibGrenadeNeedsDamageToKill)
{
	g_Config.m_SvDamageNeededForKill = 2;
	SelectGameMode("gdm");
	CCharacter *pVictim = SpawnPlayer(1, vec2(128.0f, 96.0f));
	ASSERT_NE(SpawnPlayer(0, vec2(64.0f, 96.0f)), nullptr);
	ASSERT_NE(pVictim, nullptr);
	pVictim->TakeDamage(vec2(1.0f, 0.0f), 1, 0, WEAPON_GRENADE);
	EXPECT_TRUE(pVictim->IsAlive());
	EXPECT_GT(pVictim->GetCore().m_Vel.x, 0.0f);
	pVictim->TakeDamage(vec2(), 2, 0, WEAPON_GRENADE);
	EXPECT_FALSE(pVictim->IsAlive());
}

TEST_F(GameWorld, InstagibGrenadesAsOnGctfServers)
{
	g_Config.m_SvGrenadeAmmoRegen = 1;
	g_Config.m_SvGrenadeAmmoRegenNum = 4;
	g_Config.m_SvGrenadeAmmoRegenTime = 1000;
	SelectGameMode("gctf");
	CCharacter *pShooter = SpawnPlayer(0, vec2(64.0f, 96.0f), TEAM_RED);
	CCharacter *pVictim = SpawnPlayer(1, vec2(128.0f, 96.0f), TEAM_BLUE);
	ASSERT_NE(pShooter, nullptr);
	ASSERT_NE(pVictim, nullptr);
	EXPECT_EQ(pShooter->GetWeaponAmmo(WEAPON_GRENADE), 4);

	// a grenade jump gives the grenade back
	pShooter->SetWeaponAmmo(WEAPON_GRENADE, 1);
	pShooter->TakeDamage(vec2(0.0f, -1.0f), 6, 0, WEAPON_GRENADE);
	EXPECT_EQ(pShooter->GetWeaponAmmo(WEAPON_GRENADE), 2);

	// a grenade comes back after the reload and a second
	pShooter->m_SpawnTick -= SERVER_TICK_SPEED;
	CTuningParams Tuning;
	CWeaponFireContext Fire = {pShooter, WEAPON_GRENADE, vec2(1, 0), vec2(1, 0), pShooter->m_Pos, &Tuning, true};
	ASSERT_TRUE(GameController()->OnCharacterFireWeapon(Fire).m_Fired);
	const int ReloadTicks = (int)(Tuning.GetWeaponFireDelay(WEAPON_GRENADE) * (float)SERVER_TICK_SPEED);
	for(int Tick = 0; Tick < ReloadTicks + 1 + SERVER_TICK_SPEED; Tick++)
		GameController()->TickCharacterPostCore(pShooter);
	EXPECT_EQ(pShooter->GetWeaponAmmo(WEAPON_GRENADE), 2);
	GameController()->TickCharacterPostCore(pShooter);
	EXPECT_EQ(pShooter->GetWeaponAmmo(WEAPON_GRENADE), 3);

	// a hit fills them up
	pVictim->m_SpawnTick -= SERVER_TICK_SPEED;
	pVictim->TakeDamage(vec2(), 6, 0, WEAPON_GRENADE);
	EXPECT_FALSE(pVictim->IsAlive());
	EXPECT_EQ(pShooter->GetWeaponAmmo(WEAPON_GRENADE), 4);
}

TEST_F(GameWorld, InstagibSprayProtection)
{
	g_Config.m_SvSprayprotection = 1;
	SelectGameMode("gdm");
	ASSERT_NE(SpawnPlayer(0, vec2(64.0f, 96.0f)), nullptr);
	ASSERT_NE(SpawnPlayer(1, vec2(128.0f, 96.0f)), nullptr);
	ASSERT_NE(SpawnPlayer(2, vec2(64.0f + 4000.0f, 96.0f)), nullptr);
	auto *pGrenade = new CProjectile(&GameServer()->m_World, WEAPON_GRENADE, 0, vec2(64.0f, 96.0f), vec2(1, 0), 0, false, true, -1, vec2(1, 0));
	EXPECT_TRUE(pGrenade->AffectMask().test(0));
	EXPECT_TRUE(pGrenade->AffectMask().test(1));
	EXPECT_FALSE(pGrenade->AffectMask().test(2)); // NOLINT(clang-analyzer-unix.Malloc)
	g_Config.m_SvSprayprotection = 0;
	auto *pFree = new CProjectile(&GameServer()->m_World, WEAPON_GRENADE, 0, vec2(64.0f, 96.0f), vec2(1, 0), 0, false, true, -1, vec2(1, 0));
	EXPECT_TRUE(pFree->AffectMask().all()); // NOLINT(clang-analyzer-unix.Malloc)
}

TEST_F(GameWorld, InstagibHudShowsAmmoButNoHealth)
{
	SelectGameMode("gctf");
	EXPECT_NE(GameController()->GameInfoFlags(-1) & GAMEINFOFLAG_UNLIMITED_AMMO, 0);
	EXPECT_EQ(GameController()->GameInfoFlags2(-1) & GAMEINFOFLAG2_HUD_HEALTH_ARMOR, 0);
	EXPECT_NE(GameController()->GameInfoFlags2(-1) & GAMEINFOFLAG2_HUD_AMMO, 0);
}

namespace
{
	// a client that only watches, to see what the game tells the clients
	constexpr int WATCHER_ID = MAX_CLIENTS - 1;

	protocol7::CNetObj_GameData SnapGameData7(GameWorld *pWorld)
	{
		pWorld->m_pServer->m_aClients[WATCHER_ID].m_Sixup = true;
		pWorld->m_pServer->m_SnapshotBuilder.Init(true);
		pWorld->GameController()->Snap(WATCHER_ID);
		CSnapshotBuffer Buffer;
		pWorld->m_pServer->m_SnapshotBuilder.Finish(&Buffer);
		pWorld->m_pServer->m_aClients[WATCHER_ID].m_Sixup = false;
		const auto *pGameData = static_cast<const protocol7::CNetObj_GameData *>(Buffer.AsSnapshot()->FindItem(protocol7::NETOBJTYPE_GAMEDATA, 0));
		EXPECT_NE(pGameData, nullptr);
		return pGameData ? *pGameData : protocol7::CNetObj_GameData{};
	}

	CNetObj_GameInfo SnapGameInfo6(GameWorld *pWorld)
	{
		pWorld->m_pServer->m_SnapshotBuilder.Init(false);
		pWorld->GameController()->Snap(WATCHER_ID);
		CSnapshotBuffer Buffer;
		pWorld->m_pServer->m_SnapshotBuilder.Finish(&Buffer);
		const auto *pGameInfo = static_cast<const CNetObj_GameInfo *>(Buffer.AsSnapshot()->FindItem(NETOBJTYPE_GAMEINFO, 0));
		EXPECT_NE(pGameInfo, nullptr);
		return pGameInfo ? *pGameInfo : CNetObj_GameInfo{};
	}

	std::optional<int64_t> MatchMetric(const CMatchReport &Report, const char *pMetricId)
	{
		for(const CMatchMetric &Metric : Report.m_vMetrics)
		{
			if(Metric.m_SubjectKind == EMatchSubjectKind::MATCH && Metric.m_MetricId == pMetricId)
				return Metric.m_Value;
		}
		return std::nullopt;
	}

	// the test map has no spawn point a vanilla tee fits in, these are free and safe
	void AddSpawnPoints(GameWorld *pWorld, int Type = ENTITY_SPAWN)
	{
		for(int x = 8; x < 12; x++)
			EXPECT_TRUE(pWorld->GameController()->OnEntity({Type, x, 2, LAYER_GAME, 0, true, 0}));
	}

	// the whole game ticks, the world as well, so that rounds are reset and players spawn
	void RunTicks(GameWorld *pWorld, int Ticks)
	{
		for(int i = 0; i < Ticks; i++)
		{
			pWorld->m_pServer->AdvanceTick(1);
			pWorld->GameServer()->OnTick();
		}
	}

	// from the countdown of a round to everybody playing it
	void RunCountdown(GameWorld *pWorld)
	{
		const int TickSpeed = pWorld->m_pServer->TickSpeed();
		EXPECT_NE(SnapGameData7(pWorld).m_GameStateFlags & protocol7::GAMESTATEFLAG_STARTCOUNTDOWN, 0);
		RunTicks(pWorld, 3 * TickSpeed + 2);
		EXPECT_EQ(SnapGameData7(pWorld).m_GameStateFlags, 0);
	}
}

namespace
{
	class CDeadSpectatorsController : public CGameControllerDeadSpectators<CGameControllerVanillaDM>
	{
	public:
		using CGameControllerDeadSpectators::CGameControllerDeadSpectators;
		using CGameControllerDeadSpectators::SetRespawnLocked;
		// only player 2 may be watched, if at all
		bool m_OnlyTwo = false;
		bool CanDeadSpectatorFollow(int ClientId, int TargetId) const override
		{
			if(m_OnlyTwo)
				return TargetId == 2 && IsAlive(2);
			return CGameControllerDeadSpectators::CanDeadSpectatorFollow(ClientId, TargetId);
		}
	};
}

TEST_F(GameWorld, DeadSpectatorsWaitUntilTheModeLetsThemBack)
{
	CDeadSpectatorsController &Controller = SelectController<CDeadSpectatorsController>("dm");
	CPlayer *pDead = JoinPlayer(0, TEAM_GAME, "dead");
	JoinPlayer(1, TEAM_GAME, "first");
	JoinPlayer(2, TEAM_GAME, "second");
	for(int ClientId = 0; ClientId < 3; ClientId++)
		ASSERT_NE(GameServer()->m_apPlayers[ClientId]->ForceSpawn(vec2(64.0f + 32.0f * ClientId, 96.0f)), nullptr);

	Controller.SetRespawnLocked(0, true);
	EXPECT_FALSE(Controller.IsPlayerDeadSpectator(0));
	pDead->KillCharacter(WEAPON_WORLD);
	EXPECT_TRUE(Controller.IsPlayerDeadSpectator(0));
	EXPECT_EQ(Controller.PlayerAutoRespawnTick(pDead), std::numeric_limits<int>::max());
	vec2 SpawnPos;
	EXPECT_FALSE(Controller.CanSpawn(TEAM_GAME, &SpawnPos, 0));
	const int LastActionTick = pDead->m_LastActionTick;
	Controller.Tick();
	EXPECT_EQ(pDead->SpectatorId(), 1);
	EXPECT_EQ(pDead->m_LastActionTick, LastActionTick + 1);
	EXPECT_FALSE(Controller.CanPlayerSpectate(0, 0));
	EXPECT_TRUE(Controller.CanPlayerSpectate(0, 2));

	Controller.m_OnlyTwo = true;
	Controller.Tick();
	EXPECT_EQ(pDead->SpectatorId(), 2);
	EXPECT_FALSE(Controller.CanPlayerSpectate(0, 1));

	Controller.SetRespawnLocked(0, false);
	EXPECT_FALSE(Controller.IsPlayerDeadSpectator(0));
	EXPECT_NE(Controller.PlayerAutoRespawnTick(pDead), std::numeric_limits<int>::max());
}

TEST_F(GameWorld, LMSWaitsForASecondPlayer)
{
	SelectGameMode("lms");
	AddSpawnPoints(this);
	CPlayer *pFirst = JoinPlayer(0, TEAM_GAME, "first");
	RunTicks(this, 10);

	// alone there is nothing to survive, so the player warms up and respawns
	const protocol7::CNetObj_GameData Waiting = SnapGameData7(this);
	EXPECT_EQ(Waiting.m_GameStateFlags, protocol7::GAMESTATEFLAG_WARMUP);
	EXPECT_EQ(Waiting.m_GameStateEndTick, 0);
	EXPECT_EQ(SnapGameInfo6(this).m_WarmupTimer, 0);
	EXPECT_FALSE(GameController()->IsGamePaused());
	ASSERT_NE(pFirst->GetCharacter(), nullptr);
	pFirst->GetCharacter()->Die(0, WEAPON_SELF);
	EXPECT_NE(GameController()->PlayerAutoRespawnTick(pFirst), std::numeric_limits<int>::max());
	EXPECT_FALSE(GameController()->IsPlayerDeadSpectator(0));

	// the second one starts the match with a countdown in a paused world
	JoinPlayer(1, TEAM_GAME, "second");
	EXPECT_TRUE(GameController()->IsGamePaused());
	const protocol7::CNetObj_GameData Countdown = SnapGameData7(this);
	EXPECT_EQ(Countdown.m_GameStateFlags, protocol7::GAMESTATEFLAG_STARTCOUNTDOWN | protocol7::GAMESTATEFLAG_PAUSED);
	EXPECT_EQ(Countdown.m_GameStateEndTick, m_pServer->Tick() + 3 * m_pServer->TickSpeed());
	EXPECT_NE(SnapGameInfo6(this).m_GameStateFlags & GAMESTATEFLAG_PAUSED, 0);
	EXPECT_TRUE(GameController()->IsTeamChangeAllowed());
	RunCountdown(this);
	EXPECT_FALSE(GameController()->IsGamePaused());
	// the clock stood still during the countdown, the last tick of it starts the round
	EXPECT_EQ(SnapGameData7(this).m_GameStartTick, Countdown.m_GameStateEndTick - 1);
	EXPECT_NE(pFirst->GetCharacter(), nullptr);
	EXPECT_NE(GameServer()->m_apPlayers[1]->GetCharacter(), nullptr);
}

TEST_F(GameWorld, LMSStartsEverybodyWithEveryWeaponAndHasNoPickups)
{
	SelectGameMode("lms");
	CCharacter *pCharacter = SpawnPlayer(0, vec2(64.0f, 96.0f));
	ASSERT_NE(pCharacter, nullptr);
	EXPECT_EQ(pCharacter->GetHealth(), 10);
	EXPECT_EQ(pCharacter->GetArmor(), 0);
	EXPECT_TRUE(pCharacter->GetWeaponGot(WEAPON_HAMMER));
	EXPECT_EQ(pCharacter->GetWeaponAmmo(WEAPON_GUN), 10);
	EXPECT_EQ(pCharacter->GetWeaponAmmo(WEAPON_SHOTGUN), 10);
	EXPECT_EQ(pCharacter->GetWeaponAmmo(WEAPON_GRENADE), 10);
	EXPECT_EQ(pCharacter->GetWeaponAmmo(WEAPON_LASER), 5);
	EXPECT_TRUE(pCharacter->GetWeaponGot(WEAPON_LASER));
	EXPECT_FALSE(pCharacter->GetWeaponGot(WEAPON_NINJA));
	EXPECT_EQ(pCharacter->GetActiveWeapon(), WEAPON_GUN);

	EXPECT_TRUE(GameController()->OnEntity({ENTITY_SPAWN, 1, 1, LAYER_GAME, 0, true, 0}));
	for(const int Index : {ENTITY_ARMOR_1, ENTITY_HEALTH_1, ENTITY_WEAPON_SHOTGUN, ENTITY_WEAPON_GRENADE, ENTITY_WEAPON_LASER, ENTITY_POWERUP_NINJA})
		EXPECT_FALSE(GameController()->OnEntity({Index, 1, 1, LAYER_GAME, 0, true, 0})) << Index;
}

TEST_F(GameWorld, LMSLastPlayerStandingWinsTheRound)
{
	g_Config.m_SvScorelimit = 10;
	SelectGameMode("lms");
	AddSpawnPoints(this);
	CPlayer *pWinner = JoinPlayer(0, TEAM_GAME, "winner");
	CPlayer *pFirst = JoinPlayer(1, TEAM_GAME, "first");
	CPlayer *pSecond = JoinPlayer(2, TEAM_GAME, "second");
	RunCountdown(this);
	ASSERT_NE(pWinner->GetCharacter(), nullptr);
	ASSERT_NE(pFirst->GetCharacter(), nullptr);
	ASSERT_NE(pSecond->GetCharacter(), nullptr);

	// who dies watches somebody still alive and may not look elsewhere
	pFirst->GetCharacter()->Die(0, WEAPON_GUN);
	RunTicks(this, 1);
	EXPECT_TRUE(GameController()->IsPlayerDeadSpectator(1));
	EXPECT_EQ(GameController()->PlayerAutoRespawnTick(pFirst), std::numeric_limits<int>::max());
	EXPECT_TRUE(pFirst->SpectatorId() == 0 || pFirst->SpectatorId() == 2) << pFirst->SpectatorId();
	EXPECT_TRUE(GameController()->CanPlayerSpectate(1, 2));
	EXPECT_FALSE(GameController()->CanPlayerSpectate(1, SPEC_FREEVIEW));
	EXPECT_FALSE(GameController()->CanPlayerSpectate(1, 1));
	EXPECT_TRUE(GameController()->CanPlayerSpectate(0, SPEC_FREEVIEW));
	EXPECT_FALSE(GameController()->IsGamePaused());

	// once the one watched dies too, the dead watch the last one
	pSecond->GetCharacter()->Die(0, WEAPON_GRENADE);
	RunTicks(this, 1);
	EXPECT_EQ(pFirst->SpectatorId(), 0);
	EXPECT_EQ(pSecond->SpectatorId(), 0);
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pWinner), 3);
	EXPECT_TRUE(GameController()->IsGamePaused());
	EXPECT_FALSE(GameController()->IsTeamChangeAllowed());
	const protocol7::CNetObj_GameData RoundOver = SnapGameData7(this);
	EXPECT_EQ(RoundOver.m_GameStateFlags, protocol7::GAMESTATEFLAG_ROUNDOVER);
	EXPECT_EQ(RoundOver.m_GameStateEndTick, m_pServer->Tick() - RoundOver.m_GameStartTick);
	// a 0.6 client sees a paused game, the scores keep going into the next round
	EXPECT_EQ(SnapGameInfo6(this).m_GameStateFlags, GAMESTATEFLAG_PAUSED);
	EXPECT_TRUE(ReceivedMatchReports(0).empty());

	RunTicks(this, 5 * m_pServer->TickSpeed());
	EXPECT_FALSE(GameController()->IsPlayerDeadSpectator(1));
	EXPECT_FALSE(GameController()->IsPlayerDeadSpectator(2));
	RunCountdown(this);
	EXPECT_NE(pFirst->GetCharacter(), nullptr);
	EXPECT_NE(pSecond->GetCharacter(), nullptr);
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pWinner), 3);
}

TEST_F(GameWorld, LMSScoreLimitEndsTheMatchWhenARoundEnds)
{
	g_Config.m_SvScorelimit = 2;
	SelectGameMode("lms");
	AddSpawnPoints(this);
	CPlayer *pWinner = JoinPlayer(0, TEAM_GAME, "winner");
	CPlayer *pLoser = JoinPlayer(1, TEAM_GAME, "loser");
	RunCountdown(this);

	// the round is the loser's own fault, the winner scores for being left
	pLoser->GetCharacter()->Die(1, WEAPON_SELF);
	RunTicks(this, 1);
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pWinner), 1);
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pLoser), -1);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, protocol7::GAMESTATEFLAG_ROUNDOVER);

	RunTicks(this, 5 * m_pServer->TickSpeed());
	RunCountdown(this);
	pLoser->GetCharacter()->Die(0, WEAPON_LASER);
	RunTicks(this, 1);
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pWinner), 3);
	EXPECT_NE(SnapGameData7(this).m_GameStateFlags & protocol7::GAMESTATEFLAG_GAMEOVER, 0);
	EXPECT_NE(SnapGameInfo6(this).m_GameStateFlags & GAMESTATEFLAG_GAMEOVER, 0);
	// the final scores show everybody playing
	EXPECT_FALSE(GameController()->IsPlayerDeadSpectator(1));

	const CReceivedMatchReport Received = ReceivedMatchReport(0);
	const CMatchReport &Report = Received.m_Report;
	EXPECT_EQ(Report.m_ModeId, "lms");
	EXPECT_EQ(Report.m_Termination, EMatchTermination::COMPLETED);
	EXPECT_EQ(MatchMetric(Report, "rounds"), 2);
	const int WinnerId = Received.Participant("winner")->m_ParticipantId;
	const int LoserId = Received.Participant("loser")->m_ParticipantId;
	EXPECT_EQ(Received.Metric(WinnerId, "rounds_won"), 2);
	EXPECT_EQ(Received.Metric(WinnerId, "rounds_played"), 2);
	EXPECT_EQ(Received.Metric(WinnerId, "score"), 3);
	EXPECT_EQ(Received.Metric(LoserId, "rounds_played"), 2);
	EXPECT_FALSE(Received.Metric(LoserId, "rounds_won").has_value());
	ASSERT_NE(Report.Standing(EMatchSubjectKind::PARTICIPANT, WinnerId), nullptr);
	EXPECT_EQ(Report.Standing(EMatchSubjectKind::PARTICIPANT, WinnerId)->m_Outcome, EMatchOutcome::WIN);
	EXPECT_EQ(Report.Standing(EMatchSubjectKind::PARTICIPANT, LoserId)->m_Outcome, EMatchOutcome::LOSS);

	// the next match starts with a countdown
	RunTicks(this, 10 * m_pServer->TickSpeed() + 1);
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pWinner), 0);
	RunCountdown(this);
}

TEST_F(GameWorld, LMSTimeLimitScoresEverybodyStillStanding)
{
	g_Config.m_SvScorelimit = 5;
	g_Config.m_SvTimelimit = 1;
	SelectGameMode("lms");
	AddSpawnPoints(this);
	CPlayer *pFirst = JoinPlayer(0, TEAM_GAME, "first");
	CPlayer *pSecond = JoinPlayer(1, TEAM_GAME, "second");
	CPlayer *pThird = JoinPlayer(2, TEAM_GAME, "third");
	RunCountdown(this);
	pThird->GetCharacter()->Die(0, WEAPON_GUN);
	RunTicks(this, 60 * m_pServer->TickSpeed());
	// the first one leads alone, which ends the match with the round
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pFirst), 2);
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pSecond), 1);
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pThird), 0);
	EXPECT_NE(SnapGameData7(this).m_GameStateFlags & protocol7::GAMESTATEFLAG_GAMEOVER, 0);
}

TEST_F(GameWorld, LMSLateJoinerWatchesUntilTheNextRound)
{
	SelectGameMode("lms");
	AddSpawnPoints(this);
	JoinPlayer(0, TEAM_GAME, "first");
	JoinPlayer(1, TEAM_GAME, "second");
	RunCountdown(this);

	CPlayer *pLate = JoinPlayer(2, TEAM_GAME, "late");
	RunTicks(this, 2);
	EXPECT_TRUE(GameController()->IsPlayerDeadSpectator(2));
	EXPECT_EQ(pLate->GetCharacter(), nullptr);
	EXPECT_TRUE(pLate->SpectatorId() == 0 || pLate->SpectatorId() == 1) << pLate->SpectatorId();

	CPlayer *pSpectator = JoinPlayer(3, TEAM_SPECTATORS, "spectator");
	EXPECT_FALSE(GameController()->IsPlayerDeadSpectator(3));
	EXPECT_TRUE(GameController()->CanPlayerSpectate(3, SPEC_FREEVIEW));
	// joining the game in the middle of a round is the same as joining late
	GameController()->DoTeamChange(pSpectator, TEAM_GAME, false);
	EXPECT_TRUE(GameController()->IsPlayerDeadSpectator(3));
}

TEST_F(GameWorld, LMSMatchWithoutEnoughPlayersEndsWithoutResult)
{
	SelectGameMode("lms");
	AddSpawnPoints(this);
	JoinPlayer(0, TEAM_GAME, "stays");
	JoinPlayer(1, TEAM_GAME, "leaves");
	RunCountdown(this);

	LeavePlayer(1);
	RunTicks(this, 1);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, protocol7::GAMESTATEFLAG_ROUNDOVER);
	RunTicks(this, 5 * m_pServer->TickSpeed());
	const protocol7::CNetObj_GameData Waiting = SnapGameData7(this);
	EXPECT_EQ(Waiting.m_GameStateFlags, protocol7::GAMESTATEFLAG_WARMUP);
	EXPECT_FALSE(GameController()->IsGamePaused());
	const CReceivedMatchReport Received = ReceivedMatchReport(0);
	EXPECT_EQ(Received.m_Report.m_Termination, EMatchTermination::ABORTED);
	EXPECT_EQ(MatchMetric(Received.m_Report, "rounds"), 1);

	// somebody else comes, and a new match begins
	JoinPlayer(1, TEAM_GAME, "comes");
	EXPECT_NE(SnapGameData7(this).m_GameStateFlags & protocol7::GAMESTATEFLAG_STARTCOUNTDOWN, 0);
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, GameServer()->m_apPlayers[0]), 0);
}

TEST_F(GameWorld, LTSLastTeamStandingWinsTheRound)
{
	g_Config.m_SvScorelimit = 2;
	g_Config.m_SvTimelimit = 1;
	SelectGameMode("lts");
	AddSpawnPoints(this);
	JoinPlayer(0, TEAM_RED, "red");
	CPlayer *pRedMate = JoinPlayer(1, TEAM_RED, "redmate");
	CPlayer *pBlue = JoinPlayer(2, TEAM_BLUE, "blue");
	RunCountdown(this);
	ASSERT_NE(pRedMate->GetCharacter(), nullptr);
	ASSERT_NE(pBlue->GetCharacter(), nullptr);

	// the dead watch their own team only
	pRedMate->GetCharacter()->Die(2, WEAPON_SHOTGUN);
	RunTicks(this, 1);
	EXPECT_TRUE(GameController()->IsPlayerDeadSpectator(1));
	EXPECT_EQ(pRedMate->SpectatorId(), 0);
	EXPECT_FALSE(GameController()->CanPlayerSpectate(1, 2));
	EXPECT_TRUE(GameController()->CanPlayerSpectate(1, 0));
	// a kill scores for the player and not for the team
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pBlue), 1);
	EXPECT_EQ(GameController()->TeamScore(TEAM_BLUE), 0);

	pBlue->GetCharacter()->Die(0, WEAPON_LASER);
	RunTicks(this, 1);
	EXPECT_EQ(GameController()->TeamScore(TEAM_RED), 1);
	EXPECT_EQ(GameController()->TeamScore(TEAM_BLUE), 0);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, protocol7::GAMESTATEFLAG_ROUNDOVER);

	// a round that runs out of time is a draw, and the time limit ends the match with it
	RunTicks(this, 5 * m_pServer->TickSpeed());
	RunCountdown(this);
	RunTicks(this, 60 * m_pServer->TickSpeed());
	EXPECT_EQ(GameController()->TeamScore(TEAM_RED), 2);
	EXPECT_EQ(GameController()->TeamScore(TEAM_BLUE), 1);
	EXPECT_NE(SnapGameData7(this).m_GameStateFlags & protocol7::GAMESTATEFLAG_GAMEOVER, 0);

	const CReceivedMatchReport Received = ReceivedMatchReport(2);
	const CMatchReport &Report = Received.m_Report;
	EXPECT_EQ(Report.m_ModeId, "lts");
	EXPECT_EQ(MatchMetric(Report, "rounds"), 2);
	EXPECT_EQ(Report.Metric(EMatchSubjectKind::TEAM, TEAM_RED, "score"), 2);
	EXPECT_EQ(Report.Standing(EMatchSubjectKind::TEAM, TEAM_RED)->m_Outcome, EMatchOutcome::WIN);
	EXPECT_EQ(Received.Metric(Received.Participant("redmate")->m_ParticipantId, "rounds_won"), 2);
	EXPECT_EQ(Received.Metric(Received.Participant("blue")->m_ParticipantId, "rounds_won"), 1);
	EXPECT_EQ(Received.Metric(Received.Participant("blue")->m_ParticipantId, "kills"), 1);
}

TEST_F(GameWorld, LTSBalancesTheTeamsWhenARoundBegins)
{
	g_Config.m_SvTeambalanceTime = 1;
	SelectGameMode("lts");
	AddSpawnPoints(this);
	JoinPlayer(0, TEAM_RED, "red");
	JoinPlayer(1, TEAM_BLUE, "blue");
	RunCountdown(this);
	// two more players end up in the red team, which is balanced when the next round begins
	CPlayer *pThird = JoinPlayer(2, TEAM_RED, "third");
	CPlayer *pFourth = JoinPlayer(3, TEAM_RED, "fourth");
	EXPECT_TRUE(GameController()->IsPlayerDeadSpectator(2));
	RunTicks(this, 60 * m_pServer->TickSpeed() + 1);
	// no balancing in the middle of a round
	EXPECT_EQ(pThird->GetTeam(), TEAM_RED);
	EXPECT_EQ(pFourth->GetTeam(), TEAM_RED);

	GameServer()->m_apPlayers[1]->GetCharacter()->Die(0, WEAPON_GUN);
	RunTicks(this, 1 + 5 * m_pServer->TickSpeed());
	EXPECT_NE(SnapGameData7(this).m_GameStateFlags & protocol7::GAMESTATEFLAG_STARTCOUNTDOWN, 0);
	int NumBlue = 0;
	for(const CPlayer *pPlayer : GameServer()->m_apPlayers)
		NumBlue += pPlayer && pPlayer->GetTeam() == TEAM_BLUE;
	EXPECT_EQ(NumBlue, 2);
}

TEST_F(GameWorld, VanillaPauseCommandTogglesAndEndsWithACountdown)
{
	g_Config.m_SvCountdown = 3;
	SelectGameMode("dm");
	JoinPlayer(0, TEAM_GAME, "first");
	JoinPlayer(1, TEAM_SPECTATORS, "second");
	const int TickSpeed = m_pServer->TickSpeed();

	GameServer()->Console()->ExecuteLine("pause", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_TRUE(GameController()->IsGamePaused());
	const protocol7::CNetObj_GameData Paused = SnapGameData7(this);
	EXPECT_EQ(Paused.m_GameStateFlags, protocol7::GAMESTATEFLAG_PAUSED);
	EXPECT_EQ(Paused.m_GameStateEndTick, 0);
	EXPECT_NE(SnapGameInfo6(this).m_GameStateFlags & GAMESTATEFLAG_PAUSED, 0);
	EXPECT_FALSE(GameController()->IsTeamChangeAllowed());
	// the clock stands still
	RunTicks(this, 10 * TickSpeed);
	EXPECT_TRUE(GameController()->IsGamePaused());
	EXPECT_EQ(SnapGameData7(this).m_GameStartTick, Paused.m_GameStartTick + 10 * TickSpeed);

	GameServer()->Console()->ExecuteLine("pause", IConsole::CLIENT_ID_UNSPECIFIED);
	const protocol7::CNetObj_GameData Countdown = SnapGameData7(this);
	EXPECT_EQ(Countdown.m_GameStateFlags, protocol7::GAMESTATEFLAG_STARTCOUNTDOWN | protocol7::GAMESTATEFLAG_PAUSED);
	EXPECT_EQ(Countdown.m_GameStateEndTick, m_pServer->Tick() + 3 * TickSpeed);
	// nobody joins a game that goes on
	EXPECT_FALSE(GameController()->IsTeamChangeAllowed());
	// and the countdown runs out
	GameServer()->Console()->ExecuteLine("pause", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, Countdown.m_GameStateFlags);
	RunTicks(this, 3 * TickSpeed);
	EXPECT_FALSE(GameController()->IsGamePaused());
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, 0);

	// pause_game does the same
	GameServer()->Console()->ExecuteLine("pause_game", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, protocol7::GAMESTATEFLAG_PAUSED);
	GameServer()->Console()->ExecuteLine("pause_game", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_NE(SnapGameData7(this).m_GameStateFlags & protocol7::GAMESTATEFLAG_STARTCOUNTDOWN, 0);
}

TEST_F(GameWorld, VanillaPauseCommandWithSeconds)
{
	g_Config.m_SvCountdown = -1;
	SelectGameMode("ctf");
	const int TickSpeed = m_pServer->TickSpeed();

	GameServer()->Console()->ExecuteLine("pause 2", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_EQ(SnapGameData7(this).m_GameStateEndTick, m_pServer->Tick() + 2 * TickSpeed);
	RunTicks(this, 2 * TickSpeed - 1);
	EXPECT_TRUE(GameController()->IsGamePaused());
	// without a countdown the game goes on right away
	RunTicks(this, 1);
	EXPECT_FALSE(GameController()->IsGamePaused());

	GameServer()->Console()->ExecuteLine("pause -1", IConsole::CLIENT_ID_UNSPECIFIED);
	RunTicks(this, 60 * TickSpeed);
	EXPECT_TRUE(GameController()->IsGamePaused());
	GameServer()->Console()->ExecuteLine("pause 0", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_FALSE(GameController()->IsGamePaused());

	// a match that is over is not paused
	GameController()->EndRound();
	GameServer()->Console()->ExecuteLine("pause", IConsole::CLIENT_ID_UNSPECIFIED);
	RunTicks(this, 1);
	EXPECT_NE(SnapGameData7(this).m_GameStateFlags & protocol7::GAMESTATEFLAG_GAMEOVER, 0);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags & protocol7::GAMESTATEFLAG_PAUSED, 0);
}

TEST_F(GameWorld, VanillaCountdownBeforeAMatch)
{
	SelectGameMode("tdm");
	CPlayer *pSpectator = JoinPlayer(0, TEAM_SPECTATORS, "spectator");
	const int TickSpeed = m_pServer->TickSpeed();

	// by default only the survival modes count down
	GameController()->StartRound();
	EXPECT_FALSE(GameController()->IsGamePaused());

	g_Config.m_SvCountdown = 2;
	GameController()->StartRound();
	EXPECT_TRUE(GameController()->IsGamePaused());
	const protocol7::CNetObj_GameData Countdown = SnapGameData7(this);
	EXPECT_EQ(Countdown.m_GameStateFlags, protocol7::GAMESTATEFLAG_STARTCOUNTDOWN | protocol7::GAMESTATEFLAG_PAUSED);
	EXPECT_EQ(Countdown.m_GameStateEndTick, m_pServer->Tick() + 2 * TickSpeed);
	// joining the match that is about to start is fine
	EXPECT_TRUE(GameController()->IsTeamChangeAllowed());
	GameController()->OnPlayerSetTeam(0, TEAM_RED);
	EXPECT_EQ(pSpectator->GetTeam(), TEAM_RED);
	// a countdown is not paused
	GameServer()->Console()->ExecuteLine("pause", IConsole::CLIENT_ID_UNSPECIFIED);
	RunTicks(this, 2 * TickSpeed);
	EXPECT_FALSE(GameController()->IsGamePaused());
	EXPECT_EQ(SnapGameData7(this).m_GameStartTick, Countdown.m_GameStateEndTick - 1);

	// the next match after one that ended counts down as well
	GameController()->EndRound();
	RunTicks(this, 10 * TickSpeed + 1);
	EXPECT_FALSE(SnapGameData7(this).m_GameStateFlags & protocol7::GAMESTATEFLAG_GAMEOVER);
	EXPECT_NE(SnapGameData7(this).m_GameStateFlags & protocol7::GAMESTATEFLAG_STARTCOUNTDOWN, 0);
}

TEST_F(GameWorld, VanillaRestartWithSecondsWarmsUpFirst)
{
	// what the restart command does, which the test console only stores
	SelectGameMode("dm");
	const int TickSpeed = m_pServer->TickSpeed();
	RunTicks(this, TickSpeed);

	GameController()->RestartAfterWarmup(5);
	const protocol7::CNetObj_GameData Warmup = SnapGameData7(this);
	EXPECT_EQ(Warmup.m_GameStateFlags, protocol7::GAMESTATEFLAG_WARMUP);
	EXPECT_EQ(Warmup.m_GameStateEndTick, m_pServer->Tick() + 5 * TickSpeed);
	EXPECT_EQ(SnapGameInfo6(this).m_WarmupTimer, 5 * TickSpeed);
	RunTicks(this, 5 * TickSpeed);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, 0);
	EXPECT_EQ(SnapGameData7(this).m_GameStartTick, m_pServer->Tick());

	// as in 0.7, 0 restarts right away and does not only end the warmup
	RunTicks(this, TickSpeed);
	GameController()->RestartAfterWarmup(10);
	RunTicks(this, TickSpeed);
	GameController()->RestartAfterWarmup(0);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, 0);
	EXPECT_EQ(SnapGameData7(this).m_GameStartTick, m_pServer->Tick());

	// also after a match
	GameController()->EndRound();
	GameController()->RestartAfterWarmup(3);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, protocol7::GAMESTATEFLAG_WARMUP);
	EXPECT_FALSE(GameController()->IsGamePaused());
}

TEST_F(GameWorld, LMSPauseEndsWithTheSurvivalCountdown)
{
	SelectGameMode("lms");
	AddSpawnPoints(this);
	JoinPlayer(0, TEAM_GAME, "first");
	JoinPlayer(1, TEAM_GAME, "second");
	RunCountdown(this);
	const int TickSpeed = m_pServer->TickSpeed();

	GameServer()->Console()->ExecuteLine("pause", IConsole::CLIENT_ID_UNSPECIFIED);
	// the one who leaves now does not decide the round while the game stands still
	LeavePlayer(1);
	RunTicks(this, TickSpeed);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, protocol7::GAMESTATEFLAG_PAUSED);
	GameServer()->Console()->ExecuteLine("pause", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_FALSE(GameController()->IsTeamChangeAllowed());
	EXPECT_EQ(SnapGameData7(this).m_GameStateEndTick, m_pServer->Tick() + 3 * TickSpeed);
	RunTicks(this, 3 * TickSpeed + 1);
	EXPECT_NE(SnapGameData7(this).m_GameStateFlags & protocol7::GAMESTATEFLAG_ROUNDOVER, 0);
}

TEST_F(GameWorld, PauseCommandPausesThePlayerInDDRace)
{
	const int ClientId = 0;
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	m_pServer->m_aClients[ClientId].m_State = CServer::CClient::STATE_INGAME;
	ASSERT_NE(pPlayer, nullptr);
	ASSERT_NE(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)), nullptr);
	// an admin in rcon
	const int AuthKey = m_pServer->m_AuthManager.AddKey("pause-admin", "test", RoleName::ADMIN);
	ASSERT_GE(AuthKey, 0);
	m_pServer->m_aClients[ClientId].m_AuthKey = AuthKey;
	GameServer()->Console()->ExecuteLine("pause", ClientId);
	EXPECT_EQ(pPlayer->IsPaused(), -CPlayer::PAUSE_PAUSED);
	EXPECT_FALSE(GameController()->IsGamePaused());
	delete GameServer()->m_apPlayers[ClientId];
	GameServer()->m_apPlayers[ClientId] = nullptr;
}

TEST_F(GameWorld, VotesOfA07ServerConfigAreValidBeforeThereIsAMode)
{
	// the startup config adds its votes before a map and a mode are loaded
	GameServer()->GameHost().Shutdown();
	const char *const apVoteCommands[] = {
		"restart 15",
		"pause",
		"swap_teams",
		"shuffle_teams",
		"set_team_all -1",
		"force_teambalance",
		"reload",
		"echo 0",
		"sv_gametype ctf; sv_map ctf1; sv_player_slots 4; sv_scorelimit 600",
		"sv_player_slots 12",
		"sv_countdown 3",
		"sv_powerups 0",
		"sv_silent_spectator_mode 1",
		"sv_tournament_mode 1",
	};
	for(const char *pCommand : apVoteCommands)
		EXPECT_TRUE(GameServer()->Console()->LineIsValid(pCommand)) << pCommand;
	SelectGameMode("ddnet");
}

TEST_F(GameWorld, VanillaCTFFlagLifecycle)
{
	SelectGameMode("ctf");
	auto &Controller = *dynamic_cast<CGameControllerVanillaCTF *>(GameController());

	int RedFlagX = -1;
	int RedFlagY = -1;
	int BlueFlagX = -1;
	int BlueFlagY = -1;
	for(int y = 1; y < GameServer()->Collision()->GetHeight() - 1; y++)
	{
		for(int x = 1; x < GameServer()->Collision()->GetWidth() - 1; x++)
		{
			const vec2 Pos(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
			if(GameServer()->Collision()->TestBox(Pos, vec2(28.0f, 28.0f)))
				continue;
			if(RedFlagX == -1)
			{
				RedFlagX = x;
				RedFlagY = y;
			}
			else if(distance(Pos, vec2(RedFlagX * 32.0f + 16.0f, RedFlagY * 32.0f + 16.0f)) > 128.0f)
			{
				BlueFlagX = x;
				BlueFlagY = y;
				break;
			}
		}
		if(BlueFlagX != -1)
			break;
	}
	ASSERT_NE(RedFlagX, -1);
	ASSERT_NE(BlueFlagX, -1);
	const vec2 RedStand(RedFlagX * 32.0f + 16.0f, RedFlagY * 32.0f + 16.0f);
	const vec2 BlueStand(BlueFlagX * 32.0f + 16.0f, BlueFlagY * 32.0f + 16.0f);
	EXPECT_TRUE(Controller.OnEntity({ENTITY_FLAGSTAND_RED, RedFlagX, RedFlagY, LAYER_GAME, 0, true, 0}));
	EXPECT_TRUE(Controller.OnEntity({ENTITY_FLAGSTAND_BLUE, BlueFlagX, BlueFlagY, LAYER_GAME, 0, true, 0}));
	CFlag *pRedFlag = Controller.Flag(TEAM_RED);
	EXPECT_TRUE(Controller.OnEntity({ENTITY_FLAGSTAND_RED, RedFlagX, RedFlagY, LAYER_GAME, 0, true, 0}));
	EXPECT_EQ(Controller.Flag(TEAM_RED), pRedFlag);
	ASSERT_NE(Controller.Flag(TEAM_RED), nullptr);
	ASSERT_NE(Controller.Flag(TEAM_BLUE), nullptr);
	EXPECT_EQ(Controller.Flag(TEAM_RED)->StandPosition(), RedStand);
	EXPECT_EQ(Controller.Flag(TEAM_BLUE)->StandPosition(), BlueStand);

	m_pServer->m_aClients[0].m_State = CServer::CClient::STATE_INGAME;
	m_pServer->m_aClients[1].m_State = CServer::CClient::STATE_INGAME;
	str_copy(m_pServer->m_aClients[0].m_aName, "red");
	str_copy(m_pServer->m_aClients[1].m_aName, "blue");
	GameServer()->CreatePlayer(0, TEAM_RED, false, -1);
	GameServer()->CreatePlayer(1, TEAM_BLUE, false, -1);
	CCharacter *pRedCarrier = GameServer()->m_apPlayers[0]->ForceSpawn(BlueStand);
	CCharacter *pBlueReturner = GameServer()->m_apPlayers[1]->ForceSpawn(vec2(BlueStand.x + 128.0f, BlueStand.y));
	ASSERT_NE(pRedCarrier, nullptr);
	ASSERT_NE(pBlueReturner, nullptr);
	g_Config.m_SvScorelimit = 0;
	g_Config.m_SvTimelimit = 0;

	Controller.Tick();
	EXPECT_EQ(Controller.Flag(TEAM_BLUE)->Carrier(), pRedCarrier);
	EXPECT_EQ(Controller.TeamScore(TEAM_RED), 1);
	EXPECT_EQ(Controller.SnapPlayerScore(-1, pRedCarrier->GetPlayer()), 1);

	pRedCarrier->SetPosition(RedStand);
	pRedCarrier->m_Pos = RedStand;
	Controller.Tick();
	EXPECT_TRUE(Controller.Flag(TEAM_RED)->IsAtStand());
	EXPECT_TRUE(Controller.Flag(TEAM_BLUE)->IsAtStand());
	EXPECT_EQ(Controller.TeamScore(TEAM_RED), 101);
	EXPECT_EQ(Controller.SnapPlayerScore(-1, pRedCarrier->GetPlayer()), 6);
	pRedCarrier->SetPosition(BlueStand);
	pRedCarrier->m_Pos = BlueStand;
	Controller.Tick();
	ASSERT_EQ(Controller.Flag(TEAM_BLUE)->Carrier(), pRedCarrier);
	pRedCarrier->Die(pBlueReturner->GetPlayer()->GetCid(), WEAPON_GUN);
	EXPECT_EQ(Controller.Flag(TEAM_BLUE)->Carrier(), nullptr);
	EXPECT_FALSE(Controller.Flag(TEAM_BLUE)->IsAtStand());
	pBlueReturner->SetPosition(BlueStand);
	pBlueReturner->m_Pos = BlueStand;
	Controller.Tick();
	EXPECT_TRUE(Controller.Flag(TEAM_BLUE)->IsAtStand());
	EXPECT_EQ(Controller.SnapPlayerScore(-1, pBlueReturner->GetPlayer()), 3);

	Controller.EndRound();
	const CReceivedMatchReport Red = ReceivedMatchReport(0);
	EXPECT_EQ(Red.Metric(Red.m_LocalParticipantId, "flag_grabs"), 2);
	EXPECT_EQ(Red.Metric(Red.m_LocalParticipantId, "flag_captures"), 1);
	EXPECT_EQ(Red.m_Report.Metric(EMatchSubjectKind::TEAM, TEAM_RED, "score"), Controller.TeamScore(TEAM_RED));
	const CReceivedMatchReport Blue = ReceivedMatchReport(1);
	EXPECT_EQ(Blue.Metric(Blue.m_LocalParticipantId, "flag_returns"), 1);
	EXPECT_EQ(Blue.Metric(Blue.m_LocalParticipantId, "kills"), 1);
}

TEST_F(GameWorld, VanillaPickup)
{
	constexpr int ClientId = 0;
	SelectGameMode("dm");
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	pPlayer->ForceSpawn(vec2(0, 0));
	CCharacter *pCharacter = pPlayer->GetCharacter();
	ASSERT_NE(pCharacter, nullptr);

	auto &Controller = *dynamic_cast<CGameControllerVanillaDM *>(GameController());
	pCharacter->SetHealth(9);
	const CGamePickupResult HealthResult = Controller.OnCharacterPickup(pCharacter, POWERUP_HEALTH, 0, pCharacter->m_Pos);
	EXPECT_TRUE(HealthResult.m_Picked);
	EXPECT_EQ(HealthResult.m_RespawnSeconds, 15);
	EXPECT_EQ(pCharacter->GetHealth(), 10);
	EXPECT_FALSE(Controller.OnCharacterPickup(pCharacter, POWERUP_HEALTH, 0, pCharacter->m_Pos).m_Picked);

	pCharacter->SetWeaponGot(WEAPON_SHOTGUN, false);
	pCharacter->SetWeaponAmmo(WEAPON_SHOTGUN, 0);
	const CGamePickupResult WeaponResult = Controller.OnCharacterPickup(pCharacter, POWERUP_WEAPON, WEAPON_SHOTGUN, pCharacter->m_Pos);
	EXPECT_TRUE(WeaponResult.m_Picked);
	EXPECT_EQ(WeaponResult.m_RespawnSeconds, 15);
	EXPECT_EQ(WeaponResult.m_RespawnSound, SOUND_WEAPON_SPAWN);
	EXPECT_TRUE(pCharacter->GetWeaponGot(WEAPON_SHOTGUN));
	EXPECT_EQ(pCharacter->GetWeaponAmmo(WEAPON_SHOTGUN), 10);
	pCharacter->SetWeaponAmmo(WEAPON_SHOTGUN, 4);
	EXPECT_TRUE(Controller.OnCharacterPickup(pCharacter, POWERUP_WEAPON, WEAPON_SHOTGUN, pCharacter->m_Pos).m_Picked);
	EXPECT_EQ(pCharacter->GetWeaponAmmo(WEAPON_SHOTGUN), 10);
	EXPECT_FALSE(Controller.OnCharacterPickup(pCharacter, POWERUP_WEAPON, WEAPON_SHOTGUN, pCharacter->m_Pos).m_Picked);
	EXPECT_EQ(Controller.PickupInitialSpawnDelaySeconds(POWERUP_HEALTH, 0), 0);
	EXPECT_EQ(Controller.PickupInitialSpawnDelaySeconds(POWERUP_NINJA, 0), 90);

	pCharacter->SetHealth(9);
	auto *pHealth = new CPickup(&GameServer()->m_World, POWERUP_HEALTH, 0, 0, 0, 0);
	pHealth->m_Pos = pCharacter->m_Pos;
	pHealth->Tick();
	auto *pNinja = new CPickup(&GameServer()->m_World, POWERUP_NINJA, 0, 0, 0, 0);
	EXPECT_EQ(pCharacter->GetHealth(), 10);
	EXPECT_FALSE(pHealth->IsActive());
	EXPECT_FALSE(pNinja->IsActive()); // NOLINT(clang-analyzer-unix.Malloc)
}

TEST_F(GameWorld, DDRacePickupPolicyIsModeOwned)
{
	constexpr int ClientId = 0;
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	pPlayer->ForceSpawn(vec2(0, 0));
	CCharacter *pCharacter = pPlayer->GetCharacter();
	ASSERT_NE(pCharacter, nullptr);

	auto *pFreeze = new CPickup(&GameServer()->m_World, POWERUP_FREEZE, 0, 0, 0, 0);
	pFreeze->m_Pos = pCharacter->m_Pos;
	pFreeze->Tick();
	EXPECT_GT(pCharacter->m_FreezeTime, 0);
	EXPECT_TRUE(pFreeze->IsActive());

	pCharacter->Unfreeze();
	pCharacter->SetWeaponGot(WEAPON_SHOTGUN, true);
	pCharacter->SetWeaponAmmo(WEAPON_SHOTGUN, -1);
	pCharacter->SetActiveWeapon(WEAPON_SHOTGUN);
	RaceController().OnCharacterPickup(pCharacter, POWERUP_ARMOR_SHOTGUN, 0, pCharacter->m_Pos);
	EXPECT_FALSE(pCharacter->GetWeaponGot(WEAPON_SHOTGUN));
	EXPECT_EQ(pCharacter->GetActiveWeapon(), WEAPON_HAMMER);

	RaceController().OnCharacterPickup(pCharacter, POWERUP_WEAPON, WEAPON_SHOTGUN, pCharacter->m_Pos);
	EXPECT_TRUE(pCharacter->GetWeaponGot(WEAPON_SHOTGUN));
	EXPECT_EQ(pCharacter->GetWeaponAmmo(WEAPON_SHOTGUN), -1);
}

TEST_F(GameWorld, VanillaProjectileOwnerLoss)
{
	constexpr int ClientId = 0;
	constexpr int TargetId = 1;
	EXPECT_EQ(GameController()->ProjectileRules({WEAPON_GUN, nullptr, true, false}).m_OwnerLossAction, EProjectileOwnerLossAction::DESTROY);

	vec2 SpawnPosition;
	ASSERT_TRUE(GameController()->CanSpawn(TEAM_GAME, &SpawnPosition, ClientId));
	SelectGameMode("dm");
	auto &Controller = *dynamic_cast<CGameControllerVanillaDM *>(GameController());
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	pPlayer->ForceSpawn(SpawnPosition);
	ASSERT_NE(pPlayer->GetCharacter(), nullptr);
	GameServer()->CreatePlayer(TargetId, TEAM_GAME, false, -1);
	CPlayer *pTargetPlayer = GameServer()->m_apPlayers[TargetId];
	pTargetPlayer->ForceSpawn(SpawnPosition);
	CCharacter *pTarget = pTargetPlayer->GetCharacter();
	ASSERT_NE(pTarget, nullptr);
	pTarget->SetHealth(10);

	const CGameProjectileRules ConnectedRules = Controller.ProjectileRules({WEAPON_GUN, pPlayer->GetCharacter(), true, false});
	EXPECT_TRUE(ConnectedRules.m_HitCharacters);
	EXPECT_FALSE(ConnectedRules.m_RespectCharacterCollision);
	EXPECT_FLOAT_EQ(ConnectedRules.m_DirectImpactForce, 0.001f);
	EXPECT_EQ(ConnectedRules.m_OwnerLossAction, EProjectileOwnerLossAction::KEEP);
	EXPECT_EQ(Controller.ProjectileRules({WEAPON_GUN, nullptr, false, false}).m_OwnerLossAction, EProjectileOwnerLossAction::DETACH);
	auto *pImpactProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, ClientId, SpawnPosition, vec2(1, 0), 100, false, false, -1, vec2(1, 0));
	pImpactProjectile->Tick();
	EXPECT_EQ(pTarget->GetHealth(), 9);

	auto *pProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, ClientId, SpawnPosition, vec2(1, 0), 100, false, false, -1, vec2(1, 0));
	pPlayer->KillCharacter();
	pProjectile->Tick();
	EXPECT_EQ(pProjectile->GetOwnerId(), ClientId);
	GameServer()->m_apPlayers[ClientId] = nullptr;
	pProjectile->Tick();
	GameServer()->m_apPlayers[ClientId] = pPlayer;
	EXPECT_EQ(pProjectile->GetOwnerId(), -1);
}

TEST_F(GameWorld, ExplosionPolicyIsModeOwned)
{
	constexpr int OwnerId = 0;
	constexpr int VictimId = 1;

	ASSERT_TRUE(RaceControllerOrNull() != nullptr);
	CPlayer *pRaceOwner = GameServer()->CreatePlayer(OwnerId, TEAM_GAME, false, -1);
	CPlayer *pRaceVictim = GameServer()->CreatePlayer(VictimId, TEAM_GAME, false, -1);
	ASSERT_NE(pRaceOwner, nullptr);
	ASSERT_NE(pRaceVictim, nullptr);
	ASSERT_NE(pRaceOwner->ForceSpawn(vec2(-64.0f, 0.0f)), nullptr);
	CCharacter *pRaceVictimCharacter = pRaceVictim->ForceSpawn(vec2(0.0f, 0.0f));
	ASSERT_NE(pRaceVictimCharacter, nullptr);
	pRaceVictimCharacter->SetHealth(10);
	pRaceOwner->KillCharacter();
	g_Config.m_SvHit = 0;
	GameServer()->CreateExplosion(pRaceVictimCharacter->m_Pos, OwnerId, WEAPON_GRENADE, false, -1);
	const int RaceVictimHealth = pRaceVictimCharacter->GetHealth();
	EXPECT_EQ(RaceVictimHealth, 10);

	delete GameServer()->m_apPlayers[OwnerId];
	GameServer()->m_apPlayers[OwnerId] = nullptr;
	delete GameServer()->m_apPlayers[VictimId];
	GameServer()->m_apPlayers[VictimId] = nullptr;
	SelectGameMode("dm");
	ASSERT_FALSE(RaceControllerOrNull() != nullptr);
	CPlayer *pVanillaOwner = GameServer()->CreatePlayer(OwnerId, TEAM_GAME, false, -1);
	CPlayer *pVanillaVictim = GameServer()->CreatePlayer(VictimId, TEAM_GAME, false, -1);
	ASSERT_NE(pVanillaOwner, nullptr);
	ASSERT_NE(pVanillaVictim, nullptr);
	ASSERT_NE(pVanillaOwner->ForceSpawn(vec2(-64.0f, 0.0f)), nullptr);
	CCharacter *pVanillaVictimCharacter = pVanillaVictim->ForceSpawn(vec2(0.0f, 0.0f));
	ASSERT_NE(pVanillaVictimCharacter, nullptr);
	pVanillaVictimCharacter->SetHealth(10);
	pVanillaOwner->KillCharacter();
	g_Config.m_SvHit = 0;
	GameServer()->CreateExplosion(pVanillaVictimCharacter->m_Pos, OwnerId, WEAPON_GRENADE, false, -1);
	const int VanillaVictimHealth = pVanillaVictimCharacter->GetHealth();
	EXPECT_LT(VanillaVictimHealth, 10);

	delete GameServer()->m_apPlayers[OwnerId];
	GameServer()->m_apPlayers[OwnerId] = nullptr;
	delete GameServer()->m_apPlayers[VictimId];
	GameServer()->m_apPlayers[VictimId] = nullptr;
	SelectGameMode("ddnet");
}

namespace
{
	// a mode that lets the grenades of player 0 reach nobody but player 2
	class CAffectMaskController : public CGameControllerVanillaDM
	{
	public:
		using CGameControllerVanillaDM::CGameControllerVanillaDM;
		void OnProjectileCreated(CProjectile *pProjectile) override
		{
			if(pProjectile->GetOwnerId() == 0)
				pProjectile->SetAffectMask(CClientMask().set(2));
		}
	};
}

TEST_F(GameWorld, ProjectileTellsItsExplosionWhomItMayReach)
{
	SelectController<CAffectMaskController>("dm");
	ASSERT_NE(SpawnPlayer(0, vec2(-256.0f, 0.0f)), nullptr);
	CCharacter *pSpared = SpawnPlayer(1, vec2(0.0f, 0.0f));
	CCharacter *pReached = SpawnPlayer(2, vec2(8.0f, 0.0f));
	ASSERT_NE(pSpared, nullptr);
	ASSERT_NE(pReached, nullptr);
	pSpared->SetHealth(10);
	pReached->SetHealth(10);

	auto *pGrenade = new CProjectile(&GameServer()->m_World, WEAPON_GRENADE, 0, vec2(4.0f, 0.0f), vec2(1, 0), 0, false, true, -1, vec2(1, 0));
	EXPECT_TRUE(pGrenade->AffectMask().test(2));
	EXPECT_FALSE(pGrenade->AffectMask().test(1));
	// it explodes right away, among both
	pGrenade->Tick();
	EXPECT_EQ(pSpared->GetHealth(), 10);
	EXPECT_EQ(pSpared->GetCore().m_Vel, vec2(0.0f, 0.0f));
	EXPECT_LT(pReached->GetHealth(), 10);

	// someone else's grenade reaches everybody
	auto *pOther = new CProjectile(&GameServer()->m_World, WEAPON_GRENADE, 1, vec2(4.0f, 0.0f), vec2(1, 0), 0, false, true, -1, vec2(1, 0));
	EXPECT_TRUE(pOther->AffectMask().all());
}

TEST_F(GameWorld, VanillaTeamsSwapAfterAMatch)
{
	SelectGameMode("tdm");
	CPlayer *pRed = GameServer()->CreatePlayer(0, TEAM_RED, false, -1);
	CPlayer *pBlue = GameServer()->CreatePlayer(1, TEAM_BLUE, false, -1);
	ASSERT_NE(pRed, nullptr);
	ASSERT_NE(pBlue, nullptr);

	GameController()->StartRound();
	EXPECT_EQ(pRed->GetTeam(), TEAM_RED);

	GameController()->EndRound();
	GameController()->StartRound();
	EXPECT_EQ(pRed->GetTeam(), TEAM_BLUE);
	EXPECT_EQ(pBlue->GetTeam(), TEAM_RED);
}

static int CountTeam(const CGameContext *pGameServer, int Team)
{
	int Count = 0;
	for(const CPlayer *pPlayer : pGameServer->m_apPlayers)
		Count += pPlayer && pPlayer->GetTeam() == Team;
	return Count;
}

TEST_F(GameWorld, VanillaSwapTeamsCommandSwapsPlayersAndScores)
{
	auto &Controller = SelectController<CTestVanillaTDM>("tdm");
	CPlayer *pRed = JoinPlayer(0, TEAM_RED, "red");
	CPlayer *pBlue = JoinPlayer(1, TEAM_BLUE, "blue");
	CPlayer *pSpectator = JoinPlayer(2, TEAM_SPECTATORS, "spectator");
	Controller.SetTeamScores(3, 5);
	pRed->m_LastActionTick = 123;

	GameServer()->Console()->ExecuteLine("swap_teams", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_EQ(pRed->GetTeam(), TEAM_BLUE);
	EXPECT_EQ(pBlue->GetTeam(), TEAM_RED);
	EXPECT_EQ(pSpectator->GetTeam(), TEAM_SPECTATORS);
	EXPECT_EQ(Controller.TeamScore(TEAM_RED), 5);
	EXPECT_EQ(Controller.TeamScore(TEAM_BLUE), 3);
	// the server moved them, they did nothing
	EXPECT_EQ(pRed->m_LastActionTick, 123);
}

TEST_F(GameWorld, VanillaShuffleTeamsCommandSplitsThePlayers)
{
	SelectGameMode("tdm");
	for(int ClientId = 0; ClientId < 5; ClientId++)
		JoinPlayer(ClientId, TEAM_RED, "red");
	CPlayer *pSpectator = JoinPlayer(5, TEAM_SPECTATORS, "spectator");

	GameServer()->Console()->ExecuteLine("shuffle_teams", IConsole::CLIENT_ID_UNSPECIFIED);
	const int NumRed = CountTeam(GameServer(), TEAM_RED);
	EXPECT_TRUE(NumRed == 2 || NumRed == 3) << NumRed;
	EXPECT_EQ(NumRed + CountTeam(GameServer(), TEAM_BLUE), 5);
	EXPECT_EQ(pSpectator->GetTeam(), TEAM_SPECTATORS);
}

TEST_F(GameWorld, VanillaTeamCommandsWithoutTeamsChangeNothing)
{
	SelectGameMode("dm");
	CPlayer *pFirst = JoinPlayer(0, TEAM_GAME, "first");
	CPlayer *pSecond = JoinPlayer(1, TEAM_GAME, "second");
	GameServer()->Console()->ExecuteLine("swap_teams; shuffle_teams; force_teambalance", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_EQ(pFirst->GetTeam(), TEAM_GAME);
	EXPECT_EQ(pSecond->GetTeam(), TEAM_GAME);
}

TEST_F(GameWorld, VanillaSetTeamAllCommandMovesEverybody)
{
	SelectGameMode("dm");
	CPlayer *pPlayer = JoinPlayer(0, TEAM_GAME, "player");
	CPlayer *pSpectator = JoinPlayer(1, TEAM_SPECTATORS, "spectator");
	pPlayer->m_LastActionTick = 123;

	GameServer()->Console()->ExecuteLine("set_team_all -1", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_EQ(pPlayer->GetTeam(), TEAM_SPECTATORS);
	EXPECT_EQ(pSpectator->GetTeam(), TEAM_SPECTATORS);
	EXPECT_EQ(pPlayer->m_LastActionTick, 123);

	// as in 0.7, the blue team is the game in a mode without teams
	GameServer()->Console()->ExecuteLine("set_team_all 1", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_EQ(pPlayer->GetTeam(), TEAM_GAME);
	EXPECT_EQ(pSpectator->GetTeam(), TEAM_GAME);
}

TEST_F(GameWorld, VanillaSetTeamAllCommandIgnoresTheTeamBalance)
{
	g_Config.m_SvTeambalanceTime = 1;
	SelectGameMode("tdm");
	for(int ClientId = 0; ClientId < 3; ClientId++)
		JoinPlayer(ClientId, TEAM_SPECTATORS, "spectator");
	GameServer()->Console()->ExecuteLine("set_team_all 1", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_EQ(CountTeam(GameServer(), TEAM_BLUE), 3);
}

TEST_F(GameWorld, VanillaForceTeambalanceCommandBalancesRightAway)
{
	g_Config.m_SvTeambalanceTime = 1;
	SelectGameMode("tdm");
	for(int ClientId = 0; ClientId < 3; ClientId++)
		JoinPlayer(ClientId, TEAM_RED, "red");
	GameServer()->Console()->ExecuteLine("force_teambalance", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_EQ(CountTeam(GameServer(), TEAM_RED), 2);
	EXPECT_EQ(CountTeam(GameServer(), TEAM_BLUE), 1);

	// without automatic balancing there is nothing to force either, as in 0.7
	JoinPlayer(3, TEAM_RED, "red");
	JoinPlayer(4, TEAM_RED, "red");
	g_Config.m_SvTeambalanceTime = 0;
	GameServer()->Console()->ExecuteLine("force_teambalance", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_EQ(CountTeam(GameServer(), TEAM_RED), 4);
}

TEST_F(GameWorld, LTSBalancesOnlyWhenARoundBegins)
{
	g_Config.m_SvTeambalanceTime = 1;
	SelectGameMode("lts");
	for(int ClientId = 0; ClientId < 3; ClientId++)
		JoinPlayer(ClientId, TEAM_RED, "red");
	GameServer()->Console()->ExecuteLine("force_teambalance", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_EQ(CountTeam(GameServer(), TEAM_RED), 3);
}

TEST_F(GameWorld, VanillaPlayerSlotsLimitWhoCanJoinTheGame)
{
	SelectGameMode("ctf");
	GameServer()->Console()->ExecuteLine("sv_player_slots 2", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_EQ(GameController()->PlayerSlots(), 2);
	JoinPlayer(0, TEAM_RED, "red");
	JoinPlayer(1, TEAM_BLUE, "blue");
	CPlayer *pThird = JoinPlayer(2, TEAM_SPECTATORS, "third");

	char aError[64];
	EXPECT_FALSE(GameController()->CanJoinTeam(TEAM_RED, 2, aError, sizeof(aError)));
	EXPECT_STREQ(aError, "Only 2 active players are allowed");
	EXPECT_EQ(GameController()->GetAutoTeam(2), TEAM_SPECTATORS);
	// the players in the game can still change their team
	GameServer()->Console()->ExecuteLine("set_team_all 0", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_EQ(CountTeam(GameServer(), TEAM_RED), 2);
	EXPECT_EQ(pThird->GetTeam(), TEAM_SPECTATORS);

	// as in 0.7, fewer slots move nobody out of the game
	GameServer()->Console()->ExecuteLine("sv_player_slots 1", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_EQ(CountTeam(GameServer(), TEAM_RED), 2);

	GameServer()->Console()->ExecuteLine("sv_player_slots 4", IConsole::CLIENT_ID_UNSPECIFIED);
	EXPECT_TRUE(GameController()->CanJoinTeam(TEAM_BLUE, 2, nullptr, 0));
	// the spectator slots stay free for spectators
	g_Config.m_SvSpectatorSlots = m_pServer->MaxClients() - 2;
	EXPECT_EQ(GameController()->PlayerSlots(), 2);
	EXPECT_FALSE(GameController()->CanJoinTeam(TEAM_BLUE, 2, nullptr, 0));
}

TEST_F(GameWorld, VanillaTournamentModePutsNewPlayersIntoTheSpectators)
{
	SelectGameMode("dm");
	g_Config.m_SvTournamentMode = 1;
	GameServer()->OnClientConnected(0, nullptr);
	ASSERT_NE(GameServer()->m_apPlayers[0], nullptr);
	EXPECT_EQ(GameServer()->m_apPlayers[0]->GetTeam(), TEAM_SPECTATORS);

	g_Config.m_SvTournamentMode = 0;
	GameServer()->OnClientConnected(1, nullptr);
	ASSERT_NE(GameServer()->m_apPlayers[1], nullptr);
	EXPECT_EQ(GameServer()->m_apPlayers[1]->GetTeam(), TEAM_GAME);
}

TEST_F(GameWorld, VanillaPowerupsOnlyWithSvPowerups)
{
	SelectGameMode("ctf");
	g_Config.m_SvPowerups = 0;
	EXPECT_FALSE(GameController()->OnEntity({ENTITY_POWERUP_NINJA, 1, 1, LAYER_GAME, 0, true, 0}));
	EXPECT_TRUE(GameController()->OnEntity({ENTITY_WEAPON_SHOTGUN, 1, 1, LAYER_GAME, 0, true, 0}));
	g_Config.m_SvPowerups = 1;
	EXPECT_TRUE(GameController()->OnEntity({ENTITY_POWERUP_NINJA, 1, 1, LAYER_GAME, 0, true, 0}));
}

class CChatLogger : public ILogger
{
public:
	std::vector<std::string> m_vLines;

	void Log(const CLogMessage *pMessage) override
	{
		if(str_comp(pMessage->m_aSystem, "chat") == 0)
			m_vLines.emplace_back(pMessage->Message());
	}
};

TEST_F(GameWorld, VanillaSilentSpectatorModeKeepsSpectatorsQuiet)
{
	SelectGameMode("dm");
	CChatLogger Logger;
	{
		CLogScope Scope(&Logger);
		JoinPlayer(0, TEAM_GAME, "player");
		JoinPlayer(1, TEAM_SPECTATORS, "spectator");
		LeavePlayer(1);
		LeavePlayer(0);
	}
	EXPECT_EQ(Logger.m_vLines, (std::vector<std::string>{"*** 'player' entered and joined the game", "*** 'player' has left the game (test)"}));

	g_Config.m_SvSilentSpectatorMode = 0;
	Logger.m_vLines.clear();
	{
		CLogScope Scope(&Logger);
		JoinPlayer(1, TEAM_SPECTATORS, "spectator");
		LeavePlayer(1);
	}
	EXPECT_EQ(Logger.m_vLines, (std::vector<std::string>{"*** 'spectator' entered and joined the spectators", "*** 'spectator' has left the game (test)"}));
}

TEST_F(GameWorld, VanillaCTFSpectatorsFollowAFlag)
{
	auto &Controller = SelectController<CTestVanillaCTF>("ctf");
	ASSERT_TRUE(Controller.OnEntity({ENTITY_FLAGSTAND_RED, 2, 2, LAYER_GAME, 0, true, 0}));
	CPlayer *pSpectator = GameServer()->CreatePlayer(0, TEAM_SPECTATORS, false, -1);
	ASSERT_NE(pSpectator, nullptr);

	pSpectator->SetSpectatorId(SPEC_FLAGRED);
	pSpectator->PostTick();
	EXPECT_EQ(pSpectator->SpectatorId(), SPEC_FREEVIEW);
	EXPECT_EQ(pSpectator->m_ViewPos, Controller.Flag(TEAM_RED)->m_Pos);

	// there is no blue flag to follow, so this stays a free view
	pSpectator->m_ViewPos = vec2(0.0f, 0.0f);
	pSpectator->SetSpectatorId(SPEC_FLAGBLUE);
	pSpectator->PostTick();
	EXPECT_EQ(pSpectator->m_ViewPos, vec2(0.0f, 0.0f));
}

namespace
{
	// what a 0.7 client is told about a player
	int SnapPlayerFlags7(GameWorld *pWorld, int ClientId, int SnappingClient)
	{
		pWorld->m_pServer->m_aClients[SnappingClient].m_Sixup = true;
		// a 0.7 client sees the players by the ids it was told
		pWorld->m_pServer->GetIdMap(SnappingClient)[ClientId] = ClientId;
		pWorld->m_pServer->GetReverseIdMap(SnappingClient)[ClientId] = ClientId;
		pWorld->m_pServer->m_SnapshotBuilder.Init(true);
		pWorld->GameServer()->m_apPlayers[ClientId]->Snap(SnappingClient);
		CSnapshotBuffer Buffer;
		pWorld->m_pServer->m_SnapshotBuilder.Finish(&Buffer);
		pWorld->m_pServer->m_aClients[SnappingClient].m_Sixup = false;
		const auto *pInfo = static_cast<const protocol7::CNetObj_PlayerInfo *>(Buffer.AsSnapshot()->FindItem(protocol7::NETOBJTYPE_PLAYERINFO, ClientId));
		EXPECT_NE(pInfo, nullptr);
		return pInfo ? pInfo->m_PlayerFlags : 0;
	}
}

TEST_F(GameWorld, ReadyModeStartsAMatchOnceEverybodyIsReady)
{
	g_Config.m_SvPlayerReadyMode = 1;
	g_Config.m_SvCountdown = 3;
	SelectGameMode("dm");
	JoinPlayer(0, TEAM_GAME, "first");
	JoinPlayer(1, TEAM_GAME, "second");
	JoinPlayer(2, TEAM_SPECTATORS, "spectator");
	const int TickSpeed = m_pServer->TickSpeed();
	RunTicks(this, TickSpeed);
	// everybody is ready while the game waits for nobody
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::NONE);
	EXPECT_NE(SnapPlayerFlags7(this, 0, 1) & protocol7::PLAYERFLAG_READY, 0);

	GameController()->RestartAfterWarmup(-1);
	RunTicks(this, 1);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::START);
	const protocol7::CNetObj_GameData Warmup = SnapGameData7(this);
	EXPECT_EQ(Warmup.m_GameStateFlags, protocol7::GAMESTATEFLAG_WARMUP);
	EXPECT_EQ(Warmup.m_GameStateEndTick, 0);
	EXPECT_FALSE(GameController()->IsGamePaused());
	EXPECT_FALSE(GameController()->ReadyMode().IsReady(0));
	EXPECT_EQ(SnapPlayerFlags7(this, 0, 1) & protocol7::PLAYERFLAG_READY, 0);
	// a warmup without an end is nothing a 0.6 client could count down
	EXPECT_EQ(SnapGameInfo6(this).m_WarmupTimer, 0);

	// spectators do not count and cannot be ready
	EXPECT_NE(GameController()->ReadyMode().OnPlayerReadyChange(2), nullptr);
	EXPECT_EQ(GameController()->ReadyMode().OnPlayerReadyChange(0), nullptr);
	EXPECT_TRUE(GameController()->ReadyMode().IsReady(0));
	EXPECT_NE(SnapPlayerFlags7(this, 0, 1) & protocol7::PLAYERFLAG_READY, 0);
	RunTicks(this, 10 * TickSpeed);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::START);

	// changing one's mind twice within a second does not count
	GameController()->ReadyMode().OnPlayerReadyChange(1);
	GameController()->ReadyMode().OnPlayerReadyChange(1);
	EXPECT_TRUE(GameController()->ReadyMode().IsReady(1));
	RunTicks(this, 1);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::NONE);
	// the match counts down first, from the tick that started it
	const protocol7::CNetObj_GameData Countdown = SnapGameData7(this);
	EXPECT_EQ(Countdown.m_GameStateFlags, protocol7::GAMESTATEFLAG_STARTCOUNTDOWN | protocol7::GAMESTATEFLAG_PAUSED);
	EXPECT_EQ(Countdown.m_GameStateEndTick, m_pServer->Tick() - 1 + 3 * TickSpeed);
	RunTicks(this, 3 * TickSpeed);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, 0);
	EXPECT_TRUE(GameController()->ReadyMode().IsReady(0));
	EXPECT_TRUE(GameController()->ReadyMode().IsReady(2));
}

TEST_F(GameWorld, ReadyModeWaitsForWhoIsLeftInTheGame)
{
	g_Config.m_SvPlayerReadyMode = 1;
	g_Config.m_SvWarmup = -1;
	SelectGameMode("tdm");
	// nobody there, so the match waits for somebody to be ready
	RunTicks(this, 10);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::START);
	JoinPlayer(0, TEAM_RED, "red");
	CPlayer *pBlue = JoinPlayer(1, TEAM_BLUE, "blue");
	JoinPlayer(2, TEAM_BLUE, "late");
	GameController()->ReadyMode().OnPlayerReadyChange(0);
	GameController()->ReadyMode().OnPlayerReadyChange(1);
	RunTicks(this, 1);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::START);

	// who goes to the spectators is not waited for, and not ready on coming back
	GameController()->DoTeamChange(pBlue, TEAM_SPECTATORS, false);
	RunTicks(this, m_pServer->TickSpeed());
	GameController()->DoTeamChange(pBlue, TEAM_BLUE, false);
	RunTicks(this, 1);
	EXPECT_FALSE(GameController()->ReadyMode().IsReady(1));
	GameController()->ReadyMode().OnPlayerReadyChange(1);

	// the last one who is not ready leaves
	LeavePlayer(2);
	RunTicks(this, 1);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::NONE);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, 0);
}

TEST_F(GameWorld, RestartWithoutAnEndStartsRightAwayWithoutReadyMode)
{
	SelectGameMode("dm");
	JoinPlayer(0, TEAM_GAME, "player");
	GameController()->RestartAfterWarmup(-1);
	RunTicks(this, 1);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::NONE);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, 0);
	EXPECT_EQ(SnapGameData7(this).m_GameStartTick, m_pServer->Tick());
	// and nobody can pause the game with it
	EXPECT_NE(GameController()->ReadyMode().OnPlayerReadyChange(0), nullptr);
	EXPECT_FALSE(GameController()->IsGamePaused());

	// switching it off ends a wait
	g_Config.m_SvPlayerReadyMode = 1;
	GameController()->RestartAfterWarmup(-1);
	RunTicks(this, 10);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::START);
	g_Config.m_SvPlayerReadyMode = 0;
	RunTicks(this, 1);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::NONE);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, 0);
}

TEST_F(GameWorld, ReadyModeAPlayerWhoIsNotReadyPausesTheGame)
{
	g_Config.m_SvPlayerReadyMode = 1;
	g_Config.m_SvCountdown = 3;
	SelectGameMode("ctf");
	JoinPlayer(0, TEAM_RED, "red");
	JoinPlayer(1, TEAM_BLUE, "blue");
	const int TickSpeed = m_pServer->TickSpeed();
	RunTicks(this, 3 * TickSpeed + 1);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, 0);

	CChatLogger Logger;
	{
		CLogScope Scope(&Logger);
		EXPECT_EQ(GameController()->ReadyMode().OnPlayerReadyChange(1), nullptr);
	}
	EXPECT_EQ(Logger.m_vLines, (std::vector<std::string>{"*** 'blue' paused the game until everybody is ready"}));
	EXPECT_TRUE(GameController()->IsGamePaused());
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::RESUME);
	const protocol7::CNetObj_GameData Paused = SnapGameData7(this);
	EXPECT_EQ(Paused.m_GameStateFlags, protocol7::GAMESTATEFLAG_PAUSED);
	EXPECT_EQ(Paused.m_GameStateEndTick, 0);
	// everybody has to be ready again, the one who paused as well
	EXPECT_FALSE(GameController()->ReadyMode().IsReady(0));
	EXPECT_FALSE(GameController()->ReadyMode().IsReady(1));
	RunTicks(this, TickSpeed);
	GameController()->ReadyMode().OnPlayerReadyChange(0);
	GameController()->ReadyMode().OnPlayerReadyChange(1);
	RunTicks(this, 1);
	// the game goes on after the countdown
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::NONE);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, protocol7::GAMESTATEFLAG_STARTCOUNTDOWN | protocol7::GAMESTATEFLAG_PAUSED);
	RunTicks(this, 3 * TickSpeed);
	EXPECT_FALSE(GameController()->IsGamePaused());

	// nothing waits for anybody during a countdown or a pause with an end
	GameServer()->Console()->ExecuteLine("pause 5", IConsole::CLIENT_ID_UNSPECIFIED);
	RunTicks(this, TickSpeed);
	EXPECT_NE(GameController()->ReadyMode().OnPlayerReadyChange(0), nullptr);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::NONE);
}

TEST_F(GameWorld, ReadyModeThePauseCommandWaitsForEverybody)
{
	g_Config.m_SvPlayerReadyMode = 1;
	SelectGameMode("dm");
	JoinPlayer(0, TEAM_GAME, "first");
	JoinPlayer(1, TEAM_GAME, "second");
	RunTicks(this, 10);

	// as in 0.7, a pause without an end lasts until everybody is ready
	GameServer()->Console()->ExecuteLine("pause", IConsole::CLIENT_ID_UNSPECIFIED);
	RunTicks(this, 1);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::RESUME);
	GameController()->ReadyMode().OnPlayerReadyChange(0);
	GameController()->ReadyMode().ForceReady(1);
	RunTicks(this, 1);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::NONE);
	EXPECT_FALSE(GameController()->IsGamePaused());

	// and the command still ends it
	GameServer()->Console()->ExecuteLine("pause", IConsole::CLIENT_ID_UNSPECIFIED);
	RunTicks(this, 1);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::RESUME);
	GameServer()->Console()->ExecuteLine("pause", IConsole::CLIENT_ID_UNSPECIFIED);
	RunTicks(this, 1);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::NONE);
	EXPECT_FALSE(GameController()->IsGamePaused());

	// force_ready without an id sets everybody ready
	GameServer()->Console()->ExecuteLine("pause", IConsole::CLIENT_ID_UNSPECIFIED);
	RunTicks(this, 1);
	GameServer()->Console()->ExecuteLine("force_ready", IConsole::CLIENT_ID_UNSPECIFIED);
	RunTicks(this, 1);
	EXPECT_FALSE(GameController()->IsGamePaused());
}

TEST_F(GameWorld, ReadyModeTheGameGoesOnAfterSvForceReadyAll)
{
	g_Config.m_SvPlayerReadyMode = 1;
	g_Config.m_SvForceReadyAll = 1;
	SelectGameMode("dm");
	JoinPlayer(0, TEAM_GAME, "first");
	JoinPlayer(1, TEAM_GAME, "second");
	const int TickSpeed = m_pServer->TickSpeed();
	RunTicks(this, 10);
	GameController()->ReadyMode().OnPlayerReadyChange(0);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::RESUME);

	CChatLogger Logger;
	{
		CLogScope Scope(&Logger);
		RunTicks(this, 60 * TickSpeed - 1);
		EXPECT_TRUE(GameController()->IsGamePaused());
		RunTicks(this, 1);
	}
	EXPECT_FALSE(GameController()->IsGamePaused());
	EXPECT_EQ(Logger.m_vLines, (std::vector<std::string>{"*** The game goes on in 10 seconds, ready or not", "*** Not everybody was ready in time, the game goes on"}));
}

TEST_F(GameWorld, ReadyModeBy07MessageAndByChat)
{
	g_Config.m_SvPlayerReadyMode = 1;
	SelectGameMode("dm");
	JoinPlayer(0, TEAM_GAME, "seven");
	JoinPlayer(1, TEAM_GAME, "six");
	RunTicks(this, 10);

	// the ready change of a 0.7 client
	m_pServer->m_aClients[0].m_Sixup = true;
	CUnpacker Unpacker;
	Unpacker.Reset(nullptr, 0);
	GameServer()->OnMessage(protocol7::NETMSGTYPE_CL_READYCHANGE, &Unpacker, 0);
	m_pServer->m_aClients[0].m_Sixup = false;
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::RESUME);

	// a 0.6 client says it in chat, with /pause as well
	GameServer()->Console()->ExecuteLineFlag("ready", CFGFLAG_CHAT, 1);
	EXPECT_TRUE(GameController()->ReadyMode().IsReady(1));
	RunTicks(this, m_pServer->TickSpeed());
	GameServer()->Console()->ExecuteLineFlag("pause", CFGFLAG_CHAT, 1);
	EXPECT_FALSE(GameController()->ReadyMode().IsReady(1));
}

TEST_F(GameWorld, ReadyModeLMSDoesNotWaitForTheDead)
{
	g_Config.m_SvPlayerReadyMode = 1;
	SelectGameMode("lms");
	AddSpawnPoints(this);
	JoinPlayer(0, TEAM_GAME, "first");
	JoinPlayer(1, TEAM_GAME, "second");
	CPlayer *pThird = JoinPlayer(2, TEAM_GAME, "third");
	RunCountdown(this);

	ASSERT_NE(pThird->GetCharacter(), nullptr);
	pThird->GetCharacter()->Die(2, WEAPON_SELF);
	EXPECT_TRUE(GameController()->IsPlayerDeadSpectator(2));
	EXPECT_NE(GameController()->ReadyMode().OnPlayerReadyChange(2), nullptr);
	GameController()->ReadyMode().OnPlayerReadyChange(0);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::RESUME);
	RunTicks(this, m_pServer->TickSpeed());
	GameController()->ReadyMode().OnPlayerReadyChange(0);
	GameController()->ReadyMode().OnPlayerReadyChange(1);
	RunTicks(this, 1);
	// the pause ends with the countdown of a survival mode
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::NONE);
	EXPECT_NE(SnapGameData7(this).m_GameStateFlags & protocol7::GAMESTATEFLAG_STARTCOUNTDOWN, 0);
}

TEST_F(GameWorld, ReadyModeIsNotForDDRace)
{
	g_Config.m_SvPlayerReadyMode = 1;
	JoinPlayer(0, TEAM_GAME, "racer");
	EXPECT_NE(GameController()->ReadyMode().OnPlayerReadyChange(0), nullptr);
	RunTicks(this, 10);
	EXPECT_FALSE(GameController()->IsGamePaused());
	// the DDRace modes keep their own pause command in chat
	EXPECT_NE(GameServer()->Console()->GetCommandInfo("pause", CFGFLAG_CHAT, false), nullptr);
	EXPECT_EQ(GameServer()->Console()->GetCommandInfo("ready", CFGFLAG_CHAT, false), nullptr);
	LeavePlayer(0);
}

TEST_F(GameWorld, ReadyModeTellsDDNetClientsWhoIsReady)
{
	g_Config.m_SvPlayerReadyMode = 1;
	g_Config.m_SvForceReadyAll = 2;
	SelectGameMode("dm");
	CPlayer *pFirst = JoinPlayer(0, TEAM_GAME, "first");
	JoinPlayer(1, TEAM_GAME, "second");
	JoinPlayer(2, TEAM_SPECTATORS, "spectator");
	RunTicks(this, 10);

	// the client sees the players by the ids it was told
	for(int ClientId = 0; ClientId < 3; ClientId++)
	{
		m_pServer->GetIdMap(0)[ClientId] = ClientId;
		m_pServer->GetReverseIdMap(0)[ClientId] = ClientId;
	}
	const auto &&Snap = [this](int SnappingClient, CSnapshotBuffer &Buffer) {
		m_pServer->m_SnapshotBuilder.Init(false);
		GameController()->Snap(SnappingClient);
		m_pServer->m_SnapshotBuilder.Finish(&Buffer);
		return Buffer.AsSnapshot();
	};
	CSnapshotBuffer Buffer;
	// only to the clients that asked for it
	EXPECT_EQ(Snap(0, Buffer)->FindItem(NETOBJTYPE_READYSTATE, 0), nullptr);
	pFirst->m_EnableReadyState = true;
	const auto *pState = static_cast<const CNetObj_ReadyState *>(Snap(0, Buffer)->FindItem(NETOBJTYPE_READYSTATE, 0));
	ASSERT_NE(pState, nullptr);
	// the server has ready mode even while nothing waits
	EXPECT_EQ(pState->m_Wait, READYWAIT_NONE);
	EXPECT_EQ(Snap(0, Buffer)->FindItem(NETOBJTYPE_PLAYERREADY, 0), nullptr);

	const int StartTick = m_pServer->Tick();
	CUnpacker Unpacker;
	Unpacker.Reset(nullptr, 0);
	GameServer()->OnMessage(NETMSGTYPE_CL_READYCHANGE, &Unpacker, 1);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::RESUME);
	RunTicks(this, m_pServer->TickSpeed());
	GameController()->ReadyMode().OnPlayerReadyChange(1);
	const CSnapshot *pSnapshot = Snap(0, Buffer);
	pState = static_cast<const CNetObj_ReadyState *>(pSnapshot->FindItem(NETOBJTYPE_READYSTATE, 0));
	ASSERT_NE(pState, nullptr);
	EXPECT_EQ(pState->m_Wait, READYWAIT_RESUME);
	EXPECT_EQ(pState->m_ForceReadyTick, StartTick + 2 * 60 * m_pServer->TickSpeed());
	const auto *pFirstReady = static_cast<const CNetObj_PlayerReady *>(pSnapshot->FindItem(NETOBJTYPE_PLAYERREADY, 0));
	const auto *pSecondReady = static_cast<const CNetObj_PlayerReady *>(pSnapshot->FindItem(NETOBJTYPE_PLAYERREADY, 1));
	ASSERT_NE(pFirstReady, nullptr);
	ASSERT_NE(pSecondReady, nullptr);
	EXPECT_FALSE(pFirstReady->m_Ready);
	EXPECT_TRUE(pSecondReady->m_Ready);
	// spectators are not waited for
	EXPECT_EQ(pSnapshot->FindItem(NETOBJTYPE_PLAYERREADY, 2), nullptr);
	// and server demos keep it
	EXPECT_NE(Snap(SERVER_DEMO_CLIENT, Buffer)->FindItem(NETOBJTYPE_PLAYERREADY, 1), nullptr);

	// the capability comes in a message
	CMsgPacker Packer(NETMSGTYPE_CL_ENABLEREADYSTATE, false);
	Packer.AddInt(1);
	Unpacker.Reset(Packer.Data(), Packer.Size());
	GameServer()->OnMessage(NETMSGTYPE_CL_ENABLEREADYSTATE, &Unpacker, 2);
	EXPECT_TRUE(GameServer()->m_apPlayers[2]->m_EnableReadyState);
}

namespace
{
	// a running round of zCatch with the laser, of the players 0 to Num - 1; it needs Num of them to start
	void StartZCatch(GameWorld *pWorld, int Num)
	{
		str_copy(g_Config.m_SvSpawnWeapons, "laser");
		g_Config.m_SvZcatchMinPlayers = Num;
		pWorld->SelectGameMode("zcatch");
		AddSpawnPoints(pWorld);
		for(int ClientId = 0; ClientId < Num; ClientId++)
		{
			char aName[16];
			str_format(aName, sizeof(aName), "player%d", ClientId);
			pWorld->JoinPlayer(ClientId, TEAM_GAME, aName);
		}
		// a DM starts right away
		RunTicks(pWorld, 2);
		EXPECT_EQ(SnapGameData7(pWorld).m_GameStateFlags, 0);
	}

	// the character of a player in a running round, put where nothing else hurts it
	CCharacter *ZCatchCharacter(GameWorld *pWorld, int ClientId)
	{
		CPlayer *pPlayer = pWorld->GameServer()->m_apPlayers[ClientId];
		EXPECT_NE(pPlayer, nullptr);
		if(!pPlayer)
			return nullptr;
		CCharacter *pCharacter = pWorld->Respawn(pPlayer, vec2(64.0f + 32.0f * ClientId, 96.0f));
		EXPECT_NE(pCharacter, nullptr);
		return pCharacter;
	}

	bool IsCaught(GameWorld *pWorld, int ClientId)
	{
		return pWorld->GameController()->PlayerAutoRespawnTick(pWorld->GameServer()->m_apPlayers[ClientId]) == std::numeric_limits<int>::max();
	}

	void Hit(GameWorld *pWorld, int VictimId, int AttackerId)
	{
		CCharacter *pVictim = pWorld->GameServer()->m_apPlayers[VictimId]->GetCharacter();
		ASSERT_NE(pVictim, nullptr);
		pVictim->TakeDamage(vec2(), 0, AttackerId, WEAPON_LASER);
		ASSERT_FALSE(pVictim->IsAlive());
	}
}

TEST_F(GameWorld, ZCatchPlaysGrenadeOrLaser)
{
	SelectGameMode("zcatch");
	CCharacter *pGrenade = SpawnPlayer(0, vec2(64.0f, 96.0f));
	ASSERT_NE(pGrenade, nullptr);
	EXPECT_EQ(pGrenade->GetActiveWeapon(), WEAPON_GRENADE);
	EXPECT_FALSE(pGrenade->GetWeaponGot(WEAPON_LASER));
	str_copy(g_Config.m_SvSpawnWeapons, "laser");
	CCharacter *pLaser = SpawnPlayer(1, vec2(96.0f, 96.0f));
	ASSERT_NE(pLaser, nullptr);
	EXPECT_EQ(pLaser->GetActiveWeapon(), WEAPON_LASER);
	EXPECT_FALSE(pLaser->GetWeaponGot(WEAPON_GRENADE));
}

TEST_F(GameWorld, ZCatchIsAReleaseGameWithTooFewPlayers)
{
	str_copy(g_Config.m_SvSpawnWeapons, "laser");
	SelectGameMode("zcatch");
	AddSpawnPoints(this);
	JoinPlayer(0, TEAM_GAME, "first");
	JoinPlayer(1, TEAM_GAME, "second");
	RunTicks(this, 10);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, protocol7::GAMESTATEFLAG_WARMUP);
	ZCatchCharacter(this, 0);
	ZCatchCharacter(this, 1);
	Hit(this, 1, 0);
	EXPECT_FALSE(IsCaught(this, 1));

	// the fifth one starts the round
	for(int ClientId = 2; ClientId < 5; ClientId++)
		JoinPlayer(ClientId, TEAM_GAME, "more");
	RunTicks(this, 2);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, 0);
	for(int ClientId = 0; ClientId < 5; ClientId++)
		ZCatchCharacter(this, ClientId);
	Hit(this, 1, 0);
	Hit(this, 2, 0);
	EXPECT_TRUE(IsCaught(this, 1));
	EXPECT_EQ(GameServer()->m_apPlayers[1]->SpectatorId(), 0);

	// with fewer players the round goes on only while the leader can still catch 4
	LeavePlayer(4);
	GameController()->Tick();
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, 0);
	EXPECT_TRUE(IsCaught(this, 1));
	LeavePlayer(3);
	GameController()->Tick();
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, protocol7::GAMESTATEFLAG_WARMUP);
	EXPECT_FALSE(IsCaught(this, 1));
	EXPECT_FALSE(IsCaught(this, 2));
	EXPECT_EQ(ReceivedMatchReport(0).m_Report.m_Termination, EMatchTermination::ABORTED);

	// and never with sv_release_game
	g_Config.m_SvReleaseGame = 1;
	for(int ClientId = 3; ClientId < 5; ClientId++)
		JoinPlayer(ClientId, TEAM_GAME, "more");
	RunTicks(this, 10);
	EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, protocol7::GAMESTATEFLAG_WARMUP);
}

TEST_F(GameWorld, ZCatchADeathFreesOnlyWhomTheCatcherHeld)
{
	StartZCatch(this, 4);
	for(int ClientId = 0; ClientId < 4; ClientId++)
		ZCatchCharacter(this, ClientId);
	Hit(this, 1, 0);
	Hit(this, 2, 3);
	EXPECT_TRUE(IsCaught(this, 1));
	EXPECT_TRUE(IsCaught(this, 2));
	Hit(this, 0, 3);
	EXPECT_FALSE(IsCaught(this, 1));
	EXPECT_TRUE(IsCaught(this, 2));
	EXPECT_TRUE(IsCaught(this, 0));
	// as do the ones of the other catcher when they die
	Hit(this, 3, 1);
	EXPECT_FALSE(IsCaught(this, 0));
	EXPECT_FALSE(IsCaught(this, 2));
}

TEST_F(GameWorld, ZCatchKillKeyLetsTheLastCaughtGo)
{
	g_Config.m_SvKillDelay = 0;
	StartZCatch(this, 4);
	for(int ClientId = 0; ClientId < 4; ClientId++)
		ZCatchCharacter(this, ClientId);
	Hit(this, 1, 0);
	Hit(this, 2, 0);
	GameController()->OnPlayerKill(0);
	EXPECT_TRUE(IsCaught(this, 1));
	EXPECT_FALSE(IsCaught(this, 2));
	ASSERT_NE(GameServer()->m_apPlayers[0]->GetCharacter(), nullptr);

	// who joins is caught by the leader without counting, and the last kill that counts lets everybody go
	JoinPlayer(4, TEAM_GAME, "late");
	EXPECT_TRUE(IsCaught(this, 4));
	EXPECT_EQ(GameServer()->m_apPlayers[4]->SpectatorId(), 0);
	GameController()->OnPlayerKill(0);
	EXPECT_FALSE(IsCaught(this, 4));
	EXPECT_FALSE(IsCaught(this, 1));

	// without anybody caught it kills, for three points
	const int Score = GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, GameServer()->m_apPlayers[0]);
	GameController()->OnPlayerKill(0);
	EXPECT_EQ(GameServer()->m_apPlayers[0]->GetCharacter(), nullptr);
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, GameServer()->m_apPlayers[0]), Score - 3);
	GameController()->EndRound();
	const CReceivedMatchReport Received = ReceivedMatchReport(0);
	EXPECT_EQ(Received.Metric(Received.m_LocalParticipantId, "releases"), 3);
}

TEST_F(GameWorld, ZCatchCatchingACatcherGivesAPointMore)
{
	StartZCatch(this, 4);
	for(int ClientId = 0; ClientId < 4; ClientId++)
		ZCatchCharacter(this, ClientId);
	Hit(this, 2, 1);
	Hit(this, 1, 0);
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, GameServer()->m_apPlayers[0]), 2);
	Hit(this, 3, 0);
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, GameServer()->m_apPlayers[0]), 3);
}

TEST_F(GameWorld, ZCatchTheLastOneStandingWins)
{
	StartZCatch(this, 5);
	for(int ClientId = 0; ClientId < 5; ClientId++)
		ZCatchCharacter(this, ClientId);
	for(int ClientId = 1; ClientId < 5; ClientId++)
		Hit(this, ClientId, 0);
	m_pServer->AdvanceTick(5);
	GameController()->Tick();
	EXPECT_TRUE(GameController()->IsGamePaused());
	// 4 kills, 1 win point on top
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, GameServer()->m_apPlayers[0]), 5);
	const CReceivedMatchReport Received = ReceivedMatchReport(1);
	EXPECT_EQ(Received.m_Report.m_Termination, EMatchTermination::COMPLETED);
	const CMatchParticipant *pWinner = Received.Participant("player0");
	ASSERT_NE(pWinner, nullptr);
	EXPECT_EQ(Received.Metric(pWinner->m_ParticipantId, "win_points"), 1);
	EXPECT_EQ(Received.Metric(pWinner->m_ParticipantId, "catches"), 4);
	EXPECT_EQ(Received.Metric(Received.m_LocalParticipantId, "caught_ticks"), 5);
	const CMatchStanding *pStanding = Received.m_Report.Standing(EMatchSubjectKind::PARTICIPANT, pWinner->m_ParticipantId);
	ASSERT_NE(pStanding, nullptr);
	EXPECT_EQ(pStanding->m_Rank, 1);
	EXPECT_EQ(pStanding->m_Outcome, EMatchOutcome::WIN);
}

TEST_F(GameWorld, ZCatchNobodyWinsWithoutEnoughKills)
{
	StartZCatch(this, 3);
	for(int ClientId = 0; ClientId < 3; ClientId++)
		ZCatchCharacter(this, ClientId);
	Hit(this, 1, 0);
	LeavePlayer(2);
	// the leader could still win
	GameController()->Tick();
	EXPECT_EQ(ReceivedMatchReport(0).m_Report.m_Termination, EMatchTermination::ABORTED);
	EXPECT_FALSE(IsCaught(this, 1));
}

TEST_F(GameWorld, ZCatchSpectatingDoesNotFreeACaughtPlayer)
{
	StartZCatch(this, 3);
	for(int ClientId = 0; ClientId < 3; ClientId++)
		ZCatchCharacter(this, ClientId);
	Hit(this, 1, 0);
	GameController()->OnPlayerSetTeam(1, TEAM_SPECTATORS);
	EXPECT_EQ(GameServer()->m_apPlayers[1]->GetTeam(), TEAM_GAME);
	EXPECT_TRUE(IsCaught(this, 1));
	Hit(this, 0, 2);
	EXPECT_EQ(GameServer()->m_apPlayers[1]->GetTeam(), TEAM_SPECTATORS);
}

TEST_F(GameWorld, ZCatchColorsShowTheKillsThatCount)
{
	StartZCatch(this, 3);
	for(int ClientId = 0; ClientId < 3; ClientId++)
		ZCatchCharacter(this, ClientId);
	CPlayer *pCatcher = GameServer()->m_apPlayers[0];
	pCatcher->SetTeeInfos("default", true, 0x123456, 0x654321);
	GameController()->Tick();
	EXPECT_EQ(pCatcher->TeeInfos().m_ColorBody, ZCatch::BodyColor(ZCatch::EColors::TEETIME, 0));
	Hit(this, 1, 0);
	EXPECT_EQ(pCatcher->TeeInfos().m_ColorBody, ZCatch::BodyColor(ZCatch::EColors::TEETIME, 1));
	EXPECT_EQ(pCatcher->TeeInfos().m_ColorFeet, 0x654321);
	EXPECT_EQ(pCatcher->OwnTeeInfos().m_ColorBody, 0x123456);
	str_copy(g_Config.m_SvZcatchColors, "savander");
	GameController()->Tick();
	EXPECT_EQ(pCatcher->TeeInfos().m_ColorBody, ZCatch::BodyColor(ZCatch::EColors::SAVANDER, 1));
}

TEST_F(GameWorld, ZCatchDeadSpectatorPresentationIsProtocolAware)
{
	constexpr int VictimId = 0;
	constexpr int CatcherId = 1;
	StartZCatch(this, 2);
	CCharacter *pVictimCharacter = ZCatchCharacter(this, VictimId);
	CPlayer *pVictim = pVictimCharacter->GetPlayer();

	// A 0.6 client that can address every slot sees the real ids, a 0.7 one
	// always goes through the player map, so build the map per protocol.
	const auto ExpectedIds = [&](bool Sixup) {
		m_pServer->m_aClients[VictimId].m_Sixup = Sixup;
		GameServer()->m_PlayerMapping.InitPlayerMap(CatcherId);
		GameServer()->m_PlayerMapping.InitPlayerMap(VictimId);
		int Victim = VictimId;
		EXPECT_TRUE(m_pServer->Translate(Victim, VictimId));
		int Catcher = CatcherId;
		if(!m_pServer->Translate(Catcher, VictimId))
		{
			Catcher = Victim == 0 ? 1 : 0;
			m_pServer->GetIdMap(VictimId)[Catcher] = CatcherId;
			m_pServer->GetReverseIdMap(VictimId)[CatcherId] = Catcher;
		}
		return std::make_pair(Victim, Catcher);
	};

	pVictimCharacter->TakeDamage(vec2(), 0, CatcherId, WEAPON_LASER);
	EXPECT_EQ(pVictim->GetTeam(), TEAM_GAME);
	EXPECT_EQ(pVictim->SpectatorId(), CatcherId);

	const auto [TranslatedVictimId, TranslatedCatcherId] = ExpectedIds(false);
	m_pServer->m_SnapshotBuilder.Init(false);
	pVictim->Snap(VictimId);
	CSnapshotBuffer SixBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&SixBuffer);
	const CSnapshot *pSixSnapshot = SixBuffer.AsSnapshot();
	const auto *pSixPlayerInfo = static_cast<const CNetObj_PlayerInfo *>(pSixSnapshot->FindItem(NETOBJTYPE_PLAYERINFO, TranslatedVictimId));
	const auto *pSixSpectatorInfo = static_cast<const CNetObj_SpectatorInfo *>(pSixSnapshot->FindItem(NETOBJTYPE_SPECTATORINFO, VictimId));
	ASSERT_NE(pSixPlayerInfo, nullptr);
	ASSERT_NE(pSixSpectatorInfo, nullptr);
	EXPECT_EQ(pSixPlayerInfo->m_Team, TEAM_SPECTATORS);
	EXPECT_EQ(pSixSpectatorInfo->m_SpectatorId, TranslatedCatcherId);

	const auto [SevenVictimId, SevenCatcherId] = ExpectedIds(true);
	m_pServer->m_SnapshotBuilder.Init(true);
	pVictim->Snap(VictimId);
	CSnapshotBuffer SevenBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&SevenBuffer);
	const CSnapshot *pSevenSnapshot = SevenBuffer.AsSnapshot();
	const auto *pSevenPlayerInfo = static_cast<const protocol7::CNetObj_PlayerInfo *>(pSevenSnapshot->FindItem(protocol7::NETOBJTYPE_PLAYERINFO, SevenVictimId));
	const auto *pSevenSpectatorInfo = static_cast<const protocol7::CNetObj_SpectatorInfo *>(pSevenSnapshot->FindItem(protocol7::NETOBJTYPE_SPECTATORINFO, VictimId));
	ASSERT_NE(pSevenPlayerInfo, nullptr);
	ASSERT_NE(pSevenSpectatorInfo, nullptr);
	EXPECT_NE(pSevenPlayerInfo->m_PlayerFlags & protocol7::PLAYERFLAG_DEAD, 0);
	EXPECT_EQ(pSevenSpectatorInfo->m_SpecMode, protocol7::SPEC_PLAYER);
	EXPECT_EQ(pSevenSpectatorInfo->m_SpectatorId, SevenCatcherId);
}

TEST_F(GameWorld, Catch16WhoIsHitComesBackInTheColourOfTheShooter)
{
	str_copy(g_Config.m_SvSpawnWeapons, "laser");
	SelectGameMode("catch16");
	AddSpawnPoints(this);
	for(int ClientId = 0; ClientId < 3; ClientId++)
		JoinPlayer(ClientId, TEAM_GAME, ClientId == 0 ? "founder" : "other");
	CPlayer *pVictim = GameServer()->m_apPlayers[1];
	pVictim->SetTeeInfos("default", true, 0x123456, 0x654321);
	for(int ClientId = 0; ClientId < 3; ClientId++)
		ZCatchCharacter(this, ClientId);
	Hit(this, 1, 0);
	GameController()->Tick();
	EXPECT_EQ(pVictim->TeeInfos().m_ColorBody, Catch16::GroupColors(0).m_Body);
	EXPECT_EQ(pVictim->OwnTeeInfos().m_ColorBody, 0x123456);
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, GameServer()->m_apPlayers[0]), 1);
	RunTicks(this, 3);
	ASSERT_NE(pVictim->GetCharacter(), nullptr);

	// the group only pushes itself
	CCharacter *pMember = ZCatchCharacter(this, 1);
	pMember->TakeDamage(vec2(), 0, 0, WEAPON_LASER);
	EXPECT_TRUE(pMember->IsAlive());

	// and wins with the last one
	ZCatchCharacter(this, 0);
	Hit(this, 2, 1);
	GameController()->Tick();
	EXPECT_TRUE(GameController()->IsGamePaused());
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, GameServer()->m_apPlayers[0]), 1 + 5);
	const CReceivedMatchReport Received = ReceivedMatchReport(0);
	const CMatchStanding *pStanding = Received.m_Report.Standing(EMatchSubjectKind::PARTICIPANT, Received.m_LocalParticipantId);
	ASSERT_NE(pStanding, nullptr);
	EXPECT_EQ(pStanding->m_Outcome, EMatchOutcome::WIN);
	EXPECT_EQ(Received.Metric(Received.m_LocalParticipantId, "converts"), 1);
}

TEST_F(GameWorld, Catch16NewPlayersJoinTheBiggestGroupAndFounderLeavingSplitsIt)
{
	str_copy(g_Config.m_SvSpawnWeapons, "laser");
	SelectGameMode("catch16");
	AddSpawnPoints(this);
	for(int ClientId = 0; ClientId < 3; ClientId++)
		JoinPlayer(ClientId, TEAM_GAME, "player");
	for(int ClientId = 0; ClientId < 3; ClientId++)
		ZCatchCharacter(this, ClientId);
	Hit(this, 1, 0);
	JoinPlayer(3, TEAM_GAME, "late");
	GameController()->Tick();
	EXPECT_EQ(GameServer()->m_apPlayers[3]->TeeInfos().m_ColorBody, Catch16::GroupColors(0).m_Body);
	LeavePlayer(0);
	GameController()->Tick();
	EXPECT_EQ(GameServer()->m_apPlayers[1]->TeeInfos().m_ColorBody, Catch16::GroupColors(1).m_Body);
	EXPECT_EQ(GameServer()->m_apPlayers[3]->TeeInfos().m_ColorBody, Catch16::GroupColors(3).m_Body);
}

namespace
{
	class CTestFng : public CGameControllerFng<CGameControllerVanillaTDM, WEAPON_LASER>
	{
	public:
		using CGameControllerFng::CGameControllerFng;
		using CGameControllerFng::OnSpike;
	};

	// red 0 and 2 against blue 1 and 3, all standing
	CTestFng &StartFng(GameWorld *pWorld)
	{
		CTestFng &Controller = pWorld->SelectController<CTestFng>("fng");
		for(int ClientId = 0; ClientId < 4; ClientId++)
		{
			pWorld->JoinPlayer(ClientId, ClientId % 2 == 0 ? TEAM_RED : TEAM_BLUE, "player");
			ZCatchCharacter(pWorld, ClientId);
		}
		return Controller;
	}

	int Score(GameWorld *pWorld, int ClientId)
	{
		return pWorld->GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pWorld->GameServer()->m_apPlayers[ClientId]);
	}

	CCharacter *Character(GameWorld *pWorld, int ClientId)
	{
		return pWorld->GameServer()->m_apPlayers[ClientId]->GetCharacter();
	}
}

TEST_F(GameWorld, FngHitFreezesInsteadOfKilling)
{
	StartFng(this);
	EXPECT_TRUE(Character(this, 0)->GetWeaponGot(WEAPON_HAMMER));
	EXPECT_EQ(Character(this, 0)->GetActiveWeapon(), WEAPON_LASER);
	Character(this, 1)->TakeDamage(vec2(), 0, 0, WEAPON_LASER);
	ASSERT_NE(Character(this, 1), nullptr);
	EXPECT_EQ(Character(this, 1)->m_FreezeTime, 10 * SERVER_TICK_SPEED);
	EXPECT_EQ(Score(this, 0), 1);
	EXPECT_EQ(GameController()->TeamScore(TEAM_RED), 1);
	// a frozen tee is not frozen again
	Character(this, 1)->TakeDamage(vec2(), 0, 2, WEAPON_LASER);
	EXPECT_EQ(Score(this, 2), 0);
	// and cannot kill itself
	GameController()->OnPlayerKill(1);
	EXPECT_NE(Character(this, 1), nullptr);
}

TEST_F(GameWorld, FngSpikesScoreForWhoFroze)
{
	CTestFng &Controller = StartFng(this);
	Character(this, 1)->TakeDamage(vec2(), 0, 0, WEAPON_LASER);
	Controller.OnSpike(Character(this, 1), Fng::ESpike::NORMAL);
	EXPECT_EQ(Character(this, 1), nullptr);
	EXPECT_EQ(Score(this, 0), 1 + 3);
	EXPECT_EQ(GameController()->TeamScore(TEAM_RED), 1 + 5);

	// a tee that is not frozen just dies
	Controller.OnSpike(Character(this, 3), Fng::ESpike::NORMAL);
	EXPECT_EQ(Character(this, 3), nullptr);
	EXPECT_EQ(Score(this, 0), 4);
	GameController()->EndRound();
	const CReceivedMatchReport Received = ReceivedMatchReport(0);
	EXPECT_EQ(Received.Metric(Received.m_LocalParticipantId, "freezes"), 1);
	EXPECT_EQ(Received.Metric(Received.m_LocalParticipantId, "spike_kills"), 1);
}

TEST_F(GameWorld, FngTheSpikesOfTheOtherTeamCost)
{
	CTestFng &Controller = StartFng(this);
	Character(this, 1)->TakeDamage(vec2(), 0, 0, WEAPON_LASER);
	Controller.OnSpike(Character(this, 1), Fng::ESpike::BLUE);
	EXPECT_EQ(Score(this, 0), 1 - 5);
	EXPECT_EQ(GameController()->TeamScore(TEAM_RED), 1);
	EXPECT_EQ(Character(this, 0)->m_FreezeTime, 10 * SERVER_TICK_SPEED);

	// the own team's spikes are worth more
	Character(this, 3)->TakeDamage(vec2(), 0, 2, WEAPON_LASER);
	Controller.OnSpike(Character(this, 3), Fng::ESpike::RED);
	EXPECT_EQ(Score(this, 2), 1 + 5);
	EXPECT_EQ(GameController()->TeamScore(TEAM_RED), 1 + 1 + 10);
}

TEST_F(GameWorld, FngATeamMateTouchProtects)
{
	CTestFng &Controller = StartFng(this);
	Character(this, 1)->TakeDamage(vec2(), 0, 0, WEAPON_LASER);
	Character(this, 1)->TakeDamage(vec2(1.0f, 0.0f), 0, 3, WEAPON_HAMMER);
	Controller.OnSpike(Character(this, 1), Fng::ESpike::NORMAL);
	EXPECT_EQ(Score(this, 0), 1);
	EXPECT_EQ(Score(this, 3), 0);
}

TEST_F(GameWorld, FngTheLastTouchGetsTheKill)
{
	CTestFng &Controller = StartFng(this);
	Character(this, 1)->TakeDamage(vec2(), 0, 0, WEAPON_LASER);
	Character(this, 1)->TakeDamage(vec2(1.0f, 0.0f), 0, 2, WEAPON_HAMMER);
	Controller.OnSpike(Character(this, 1), Fng::ESpike::GOLD);
	EXPECT_EQ(Score(this, 0), 1);
	EXPECT_EQ(Score(this, 2), 6);
}

TEST_F(GameWorld, FngTheHammerMeltsFrozenTeamMates)
{
	StartFng(this);
	CCharacter *pMate = Character(this, 2);
	pMate->Freeze(10);
	pMate->TakeDamage(vec2(), 0, 0, WEAPON_HAMMER);
	EXPECT_EQ(pMate->m_FreezeTime, 7 * SERVER_TICK_SPEED);
	EXPECT_EQ(Score(this, 0), 0);
	pMate->m_FreezeTime = SERVER_TICK_SPEED;
	pMate->TakeDamage(vec2(), 0, 0, WEAPON_HAMMER);
	EXPECT_EQ(pMate->m_FreezeTime, 2);
	EXPECT_EQ(Score(this, 0), 1);
	// and never freezes or hurts
	Character(this, 1)->TakeDamage(vec2(), 3, 0, WEAPON_HAMMER);
	EXPECT_EQ(Character(this, 1)->m_FreezeTime, 0);
	EXPECT_EQ(Character(this, 1)->GetHealth(), 10);
}

TEST_F(GameWorld, FngFamily)
{
	SelectGameMode("solofng");
	EXPECT_FALSE(GameController()->IsTeamPlay());
	CCharacter *pLaser = SpawnPlayer(0, vec2(64.0f, 96.0f));
	ASSERT_NE(pLaser, nullptr);
	EXPECT_EQ(pLaser->GetActiveWeapon(), WEAPON_LASER);
	DeletePlayers();
	SelectGameMode("bolofng");
	CCharacter *pGrenade = SpawnPlayer(0, vec2(64.0f, 96.0f));
	ASSERT_NE(pGrenade, nullptr);
	EXPECT_EQ(pGrenade->GetActiveWeapon(), WEAPON_GRENADE);
	DeletePlayers();
	SelectGameMode("boomfng");
	EXPECT_TRUE(GameController()->IsTeamPlay());
	const int Flags = GameController()->GameInfoFlags(-1);
	EXPECT_NE(Flags & GAMEINFOFLAG_GAMETYPE_FNG, 0);
	EXPECT_NE(Flags & GAMEINFOFLAG_ENTITIES_FNG, 0);
	EXPECT_EQ(Flags & GAMEINFOFLAG_PREDICT_FNG, 0);
	g_Config.m_SvFngHammer = 1;
	EXPECT_NE(GameController()->GameInfoFlags(-1) & GAMEINFOFLAG_PREDICT_FNG, 0);
}

// What the DDNet client is told about every PvP mode, see the flag table of the report.
TEST_F(GameWorld, PvPModesTellTheClientWhatTheyAre)
{
	constexpr int VanillaFlags = GAMEINFOFLAG_GAMETYPE_VANILLA | GAMEINFOFLAG_BUG_VANILLA_BOUNCE | GAMEINFOFLAG_PREDICT_VANILLA | GAMEINFOFLAG_ENTITIES_VANILLA | GAMEINFOFLAG_DONT_MASK_ENTITIES;
	constexpr int InstagibFlags = VanillaFlags | GAMEINFOFLAG_UNLIMITED_AMMO;
	constexpr int FngFlags = InstagibFlags | GAMEINFOFLAG_GAMETYPE_FNG | GAMEINFOFLAG_ENTITIES_FNG;
	constexpr int HudFlags = GAMEINFOFLAG2_HUD_AMMO | GAMEINFOFLAG2_HUD_HEALTH_ARMOR | GAMEINFOFLAG2_PREDICT_EVENTS;
	// a hit decides, health and armour say nothing
	constexpr int HudInstagibFlags = GAMEINFOFLAG2_HUD_AMMO | GAMEINFOFLAG2_PREDICT_EVENTS;
	struct SMode
	{
		const char *m_pName;
		int m_Flags;
		int m_Flags2;
		// the PvP layer, which allows zooming with sv_allow_zoom
		bool m_PvP;
	};
	const SMode aModes[] = {
		{"dm", VanillaFlags, HudFlags, false},
		{"tdm", VanillaFlags, HudFlags, false},
		{"ctf", VanillaFlags, HudFlags, false},
		{"lms", VanillaFlags, HudFlags, false},
		{"lts", VanillaFlags, HudFlags, false},
		{"idm", InstagibFlags, HudInstagibFlags, true},
		{"itdm", InstagibFlags, HudInstagibFlags, true},
		{"ictf", InstagibFlags, HudInstagibFlags, true},
		{"gdm", InstagibFlags, HudInstagibFlags, true},
		{"gtdm", InstagibFlags, HudInstagibFlags, true},
		{"gctf", InstagibFlags, HudInstagibFlags, true},
		{"zcatch", InstagibFlags, HudInstagibFlags, true},
		{"catch16", InstagibFlags, HudInstagibFlags, true},
		{"fng", FngFlags, HudInstagibFlags, true},
		{"boomfng", FngFlags, HudInstagibFlags, true},
		{"solofng", FngFlags, HudInstagibFlags, true},
		{"bolofng", FngFlags, HudInstagibFlags, true},
	};
	for(const SMode &Mode : aModes)
	{
		SCOPED_TRACE(Mode.m_pName);
		g_Config.m_SvAllowZoom = 0;
		g_Config.m_SvFngHammer = 0;
		SelectGameMode(Mode.m_pName);
		EXPECT_EQ(GameController()->GameInfoFlags(-1), Mode.m_Flags);
		EXPECT_EQ(GameController()->GameInfoFlags2(-1), Mode.m_Flags2);
		g_Config.m_SvAllowZoom = 1;
		EXPECT_EQ(GameController()->GameInfoFlags(-1), Mode.m_PvP ? Mode.m_Flags | GAMEINFOFLAG_ALLOW_ZOOM : Mode.m_Flags);
		// only with the hammer the client predicts
		g_Config.m_SvAllowZoom = 0;
		g_Config.m_SvFngHammer = 1;
		EXPECT_EQ(GameController()->GameInfoFlags(-1), Mode.m_Flags & GAMEINFOFLAG_GAMETYPE_FNG ? Mode.m_Flags | GAMEINFOFLAG_PREDICT_FNG : Mode.m_Flags);
	}
}

TEST_F(GameWorld, ReadyModeWaitsInEveryPvPMode)
{
	g_Config.m_SvPlayerReadyMode = 1;
	g_Config.m_SvCountdown = -1;
	const int TickSpeed = m_pServer->TickSpeed();
	for(const char *pMode : {"idm", "itdm", "ictf", "gdm", "gtdm", "gctf", "zcatch", "catch16", "fng", "boomfng", "solofng", "bolofng"})
	{
		SCOPED_TRACE(pMode);
		g_Config.m_SvZcatchMinPlayers = 2;
		SelectGameMode(pMode);
		AddSpawnPoints(this);
		JoinPlayer(0, TEAM_GAME, "first");
		JoinPlayer(1, TEAM_GAME, "second");
		RunTicks(this, 2);

		// the match waits for everybody to be ready
		GameController()->RestartAfterWarmup(-1);
		RunTicks(this, 1);
		EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::START);
		EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, protocol7::GAMESTATEFLAG_WARMUP);
		EXPECT_EQ(GameController()->ReadyMode().OnPlayerReadyChange(0), nullptr);
		RunTicks(this, 1);
		EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::START);
		EXPECT_EQ(GameController()->ReadyMode().OnPlayerReadyChange(1), nullptr);
		RunTicks(this, 2);
		EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::NONE);
		EXPECT_EQ(SnapGameData7(this).m_GameStateFlags, 0);

		// and a player who is not ready any more pauses it
		RunTicks(this, TickSpeed);
		EXPECT_EQ(GameController()->ReadyMode().OnPlayerReadyChange(0), nullptr);
		EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::RESUME);
		EXPECT_TRUE(GameController()->IsGamePaused());
		RunTicks(this, TickSpeed);
		GameController()->ReadyMode().OnPlayerReadyChange(0);
		GameController()->ReadyMode().OnPlayerReadyChange(1);
		RunTicks(this, 2);
		EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::NONE);
		EXPECT_FALSE(GameController()->IsGamePaused());

		LeavePlayer(0);
		LeavePlayer(1);
	}
}

TEST_F(GameWorld, ReadyModeZCatchDoesNotWaitForTheCaught)
{
	g_Config.m_SvPlayerReadyMode = 1;
	g_Config.m_SvCountdown = -1;
	StartZCatch(this, 3);
	for(int ClientId = 0; ClientId < 3; ClientId++)
		ZCatchCharacter(this, ClientId);
	Hit(this, 2, 0);
	ASSERT_TRUE(IsCaught(this, 2));
	EXPECT_TRUE(GameController()->IsPlayerDeadSpectator(2));

	// who is caught is not in the game until the catcher dies
	EXPECT_NE(GameController()->ReadyMode().OnPlayerReadyChange(2), nullptr);
	EXPECT_EQ(GameController()->ReadyMode().OnPlayerReadyChange(1), nullptr);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::RESUME);
	RunTicks(this, m_pServer->TickSpeed());
	GameController()->ReadyMode().OnPlayerReadyChange(0);
	GameController()->ReadyMode().OnPlayerReadyChange(1);
	RunTicks(this, 1);
	EXPECT_EQ(GameController()->ReadyMode().Wait(), CReadyCheck::EWait::NONE);
	EXPECT_FALSE(GameController()->IsGamePaused());
	// and stays caught
	EXPECT_TRUE(IsCaught(this, 2));
}

TEST_F(GameWorld, PvPAnticamperClockStandsStillInAPause)
{
	g_Config.m_SvAnticamper = 1;
	g_Config.m_SvAnticamperTime = 5;
	g_Config.m_SvAnticamperFreeze = 7;
	g_Config.m_SvCountdown = -1;
	SelectGameMode("idm");
	CCharacter *pCamper = SpawnPlayer(0, vec2(64.0f, 96.0f));
	ASSERT_NE(pCamper, nullptr);
	for(int Tick = 0; Tick < 3 * SERVER_TICK_SPEED; Tick++)
	{
		m_pServer->AdvanceTick(1);
		GameController()->Tick();
	}
	// a pause longer than the time to camp, as one that waits for the players to be ready
	GameController()->DoPause(-1);
	for(int Tick = 0; Tick < 10 * SERVER_TICK_SPEED; Tick++)
	{
		m_pServer->AdvanceTick(1);
		GameController()->Tick();
	}
	GameController()->DoPause(0);
	for(int Tick = 0; Tick < 3 * SERVER_TICK_SPEED; Tick++)
	{
		m_pServer->AdvanceTick(1);
		GameController()->Tick();
	}
	EXPECT_FALSE(GameController()->IsGamePaused());
	EXPECT_EQ(pCamper->m_FreezeTime, 0);
	// the clock started anew after the pause
	for(int Tick = 0; Tick <= 2 * SERVER_TICK_SPEED; Tick++)
	{
		m_pServer->AdvanceTick(1);
		GameController()->Tick();
	}
	EXPECT_EQ(pCamper->m_FreezeTime, 7 * SERVER_TICK_SPEED);
}
