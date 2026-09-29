#ifndef ENGINE_SHARED_SYSTEMD_NOTIFY_H
#define ENGINE_SHARED_SYSTEMD_NOTIFY_H

/**
 * Tells the service manager how the process is doing, like `sd_notify` of
 * libsystemd without linking it.
 *
 * The state goes as one datagram to the Unix socket named by the environment
 * variable `NOTIFY_SOCKET`, a path or, with a leading `@`, an abstract name.
 * Without the variable there is nobody to tell, and nothing is sent.
 *
 * @param pState Newline separated assignments, e.g. `READY=1` or `STOPPING=1`.
 *
 * @return Whether the state was sent.
 */
bool SystemdNotify(const char *pState);

#endif
