#ifndef ENGINE_CLIENT_WEB_PROGRAM_THREAD_WEB_H
#define ENGINE_CLIENT_WEB_PROGRAM_THREAD_WEB_H

/**
 * Starts a web tool's program on a thread of its own and returns, so that the
 * page's thread is free for the page from then on. The program's waits are
 * real ones there, and its exit code ends the runtime when it returns.
 *
 * This is what Emscripten's `PROXY_TO_PTHREAD` does, but without handing the
 * program the page's canvas: that setting starts nothing where there is no
 * canvas, which a program rendering in a worker of its own does not have, and
 * the canvas goes from the page's thread straight to the render thread
 * instead (see `CWebRenderThread`).
 *
 * Only from main() on the page's thread.
 *
 * @param pfnMain The program's main().
 * @param ArgumentCount What main() was given.
 * @param ppArguments What main() was given, copied for the program.
 *
 * @return What main() returns to the page: 0 once the program runs.
 */
int WebRunProgram(int (*pfnMain)(int ArgumentCount, const char **ppArguments), int ArgumentCount, const char **ppArguments);

#endif
