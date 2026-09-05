#include <base/detect.h>

// The half of the OpenGL backend that does not care whether there are
// programs: textures, handles, context probing and command dispatch. It is
// compiled a second time here because the GL entry points a GLES translation
// unit sees are not the ones the desktop headers declare.
#if defined(CONF_BACKEND_OPENGL_ES) || defined(CONF_BACKEND_OPENGL_ES3)

#define GLES_CLASS_DEFINES_DO_DEFINE
#include "gles_class_defines.h"
#undef GLES_CLASS_DEFINES_DO_DEFINE

#define BACKEND_AS_OPENGL_ES 1

#include <engine/client/backend/opengl/backend_opengl_base.cpp>

#undef BACKEND_AS_OPENGL_ES

#include "gles_class_defines.h"

#endif
