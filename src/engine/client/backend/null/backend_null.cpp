#include "backend_null.h"

#include <engine/client/backend_sdl.h>

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#endif

ERunCommandReturnTypes CCommandProcessorFragment_Null::RunCommand(const CCommandBuffer::SCommand *pBaseCommand)
{
	switch(pBaseCommand->m_Cmd)
	{
	case CCommandProcessorFragment_Null::CMD_INIT:
		*static_cast<const SCommand_Init *>(pBaseCommand)->m_pCapabilities = {};
		break;
	case CCommandBuffer::CMD_SWAP:
		Cmd_Swap(static_cast<const CCommandBuffer::SCommand_Swap *>(pBaseCommand));
		break;
	}
	return ERunCommandReturnTypes::RUN_COMMAND_COMMAND_HANDLED;
}

void CCommandProcessorFragment_Null::Cmd_Swap(const CCommandBuffer::SCommand_Swap *pCommand)
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	// Return control to the browser's main thread. This is normally done in SDL_GL_SwapWindow,
	// but with headless graphics we do not have a GL context to call this function.
	emscripten_sleep(0);
#endif
}
