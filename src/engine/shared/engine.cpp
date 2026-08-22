/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include <base/io.h>
#include <base/logger.h>
#include <base/net.h>
#include <base/os.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/console.h>
#include <engine/engine.h>
#include <engine/shared/config.h>
#include <engine/shared/jobs.h>
#include <engine/storage.h>

#include <thread>

class CEngine : public IEngine
{
	IConsole *m_pConsole;
	IStorage *m_pStorage;

	std::shared_ptr<CFutureLogger> m_pFutureLogger;

	char m_aAppName[256];

	CJobPool m_JobPool;
	size_t m_JobThreadCount = 0;

public:
	CEngine(bool Test, const char *pAppname, std::shared_ptr<CFutureLogger> pFutureLogger) :
		m_pFutureLogger(std::move(pFutureLogger))
	{
		str_copy(m_aAppName, pAppname);
		if(!Test)
		{
			log_info("engine", "running on %s-%s-%s", CONF_FAMILY_STRING, CONF_PLATFORM_STRING, CONF_ARCH_STRING);
			log_info("engine", "arch is %s", CONF_ARCH_ENDIAN_STRING);

			char aVersionStr[128];
			if(os_version_str(aVersionStr, sizeof(aVersionStr)))
			{
				log_info("engine", "operating system version: %s", aVersionStr);
			}

			// init the network
			net_init();
		}

#if defined(CONF_PLATFORM_EMSCRIPTEN)
		// Make sure we don't use more threads than allowed in total (see PTHREAD_POOL_SIZE in Emscripten.toolchain)
		// otherwise starting more threads may lead to deadlocks as the threads will simply not start.
		m_JobThreadCount = 4;
#else
		m_JobThreadCount = std::max(4, (int)std::thread::hardware_concurrency()) - 2;
#endif
		m_JobPool.Init(m_JobThreadCount);
	}

	void Init() override
	{
		m_pConsole = Kernel()->RequestInterface<IConsole>();
		m_pStorage = Kernel()->RequestInterface<IStorage>();

		if(!m_pConsole || !m_pStorage)
			return;
	}

	void AddJob(std::shared_ptr<IJob> pJob) override
	{
		m_JobPool.Add(std::move(pJob));
	}

	size_t JobThreadCount() const override
	{
		return m_JobThreadCount;
	}

	void ShutdownJobs() override
	{
		m_JobPool.Shutdown();
	}

	void SetAdditionalLogger(std::shared_ptr<ILogger> &&pLogger) override
	{
		m_pFutureLogger->Set(pLogger);
	}
};

IEngine *CreateEngine(const char *pAppname, std::shared_ptr<CFutureLogger> pFutureLogger) { return new CEngine(false, pAppname, std::move(pFutureLogger)); }
IEngine *CreateTestEngine(const char *pAppname) { return new CEngine(true, pAppname, nullptr); }
