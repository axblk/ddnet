#ifndef ENGINE_SHARED_WEBSOCKETS_H
#define ENGINE_SHARED_WEBSOCKETS_H

// The transport itself is reached through the table `base` is handed, see
// `NETWEBSOCKET` in `base/net.h`. Only the certificate reload is a command of
// its own rather than something a socket does.
// NOLINTBEGIN(readability-identifier-naming)
void websocket_reload_certs();
// NOLINTEND(readability-identifier-naming)

#endif // ENGINE_SHARED_WEBSOCKETS_H
