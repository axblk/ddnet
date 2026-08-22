/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "demo_render_client.h"

#include <base/log.h>
#include <base/logger.h>
#include <base/os.h>

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

// Entry point of the demo render tool. The client's own entry point starts a
// game to play: it initializes SDL, registers the connect link handler with the
// system, restarts the binary when the graphics settings demand it, saves the
// configuration on the way out and reports what went wrong in message boxes.
// None of that belongs in a program that turns one demo into one video file and
// then reports success with an exit code, so the tool starts here instead and
// only puts together the interfaces the export runs on.
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

	CDemoRenderClient *pClient = new CDemoRenderClient;
	pClient->SetLoggers(std::shared_ptr<ILogger>(pFutureFileLogger), std::move(pStdoutLogger));

	IKernel *pKernel = IKernel::Create();
	pKernel->RegisterInterface(static_cast<IClient *>(pClient), false);
	pClient->RegisterInterfaces();

	IEngine *pEngine = CreateEngine(GAME_NAME, pFutureConsoleLogger);
	pKernel->RegisterInterface(pEngine, false);

	// The engine has to be destroyed before the graphics, and both before the
	// kernel that hands them out.
	const auto Cleanup = [&]() {
		delete pEngine;
		pKernel->Shutdown();
		delete pKernel;
		delete pClient;
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

	// There is nobody at the keyboard of a program that renders a file, but the
	// interface elements the game draws still ask what the pointer is doing.
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

	// The video settings that have no command line argument of their own come
	// from the configuration file, so a demo renders the way it would render in
	// the client. Nothing is written back to it.
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

	CCommandLineVideoExport VideoExport;
	std::vector<const char *> vArguments;
	if(!VideoExport.ParseArguments(argc, argv, vArguments, "ddnet-demo-render", true))
	{
		Cleanup();
		return -1;
	}
	if(VideoExport.m_ListCodecs)
	{
		for(const CVideoEncoder &Encoder : VideoEncoders())
			log_info("videorecorder", "%-20s %s", Encoder.m_aName[0] == 0 ? "(default)" : Encoder.m_aName, Encoder.m_aDisplayName);
		Cleanup();
		return 0;
	}
	if(VideoExport.m_Help)
	{
		Cleanup();
		return 0;
	}
	if(!VideoExport.m_Export)
	{
		PrintVideoExportUsage("ddnet-demo-render");
		Cleanup();
		return -1;
	}

	// Everything the video arguments left over configures the client the same
	// way it does in the client itself, so `gfx_backend` and the like work here.
	pConsole->ParseArguments(argc - 1, &argv[1]);

	if(!pClient->Configure(VideoExport))
	{
		Cleanup();
		return -1;
	}

	pClient->Run();

	const int ExitCode = pClient->ExitCode();
	for(const SWarning &Warning : pClient->QuittingWarnings())
	{
		log_warn("videorecorder", "%s: %s", Warning.m_aWarningTitle, Warning.m_aWarningMsg);
	}
	Cleanup();
	return ExitCode;
}
