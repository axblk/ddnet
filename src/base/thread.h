/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#ifndef BASE_THREAD_H
#define BASE_THREAD_H

#include "detect.h"

#include <chrono>

/**
 * Threading related functions.
 *
 * @defgroup Threads Threading
 *
 * @see Locks
 * @see Semaphore
 */

/**
 * Creates a new thread.
 *
 * @ingroup Threads
 *
 * @param threadfunc Entry point for the new thread.
 * @param user Pointer to pass to the thread.
 * @param name Name describing the use of the thread.
 *
 * @return Handle for the new thread.
 */
void *thread_init(void (*threadfunc)(void *), void *user, const char *name);

/**
 * Waits for a thread to be done or destroyed.
 *
 * @ingroup Threads
 *
 * @param thread Thread to wait for.
 */
void thread_wait(void *thread);

/**
 * Yield the current thread's execution slice.
 *
 * @ingroup Threads
 */
void thread_yield();

/**
 * Waits a short moment for work that is going on in other threads.
 *
 * @ingroup Threads
 *
 * @remark For a thread that has nothing to do but wait for a result. In the
 * browser that is not merely polite: a worker there cannot open a file or
 * write a line of its own, it hands the call to the main thread, and the main
 * thread only makes such a call while it is not running. So a main thread
 * spinning on a result is standing in the way of the very work it waits for -
 * and a sleep alone is not enough either, because a browser may hold what it
 * fetched until the page has had its turn. This hands it over.
 */
void thread_wait_for_other_threads();

/**
 * Sleeps until the given duration has passed.
 *
 * @ingroup Threads
 *
 * @param duration How long to sleep. A duration that has already passed still
 * gives up the slice once.
 *
 * @remark In the browser this is also what hands the page its turn back: for
 * as long as a thread there sleeps by any other means, nothing is painted and
 * no input is delivered.
 */
void thread_sleep_idle(std::chrono::nanoseconds duration);

/**
 * Waits out the rest of a frame.
 *
 * @ingroup Threads
 *
 * @param next_frame_time When the next frame is due, carried from one call to
 * the next. A duration of none starts the pacing at the moment of the call.
 * @param refresh_rate How many frames a second are wanted, or zero for as many
 * as there are.
 *
 * @remark A frame that ran long does not make the next one wait for it, it
 * only gives up its own slice.
 */
void thread_sleep_until_next_frame(std::chrono::nanoseconds &next_frame_time, int refresh_rate);

/**
 * Requests the most precise timer wakeups that the system offers for the calling thread.
 *
 * @ingroup Threads
 *
 * @remark Wakeups from sleeping and waiting are delayed to save power by default,
 * on macOS by a quarter of the requested duration and on Linux by 50 microseconds.
 * On Windows SDL already raises the timer resolution while initializing.
 */
void thread_request_precise_wakeups();

/**
 * Puts the thread in the detached state, guaranteeing that
 * resources of the thread will be freed immediately when the
 * thread terminates.
 *
 * @ingroup Threads
 *
 * @param thread Thread to detach.
 */
void thread_detach(void *thread);

/**
 * Creates a new thread and detaches it.
 *
 * @ingroup Threads
 *
 * @param threadfunc Entry point for the new thread.
 * @param user Pointer to pass to the thread.
 * @param name Name describing the use of the thread.
 */
void thread_init_and_detach(void (*threadfunc)(void *), void *user, const char *name);

#if defined(CONF_PLATFORM_EMSCRIPTEN)
/**
 * Hands the browser back its turn for a moment and takes it up again
 * afterwards.
 *
 * Everything that waits on the browser's main thread goes through here.
 * Waiting without handing the turn back stops the page dead, and handing it
 * back means the program's stack is unwound and put together again when the
 * wait is over - which is what `web_unwound` is there to say.
 *
 * @ingroup Threads
 *
 * @param milliseconds How long to wait at least. Zero hands the turn back and
 * takes it again as soon as the browser has done what it had to do.
 */
void web_yield(int64_t milliseconds);

/**
 * Says that this thread is handing the browser its turn back for as long as
 * this lives, for a wait that does not go through `web_yield` - something
 * awaited in JavaScript, say. `web_unwound` counts it the same.
 *
 * @ingroup Threads
 */
class CWebYieldScope
{
public:
	CWebYieldScope();
	~CWebYieldScope();
	CWebYieldScope(const CWebYieldScope &Other) = delete;
	CWebYieldScope &operator=(const CWebYieldScope &Other) = delete;
};
#endif

/**
 * Whether this thread is standing in `web_yield` right now: its stack is
 * unwound, the browser is running the page, and the program goes back on the
 * stack when the wait is over.
 *
 * A call that arrives from the page while that is so must not do anything
 * that waits again - a second unwinding from underneath the first takes the
 * program down with `memory access out of bounds`. What such a call can do
 * instead is put the work aside for whoever is waiting to do afterwards.
 *
 * @ingroup Threads
 *
 * @return `true` for as long as the wait lasts, and always `false` outside a
 * browser, where nothing is ever unwound.
 */
bool web_unwound();

#endif
