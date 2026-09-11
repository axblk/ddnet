/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "demo_viewer_client.h"

#include <base/log.h>
#include <base/logger.h>
#include <base/os.h>
#include <base/str.h>

#include <engine/client.h>
#include <engine/config.h>
#include <engine/console.h>
#include <engine/engine.h>
#include <engine/input.h>
#include <engine/shared/assertion_logger.h>
#include <engine/shared/config.h>
#include <engine/shared/video.h>
#include <engine/sound.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <game/version.h>

#include <memory>
#include <vector>

// Entry point of the demo viewer. The client's own entry point starts a game to
// play: it registers the connect link handler with the system, restarts the
// binary when the graphics settings demand it, saves the configuration on the
// way out and reports what went wrong in message boxes. None of that belongs in
// a program that shows one demo and closes, so the viewer starts here instead
// and only puts together the interfaces the playback runs on.
int main(int argc, const char **argv)
{
	CCmdlineFix CmdlineFix(&argc, &argv);

	std::shared_ptr<ILogger> pStdoutLogger = std::shared_ptr<ILogger>(log_logger_stdout());
	std::shared_ptr<CFutureLogger> pFutureConsoleLogger = std::make_shared<CFutureLogger>();
	std::shared_ptr<CFutureLogger> pFutureAssertionLogger = std::make_shared<CFutureLogger>();
	// The client can be told to write a log file, but a command line tool
	// reports on its output, so this stays a place for the console commands that
	// configure logging to write to.
	std::shared_ptr<CFutureLogger> pFutureFileLogger = std::make_shared<CFutureLogger>();
	pFutureFileLogger->Set(log_logger_noop());
	log_set_global_logger(log_logger_collection({pStdoutLogger, pFutureConsoleLogger, pFutureAssertionLogger, pFutureFileLogger}).release());

	// Setting the game up and reading its settings is not news, and it complains
	// about every command in the configuration that this program does not have.
	// Until the settings say what was actually asked for, only warnings and
	// errors are printed.
	pStdoutLogger->SetFilter(CLogFilter{IConsole::ToLogLevelFilter(-1)});

	CDemoViewerClient *pClient = new CDemoViewerClient;
	pClient->SetLoggers(std::shared_ptr<ILogger>(pFutureFileLogger), std::shared_ptr<ILogger>(pStdoutLogger));

	IKernel *pKernel = IKernel::Create();
	pKernel->RegisterInterface(static_cast<IClient *>(pClient), false);
	pClient->RegisterInterfaces();

	IEngine *pEngine = CreateEngine(GAME_NAME, pFutureConsoleLogger);
	pKernel->RegisterInterface(pEngine, false);

	// The engine has to be destroyed before the graphics, and the client before
	// the kernel, which owns the graphics the client keeps looking at.
	const auto Cleanup = [&]() {
		delete pEngine;
		pKernel->Shutdown();
		delete pClient;
		delete pKernel;
	};

	IStorage *pStorage = CreateStorage(IStorage::EInitializationType::CLIENT, argc, argv);
	if(pStorage == nullptr)
	{
		log_error("client", "Failed to initialize the storage location (see details above)");
		Cleanup();
		return -1;
	}
	pKernel->RegisterInterface(pStorage);
	pFutureAssertionLogger->Set(CreateAssertionLogger(pStorage, GAME_NAME));

	IConsole *pConsole = CreateConsole(CFGFLAG_CLIENT).release();
	pKernel->RegisterInterface(pConsole);

	IConfigManager *pConfigManager = CreateConfigManager();
	pKernel->RegisterInterface(pConfigManager);

	IEngineSound *pEngineSound = CreateEngineSound();
	pKernel->RegisterInterface(pEngineSound); // IEngineSound
	pKernel->RegisterInterface(static_cast<ISound *>(pEngineSound), false);

	// Somebody is at the keyboard of this one: it is how the demo is steered.
	IEngineInput *pEngineInput = CreateEngineInput();
	pKernel->RegisterInterface(pEngineInput); // IEngineInput
	pKernel->RegisterInterface(static_cast<IInput *>(pEngineInput), false);

	IEngineTextRender *pEngineTextRender = CreateEngineTextRender();
	pKernel->RegisterInterface(pEngineTextRender); // IEngineTextRender
	pKernel->RegisterInterface(static_cast<ITextRender *>(pEngineTextRender), false);

	pKernel->RegisterInterface(CreateGameClient());

	pEngine->Init();
	pConsole->Init();
	pConfigManager->Init();
	pKernel->RequestInterface<IGameClient>()->OnConsoleInit();
	pClient->InitInterfaces();

	// The settings come from the configuration file, so a demo looks the way it
	// would look in the client. Nothing is written back to it.
	if(pStorage->FileExists(CONFIG_FILE, IStorage::TYPE_ALL) && !pConsole->ExecuteFile(CONFIG_FILE, IConsole::CLIENT_ID_UNSPECIFIED))
	{
		log_warn("client", "Failed to load config from '" CONFIG_FILE "', continuing with the default settings.");
	}
	if(g_Config.m_ClConfigVersion < 1 && g_Config.m_ClAntiPing == 0)
	{
		g_Config.m_ClAntiPingPlayers = 1;
		g_Config.m_ClAntiPingGrenade = 1;
		g_Config.m_ClAntiPingWeapons = 1;
	}
	// The settings decide how much the client says while it runs, so
	// `stdout_output_level` counts here too. The command line may set it as
	// well, which is why this runs again once the arguments have been read.
	const auto ApplyStdoutLogLevel = [&]() {
		pStdoutLogger->SetFilter(CLogFilter{IConsole::ToLogLevelFilter(g_Config.m_StdoutOutputLevel)});
	};
	ApplyStdoutLogLevel();

	// What the viewer takes is a demo, optionally a file to write a video of it
	// into, and otherwise the settings the client takes. How that video is
	// encoded is `cl_video_width` and the rest, which are settings like any
	// other, so there is nothing here to parse for them.
	const char *pDemoPath = nullptr;
	const char *pVideoPath = nullptr;
	std::vector<const char *> vConsoleArguments;
	for(int Index = 1; Index < argc; Index++)
	{
		if(str_comp(argv[Index], "--output") == 0)
		{
			if(Index + 1 >= argc)
			{
				log_error("client", "Missing value for --output.");
				Cleanup();
				return -1;
			}
			pVideoPath = argv[++Index];
		}
		else if(str_comp(argv[Index], "--help") == 0)
		{
			log_info("client", "Usage: ddnet-demo-viewer [<demo>] [--output <video.mp4>] [settings]");
			log_info("client", "Anything else is a console command, so `cl_video_width 1920` and the");
			log_info("client", "rest of the cl_ settings work here just as they do in the client.");
			log_info("client", "Space pauses, the arrow keys seek and change the speed, Home starts");
			log_info("client", "over and Escape closes the window. A demo dropped on the window");
			log_info("client", "replaces the one that is playing, and is what the viewer waits for");
			log_info("client", "when it was given none.");
			Cleanup();
			return 0;
		}
		else if(argv[Index][0] != '-' && str_find(argv[Index], "=") == nullptr && pDemoPath == nullptr)
		{
			pDemoPath = argv[Index];
		}
		else
		{
			vConsoleArguments.push_back(argv[Index]);
		}
	}
	// A demo is not required: given none, the viewer opens its window and waits
	// for one to be dropped into it, the same way the map viewer waits for a map
	// and the client waits for whatever it is given. Writing a video of a demo
	// that has not been named is the one thing that makes no sense.
	if(pDemoPath == nullptr && pVideoPath != nullptr)
	{
		log_error("client", "There is no demo to write a video of.");
		Cleanup();
		return -1;
	}

	// Everything that was not the demo configures the client the same way it
	// does in the client itself, so `gfx_backend` and the like work here.
	pConsole->ParseArguments(vConsoleArguments.size(), vConsoleArguments.data());
	ApplyStdoutLogLevel();

	// Nothing on the command line sets an encoding option, so these are what
	// the settings say.
	const CCommandLineVideoExport VideoDefaults;
	pClient->Configure(pDemoPath == nullptr ? "" : pDemoPath, pVideoPath, VideoDefaults.Settings());
	pClient->Run();

	const int ExitCode = pClient->ExitCode();
	for(const SWarning &Warning : pClient->QuittingWarnings())
	{
		log_warn("client", "%s: %s", Warning.m_aWarningTitle, Warning.m_aWarningMsg);
	}
	Cleanup();
	return ExitCode;
}
