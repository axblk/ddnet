#include "systemd_notify.h"

#include <base/detect.h>

#if defined(CONF_FAMILY_UNIX)
#include <base/mem.h>
#include <base/str.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstddef>
#include <cstdlib>

bool SystemdNotify(const char *pState)
{
	const char *pSocket = getenv("NOTIFY_SOCKET");
	if(pSocket == nullptr || (pSocket[0] != '/' && pSocket[0] != '@'))
		return false;

	sockaddr_un Addr;
	mem_zero(&Addr, sizeof(Addr));
	Addr.sun_family = AF_UNIX;
	const int SocketLength = str_length(pSocket);
	if(SocketLength < 2 || SocketLength >= (int)sizeof(Addr.sun_path))
		return false;
	mem_copy(Addr.sun_path, pSocket, SocketLength);
	// An abstract name starts with a zero byte and is as long as the address
	// says, not up to the next zero byte.
	const bool Abstract = Addr.sun_path[0] == '@';
	if(Abstract)
		Addr.sun_path[0] = '\0';
	const socklen_t AddrLength = offsetof(sockaddr_un, sun_path) + SocketLength + (Abstract ? 0 : 1);

	const int Socket = socket(AF_UNIX, SOCK_DGRAM, 0);
	if(Socket < 0)
		return false;
	// The server may start other programs, which have no business with the socket.
	fcntl(Socket, F_SETFD, FD_CLOEXEC);
	const int StateLength = str_length(pState);
	const bool Sent = sendto(Socket, pState, StateLength, 0, reinterpret_cast<const sockaddr *>(&Addr), AddrLength) == StateLength;
	close(Socket);
	return Sent;
}
#else
bool SystemdNotify(const char *pState)
{
	return false;
}
#endif
