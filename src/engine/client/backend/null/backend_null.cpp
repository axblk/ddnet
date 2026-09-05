#include "backend_null.h"

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#endif

ERunCommandReturnTypes CCommandProcessorFragment_Null::RunCommand(const CCommandBuffer::SCommand *pBaseCommand)
{
	switch(pBaseCommand->m_Cmd)
	{
	case CCommandProcessorFragment_Null::CMD_INIT:
	{
		// Nothing is drawn here, so nothing is out of reach either. Reporting
		// zeroes would send the frontend down its narrowest path - six vertices
		// per quad, no buffering - to build geometry for a backend that throws
		// it away, and would run the tests through a path no GPU takes.
		SBackendCapabilities &Capabilities = *static_cast<const SCommand_Init *>(pBaseCommand)->m_pCapabilities;
		Capabilities = {};
		Capabilities.m_2DArrayTextures = true;
		Capabilities.m_RenderTargets = true;
		Capabilities.m_PlanarYuvConversion = true;
		break;
	}
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
