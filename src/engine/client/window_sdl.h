#ifndef ENGINE_CLIENT_WINDOW_SDL_H
#define ENGINE_CLIENT_WINDOW_SDL_H

#include <engine/graphics_window.h>

// The SDL window and, for OpenGL, its context. Everything the graphics
// need from the window system comes through here, and nothing in the
// graphics includes SDL.
extern IEngineGraphicsWindow *CreateSdlGraphicsWindow();

#endif
