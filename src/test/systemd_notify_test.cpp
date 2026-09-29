#include "test.h"

#include <base/detect.h>

#include <engine/shared/systemd_notify.h>

#include <gtest/gtest.h>

#if defined(CONF_FAMILY_UNIX)
#include <base/fs.h>
#include <base/mem.h>
#include <base/str.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <string>

namespace
{
	// Stands in for the service manager: a datagram socket bound to a name.
	class CNotifyListener
	{
		int m_Socket = -1;

	public:
		bool Bind(const char *pPath, bool Abstract)
		{
			m_Socket = socket(AF_UNIX, SOCK_DGRAM, 0);
			if(m_Socket < 0)
				return false;
			sockaddr_un Addr;
			mem_zero(&Addr, sizeof(Addr));
			Addr.sun_family = AF_UNIX;
			str_copy(Addr.sun_path + (Abstract ? 1 : 0), pPath, sizeof(Addr.sun_path) - 1);
			const socklen_t Length = offsetof(sockaddr_un, sun_path) + (Abstract ? 1 + str_length(pPath) : sizeof(Addr.sun_path));
			return bind(m_Socket, reinterpret_cast<const sockaddr *>(&Addr), Length) == 0;
		}

		std::string Receive() const
		{
			char aBuf[256];
			const ssize_t Size = recv(m_Socket, aBuf, sizeof(aBuf), MSG_DONTWAIT);
			return Size < 0 ? std::string() : std::string(aBuf, Size);
		}

		~CNotifyListener()
		{
			if(m_Socket >= 0)
				close(m_Socket);
		}
	};

	class CNotifySocketEnv
	{
	public:
		explicit CNotifySocketEnv(const char *pValue) { setenv("NOTIFY_SOCKET", pValue, 1); }
		~CNotifySocketEnv() { unsetenv("NOTIFY_SOCKET"); }
	};
}

TEST(SystemdNotify, WithoutSocket)
{
	unsetenv("NOTIFY_SOCKET");
	EXPECT_FALSE(SystemdNotify("READY=1"));
	CNotifySocketEnv Env("relative/notify");
	EXPECT_FALSE(SystemdNotify("READY=1"));
}

TEST(SystemdNotify, Path)
{
	CTestInfo Info;
	char aCwd[IO_MAX_PATH_LENGTH];
	ASSERT_NE(fs_getcwd(aCwd, sizeof(aCwd)), nullptr);
	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "%s/%s.sock", aCwd, Info.m_aFilenamePrefix);
	if(str_length(aPath) >= (int)sizeof(sockaddr_un::sun_path))
		str_format(aPath, sizeof(aPath), "/tmp/%s.sock", Info.m_aFilenamePrefix);
	{
		CNotifyListener Listener;
		ASSERT_TRUE(Listener.Bind(aPath, false));
		CNotifySocketEnv Env(aPath);
		EXPECT_TRUE(SystemdNotify("READY=1"));
		EXPECT_EQ(Listener.Receive(), "READY=1");
		EXPECT_TRUE(SystemdNotify("STOPPING=1"));
		EXPECT_EQ(Listener.Receive(), "STOPPING=1");
	}
	EXPECT_EQ(fs_remove(aPath), 0);
}

#if defined(CONF_PLATFORM_LINUX)
TEST(SystemdNotify, AbstractName)
{
	CTestInfo Info;
	CNotifyListener Listener;
	ASSERT_TRUE(Listener.Bind(Info.m_aFilenamePrefix, true));
	char aName[128];
	str_format(aName, sizeof(aName), "@%s", Info.m_aFilenamePrefix);
	CNotifySocketEnv Env(aName);
	EXPECT_TRUE(SystemdNotify("READY=1"));
	EXPECT_EQ(Listener.Receive(), "READY=1");
}
#endif
#endif
