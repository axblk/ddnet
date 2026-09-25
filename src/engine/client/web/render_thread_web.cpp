#include "render_thread_web.h"

#include "web_platform.h"

#include <base/dbg.h>
#include <base/log.h>

#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <atomic>

namespace
{
	struct SStart
	{
		bool m_TakeCanvas;
		std::atomic<int> m_Result{0};
	};

	void *ThreadStart(void *pUser)
	{
		SStart *pStart = static_cast<SStart *>(pUser);
		const bool HasCanvas = !pStart->m_TakeCanvas || ddnet_web_render_thread_attach() != 0;
		// The thread lives on in the event loop after this returns, until
		// Stop lets it go.
		emscripten_runtime_keepalive_push();
		pStart->m_Result.store(HasCanvas ? 1 : -1);
		emscripten_futex_wake(&pStart->m_Result, 1);
		return nullptr;
	}

	void EndTask(void *)
	{
		// The thread ends when this task is over and nothing keeps it.
		emscripten_runtime_keepalive_pop();
	}

	void CallTask(void *pUser)
	{
		(*static_cast<const std::function<void()> *>(pUser))();
	}

	struct SCreate
	{
		SStart *m_pStart;
		pthread_t m_Thread{};
		int m_Error = 0;
	};

	void CreateTask(void *pUser)
	{
		SCreate *pCreate = static_cast<SCreate *>(pUser);
		pthread_attr_t Attributes;
		pthread_attr_init(&Attributes);
		// The canvas goes to the thread as it is made, the only way a canvas
		// changes threads. Emscripten finds the page's canvas under this name
		// whatever its id.
		if(pCreate->m_pStart->m_TakeCanvas)
			emscripten_pthread_attr_settransferredcanvases(&Attributes, "#canvas");
		pCreate->m_Error = pthread_create(&pCreate->m_Thread, &Attributes, ThreadStart, pCreate->m_pStart);
		pthread_attr_destroy(&Attributes);
	}

} // namespace

bool CWebRenderThread::Start(bool TakeCanvas)
{
	dbg_assert(!m_Running, "The render thread runs already");
	SStart Start;
	Start.m_TakeCanvas = TakeCanvas;
	SCreate Create;
	Create.m_pStart = &Start;
	// Only the page's thread has the page's canvas to hand over, so the
	// thread that is to own it is made there.
	if(TakeCanvas && !emscripten_is_main_runtime_thread())
		emscripten_proxy_sync(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), CreateTask, &Create);
	else
		CreateTask(&Create);
	m_Thread = Create.m_Thread;
	if(Create.m_Error != 0)
	{
		log_error("gfx", "Could not start the render thread (%d)", Create.m_Error);
		return false;
	}
	// A thread that is handed a canvas is started by the page's thread,
	// which is only told to and answers later; this waits for it here, never
	// on the page's thread.
	while(Start.m_Result.load() == 0)
		emscripten_futex_wait(&Start.m_Result, 0, 100.0);
	m_Running = true;
	if(Start.m_Result.load() < 0)
	{
		log_error("gfx", "The render thread was not given the canvas");
		Stop();
		return false;
	}
	return true;
}

void CWebRenderThread::Stop()
{
	if(!m_Running)
		return;
	Post(EndTask, nullptr);
	pthread_join(m_Thread, nullptr);
	m_Running = false;
}

bool CWebRenderThread::OnThread() const
{
	return m_Running && pthread_equal(pthread_self(), m_Thread);
}

void CWebRenderThread::Post(void (*pfnTask)(void *), void *pUser) const
{
	dbg_assert(m_Running, "Posted to a render thread that does not run");
	emscripten_proxy_async(emscripten_proxy_get_system_queue(), m_Thread, pfnTask, pUser);
}

void CWebRenderThread::Call(const std::function<void()> &Task) const
{
	dbg_assert(m_Running, "Called a render thread that does not run");
	if(OnThread())
	{
		Task();
		return;
	}
	dbg_assert(!emscripten_is_main_runtime_thread(), "The page's thread must not wait for the render thread");
	emscripten_proxy_sync(emscripten_proxy_get_system_queue(), m_Thread, CallTask, const_cast<std::function<void()> *>(&Task));
}

void CWebRenderThread::PostAtFrame(void (*pfnTask)(void *), void *pUser)
{
	ddnet_web_render_thread_at_frame(WebPageVisible() ? 1 : 0, reinterpret_cast<void *>(pfnTask), pUser);
}

extern "C" EMSCRIPTEN_KEEPALIVE void WebRenderThreadRun(void *pfnTask, void *pUser)
{
	reinterpret_cast<void (*)(void *)>(pfnTask)(pUser);
}
