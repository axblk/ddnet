/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "sound.h"

#include <base/bytes.h>
#include <base/dbg.h>
#include <base/log.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#if !defined(CONF_DEMO_RENDER_TOOL)
#include <SDL.h>
#endif

extern "C" {
#include <opusfile.h>
#include <wavpack.h>
}

#include <cmath>

static constexpr int SAMPLE_INDEX_USED = -2;
static constexpr int SAMPLE_INDEX_FULL = -1;

void CSound::MixVoices(short *pFinalOut, unsigned Frames, bool Offline)
{
	const CLockScope LockScope(m_SoundLock);
	Frames = std::min(Frames, m_MaxFrames);
	mem_zero(m_pMixBuffer, Frames * 2 * sizeof(int));

	const int MasterVol = (Offline ? m_OfflineSoundVolume : m_SoundVolume).load(std::memory_order_relaxed);
	const float ListenerPositionX = (Offline ? m_OfflineListenerPositionX : m_ListenerPositionX).load(std::memory_order_relaxed);
	const float ListenerPositionY = (Offline ? m_OfflineListenerPositionY : m_ListenerPositionY).load(std::memory_order_relaxed);
	const int FirstVoice = Offline ? NUM_VOICES_PER_CONTEXT : 0;
	const int LastVoice = FirstVoice + NUM_VOICES_PER_CONTEXT;

	for(int VoiceId = FirstVoice; VoiceId < LastVoice; ++VoiceId)
	{
		CVoice &Voice = m_aVoices[VoiceId];
		if(!Voice.m_pSample)
			continue;

		// mix voice
		int *pOut = m_pMixBuffer;

		const int Step = Voice.m_pSample->m_Channels; // setup input sources
		short *pInL = &Voice.m_pSample->m_pData[Voice.m_Tick * Step];
		short *pInR = &Voice.m_pSample->m_pData[Voice.m_Tick * Step + 1];

		unsigned End = Voice.m_pSample->m_NumFrames - Voice.m_Tick;

		int VolumeR = round_truncate(Voice.m_pChannel->m_Vol * (Voice.m_Vol / 255.0f));
		int VolumeL = VolumeR;

		// make sure that we don't go outside the sound data
		if(Frames < End)
			End = Frames;

		// check if we have a mono sound
		if(Voice.m_pSample->m_Channels == 1)
			pInR = pInL;

		// volume calculation
		if(Voice.m_Flags & ISound::FLAG_POS && Voice.m_pChannel->m_Pan)
		{
			// TODO: we should respect the channel panning value
			const vec2 Delta = Voice.m_Position - vec2(ListenerPositionX, ListenerPositionY);
			vec2 Falloff = vec2(0.0f, 0.0f);

			float RangeX = 0.0f; // for panning
			bool InVoiceField = false;

			switch(Voice.m_Shape)
			{
			case ISound::SHAPE_CIRCLE:
			{
				const float Radius = Voice.m_Circle.m_Radius;
				RangeX = Radius;

				const float Dist = length(Delta);
				if(Dist < Radius)
				{
					InVoiceField = true;

					// falloff
					const float FalloffDistance = Radius * Voice.m_Falloff;
					Falloff.x = Falloff.y = Dist > FalloffDistance ? (Radius - Dist) / (Radius - FalloffDistance) : 1.0f;
				}
				break;
			}

			case ISound::SHAPE_RECTANGLE:
			{
				const vec2 AbsoluteDelta = vec2(absolute(Delta.x), absolute(Delta.y));
				const float w = Voice.m_Rectangle.m_Width / 2.0f;
				const float h = Voice.m_Rectangle.m_Height / 2.0f;
				RangeX = w;

				if(AbsoluteDelta.x < w && AbsoluteDelta.y < h)
				{
					InVoiceField = true;

					// falloff
					const vec2 FalloffDistance = vec2(w, h) * Voice.m_Falloff;
					Falloff.x = AbsoluteDelta.x > FalloffDistance.x ? (w - AbsoluteDelta.x) / (w - FalloffDistance.x) : 1.0f;
					Falloff.y = AbsoluteDelta.y > FalloffDistance.y ? (h - AbsoluteDelta.y) / (h - FalloffDistance.y) : 1.0f;
				}
				break;
			}
			};

			if(InVoiceField)
			{
				// panning
				if(!(Voice.m_Flags & ISound::FLAG_NO_PANNING))
				{
					if(Delta.x > 0)
						VolumeL = ((RangeX - absolute(Delta.x)) * VolumeL) / RangeX;
					else
						VolumeR = ((RangeX - absolute(Delta.x)) * VolumeR) / RangeX;
				}

				{
					VolumeL *= Falloff.x * Falloff.y;
					VolumeR *= Falloff.x * Falloff.y;
				}
			}
			else
			{
				VolumeL = 0;
				VolumeR = 0;
			}
		}

		// process all frames
		for(unsigned s = 0; s < End; s++)
		{
			*pOut++ += (*pInL) * VolumeL;
			*pOut++ += (*pInR) * VolumeR;
			pInL += Step;
			pInR += Step;
			Voice.m_Tick++;
		}

		// free voice if not used any more
		if(Voice.m_Tick == Voice.m_pSample->m_NumFrames)
		{
			if(Voice.m_Flags & ISound::FLAG_LOOP)
			{
				Voice.m_Tick = Voice.m_pSample->m_LoopStart;
			}
			else
			{
				Voice.m_pSample = nullptr;
				Voice.m_Age++;
			}
		}
	}

	// clamp accumulated values
	for(unsigned i = 0; i < Frames * 2; i++)
		pFinalOut[i] = std::clamp<int>(((m_pMixBuffer[i] * MasterVol) / 101) >> 8, std::numeric_limits<short>::min(), std::numeric_limits<short>::max());

#if defined(CONF_ARCH_ENDIAN_BIG)
	swap_endian(pFinalOut, sizeof(short), Frames * 2);
#endif
}

