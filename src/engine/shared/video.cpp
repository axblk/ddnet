#if defined(CONF_VIDEORECORDER)

#include "video.h"

#include "config.h"

#include <base/log.h>
#include <base/str.h>

IVideo *IVideo::ms_pCurrentVideo = nullptr;

void PrintVideoExportUsage(const char *pUsageName)
{
	log_info("videorecorder", "Usage: %s <demo> <video.mp4> [options]", pUsageName);
	log_info("videorecorder", "  --width <even>     Video width in pixels, cl_video_width otherwise");
	log_info("videorecorder", "  --height <even>    Video height in pixels, cl_video_height otherwise");
	log_info("videorecorder", "  --fps <1-1000>     Frames per second, cl_video_recorder_fps otherwise");
	log_info("videorecorder", "  --codec <name>     Encoder to use, cl_video_codec otherwise");
	log_info("videorecorder", "  --threads <0-64>   Encoder threads, 0 to decide automatically");
	log_info("videorecorder", "  --crf <0-51>       Constant rate factor, lower is better");
	log_info("videorecorder", "  --preset <0-9>     Encoder effort, 0 is fastest and 9 is smallest");
	log_info("videorecorder", "  --no-audio         Render without an audio track");
	log_info("videorecorder", "  --hud              Show the ingame interface");
	log_info("videorecorder", "  --no-chat          Hide the chat");
	log_info("videorecorder", "  --list-codecs      List the encoders that work on this machine");
	log_info("videorecorder", "  --help             Show this");
	log_info("videorecorder", "Anything else is a console command, so `cl_showfps 1` and the rest of the");
	log_info("videorecorder", "cl_ settings work here just as they do in the client.");
}

