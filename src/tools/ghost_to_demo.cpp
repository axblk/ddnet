/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <base/fs.h>
#include <base/hash.h>
#include <base/log.h>
#include <base/logger.h>
#include <base/os.h>
#include <base/str.h>

#include <engine/client/ghost.h>
#include <engine/shared/demo.h>
#include <engine/shared/snapshot.h>
#include <engine/storage.h>

#include <generated/protocol.h>

#include <game/gamecore.h>
#include <game/ghost_data.h>
#include <game/version.h>

#include <zlib.h>

#include <memory>
#include <vector>

// A ghost is a race written down: one character per tick, a skin and a name,
// and the map it was run on. A demo is the same race as the client records it,
// which is one snapshot per tick. Turning the one into the other is therefore
// mostly copying - and what it buys is every demo tool there is: a demo of a
// ghost can be watched, scrubbed through, and rendered into a video.
//
// The tee is the local player of the demo, so a player follows it without
// having to be told to.

namespace
{
	constexpr const char *TOOL_NAME = "ghost_to_demo";
	// The one player there is in a ghost.
	constexpr int GHOST_CLIENT_ID = 0;

	void PrintUsage(const char *pProgramName)
	{
		log_info(TOOL_NAME, "Usage: %s <ghost.gho> [-o <demo>] [--map <map.map>] [--no-map-data]", pProgramName);
		log_info(TOOL_NAME, "  -o <demo>      Where to write the demo (default: beside the ghost)");
		log_info(TOOL_NAME, "  --map <file>   The map the ghost was run on, if it is not in the data directory");
		log_info(TOOL_NAME, "  --no-map-data  Do not write the map into the demo, which then only plays");
		log_info(TOOL_NAME, "                 where the map is already there");
	}

	// What is in the ghost, once it has been read.
	struct SGhost
	{
		CGhostSkin m_Skin = {};
		std::vector<CGhostCharacter> m_vPath;
		int m_StartTick = -1;
		char m_aOwner[MAX_NAME_LENGTH] = {};
		char m_aMap[64] = {};
	};

	bool ReadGhost(CGhostLoader &Loader, const char *pFilename, SGhost *pGhost)
	{
		if(!Loader.LoadAnyMap(pFilename, IStorage::TYPE_ALL_OR_ABSOLUTE))
		{
			return false;
		}

		const CGhostInfo *pInfo = Loader.GetInfo();
		str_copy(pGhost->m_aOwner, pInfo->m_aOwner);
		str_copy(pGhost->m_aMap, pInfo->m_aMap);
		pGhost->m_vPath.reserve(pInfo->m_NumTicks);

		bool FoundSkin = false;
		bool FoundCharacterNoTick = false;
		bool FoundCharacterTick = false;
		int Type;
		while(Loader.ReadNextType(&Type))
		{
			if(Type == GHOSTDATA_TYPE_SKIN && !FoundSkin)
			{
				if(!Loader.ReadData(Type, &pGhost->m_Skin, sizeof(CGhostSkin)))
				{
					log_error(TOOL_NAME, "Failed to read the ghost's skin");
					return false;
				}
				FoundSkin = true;
			}
			else if(Type == GHOSTDATA_TYPE_CHARACTER_NO_TICK || Type == GHOSTDATA_TYPE_CHARACTER)
			{
				const bool WithTick = Type == GHOSTDATA_TYPE_CHARACTER;
				if(WithTick ? FoundCharacterNoTick : FoundCharacterTick)
				{
					log_error(TOOL_NAME, "The ghost mixes characters with and without a tick");
					return false;
				}
				CGhostCharacter &Character = pGhost->m_vPath.emplace_back();
				if(!Loader.ReadData(Type, &Character, WithTick ? sizeof(CGhostCharacter) : sizeof(CGhostCharacter_NoTick)))
				{
					log_error(TOOL_NAME, "Failed to read the ghost's character of tick %d", (int)pGhost->m_vPath.size());
					return false;
				}
				FoundCharacterTick = FoundCharacterTick || WithTick;
				FoundCharacterNoTick = FoundCharacterNoTick || !WithTick;
			}
			else if(Type == GHOSTDATA_TYPE_START_TICK)
			{
				if(!Loader.ReadData(Type, &pGhost->m_StartTick, sizeof(int)))
				{
					log_error(TOOL_NAME, "Failed to read the ghost's start tick");
					return false;
				}
			}
		}
		Loader.Close();

		if((int)pGhost->m_vPath.size() != pInfo->m_NumTicks)
		{
			log_error(TOOL_NAME, "The ghost has %d ticks where its header says %d", (int)pGhost->m_vPath.size(), pInfo->m_NumTicks);
			return false;
		}
		if(pGhost->m_vPath.empty())
		{
			log_error(TOOL_NAME, "The ghost has no ticks");
			return false;
		}

		// The oldest ghosts carry no tick of their own, so the ticks are counted
		// out from where a shot was fired, exactly as the client counts them.
		if(FoundCharacterNoTick)
		{
			int StartTick = 0;
			for(size_t i = 1; i < pGhost->m_vPath.size(); ++i)
			{
				if(pGhost->m_vPath[i].m_AttackTick != pGhost->m_vPath[i - 1].m_AttackTick)
				{
					StartTick = pGhost->m_vPath[i].m_AttackTick - i;
				}
			}
			for(size_t i = 0; i < pGhost->m_vPath.size(); ++i)
			{
				pGhost->m_vPath[i].m_Tick = StartTick + i;
			}
		}
		if(pGhost->m_StartTick == -1)
		{
			pGhost->m_StartTick = pGhost->m_vPath.front().m_Tick;
		}
		if(!FoundSkin)
		{
			StrToInts(pGhost->m_Skin.m_aSkin, std::size(pGhost->m_Skin.m_aSkin), "default");
		}
		return true;
	}

