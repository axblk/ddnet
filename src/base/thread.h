/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#ifndef BASE_THREAD_H
#define BASE_THREAD_H

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

#endif
