#include <base/lock.h>
#include <base/thread.h>

#include <engine/client/sound.h>
#include <engine/shared/config.h>
#include <engine/shared/jobs.h>

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

class CSoundTestAccess
{
public:
	struct CSampleData
	{
		int m_NumFrames;
		int m_Rate;
		int m_Channels;
		std::vector<short> m_vPcm;

		bool operator==(const CSampleData &) const = default;
	};

	static CSampleData SampleData(CSound &Sound, int SampleId)
	{
		const CLockScope LockScope(Sound.m_SoundLock);
		const CSample &Sample = Sound.m_aSamples[SampleId];
		return {
			Sample.m_NumFrames,
			Sample.m_Rate,
			Sample.m_Channels,
			std::vector<short>(Sample.m_pData, Sample.m_pData + Sample.m_NumFrames * Sample.m_Channels),
		};
	}

	static void Init(CSound &Sound)
	{
		const CLockScope LockScope(Sound.m_SoundLock);
		Sound.m_SoundEnabled = true;
		Sound.m_FirstFreeSampleIndex = 0;
		for(size_t i = 0; i < std::size(Sound.m_aSamples); ++i)
		{
			CSample &Sample = Sound.m_aSamples[i];
			Sample.m_Index = i;
			Sample.m_NextFreeSampleIndex = i + 1;
			Sample.m_pData = nullptr;
		}
		Sound.m_aSamples[std::size(Sound.m_aSamples) - 1].m_NextFreeSampleIndex = -1;
	}
};

namespace
{
	class CLoadWavpackJob final : public IJob
	{
		ISound *m_pSound;
		const std::vector<uint8_t> &m_vData;
		const char *m_pName;
		std::atomic<int> &m_Ready;
		std::atomic<bool> &m_Start;

		void Run() override
		{
			++m_Ready;
			while(!m_Start.load())
				thread_yield();
			m_SampleId = m_pSound->LoadWVFromMem(m_vData.data(), m_vData.size(), true, m_pName);
		}

	public:
		int m_SampleId = -1;

		CLoadWavpackJob(ISound *pSound, const std::vector<uint8_t> &vData, const char *pName, std::atomic<int> &Ready, std::atomic<bool> &Start) :
			m_pSound(pSound),
			m_vData(vData),
			m_pName(pName),
			m_Ready(Ready),
			m_Start(Start)
		{
		}
	};

	std::vector<uint8_t> ReadFile(const char *pPath)
	{
		std::ifstream File(pPath, std::ios::binary);
		return {std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>()};
	}

	struct CSoundInstance
	{
		std::unique_ptr<CSound> m_pSound{std::make_unique<CSound>()};

		CSoundInstance() { CSoundTestAccess::Init(*m_pSound); }

		~CSoundInstance()
		{
			m_pSound->Shutdown();
		}
	};

	class CScopedSoundDisable
	{
		int m_OldSoundEnable = g_Config.m_SndEnable;

	public:
		CScopedSoundDisable() { g_Config.m_SndEnable = 0; }
		~CScopedSoundDisable() { g_Config.m_SndEnable = m_OldSoundEnable; }
	};
}

TEST(Sound, ParallelWavpackMatchesSerialPcm)
{
	const CScopedSoundDisable DisableSound;
	const std::vector<uint8_t> vFirst = ReadFile("data/audio/foley_body_impact-01.wv");
	const std::vector<uint8_t> vSecond = ReadFile("data/audio/sfx_msg-client.wv");
	ASSERT_FALSE(vFirst.empty());
	ASSERT_FALSE(vSecond.empty());

	CSoundInstance Serial;
	const int SerialFirstId = Serial.m_pSound->LoadWVFromMem(vFirst.data(), vFirst.size(), true, "serial-first.wv");
	const int SerialSecondId = Serial.m_pSound->LoadWVFromMem(vSecond.data(), vSecond.size(), true, "serial-second.wv");
	ASSERT_GE(SerialFirstId, 0);
	ASSERT_GE(SerialSecondId, 0);
	const CSoundTestAccess::CSampleData SerialFirst = CSoundTestAccess::SampleData(*Serial.m_pSound, SerialFirstId);
	const CSoundTestAccess::CSampleData SerialSecond = CSoundTestAccess::SampleData(*Serial.m_pSound, SerialSecondId);

	CSoundInstance Parallel;
	std::atomic<int> Ready{0};
	std::atomic<bool> Start{false};
	CJobPool Pool;
	Pool.Init(2);
	auto pFirstJob = std::make_shared<CLoadWavpackJob>(Parallel.m_pSound.get(), vFirst, "parallel-first.wv", Ready, Start);
	auto pSecondJob = std::make_shared<CLoadWavpackJob>(Parallel.m_pSound.get(), vSecond, "parallel-second.wv", Ready, Start);
	Pool.Add(pFirstJob);
	Pool.Add(pSecondJob);
	while(Ready.load() != 2)
		thread_yield();
	Start = true;
	while(!pFirstJob->Done() || !pSecondJob->Done())
		thread_yield();
	Pool.Shutdown();
	ASSERT_GE(pFirstJob->m_SampleId, 0);
	ASSERT_GE(pSecondJob->m_SampleId, 0);
	EXPECT_EQ(CSoundTestAccess::SampleData(*Parallel.m_pSound, pFirstJob->m_SampleId), SerialFirst);
	EXPECT_EQ(CSoundTestAccess::SampleData(*Parallel.m_pSound, pSecondJob->m_SampleId), SerialSecond);
}
