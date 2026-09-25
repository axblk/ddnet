#ifndef ENGINE_CLIENT_WEB_WINDOW_WEB_H
#define ENGINE_CLIENT_WEB_WINDOW_WEB_H

#include <engine/graphics_window.h>

/**
 * The window of a program on a page: the canvas the page handed over. It
 * draws with WebGPU and falls back to WebGL 2 where the browser offers no
 * adapter. The canvas is as big as the page makes it; `Resize` takes the size
 * the page measured, in CSS pixels, and draws at the device's pixel ratio.
 */
IEngineGraphicsWindow *CreateWebGraphicsWindow();

#endif