bool CCommandLineVideoExport::ParseArguments(int &ArgumentCount, const char **&ppArguments, std::vector<const char *> &vArguments, const char *pUsageName, bool AcceptPositional)
{
	CCommandLineVideoExport Parsed;
	bool HasArgument = false;
	char aError[256] = {};
	std::vector<const char *> vRemaining;
	vRemaining.reserve(ArgumentCount);
	vRemaining.push_back(ppArguments[0]);
	for(int Argument = 1; Argument < ArgumentCount && aError[0] == '\0'; ++Argument)
	{
		const char *pArgument = ppArguments[Argument];
		auto ReadValue = [&]() -> const char * {
			if(Argument + 1 >= ArgumentCount)
				return nullptr;
			return ppArguments[++Argument];
		};
		auto ReadInteger = [&](int &Value) {
			const char *pValue = ReadValue();
			return pValue != nullptr && pValue[0] != '\0' && str_toint(pValue, &Value);
		};
		if(str_comp(pArgument, "--render-demo") == 0)
		{
			HasArgument = true;
			const char *pValue = ReadValue();
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
			HasArgument = true;
			const char *pValue = ReadValue();
			if(pValue == nullptr || pValue[0] == '\0' || str_length(pValue) >= static_cast<int>(sizeof(Parsed.m_aVideoPath)) - str_length(".mp4.partial"))
				str_copy(aError, "Invalid value for --output.");
			else
				str_copy(Parsed.m_aVideoPath, pValue);
		}
		else if(str_comp(pArgument, "--width") == 0)
		{
			HasArgument = true;
			if(!ReadInteger(Parsed.m_Width) || Parsed.m_Width < 2 || Parsed.m_Width > 8192 || Parsed.m_Width % 2 != 0)
				str_copy(aError, "--width must be an even number between 2 and 8192.");
		}
		else if(str_comp(pArgument, "--height") == 0)
		{
			HasArgument = true;
			if(!ReadInteger(Parsed.m_Height) || Parsed.m_Height < 2 || Parsed.m_Height > 8192 || Parsed.m_Height % 2 != 0)
				str_copy(aError, "--height must be an even number between 2 and 8192.");
		}
		else if(str_comp(pArgument, "--fps") == 0)
		{
			HasArgument = true;
			if(!ReadInteger(Parsed.m_Fps) || Parsed.m_Fps < 1 || Parsed.m_Fps > 1000)
				str_copy(aError, "--fps must be between 1 and 1000.");
		}
		else if(str_comp(pArgument, "--crf") == 0)
		{
			HasArgument = true;
			if(!ReadInteger(Parsed.m_Crf) || Parsed.m_Crf < 0 || Parsed.m_Crf > 51)
				str_copy(aError, "--crf must be between 0 and 51.");
		}
		else if(str_comp(pArgument, "--preset") == 0)
		{
			HasArgument = true;
			if(!ReadInteger(Parsed.m_Preset) || Parsed.m_Preset < 0 || Parsed.m_Preset > 9)
				str_copy(aError, "--preset must be between 0 and 9.");
		}
		else if(str_comp(pArgument, "--codec") == 0)
		{
			HasArgument = true;
			const char *pValue = ReadValue();
			if(pValue == nullptr || pValue[0] == '\0' || str_length(pValue) >= static_cast<int>(sizeof(Parsed.m_aCodec)))
				str_copy(aError, "Invalid value for --codec.");
			else
				str_copy(Parsed.m_aCodec, pValue);
		}
		else if(str_comp(pArgument, "--threads") == 0)
		{
			HasArgument = true;
			if(!ReadInteger(Parsed.m_EncodeThreads) || Parsed.m_EncodeThreads < 0 || Parsed.m_EncodeThreads > 64)
				str_copy(aError, "--threads must be between 0 and 64.");
		}
		else if(str_comp(pArgument, "--no-audio") == 0)
		{
			HasArgument = true;
			Parsed.m_NoAudio = true;
		}
		else if(str_comp(pArgument, "--hud") == 0)
		{
			HasArgument = true;
			Parsed.m_Hud = 1;
		}
		else if(str_comp(pArgument, "--no-chat") == 0)
		{
			HasArgument = true;
			Parsed.m_Chat = 0;
		}
		else if(str_comp(pArgument, "--list-codecs") == 0)
		{
			HasArgument = true;
			Parsed.m_ListCodecs = true;
		}
		else if(str_comp(pArgument, "--help") == 0 || str_comp(pArgument, "-h") == 0)
		{
			HasArgument = true;
			Parsed.m_Help = true;
		}
		else if(AcceptPositional && pArgument[0] != '-' && Parsed.m_aVideoPath[0] == '\0')
		{
			// A program that renders one demo into one file needs no flag to
			// say which of the two paths is which.
			HasArgument = true;
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
	if(Parsed.m_Help || Parsed.m_ListCodecs)
	{
		*this = Parsed;
		vArguments = std::move(vRemaining);
		ArgumentCount = static_cast<int>(vArguments.size());
		ppArguments = vArguments.data();
		if(Parsed.m_Help)
			PrintVideoExportUsage(pUsageName);
		return true;
	}
	if(aError[0] == '\0' && HasArgument && Parsed.m_Export && Parsed.m_aVideoPath[0] == '\0')
		str_copy(aError, "A demo needs an output file to render into.");
	if(aError[0] == '\0' && HasArgument && !Parsed.m_Export && Parsed.m_aVideoPath[0] != '\0')
		str_copy(aError, "An output file needs a demo to render.");
	if(aError[0] != '\0')
	{
		log_error("videorecorder", "%s", aError);
		PrintVideoExportUsage(pUsageName);
		return false;
	}

	*this = Parsed;
	vArguments = std::move(vRemaining);
	ArgumentCount = static_cast<int>(vArguments.size());
	ppArguments = vArguments.data();
	return true;
}

CVideoExportSettings CCommandLineVideoExport::Settings() const
{
	CVideoExportSettings Settings;
	// An argument wins, the configuration decides what no argument named, which
	// is how a render started from the menu is configured as well.
	Settings.m_Width = m_Width == 0 ? g_Config.m_ClVideoWidth : m_Width;
	Settings.m_Height = m_Height == 0 ? g_Config.m_ClVideoHeight : m_Height;
	Settings.m_FPS = m_Fps == 0 ? g_Config.m_ClVideoRecorderFPS : m_Fps;
	Settings.m_Audio = !m_NoAudio && g_Config.m_ClVideoSndEnable != 0;
	Settings.m_Crf = m_Crf < 0 ? g_Config.m_ClVideoX264Crf : m_Crf;
	Settings.m_Preset = m_Preset < 0 ? g_Config.m_ClVideoX264Preset : m_Preset;
	str_copy(Settings.m_aVideoCodec, m_aCodec[0] == '\0' ? g_Config.m_ClVideoCodec : m_aCodec);
	Settings.m_EncodeThreads = m_EncodeThreads < 0 ? g_Config.m_ClVideoEncodeThreads : m_EncodeThreads;
	Settings.m_ShowHud = m_Hud < 0 ? g_Config.m_ClVideoShowhud != 0 : m_Hud != 0;
	Settings.m_ShowChat = m_Chat < 0 ? g_Config.m_ClVideoShowChat != 0 : m_Chat != 0;
	Settings.m_ShowHookCollOther = g_Config.m_ClVideoShowHookCollOther != 0;
	Settings.m_ShowDirection = g_Config.m_ClVideoShowDirection;
	Settings.m_ShowImportantAlerts = g_Config.m_ClVideoShowImportantAlerts != 0;
	return Settings;
}

#endif
