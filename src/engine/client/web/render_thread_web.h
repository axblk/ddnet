#ifndef ENGINE_CLIENT_WEB_RENDER_THREAD_WEB_H
#define ENGINE_CLIENT_WEB_RENDER_THREAD_WEB_H

#include <pthread.h>

#include <functional>

/**
 * The thread the web tools draw on. Unlike a native render thread it does not
 * wait for work in a loop: it returns to the browser's event loop after every
 * piece of work, because that is where the browser answers what the renderer
 * asked it for (an adapter, a device, a mapped buffer) and where it shows a
 * frame. Work is posted to it and runs as a task of its own.
 *
 * A window's thread owns the page's canvas, which is handed to it when it
 * starts; every WebGPU device and WebGL context made for the canvas lives on
 * it. It outlives the backends drawn with, so that a WebGL fallback after a
 * failed WebGPU start finds the canvas where WebGPU left it.
 */
class CWebRenderThread
{
public:
	CWebRenderThread() = default;
	~CWebRenderThread() { Stop(); }
	CWebRenderThread(const CWebRenderThread &) = delete;
	CWebRenderThread &operator=(const CWebRenderThread &) = delete;

	/**
	 * Starts the thread.
	 *
	 * @param TakeCanvas Whether the thread is given the page's canvas.
	 *
	 * @return false when it could not be started, or the canvas not handed
	 * over.
	 */
	bool Start(bool TakeCanvas);
	/**
	 * Ends the thread once what was posted before has run.
	 */
	void Stop();
	bool Running() const { return m_Running; }
	bool OnThread() const;

	/**
	 * Runs `pfnTask` on the thread, later.
	 */
	void Post(void (*pfnTask)(void *), void *pUser) const;
	/**
	 * Runs `Task` on the thread and returns once it has run. Only from
	 * another thread than this one and the page's, which must not wait.
	 */
	void Call(const std::function<void()> &Task) const;

	/**
	 * Runs `pfnTask` on the thread when the page next paints, or a moment
	 * later where it does not (a hidden page, a browser without animation
	 * frames in workers). Only on the thread.
	 */
	static void PostAtFrame(void (*pfnTask)(void *), void *pUser);

private:
	pthread_t m_Thread{};
	bool m_Running = false;
};

#endif