	// One snapshot of the race: the tee of this tick, who it is, and the little a
	// demo player expects to find beside it.
	int BuildSnapshot(CSnapshotBuilder &Builder, CSnapshotBuffer *pBuffer, const SGhost &Ghost, const CGhostCharacter &Character)
	{
		Builder.Init();

		CNetObj_Character NetCharacter;
		GhostCharacterToNetObj(&NetCharacter, &Character);
		Builder.NewItem(NETOBJTYPE_CHARACTER, GHOST_CLIENT_ID, &NetCharacter, sizeof(NetCharacter));

		CNetObj_PlayerInfo PlayerInfo = {};
		// The one tee there is, is the one whoever watches this is watching.
		PlayerInfo.m_Local = 1;
		PlayerInfo.m_ClientId = GHOST_CLIENT_ID;
		PlayerInfo.m_Team = 0;
		PlayerInfo.m_Score = -9999;
		PlayerInfo.m_Latency = 0;
		Builder.NewItem(NETOBJTYPE_PLAYERINFO, GHOST_CLIENT_ID, &PlayerInfo, sizeof(PlayerInfo));

		CNetObj_ClientInfo ClientInfo = {};
		StrToInts(ClientInfo.m_aName, std::size(ClientInfo.m_aName), Ghost.m_aOwner);
		StrToInts(ClientInfo.m_aClan, std::size(ClientInfo.m_aClan), "");
		mem_copy(ClientInfo.m_aSkin, Ghost.m_Skin.m_aSkin, sizeof(ClientInfo.m_aSkin));
		ClientInfo.m_UseCustomColor = Ghost.m_Skin.m_UseCustomColor;
		ClientInfo.m_ColorBody = Ghost.m_Skin.m_ColorBody;
		ClientInfo.m_ColorFeet = Ghost.m_Skin.m_ColorFeet;
		Builder.NewItem(NETOBJTYPE_CLIENTINFO, GHOST_CLIENT_ID, &ClientInfo, sizeof(ClientInfo));

		CNetObj_GameInfo GameInfo = {};
		GameInfo.m_RoundStartTick = Ghost.m_StartTick;
		GameInfo.m_RoundNum = 0;
		GameInfo.m_RoundCurrent = 1;
		Builder.NewItem(NETOBJTYPE_GAMEINFO, 0, &GameInfo, sizeof(GameInfo));

		return Builder.Finish(pBuffer);
	}
} // namespace

