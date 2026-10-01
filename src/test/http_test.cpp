#include <engine/http.h>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace
{
	class CTestRequest : public IHttpRequest
	{
	public:
		CTestRequest() :
			IHttpRequest("https://example.invalid/")
		{
		}
		void Header(const char *pNameColonValue) override {}
	};
} // namespace

// Aborting a request wakes up whoever waits for it, even if the request itself
// never finishes (a request stuck in curl must not keep exiting waiting).
TEST(Http, AbortWakesWait)
{
	CTestRequest Request;
	std::thread Aborter([&]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		Request.Abort();
	});
	Request.Wait();
	EXPECT_TRUE(Request.IsAbortRequested());
	EXPECT_EQ(Request.State(), EHttpState::QUEUED);
	Aborter.join();
}