void CSound::Mix(short *pFinalOut, unsigned Frames)
{
	MixVoices(pFinalOut, Frames, false);
}

void CSound::MixOffline(short *pFinalOut, unsigned Frames)
{
	MixVoices(pFinalOut, Frames, true);
}

#if !defined(CONF_DEMO_RENDER_TOOL)
static void SdlCallback(void *pUser, Uint8 *pStream, int Len)
{
	CSound *pSound = static_cast<CSound *>(pUser);
	pSound->Mix((short *)pStream, Len / sizeof(short) / 2);
}
#endif

int CSound::Init()
{
	m_SoundEnabled = false;
	m_pGraphics = Kernel()->RequestInterface<IEngineGraphics>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();

	// Initialize sample indices. We always need them to load sounds in
	// the editor even if sound is disabled or failed to be enabled.
	const CLockScope LockScope(m_SoundLock);
	m_FirstFreeSampleIndex = 0;
	for(size_t i = 0; i < std::size(m_aSamples) - 1; ++i)
	{
		m_aSamples[i].m_Index = i;
		m_aSamples[i].m_NextFreeSampleIndex = i + 1;
		m_aSamples[i].m_pData = nullptr;
	}
	m_aSamples[std::size(m_aSamples) - 1].m_Index = std::size(m_aSamples) - 1;
	m_aSamples[std::size(m_aSamples) - 1].m_NextFreeSampleIndex = SAMPLE_INDEX_FULL;

#if !defined(CONF_DEMO_RENDER_TOOL)
	if(!g_Config.m_SndEnable)
		return 0;
#endif

#if defined(CONF_DEMO_RENDER_TOOL)
	m_MixingRate = g_Config.m_SndRate;
	m_MaxFrames = 2048;
	m_pMixBuffer = static_cast<int *>(calloc(m_MaxFrames * 2, sizeof(int)));
	if(m_pMixBuffer == nullptr)
		return -1;
	m_SoundEnabled = true;
	Update();
	return 0;
#else
	if(SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
	{
		log_error("sound", "Unable to init SDL audio: %s", SDL_GetError());
		return -1;
	}

	SDL_AudioSpec Format, FormatOut;
	Format.freq = g_Config.m_SndRate;
	Format.format = AUDIO_S16;
	Format.channels = 2;
	Format.samples = g_Config.m_SndBufferSize;
	Format.callback = SdlCallback;
	Format.userdata = this;

	// Open the audio device and start playing sound!
	m_Device = SDL_OpenAudioDevice(nullptr, 0, &Format, &FormatOut, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if(m_Device == 0)
	{
		log_error("sound", "Unable to open audio device: %s", SDL_GetError());
		return -1;
	}
	else
	{
		log_info("sound", "Sound init successful using audio driver '%s'", SDL_GetCurrentAudioDriver());
	}

	m_MixingRate = FormatOut.freq;
	m_MaxFrames = FormatOut.samples * 2;
#if defined(CONF_VIDEORECORDER)
	m_MaxFrames = std::max(m_MaxFrames, 1024u * 2u); // make the buffer bigger just in case
#endif
	m_pMixBuffer = (int *)calloc(m_MaxFrames * 2, sizeof(int));

	m_SoundEnabled = true;
	Update();

	SDL_PauseAudioDevice(m_Device, 0);
	return 0;
#endif
}

int CSound::Update()
{
	UpdateVolume();
	return 0;
}

void CSound::UpdateVolume()
{
	int WantedVolume = g_Config.m_SndVolume;
	m_OfflineSoundVolume.store(WantedVolume, std::memory_order_relaxed);
	if(!m_pGraphics->WindowActive() && g_Config.m_SndNonactiveMute)
		WantedVolume = 0;
	m_SoundVolume.store(WantedVolume, std::memory_order_relaxed);
}

void CSound::Shutdown()
{
	StopAll();
	StopOffline();

#if !defined(CONF_DEMO_RENDER_TOOL)
	// Stop sound callback before freeing sample data
	SDL_CloseAudioDevice(m_Device);
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
	m_Device = 0;
#endif

	const CLockScope LockScope(m_SoundLock);
	for(auto &Sample : m_aSamples)
	{
		free(Sample.m_pData);
		Sample.m_pData = nullptr;
	}

	free(m_pMixBuffer);
	m_pMixBuffer = nullptr;
	m_SoundEnabled = false;
}

CSample *CSound::AllocSample()
{
	const CLockScope LockScope(m_SoundLock);
	if(m_FirstFreeSampleIndex == SAMPLE_INDEX_FULL)
		return nullptr;

	CSample *pSample = &m_aSamples[m_FirstFreeSampleIndex];
	dbg_assert(
		pSample->m_pData == nullptr && pSample->m_NextFreeSampleIndex != SAMPLE_INDEX_USED,
		"Sample was not unloaded (index=%d, next=%d, duration=%f, data=%p)",
		pSample->m_Index, pSample->m_NextFreeSampleIndex, pSample->TotalTime(), pSample->m_pData);
	m_FirstFreeSampleIndex = pSample->m_NextFreeSampleIndex;
	pSample->m_NextFreeSampleIndex = SAMPLE_INDEX_USED;
	return pSample;
}

void CSound::RateConvert(CSample &Sample) const
{
	dbg_assert(Sample.IsLoaded(), "Sample not loaded");
	// make sure that we need to convert this sound
	if(Sample.m_Rate == m_MixingRate)
		return;

	// allocate new data
	const int NumFrames = (int)((Sample.m_NumFrames / (float)Sample.m_Rate) * m_MixingRate);
	short *pNewData = (short *)calloc((size_t)NumFrames * Sample.m_Channels, sizeof(short));

	for(int i = 0; i < NumFrames; i++)
	{
		// resample TODO: this should be done better, like linear at least
		float a = i / (float)NumFrames;
		int f = (int)(a * Sample.m_NumFrames);
		if(f >= Sample.m_NumFrames)
			f = Sample.m_NumFrames - 1;

		// set new data
		if(Sample.m_Channels == 1)
			pNewData[i] = Sample.m_pData[f];
		else if(Sample.m_Channels == 2)
		{
			pNewData[i * 2] = Sample.m_pData[f * 2];
			pNewData[i * 2 + 1] = Sample.m_pData[f * 2 + 1];
		}
	}

	// adjust looping position, note that this is not precise
	const double Factor = (double)m_MixingRate / (double)Sample.m_Rate;
	Sample.m_LoopStart = std::round(Sample.m_LoopStart * Factor);

	// free old data and apply new
	free(Sample.m_pData);
	Sample.m_pData = pNewData;
	Sample.m_NumFrames = NumFrames;
	Sample.m_Rate = m_MixingRate;
}

bool CSound::DecodeOpus(CSample &Sample, const void *pData, unsigned DataSize, const char *pContextName) const
{
	int OpusError = 0;
	OggOpusFile *pOpusFile = op_open_memory((const unsigned char *)pData, DataSize, &OpusError);
	if(pOpusFile)
	{
		const int NumChannels = op_channel_count(pOpusFile, -1);
		if(NumChannels > 2)
		{
			op_free(pOpusFile);
			log_error("sound/opus", "File is not mono or stereo. Filename='%s'", pContextName);
			return false;
		}

		const int NumSamples = op_pcm_total(pOpusFile, -1); // per channel!
		if(NumSamples < 0)
		{
			op_free(pOpusFile);
			log_error("sound/opus", "Failed to get number of samples, error %d. Filename='%s'", NumSamples, pContextName);
			return false;
		}

		short *pSampleData = (short *)calloc((size_t)NumSamples * NumChannels, sizeof(short));

		int Pos = 0;
		while(Pos < NumSamples)
		{
			const int Read = op_read(pOpusFile, pSampleData + Pos * NumChannels, (NumSamples - Pos) * NumChannels, nullptr);
			if(Read < 0)
			{
				free(pSampleData);
				op_free(pOpusFile);
				log_error("sound/opus", "op_read error %d at %d. Filename='%s'", Read, Pos, pContextName);
				return false;
			}
			else if(Read == 0) // EOF
				break;
			Pos += Read;
		}

		Sample.m_pData = pSampleData;
		Sample.m_NumFrames = Pos;
		Sample.m_Rate = 48000;
		Sample.m_Channels = NumChannels;
		Sample.m_LoopStart = 0;
		Sample.m_PausedAt = 0;

		const OpusTags *pTags = op_tags(pOpusFile, -1);
		if(pTags)
		{
			for(int i = 0; i < pTags->comments; ++i)
			{
				const char *pComment = pTags->user_comments[i];
				if(!pComment)
					continue;
				if(!str_startswith(pComment, "LOOP_START="))
					continue;
				int LoopStart = -1;
				if(!str_toint(pComment + str_length("LOOP_START="), &LoopStart))
				{
					log_error("sound/opus", "Invalid LOOP_START tag. Value='%s' Filename='%s'", pComment + str_length("LOOP_START="), pContextName);
					break;
				}
				if(LoopStart < 0 || LoopStart >= Sample.m_NumFrames)
				{
					log_error("sound/opus", "Tag LOOP_START out of range. Value=%d Min=0 Max=%d Filename='%s'", LoopStart, Sample.m_NumFrames - 1, pContextName);
					break;
				}
				Sample.m_LoopStart = LoopStart;
				break;
			}
		}

		op_free(pOpusFile);
	}
	else
	{
		log_error("sound/opus", "Failed to decode sample, error %d. Filename='%s'", OpusError, pContextName);
		return false;
	}

	return true;
}

struct CWavpackMemoryReader
{
	const uint8_t *m_pData;
	unsigned m_DataSize;
	unsigned m_Position;
};

static int ReadWavpackData(CWavpackMemoryReader &Reader, void *pBuffer, int Size)
{
	if(Size <= 0 || Reader.m_Position >= Reader.m_DataSize)
		return 0;

	const unsigned ChunkSize = std::min<unsigned>(Size, Reader.m_DataSize - Reader.m_Position);
	mem_copy(pBuffer, Reader.m_pData + Reader.m_Position, ChunkSize);
	Reader.m_Position += ChunkSize;
	return ChunkSize;
}

#if defined(CONF_WAVPACK_OPEN_FILE_INPUT_EX)
static int ReadData(void *pId, void *pBuffer, int Size)
{
	return ReadWavpackData(*static_cast<CWavpackMemoryReader *>(pId), pBuffer, Size);
}

static int ReturnFalse(void *pId)
{
	(void)pId;
	return 0;
}

static unsigned int GetPos(void *pId)
{
	return static_cast<CWavpackMemoryReader *>(pId)->m_Position;
}

static unsigned int GetLength(void *pId)
{
	return static_cast<CWavpackMemoryReader *>(pId)->m_DataSize;
}

static int PushBackByte(void *pId, int Char)
{
	CWavpackMemoryReader &Reader = *static_cast<CWavpackMemoryReader *>(pId);
	if(Reader.m_Position == 0)
		return -1;
	--Reader.m_Position;
	return Char;
}
#else
// The bundled WavPack 4.40 decoder has neither a reader ID nor a reentrant context.
// The lock in DecodeWV therefore covers the entire decode when using this fallback.
static CLock s_WavpackLock;
static CWavpackMemoryReader s_WavpackReader;

static int ReadDataOld(void *pBuffer, int Size)
{
	return ReadWavpackData(s_WavpackReader, pBuffer, Size);
}
#endif

static void CloseWavpackContext(WavpackContext *pContext)
{
#ifdef CONF_WAVPACK_CLOSE_FILE
	WavpackCloseFile(pContext);
#else
	(void)pContext;
#endif
}

bool CSound::DecodeWV(CSample &Sample, const void *pData, unsigned DataSize, const char *pContextName) const
{
	// no need to load sound when we are running with no sound
	if(!m_SoundEnabled)
		return false;

	CWavpackMemoryReader Reader{static_cast<const uint8_t *>(pData), DataSize, 0};
	char aError[100] = {};

#if defined(CONF_WAVPACK_OPEN_FILE_INPUT_EX)
	WavpackStreamReader Callback = {};
	Callback.can_seek = ReturnFalse;
	Callback.get_length = GetLength;
	Callback.get_pos = GetPos;
	Callback.push_back_byte = PushBackByte;
	Callback.read_bytes = ReadData;
	WavpackContext *pContext = WavpackOpenFileInputEx(&Callback, &Reader, nullptr, aError, 0, 0);
#else
	const CLockScope LockScope(s_WavpackLock);
	s_WavpackReader = Reader;
	WavpackContext *pContext = WavpackOpenFileInput(ReadDataOld, aError);
#endif
	if(!pContext)
	{
		log_error("sound/wv", "Failed to decode sample (%s). Filename='%s'", aError, pContextName);
		return false;
	}

	const unsigned NumSamples = WavpackGetNumSamples(pContext);
	const int BitsPerSample = WavpackGetBitsPerSample(pContext);
	const unsigned SampleRate = WavpackGetSampleRate(pContext);
	const int NumChannels = WavpackGetNumChannels(pContext);

	if(NumChannels < 1 || NumChannels > 2)
	{
		CloseWavpackContext(pContext);
		log_error("sound/wv", "File is not mono or stereo. Filename='%s'", pContextName);
		return false;
	}

	if(BitsPerSample != 16)
	{
		CloseWavpackContext(pContext);
		log_error("sound/wv", "Bits per sample is %d, not 16. Filename='%s'", BitsPerSample, pContextName);
		return false;
	}

	if(NumSamples == 0 || NumSamples > std::numeric_limits<int>::max() || SampleRate == 0 || SampleRate > std::numeric_limits<int>::max() ||
		NumSamples > std::numeric_limits<size_t>::max() / NumChannels / sizeof(int))
	{
		CloseWavpackContext(pContext);
		log_error("sound/wv", "Invalid sample metadata. NumSamples=%u SampleRate=%u NumChannels=%d Filename='%s'", NumSamples, SampleRate, NumChannels, pContextName);
		return false;
	}

	const size_t NumValues = static_cast<size_t>(NumSamples) * NumChannels;
	int *pBuffer = static_cast<int *>(calloc(NumValues, sizeof(int)));
	if(!pBuffer)
	{
		CloseWavpackContext(pContext);
		log_error("sound/wv", "Failed to allocate decode buffer. NumSamples=%u NumChannels=%d Filename='%s'", NumSamples, NumChannels, pContextName);
		return false;
	}

	const unsigned UnpackedSamples = WavpackUnpackSamples(pContext, pBuffer, NumSamples);
	CloseWavpackContext(pContext);
	if(UnpackedSamples != NumSamples)
	{
		free(pBuffer);
		log_error("sound/wv", "WavpackUnpackSamples failed. NumSamples=%u UnpackedSamples=%u NumChannels=%d Filename='%s'", NumSamples, UnpackedSamples, NumChannels, pContextName);
		return false;
	}

	short *pSampleData = static_cast<short *>(calloc(NumValues, sizeof(short)));
	if(!pSampleData)
	{
		free(pBuffer);
		log_error("sound/wv", "Failed to allocate sample buffer. NumSamples=%u NumChannels=%d Filename='%s'", NumSamples, NumChannels, pContextName);
		return false;
	}

	for(size_t i = 0; i < NumValues; ++i)
		pSampleData[i] = static_cast<short>(pBuffer[i]);
	free(pBuffer);

	Sample.m_pData = pSampleData;
	Sample.m_NumFrames = NumSamples;
	Sample.m_Rate = SampleRate;
	Sample.m_Channels = NumChannels;
	Sample.m_LoopStart = 0;
	Sample.m_PausedAt = 0;

	return true;
}

int CSound::LoadOpus(const char *pFilename, int StorageType)
{
	// no need to load sound when we are running with no sound
	if(!m_SoundEnabled)
		return -1;

	CSample *pSample = AllocSample();
	if(!pSample)
	{
		log_error("sound/opus", "Failed to allocate sample ID. Filename='%s'", pFilename);
		return -1;
	}

	void *pData;
	unsigned DataSize;
	if(!m_pStorage->ReadFile(pFilename, StorageType, &pData, &DataSize))
	{
		UnloadSample(pSample->m_Index);
		log_error("sound/opus", "Failed to open file. Filename='%s'", pFilename);
		return -1;
	}

	const bool DecodeSuccess = DecodeOpus(*pSample, pData, DataSize, pFilename);
	free(pData);
	if(!DecodeSuccess)
	{
		UnloadSample(pSample->m_Index);
		return -1;
	}

	if(g_Config.m_Debug)
		log_trace("sound/opus", "Loaded '%s' (index %d)", pFilename, pSample->m_Index);

	RateConvert(*pSample);
	return pSample->m_Index;
}

int CSound::LoadWV(const char *pFilename, int StorageType)
{
	// no need to load sound when we are running with no sound
	if(!m_SoundEnabled)
		return -1;

	CSample *pSample = AllocSample();
	if(!pSample)
	{
		log_error("sound/wv", "Failed to allocate sample ID. Filename='%s'", pFilename);
		return -1;
	}

	void *pData;
	unsigned DataSize;
	if(!m_pStorage->ReadFile(pFilename, StorageType, &pData, &DataSize))
	{
		UnloadSample(pSample->m_Index);
		log_error("sound/wv", "Failed to open file. Filename='%s'", pFilename);
		return -1;
	}

	const bool DecodeSuccess = DecodeWV(*pSample, pData, DataSize, pFilename);
	free(pData);
	if(!DecodeSuccess)
	{
		UnloadSample(pSample->m_Index);
		return -1;
	}

	if(g_Config.m_Debug)
		log_trace("sound/wv", "Loaded '%s' (index %d)", pFilename, pSample->m_Index);

	RateConvert(*pSample);
	return pSample->m_Index;
}

int CSound::LoadOpusFromMem(const void *pData, unsigned DataSize, bool ForceLoad, const char *pContextName)
{
	// no need to load sound when we are running with no sound
	if(!m_SoundEnabled && !ForceLoad)
		return -1;

	CSample *pSample = AllocSample();
	if(!pSample)
		return -1;

	if(!DecodeOpus(*pSample, pData, DataSize, pContextName))
	{
		UnloadSample(pSample->m_Index);
		return -1;
	}

	RateConvert(*pSample);
	return pSample->m_Index;
}

int CSound::LoadWVFromMem(const void *pData, unsigned DataSize, bool ForceLoad, const char *pContextName)
{
	// no need to load sound when we are running with no sound
	if(!m_SoundEnabled && !ForceLoad)
		return -1;

	CSample *pSample = AllocSample();
	if(!pSample)
		return -1;

	if(!DecodeWV(*pSample, pData, DataSize, pContextName))
	{
		UnloadSample(pSample->m_Index);
		return -1;
	}

	RateConvert(*pSample);
	return pSample->m_Index;
}

void CSound::UnloadSample(int SampleId)
{
	if(SampleId == -1)
		return;

	dbg_assert(SampleId >= 0 && SampleId < NUM_SAMPLES, "SampleId invalid");
	const CLockScope LockScope(m_SoundLock);
	CSample &Sample = m_aSamples[SampleId];

	if(Sample.IsLoaded())
	{
		// Stop voices using this sample
		for(auto &Voice : m_aVoices)
		{
			if(Voice.m_pSample == &Sample)
			{
				Voice.m_pSample = nullptr;
			}
		}

		// Free data
		free(Sample.m_pData);
		Sample.m_pData = nullptr;
	}

	// Free slot
	if(Sample.m_NextFreeSampleIndex == SAMPLE_INDEX_USED)
	{
		Sample.m_NextFreeSampleIndex = m_FirstFreeSampleIndex;
		m_FirstFreeSampleIndex = Sample.m_Index;
	}
}

float CSound::GetSampleTotalTime(int SampleId)
{
	dbg_assert(SampleId >= 0 && SampleId < NUM_SAMPLES, "SampleId invalid");

	const CLockScope LockScope(m_SoundLock);
	dbg_assert(m_aSamples[SampleId].IsLoaded(), "Sample not loaded");
	return m_aSamples[SampleId].TotalTime();
}

float CSound::GetSampleCurrentTime(int SampleId)
{
	dbg_assert(SampleId >= 0 && SampleId < NUM_SAMPLES, "SampleId invalid");

	const CLockScope LockScope(m_SoundLock);
	dbg_assert(m_aSamples[SampleId].IsLoaded(), "Sample not loaded");
	CSample *pSample = &m_aSamples[SampleId];
	for(int VoiceId = 0; VoiceId < NUM_VOICES_PER_CONTEXT; ++VoiceId)
	{
		CVoice &Voice = m_aVoices[VoiceId];
		if(Voice.m_pSample == pSample)
		{
			return Voice.m_Tick / (float)pSample->m_Rate;
		}
	}

	return pSample->m_PausedAt / (float)pSample->m_Rate;
}

void CSound::SetSampleCurrentTime(int SampleId, float Time)
{
	dbg_assert(SampleId >= 0 && SampleId < NUM_SAMPLES, "SampleId invalid");

	const CLockScope LockScope(m_SoundLock);
	dbg_assert(m_aSamples[SampleId].IsLoaded(), "Sample not loaded");
	CSample *pSample = &m_aSamples[SampleId];
	for(int VoiceId = 0; VoiceId < NUM_VOICES_PER_CONTEXT; ++VoiceId)
	{
		CVoice &Voice = m_aVoices[VoiceId];
		if(Voice.m_pSample == pSample)
		{
			Voice.m_Tick = pSample->m_NumFrames * Time;
			return;
		}
	}

	pSample->m_PausedAt = pSample->m_NumFrames * Time;
}

void CSound::SetChannel(int ChannelId, float Vol, float Pan)
{
	dbg_assert(ChannelId >= 0 && ChannelId < NUM_CHANNELS, "ChannelId invalid");

	const CLockScope LockScope(m_SoundLock);
	m_aChannels[ChannelId].m_Vol = (int)(Vol * 255.0f);
	m_aChannels[ChannelId].m_Pan = (int)(Pan * 255.0f); // TODO: this is only on and off right now
}

void CSound::SetListenerPosition(vec2 Position)
{
	m_ListenerPositionX.store(Position.x, std::memory_order_relaxed);
	m_ListenerPositionY.store(Position.y, std::memory_order_relaxed);
}

void CSound::SetOfflineListenerPosition(vec2 Position)
{
	m_OfflineListenerPositionX.store(Position.x, std::memory_order_relaxed);
	m_OfflineListenerPositionY.store(Position.y, std::memory_order_relaxed);
}

void CSound::SetVoiceVolume(CVoiceHandle Voice, float Volume)
{
	if(!Voice.IsValid())
		return;

	int VoiceId = Voice.Id();

	const CLockScope LockScope(m_SoundLock);
	if(m_aVoices[VoiceId].m_Age != Voice.Age())
		return;

	Volume = std::clamp(Volume, 0.0f, 1.0f);
	m_aVoices[VoiceId].m_Vol = (int)(Volume * 255.0f);
}

void CSound::SetVoiceFalloff(CVoiceHandle Voice, float Falloff)
{
	if(!Voice.IsValid())
		return;

	int VoiceId = Voice.Id();

	const CLockScope LockScope(m_SoundLock);
	if(m_aVoices[VoiceId].m_Age != Voice.Age())
		return;

	Falloff = std::clamp(Falloff, 0.0f, 1.0f);
	m_aVoices[VoiceId].m_Falloff = Falloff;
}

void CSound::SetVoicePosition(CVoiceHandle Voice, vec2 Position)
{
	if(!Voice.IsValid())
		return;

	int VoiceId = Voice.Id();

	const CLockScope LockScope(m_SoundLock);
	if(m_aVoices[VoiceId].m_Age != Voice.Age())
		return;

	m_aVoices[VoiceId].m_Position = Position;
}

void CSound::SetVoiceTimeOffset(CVoiceHandle Voice, float TimeOffset)
{
	if(!Voice.IsValid())
		return;

	int VoiceId = Voice.Id();

	const CLockScope LockScope(m_SoundLock);
	if(m_aVoices[VoiceId].m_Age != Voice.Age())
		return;

	if(!m_aVoices[VoiceId].m_pSample)
		return;

	int Tick = 0;
	bool IsLooping = m_aVoices[VoiceId].m_Flags & ISound::FLAG_LOOP;
	uint64_t TickOffset = m_aVoices[VoiceId].m_pSample->m_Rate * TimeOffset;
	if(m_aVoices[VoiceId].m_pSample->m_NumFrames > 0 && IsLooping)
	{
		const int LoopStart = m_aVoices[VoiceId].m_pSample->m_LoopStart;
		const int NumFrames = m_aVoices[VoiceId].m_pSample->m_NumFrames;
		if(TickOffset < static_cast<uint64_t>(NumFrames))
		{
			// Still in first playthrough
			Tick = TickOffset;
		}
		else
		{
			// Past first playthrough, wrap within loop section only
			const int LoopLength = NumFrames - LoopStart;
			if(LoopLength > 0)
				Tick = LoopStart + ((TickOffset - NumFrames) % LoopLength);
			else
				Tick = LoopStart;
		}
	}
	else
	{
		Tick = std::clamp<uint64_t>(TickOffset, 0, m_aVoices[VoiceId].m_pSample->m_NumFrames);
	}

	// at least 200msec off, else depend on buffer size
	float Threshold = std::max(0.2f * m_aVoices[VoiceId].m_pSample->m_Rate, (float)m_MaxFrames);
	if(absolute(m_aVoices[VoiceId].m_Tick - Tick) > Threshold)
	{
		// take care of looping (modulo!)
		if(!(IsLooping && (std::min(m_aVoices[VoiceId].m_Tick, Tick) + m_aVoices[VoiceId].m_pSample->m_NumFrames - std::max(m_aVoices[VoiceId].m_Tick, Tick)) <= Threshold))
		{
			m_aVoices[VoiceId].m_Tick = Tick;
		}
	}
}

void CSound::SetVoiceCircle(CVoiceHandle Voice, float Radius)
{
	if(!Voice.IsValid())
		return;

	int VoiceId = Voice.Id();

	const CLockScope LockScope(m_SoundLock);
	if(m_aVoices[VoiceId].m_Age != Voice.Age())
		return;

	m_aVoices[VoiceId].m_Shape = ISound::SHAPE_CIRCLE;
	m_aVoices[VoiceId].m_Circle.m_Radius = std::max(0.0f, Radius);
}

void CSound::SetVoiceRectangle(CVoiceHandle Voice, float Width, float Height)
{
	if(!Voice.IsValid())
		return;

	int VoiceId = Voice.Id();

	const CLockScope LockScope(m_SoundLock);
	if(m_aVoices[VoiceId].m_Age != Voice.Age())
		return;

	m_aVoices[VoiceId].m_Shape = ISound::SHAPE_RECTANGLE;
	m_aVoices[VoiceId].m_Rectangle.m_Width = std::max(0.0f, Width);
	m_aVoices[VoiceId].m_Rectangle.m_Height = std::max(0.0f, Height);
}

ISound::CVoiceHandle CSound::StartVoice(int ChannelId, int SampleId, int Flags, float Volume, vec2 Position, bool Offline)
{
	const CLockScope LockScope(m_SoundLock);

	// search for voice in the selected context
	const int FirstVoice = Offline ? NUM_VOICES_PER_CONTEXT : 0;
	int &NextVoice = Offline ? m_NextOfflineVoice : m_NextVoice;
	int VoiceId = -1;
	for(int i = 0; i < NUM_VOICES_PER_CONTEXT; i++)
	{
		const int NextId = FirstVoice + (NextVoice + i) % NUM_VOICES_PER_CONTEXT;
		if(!m_aVoices[NextId].m_pSample)
		{
			VoiceId = NextId;
			NextVoice = (NextVoice + i + 1) % NUM_VOICES_PER_CONTEXT;
			break;
		}
	}
	if(VoiceId == -1)
	{
		return CreateVoiceHandle(-1, -1);
	}

	// voice found, use it
	m_aVoices[VoiceId].m_pSample = &m_aSamples[SampleId];
	m_aVoices[VoiceId].m_pChannel = &m_aChannels[ChannelId];
	if(!Offline && Flags & FLAG_LOOP)
	{
		m_aVoices[VoiceId].m_Tick = m_aSamples[SampleId].m_PausedAt;
	}
	else if(!Offline && Flags & FLAG_PREVIEW)
	{
		m_aVoices[VoiceId].m_Tick = m_aSamples[SampleId].m_PausedAt;
		m_aSamples[SampleId].m_PausedAt = 0;
	}
	else
	{
		m_aVoices[VoiceId].m_Tick = 0;
	}
	m_aVoices[VoiceId].m_Vol = (int)(std::clamp(Volume, 0.0f, 1.0f) * 255.0f);
	m_aVoices[VoiceId].m_Flags = Flags;
	m_aVoices[VoiceId].m_Position = Position;
	m_aVoices[VoiceId].m_Falloff = 0.0f;
	m_aVoices[VoiceId].m_Shape = ISound::SHAPE_CIRCLE;
	m_aVoices[VoiceId].m_Circle.m_Radius = 1500;
	return CreateVoiceHandle(VoiceId, m_aVoices[VoiceId].m_Age);
}

ISound::CVoiceHandle CSound::PlayAt(int ChannelId, int SampleId, int Flags, float Volume, vec2 Position)
{
	return StartVoice(ChannelId, SampleId, Flags | ISound::FLAG_POS, Volume, Position, false);
}

ISound::CVoiceHandle CSound::Play(int ChannelId, int SampleId, int Flags, float Volume)
{
	return StartVoice(ChannelId, SampleId, Flags, Volume, vec2(0.0f, 0.0f), false);
}

ISound::CVoiceHandle CSound::PlayAtOffline(int ChannelId, int SampleId, int Flags, float Volume, vec2 Position)
{
	return StartVoice(ChannelId, SampleId, Flags | ISound::FLAG_POS, Volume, Position, true);
}

ISound::CVoiceHandle CSound::PlayOffline(int ChannelId, int SampleId, int Flags, float Volume)
{
	return StartVoice(ChannelId, SampleId, Flags, Volume, vec2(0.0f, 0.0f), true);
}

void CSound::Pause(int SampleId)
{
	dbg_assert(SampleId >= 0 && SampleId < NUM_SAMPLES, "SampleId invalid");

	// TODO: a nice fade out
	const CLockScope LockScope(m_SoundLock);
	CSample *pSample = &m_aSamples[SampleId];
	dbg_assert(m_aSamples[SampleId].IsLoaded(), "Sample not loaded");
	for(int VoiceId = 0; VoiceId < NUM_VOICES_PER_CONTEXT; ++VoiceId)
	{
		CVoice &Voice = m_aVoices[VoiceId];
		if(Voice.m_pSample == pSample)
		{
			Voice.m_pSample->m_PausedAt = Voice.m_Tick;
			Voice.m_pSample = nullptr;
		}
	}
}

void CSound::Stop(int SampleId)
{
	dbg_assert(SampleId >= 0 && SampleId < NUM_SAMPLES, "SampleId invalid");

	// TODO: a nice fade out
	const CLockScope LockScope(m_SoundLock);
	CSample *pSample = &m_aSamples[SampleId];
	dbg_assert(m_aSamples[SampleId].IsLoaded(), "Sample not loaded");
	for(int VoiceId = 0; VoiceId < NUM_VOICES_PER_CONTEXT; ++VoiceId)
	{
		CVoice &Voice = m_aVoices[VoiceId];
		if(Voice.m_pSample == pSample)
		{
			if(Voice.m_Flags & FLAG_LOOP)
				Voice.m_pSample->m_PausedAt = Voice.m_Tick;
			else
				Voice.m_pSample->m_PausedAt = 0;
			Voice.m_pSample = nullptr;
		}
	}
}

void CSound::StopAll()
{
	// TODO: a nice fade out
	const CLockScope LockScope(m_SoundLock);
	for(int VoiceId = 0; VoiceId < NUM_VOICES_PER_CONTEXT; ++VoiceId)
	{
		CVoice &Voice = m_aVoices[VoiceId];
		if(Voice.m_pSample)
		{
			if(Voice.m_Flags & FLAG_LOOP)
				Voice.m_pSample->m_PausedAt = Voice.m_Tick;
			else
				Voice.m_pSample->m_PausedAt = 0;
		}
		Voice.m_pSample = nullptr;
	}
}

void CSound::StopOffline()
{
	const CLockScope LockScope(m_SoundLock);
	for(int VoiceId = NUM_VOICES_PER_CONTEXT; VoiceId < NUM_VOICES; ++VoiceId)
	{
		CVoice &Voice = m_aVoices[VoiceId];
		Voice.m_pSample = nullptr;
		Voice.m_Age++;
	}
}

void CSound::StopVoice(CVoiceHandle Voice)
{
	if(!Voice.IsValid())
		return;

	int VoiceId = Voice.Id();

	const CLockScope LockScope(m_SoundLock);
	if(m_aVoices[VoiceId].m_Age != Voice.Age())
		return;

	m_aVoices[VoiceId].m_pSample = nullptr;
	m_aVoices[VoiceId].m_Age++;
}

bool CSound::IsPlaying(int SampleId)
{
	dbg_assert(SampleId >= 0 && SampleId < NUM_SAMPLES, "SampleId invalid");
	const CLockScope LockScope(m_SoundLock);
	const CSample *pSample = &m_aSamples[SampleId];
	dbg_assert(m_aSamples[SampleId].IsLoaded(), "Sample not loaded");
	return std::any_of(std::begin(m_aVoices), std::begin(m_aVoices) + NUM_VOICES_PER_CONTEXT, [pSample](const auto &Voice) { return Voice.m_pSample == pSample; });
}

void CSound::PauseAudioDevice()
{
#if !defined(CONF_DEMO_RENDER_TOOL)
	SDL_PauseAudioDevice(m_Device, 1);
#endif
}

void CSound::UnpauseAudioDevice()
{
#if !defined(CONF_DEMO_RENDER_TOOL)
	SDL_PauseAudioDevice(m_Device, 0);
#endif
}

IEngineSound *CreateEngineSound() { return new CSound; }