int main(int argc, const char **argv)
{
	CCmdlineFix CmdlineFix(&argc, &argv);
	log_set_global_logger_default();

	const char *pGhostPath = nullptr;
	const char *pDemoPath = nullptr;
	const char *pMapPath = nullptr;
	bool NoMapData = false;
	for(int i = 1; i < argc; ++i)
	{
		if(str_comp(argv[i], "-o") == 0 && i + 1 < argc)
		{
			pDemoPath = argv[++i];
		}
		else if(str_comp(argv[i], "--map") == 0 && i + 1 < argc)
		{
			pMapPath = argv[++i];
		}
		else if(str_comp(argv[i], "--no-map-data") == 0)
		{
			NoMapData = true;
		}
		else if(argv[i][0] != '-' && pGhostPath == nullptr)
		{
			pGhostPath = argv[i];
		}
		else
		{
			PrintUsage(argv[0]);
			return 1;
		}
	}
	if(pGhostPath == nullptr)
	{
		PrintUsage(argv[0]);
		return 1;
	}

	IStorage *pStorage = CreateStorage(IStorage::EInitializationType::BASIC, argc, argv);
	if(pStorage == nullptr)
	{
		log_error(TOOL_NAME, "Failed to initialize the storage location");
		return 1;
	}

	CGhostLoader Loader;
	Loader.Init(pStorage);
	SGhost Ghost;
	if(!ReadGhost(Loader, pGhostPath, &Ghost))
	{
		return 1;
	}

	// The map goes into the demo the way the client puts it there, so that the
	// demo plays for somebody who does not have it. Where it is not to be
	// found, the recorder looks for it in the data directories itself.
	std::vector<unsigned char> vMapData;
	SHA256_DIGEST MapSha256;
	mem_zero(&MapSha256, sizeof(MapSha256));
	unsigned MapCrc = 0;
	if(pMapPath != nullptr)
	{
		void *pData;
		unsigned DataSize;
		if(!pStorage->ReadFile(pMapPath, IStorage::TYPE_ALL_OR_ABSOLUTE, &pData, &DataSize))
		{
			log_error(TOOL_NAME, "Failed to read the map '%s'", pMapPath);
			return 1;
		}
		vMapData.assign(static_cast<unsigned char *>(pData), static_cast<unsigned char *>(pData) + DataSize);
		free(pData);
		MapSha256 = sha256(vMapData.data(), vMapData.size());
		MapCrc = crc32(0, vMapData.data(), vMapData.size());
	}

	char aDemoPath[IO_MAX_PATH_LENGTH];
	if(pDemoPath != nullptr)
	{
		str_copy(aDemoPath, pDemoPath);
	}
	else
	{
		char aName[IO_MAX_PATH_LENGTH];
		fs_split_file_extension(fs_filename(pGhostPath), aName, sizeof(aName));
		str_format(aDemoPath, sizeof(aDemoPath), "%s.demo", aName);
	}

	CSnapshotDelta SnapshotDelta;
	CDemoRecorder Recorder(&SnapshotDelta, NoMapData);
	if(Recorder.Start(pStorage, nullptr, aDemoPath, GAME_NETVERSION, Ghost.m_aMap, MapSha256, MapCrc, "ghost",
		   vMapData.size(), vMapData.empty() ? nullptr : vMapData.data(), nullptr, nullptr) != 0)
	{
		log_error(TOOL_NAME, "Failed to write the demo '%s'", aDemoPath);
		return 1;
	}

	CSnapshotBuilder Builder;
	auto pBuffer = std::make_unique<CSnapshotBuffer>();
	for(const CGhostCharacter &Character : Ghost.m_vPath)
	{
		const int Size = BuildSnapshot(Builder, pBuffer.get(), Ghost, Character);
		if(Size < 0)
		{
			log_error(TOOL_NAME, "Failed to build the snapshot of tick %d", Character.m_Tick);
			Recorder.Stop(IDemoRecorder::EStopMode::REMOVE_FILE);
			return 1;
		}
		Recorder.RecordSnapshot(Character.m_Tick, pBuffer->m_aData, Size);
	}
	Recorder.Stop(IDemoRecorder::EStopMode::KEEP_FILE);

	const int Ticks = Ghost.m_vPath.back().m_Tick - Ghost.m_vPath.front().m_Tick;
	log_info(TOOL_NAME, "Wrote '%s': %s on '%s', %d ticks (%.2f seconds)",
		aDemoPath, Ghost.m_aOwner, Ghost.m_aMap, Ticks, Ticks / (float)SERVER_TICK_SPEED);
	return 0;
}
