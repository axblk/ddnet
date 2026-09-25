#ifndef ENGINE_CLIENT_WEB_WEB_PLATFORM_H
#define ENGINE_CLIENT_WEB_WEB_PLATFORM_H

// The browser builds of the tools stand on a platform layer of their own where
// the native programs stand on SDL: the canvas they draw into
// (`window_web.h`), the canvas's events (`input_web.cpp`) and the sound output
// (`audio_web.h`). The page's side of it is `web_platform.js`, which every
// function below calls into; each of them runs on the page's thread, whichever
// thread calls it. A build with `CONF_WEB_PLATFORM` links none of SDL.

/**
 * Whether the page is shown, which the input learns from the page. A page
 * in a tab in the background is not.
 */
bool WebPageVisible();

extern "C" {
/**
 * Registers the page's canvas as `#canvas` and measures it.
 *
 * @param pWidth Takes the canvas's width in CSS pixels.
 * @param pHeight Takes the canvas's height in CSS pixels.
 * @param pScale Takes the device pixel ratio.
 *
 * @return 0 when the page handed over no canvas.
 */
int ddnet_web_canvas_prepare(int *pWidth, int *pHeight, float *pScale);

/**
 * Starts handing the canvas's events to the input.
 *
 * @return 0 without a canvas.
 */
int ddnet_web_input_install();
void ddnet_web_input_uninstall();

/**
 * Opens the audio output, which plays what is written into a ring.
 *
 * @param Rate The sample rate asked for.
 * @param pRing The ring, see `SWebAudioRing`.
 * @param Capacity The frames the ring holds, a power of two.
 *
 * @return The sample rate the browser plays at, 0 without an output.
 */
int ddnet_web_audio_open(int Rate, void *pRing, int Capacity);
void ddnet_web_audio_pause(int Paused);
void ddnet_web_audio_close();

/**
 * Makes the canvas the render thread was handed findable as `#canvas` on it.
 * Runs on the render thread.
 *
 * @return 0 when the thread was handed no canvas.
 */
int ddnet_web_render_thread_attach();
/**
 * Makes a canvas nobody sees findable as `#canvas` on the render thread, for
 * drawing without a page. Runs on the render thread.
 *
 * @return 0 where the browser has no OffscreenCanvas.
 */
int ddnet_web_render_thread_make_canvas(int Width, int Height);
/**
 * Runs `pfnTask(pUser)` on the calling thread at its next animation frame, or
 * after a short timeout where there is none. Runs on the render thread.
 *
 * @param Visible Whether the page is shown; a hidden page paints nothing, so
 * it gets the timeout.
 * @param pfnTask A `void (*)(void *)` to run.
 * @param pUser What `pfnTask` is given.
 */
void ddnet_web_render_thread_at_frame(int Visible, void *pfnTask, void *pUser);
}

#endif
