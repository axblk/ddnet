#include "program_thread_web.h"

#include <base/dbg.h>
#include <base/log.h>

#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/stack.h>
#include <emscripten/threading.h>
#include <pthread.h>

#include <memory>
#include <string>
#include <vector>

namespace
{
	struct SProgram
	{
		int (*m_pfnMain)(int ArgumentCount, const char **ppArguments);
		// The page's copies of the arguments are gone once main() returned.
		std::vector<std::string> m_vArguments;
	};

	void *ProgramThread(void *pUser)
	{
		const std::unique_ptr<SProgram> pProgram(static_cast<SProgram *>(pUser));
		std::vector<const char *> vpArguments;
		vpArguments.reserve(pProgram->m_vArguments.size() + 1);
		for(const std::string &Argument : pProgram->m_vArguments)
			vpArguments.push_back(Argument.c_str());
		vpArguments.push_back(nullptr);
		const int Code = pProgram->m_pfnMain(static_cast<int>(pProgram->m_vArguments.size()), vpArguments.data());
		// Lets go of the runtime main() kept and ends it, on the page's
		// thread, which tells the page the exit code.
		emscripten_force_exit(Code);
		return nullptr;
	}
} // namespace

int WebRunProgram(int (*pfnMain)(int ArgumentCount, const char **ppArguments), int ArgumentCount, const char **ppArguments)
{
	dbg_assert(emscripten_is_main_runtime_thread(), "The program is started from the page's thread");
	SProgram *pProgram = new SProgram;
	pProgram->m_pfnMain = pfnMain;
	pProgram->m_vArguments.assign(ppArguments, ppArguments + ArgumentCount);

	pthread_attr_t Attributes;
	pthread_attr_init(&Attributes);
	pthread_attr_setdetachstate(&Attributes, PTHREAD_CREATE_DETACHED);
	// The stack main() would have had on the page's thread, as elsewhere.
	pthread_attr_setstacksize(&Attributes, emscripten_stack_get_base() - emscripten_stack_get_end());
	pthread_t Thread;
	const int Error = pthread_create(&Thread, &Attributes, ProgramThread, pProgram);
	pthread_attr_destroy(&Attributes);
	if(Error != 0)
	{
		delete pProgram;
		log_error("client", "Could not start the program's thread (%d)", Error);
		return 1;
	}
	// The runtime stays when main() returns, until the program ends it.
	emscripten_runtime_keepalive_push();
	return 0;
}
