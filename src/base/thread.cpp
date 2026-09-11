/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "thread.h"

#include "dbg.h"
#include "detect.h"
#include "time.h"
#include "windows.h"

#include <algorithm>
#include <thread>

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#endif

#if defined(CONF_FAMILY_UNIX)
#include <pthread.h>
#include <sched.h>

#include <cstdlib>

#if defined(CONF_PLATFORM_MACOS)
#if defined(__MAC_10_10) && __MAC_OS_X_VERSION_MIN_REQUIRED >= __MAC_10_10
#include <pthread/qos.h>
#endif
#endif

#if defined(CONF_PLATFORM_MACOS) || defined(CONF_PLATFORM_IOS)
#include <mach/task_policy.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#elif defined(CONF_PLATFORM_LINUX) || defined(CONF_PLATFORM_ANDROID)
#include <sys/prctl.h>
#endif

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#endif

#elif defined(CONF_FAMILY_WINDOWS)
#include <windows.h>
#else
#error NOT IMPLEMENTED
#endif

struct THREAD_RUN
{
	void (*threadfunc)(void *);
	void *u;
};

#if defined(CONF_FAMILY_UNIX)
static void *thread_run(void *user)
#elif defined(CONF_FAMILY_WINDOWS)
static DWORD __stdcall thread_run(void *user)
#else
#error not implemented
#endif
{
#if defined(CONF_FAMILY_WINDOWS)
	CWindowsComLifecycle WindowsComLifecycle(false);
#endif
	struct THREAD_RUN *data = (THREAD_RUN *)user;
	void (*threadfunc)(void *) = data->threadfunc;
	void *u = data->u;
	free(data);
	threadfunc(u);
#if defined(CONF_FAMILY_UNIX)
	return nullptr;
#elif defined(CONF_FAMILY_WINDOWS)
	return 0;
#else
#error not implemented
#endif
}

void *thread_init(void (*threadfunc)(void *), void *u, const char *name)
{
	struct THREAD_RUN *data = (THREAD_RUN *)malloc(sizeof(*data));
	data->threadfunc = threadfunc;
	data->u = u;
#if defined(CONF_FAMILY_UNIX)
	{
		pthread_attr_t attr;
		dbg_assert(pthread_attr_init(&attr) == 0, "pthread_attr_init failure");
#if defined(CONF_PLATFORM_MACOS) && defined(__MAC_10_10) && __MAC_OS_X_VERSION_MIN_REQUIRED >= __MAC_10_10
		dbg_assert(pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0) == 0, "pthread_attr_set_qos_class_np failure");
#endif
		pthread_t id;
		dbg_assert(pthread_create(&id, &attr, thread_run, data) == 0, "pthread_create failure");
#if defined(CONF_PLATFORM_EMSCRIPTEN)
		// Return control to the browser's main thread to allow the pthread to be started,
		// otherwise we deadlock when waiting for a thread immediately after starting it.
		emscripten_sleep(0);
#endif
		return (void *)id;
	}
#elif defined(CONF_FAMILY_WINDOWS)
	HANDLE thread = CreateThread(nullptr, 0, thread_run, data, 0, nullptr);
	dbg_assert(thread != nullptr, "CreateThread failure");
	HMODULE kernel_base_handle;
	if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"KernelBase.dll", &kernel_base_handle))
	{
		// Intentional
#ifdef __MINGW32__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
		auto set_thread_description_function = reinterpret_cast<HRESULT(WINAPI *)(HANDLE, PCWSTR)>(GetProcAddress(kernel_base_handle, "SetThreadDescription"));
#ifdef __MINGW32__
#pragma GCC diagnostic pop
#endif
		if(set_thread_description_function)
			set_thread_description_function(thread, windows_utf8_to_wide(name).c_str());
	}
	return thread;
#else
#error not implemented
#endif
}

void thread_wait(void *thread)
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	// TODO: Remove this workaround when https://github.com/emscripten-core/emscripten/issues/9910 is fixed.
	while(true)
	{
		const int join_result = pthread_tryjoin_np((pthread_t)thread, nullptr);
		if(join_result == 0)
		{
			break;
		}
		dbg_assert(join_result == EBUSY, "pthread_tryjoin_np failure");
		// Busy waiting so we can periodically yield control to browser's
		// main thread because blocking on the main thread is very bad.
		emscripten_sleep(10);
	}
#elif defined(CONF_FAMILY_UNIX)
	dbg_assert(pthread_join((pthread_t)thread, nullptr) == 0, "pthread_join failure");
#elif defined(CONF_FAMILY_WINDOWS)
	dbg_assert(WaitForSingleObject((HANDLE)thread, INFINITE) == WAIT_OBJECT_0, "WaitForSingleObject failure");
	dbg_assert(CloseHandle(thread), "CloseHandle failure");
#else
#error not implemented
#endif
}

void thread_wait_for_other_threads()
{
	// Long enough not to be a spin, short enough that nobody waiting on the
	// result notices the wait got longer.
	std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void thread_sleep_idle(std::chrono::nanoseconds duration)
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	// Sleeping keeps the browser's main thread to itself, so the page neither
	// paints nor delivers input for as long as it lasts. Emscripten's sleep is
	// the one that hands control back, and it counts in whole milliseconds; a
	// duration of none still hands it back once.
	const int64_t milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
	emscripten_sleep(milliseconds > 0 ? milliseconds : 0);
#else
	if(duration > std::chrono::nanoseconds::zero())
		std::this_thread::sleep_for(duration);
	else
		thread_yield();
#endif
}

void thread_sleep_until_next_frame(std::chrono::nanoseconds &next_frame_time, int refresh_rate)
{
	const std::chrono::nanoseconds now = time_get_nanoseconds();
	if(refresh_rate > 0 && next_frame_time > std::chrono::nanoseconds::zero())
	{
		const std::chrono::nanoseconds frame_time = std::chrono::nanoseconds(std::chrono::seconds(1)) / refresh_rate;
		next_frame_time = std::max(next_frame_time + frame_time, now);
	}
	else
	{
		next_frame_time = now;
	}
	thread_sleep_idle(next_frame_time - now);
}

void thread_yield()
{
#if defined(CONF_FAMILY_UNIX)
	dbg_assert(sched_yield() == 0, "sched_yield failure");
#elif defined(CONF_FAMILY_WINDOWS)
	Sleep(0);
#else
#error not implemented
#endif
}

void thread_request_precise_wakeups()
{
#if defined(CONF_PLATFORM_MACOS) || defined(CONF_PLATFORM_IOS)
	// Not asserted, the system is free to decline the policy.
	thread_latency_qos_policy_data_t policy = {LATENCY_QOS_TIER_0};
	thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_LATENCY_QOS_POLICY, (thread_policy_t)&policy, THREAD_LATENCY_QOS_POLICY_COUNT);
#elif defined(CONF_PLATFORM_LINUX) || defined(CONF_PLATFORM_ANDROID)
	dbg_assert(prctl(PR_SET_TIMERSLACK, 1) == 0, "prctl(PR_SET_TIMERSLACK) failure");
#endif
}

void thread_detach(void *thread)
{
#if defined(CONF_FAMILY_UNIX)
	dbg_assert(pthread_detach((pthread_t)thread) == 0, "pthread_detach failure");
#elif defined(CONF_FAMILY_WINDOWS)
	dbg_assert(CloseHandle(thread), "CloseHandle failure");
#else
#error not implemented
#endif
}

void thread_init_and_detach(void (*threadfunc)(void *), void *u, const char *name)
{
	void *thread = thread_init(threadfunc, u, name);
	thread_detach(thread);
}
