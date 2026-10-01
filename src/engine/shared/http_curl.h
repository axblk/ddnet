#ifndef ENGINE_SHARED_HTTP_CURL_H
#define ENGINE_SHARED_HTTP_CURL_H

#include <base/detect.h>
#if !defined(CONF_PLATFORM_EMSCRIPTEN)

#include <engine/http.h>

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

class CHttpRequestCurl : public IHttpRequest
{
	friend class CHttpCurl;

public:
	CHttpRequestCurl(const char *pUrl);
	~CHttpRequestCurl() override;

	void Header(const char *pNameColonValue) override;

private:
	curl_slist *m_pRequestHeaders = nullptr;

	char m_aErr[CURL_ERROR_SIZE];

	bool m_ResponseHeadersEnded = false;

	bool ConfigureHandle(CURL *pHandle);

	// Abort the request if `OnHeader()` returns something other than
	// `DataSize`. `pHeader` is NOT null-terminated.
	size_t OnHeader(char *pHeader, size_t HeaderSize);

	// `pHandle` can be nullptr if no handle was ever created for this request.
	void OnCompletionInternal(CURL *pHandle, CURLcode Code);

	static int ProgressCallback(void *pUser, double DlTotal, double DlCurr, double UlTotal, double UlCurr);
	static size_t HeaderCallback(char *pData, size_t Size, size_t Number, void *pUser);
	static size_t WriteCallback(char *pData, size_t Size, size_t Number, void *pUser);
};

// The HTTP thread and its requests. The thread holds the worker as well, so
// that it can be left behind when it is stuck inside curl.
class CHttpCurl
{
public:
	bool Init(std::chrono::milliseconds ShutdownDelay, const std::shared_ptr<CHttpCurl> &pSelf);
	void Shutdown();
	// Waits for the thread at most for the shutdown delay and a second more,
	// returns whether it ended.
	bool WaitForThread();
	void *Thread() const { return m_pThread; }

	void Run(const std::shared_ptr<IHttpRequest> &pRequest);
	bool HasIpresolveBug() const;

private:
	enum EState
	{
		UNINITIALIZED,
		RUNNING,
		ERROR,
	};

	void *m_pThread = nullptr;

	std::mutex m_Lock;
	std::condition_variable m_ConditionVariableInit;
	std::atomic<EState> m_State = UNINITIALIZED;
	std::deque<std::shared_ptr<CHttpRequestCurl>> m_PendingRequests;
	std::unordered_map<CURL *, std::shared_ptr<CHttpRequestCurl>> m_RunningRequests;
	std::chrono::milliseconds m_ShutdownDelay{};
	std::optional<std::chrono::time_point<std::chrono::steady_clock>> m_ShutdownTime;
	std::atomic<bool> m_Shutdown = false;

	// Only to be used with curl_multi_wakeup
	CURLM *m_pMultiH = nullptr;
	int m_NextTimeout;

	std::mutex m_FinishedLock;
	std::condition_variable m_FinishedCondition;
	bool m_Finished = false;

	static void ThreadMain(void *pUser);
	void RunLoop();
};

class CEngineHttpCurl : public IEngineHttp
{
public:
	// Lifecycle
	bool Init(std::chrono::milliseconds ShutdownDelay) override;
	void Shutdown() override;
	~CEngineHttpCurl() override;

	// User
	void Run(std::shared_ptr<IHttpRequest> pRequest) override;
	bool HasIpresolveBug() const override;

private:
	std::shared_ptr<CHttpCurl> m_pWorker = std::make_shared<CHttpCurl>();
};

#endif // !CONF_PLATFORM_EMSCRIPTEN
#endif // ENGINE_SHARED_HTTP_CURL_H
