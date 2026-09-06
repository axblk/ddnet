#ifndef ENGINE_CLIENT_BACKEND_NULL_BACKEND_NULL_H
#define ENGINE_CLIENT_BACKEND_NULL_BACKEND_NULL_H

#include <engine/client/backend/backend_base.h>

class CCommandProcessorFragment_Null : public CCommandProcessorFragment_Renderer
{
	ERunCommandReturnTypes RunCommand(const CCommandBuffer::SCommand *pBaseCommand) override;
	void Cmd_Swap(const CCommandBuffer::SCommand_Swap *pCommand);
};

#endif
