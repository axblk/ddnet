#if defined(CONF_VIDEORECORDER)

#include "video.h"

#include "config.h"

#include <base/log.h>
#include <base/str.h>

#include <csignal>

IVideo *IVideo::ms_pCurrentVideo = nullptr;

namespace
{
	volatile sig_atomic_t gs_InterruptSignaled = 0;

	void HandleVideoExportInterrupt(int)
	{
		gs_InterruptSignaled = 1;
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
	}
} // namespace

void CatchVideoExportInterrupt()
{
	signal(SIGINT, HandleVideoExportInterrupt);
	signal(SIGTERM, HandleVideoExportInterrupt);
}

void InterruptVideoExport()
{
	gs_InterruptSignaled = 1;
}

bool VideoExportInterrupted()
{
	if(gs_InterruptSignaled == 0)
		return false;
	gs_InterruptSignaled = 0;
	return true;
}

void PrintVideoExportUsage(const char *pUsageName)
{
	log_info("videorecorder", "Usage: %s --render-demo <demo> --output <video.mp4> [console commands]", pUsageName);
	log_info("videorecorder", "  --list-codecs      List the encoders that work on this machine");
	log_info("videorecorder", "  --help             Show this");
	log_info("videorecorder", "The video is configured by the cl_video_* settings, which can be set on the");
	log_info("videorecorder", "command line like any other console command, e.g. `cl_video_width 1280`.");
}

bool CCommandLineVideoExport::ParseArguments(int &ArgumentCount, const char **&ppArguments, std::vector<const char *> &vArguments, const char *pUsageName, bool AcceptPositional)
{
	CCommandLineVideoExport Parsed;
	char aError[256] = {};
	std::vector<const char *> vRemaining;
	vRemaining.reserve(ArgumentCount);
	vRemaining.push_back(ppArguments[0]);
	for(int Argument = 1; Argument < ArgumentCount && aError[0] == '\0'; ++Argument)
	{
		const char *pArgument = ppArguments[Argument];
		const char *pValue = Argument + 1 < ArgumentCount ? ppArguments[Argument + 1] : nullptr;
		if(str_comp(pArgument, "--render-demo") == 0)
		{
			++Argument;
			if(pValue == nullptr || pValue[0] == '\0' || str_length(pValue) >= static_cast<int>(sizeof(Parsed.m_aDemoPath)))
				str_copy(aError, "Invalid value for --render-demo.");
			else
			{
				str_copy(Parsed.m_aDemoPath, pValue);
				Parsed.m_Export = true;
			}
		}
		else if(str_comp(pArgument, "--output") == 0)
		{
			++Argument;
			if(pValue == nullptr || pValue[0] == '\0' || str_length(pValue) >= static_cast<int>(sizeof(Parsed.m_aVideoPath)) - str_length(".mp4.partial"))
				str_copy(aError, "Invalid value for --output.");
			else
				str_copy(Parsed.m_aVideoPath, pValue);
		}
		else if(str_comp(pArgument, "--list-codecs") == 0)
			Parsed.m_ListCodecs = true;
		else if(str_comp(pArgument, "--help") == 0 || str_comp(pArgument, "-h") == 0)
			Parsed.m_Help = true;
		else if(AcceptPositional && pArgument[0] != '-' && Parsed.m_aVideoPath[0] == '\0')
		{
			if(!Parsed.m_Export)
			{
				if(str_length(pArgument) >= static_cast<int>(sizeof(Parsed.m_aDemoPath)))
					str_copy(aError, "The demo path is too long.");
				else
				{
					str_copy(Parsed.m_aDemoPath, pArgument);
					Parsed.m_Export = true;
				}
			}
			else if(str_length(pArgument) >= static_cast<int>(sizeof(Parsed.m_aVideoPath)) - str_length(".mp4.partial"))
				str_copy(aError, "The output path is too long.");
			else
				str_copy(Parsed.m_aVideoPath, pArgument);
		}
		else
			vRemaining.push_back(pArgument);
	}
	if(aError[0] == '\0' && !Parsed.m_Help && !Parsed.m_ListCodecs && Parsed.m_Export != (Parsed.m_aVideoPath[0] != '\0'))
		str_copy(aError, "--render-demo and --output must be used together.");
	if(aError[0] != '\0')
	{
		log_error("videorecorder", "%s", aError);
		PrintVideoExportUsage(pUsageName);
		return false;
	}
	if(Parsed.m_Help)
		PrintVideoExportUsage(pUsageName);

	*this = Parsed;
	vArguments = std::move(vRemaining);
	ArgumentCount = static_cast<int>(vArguments.size());
	ppArguments = vArguments.data();
	return true;
}

CVideoExportSettings CCommandLineVideoExport::Settings()
{
	CVideoExportSettings Settings;
	Settings.m_Width = g_Config.m_ClVideoWidth;
	Settings.m_Height = g_Config.m_ClVideoHeight;
	Settings.m_FPS = g_Config.m_ClVideoRecorderFPS;
	Settings.m_Audio = g_Config.m_ClVideoSndEnable != 0;
	Settings.m_Crf = g_Config.m_ClVideoX264Crf;
	Settings.m_Preset = g_Config.m_ClVideoX264Preset;
	str_copy(Settings.m_aVideoCodec, g_Config.m_ClVideoCodec);
	Settings.m_EncodeThreads = g_Config.m_ClVideoEncodeThreads;
	Settings.m_ShowHud = g_Config.m_ClVideoShowhud != 0;
	Settings.m_ShowChat = g_Config.m_ClVideoShowChat != 0;
	Settings.m_ShowHookCollOther = g_Config.m_ClVideoShowHookCollOther != 0;
	Settings.m_ShowDirection = g_Config.m_ClVideoShowDirection;
	Settings.m_ShowImportantAlerts = g_Config.m_ClVideoShowImportantAlerts != 0;
	return Settings;
}

#endif
