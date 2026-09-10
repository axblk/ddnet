#ifndef NET_NET_H
#define NET_NET_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define DDNET_NET_EV_NONE 0

#define DDNET_NET_EV_CONNECT 1

#define DDNET_NET_EV_CHUNK 2

#define DDNET_NET_EV_DISCONNECT 3

#define DDNET_NET_EV_CONNLESS_CHUNK 4

#define DDNET_NET_EV_MAP 5

#define DDNET_NET_EV_MOVED 6

#define DDNET_NET_MAP_HEADER 0

#define DDNET_NET_MAP_DATA 1

#define DDNET_NET_MAP_END 2

#define DDNET_NET_MAP_FAILED 3

#define DDNET_NET_PROTOCOL_TW06 0

#define DDNET_NET_PROTOCOL_TW07 1

#define DDNET_NET_PROTOCOL_QUIC 2

#define DDNET_NET_PROTOCOL_WEBTRANSPORT 3

#define DDNET_NET_PROTOCOL_WEBSOCKET 4

typedef struct DdnetNet DdnetNet;

typedef struct DdnetNetEvent DdnetNetEvent;

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

void ddnet_net_ev_new(struct DdnetNetEvent **ev);

void ddnet_net_ev_free(struct DdnetNetEvent *ev);

uint64_t ddnet_net_ev_kind(const struct DdnetNetEvent *ev);

uint64_t ddnet_net_ev_map_peer_index(const struct DdnetNetEvent *ev);

/**
 * One of `DDNET_NET_MAP_*`.
 */
uint64_t ddnet_net_ev_map_kind(const struct DdnetNetEvent *ev);

/**
 * Bytes in the buffer: the header frame, a piece of the map, or the reason
 * of the failure.
 */
size_t ddnet_net_ev_map_len(const struct DdnetNetEvent *ev);

/**
 * Takes a map header as a `DDNET_NET_MAP_HEADER` event delivers it apart.
 * The name is not NUL-terminated and points into `payload`.
 */
bool ddnet_net_decode_map_header(const uint8_t *payload,
                                 size_t payload_len,
                                 uint64_t *size,
                                 uint32_t *crc,
                                 uint8_t (*sha256)[32],
                                 const uint8_t **name,
                                 size_t *name_len);

uint64_t ddnet_net_ev_connect_peer_index(const struct DdnetNetEvent *ev);

void ddnet_net_ev_connect_addr(struct DdnetNetEvent *ev, const char **addr_ptr, size_t *addr_len);

uint64_t ddnet_net_ev_moved_peer_index(const struct DdnetNetEvent *ev);

/**
 * The peer's new address, as a URL like the one of its connect event.
 */
void ddnet_net_ev_moved_addr(struct DdnetNetEvent *ev, const char **addr_ptr, size_t *addr_len);

uint64_t ddnet_net_ev_chunk_peer_index(const struct DdnetNetEvent *ev);

size_t ddnet_net_ev_chunk_len(const struct DdnetNetEvent *ev);

bool ddnet_net_ev_chunk_is_unreliable(const struct DdnetNetEvent *ev);

uint64_t ddnet_net_ev_disconnect_peer_index(const struct DdnetNetEvent *ev);

size_t ddnet_net_ev_disconnect_reason_len(const struct DdnetNetEvent *ev);

bool ddnet_net_ev_disconnect_is_remote(const struct DdnetNetEvent *ev);

/**
 * The four bytes of the 0.6 extended header, if the packet had one.
 */
bool ddnet_net_ev_connless_chunk_extra(struct DdnetNetEvent *ev, uint8_t (*extra)[4]);

/**
 * The 0.7 sender's token for answering it, if the packet came over 0.7.
 */
bool ddnet_net_ev_connless_chunk_token7(struct DdnetNetEvent *ev, uint32_t *token);

size_t ddnet_net_ev_connless_chunk_len(struct DdnetNetEvent *ev);

void ddnet_net_ev_connless_chunk_addr(struct DdnetNetEvent *ev,
                                      const char **addr_ptr,
                                      size_t *addr_len);

bool ddnet_net_new(struct DdnetNet **net);

void ddnet_net_free(struct DdnetNet *net);

bool ddnet_net_set_bindaddr(struct DdnetNet *net, const char *addr, size_t addr_len);

bool ddnet_net_set_identity(struct DdnetNet *net, const uint8_t (*private_identity)[32]);

bool ddnet_net_set_accept_connections(struct DdnetNet *net, bool accept);

/**
 * PEM files with the certificate chain and the private key a server
 * shows browsers, from a CA; without them a server accepting WebTransport
 * makes a short-lived certificate itself. Before `ddnet_net_open`.
 */
bool ddnet_net_set_tls_files(struct DdnetNet *net,
                             const char *cert_path,
                             size_t cert_path_len,
                             const char *key_path,
                             size_t key_path_len);

/**
 * The SHA-256 a browser accepts the server's certificate by, the one in
 * use or, with `next`, the one that takes over at the next rotation.
 * Returns `false` and leaves `sha256` alone when there is none.
 */
bool ddnet_net_certificate_sha256(struct DdnetNet *net, bool next, uint8_t (*sha256)[32]);

/**
 * Writes the server's own public identity, the 32 bytes clients pin it by.
 * Returns `false` and leaves `identity` alone before `ddnet_net_open`, and
 * in a browser, which has none.
 */
bool ddnet_net_identity(struct DdnetNet *net, uint8_t (*identity)[32]);

/**
 * How long a connection may go without a packet before it counts as
 * lost. Before `ddnet_net_open`.
 */
bool ddnet_net_set_timeout(struct DdnetNet *net, uint64_t seconds);

/**
 * Writes the TLS session keys to the file `SSLKEYLOGFILE` names, so the
 * traffic can be read in Wireshark. A debugging aid; off by default.
 */
bool ddnet_net_set_key_log(struct DdnetNet *net, bool key_log);

/**
 * Switches a single protocol on or off, after `ddnet_net_set_accept_connections`.
 */
bool ddnet_net_set_accept_protocol(struct DdnetNet *net, uint64_t protocol, bool accept);

/**
 * Whether the library takes connections over `protocol`: what was asked
 * for, less what is not compiled in. After `ddnet_net_open`.
 */
bool ddnet_net_accepts_protocol(struct DdnetNet *net, uint64_t protocol, bool *accepts);

bool ddnet_net_open(struct DdnetNet *net);

bool ddnet_net_is_broken(const struct DdnetNet *net);

const char *ddnet_net_error(const struct DdnetNet *net);

size_t ddnet_net_error_len(const struct DdnetNet *net);

bool ddnet_net_set_userdata(struct DdnetNet *net, uint64_t peer_index, void *userdata);

bool ddnet_net_userdata(struct DdnetNet *net, uint64_t peer_index, void **userdata);

bool ddnet_net_wait(struct DdnetNet *net);

bool ddnet_net_wait_timeout(struct DdnetNet *net, uint64_t ns);

bool ddnet_net_recv(struct DdnetNet *net,
                    uint8_t *buf,
                    size_t buf_cap,
                    struct DdnetNetEvent *event);

bool ddnet_net_send_chunk(struct DdnetNet *net,
                          uint64_t peer_index,
                          const uint8_t *chunk,
                          size_t chunk_len,
                          bool unreliable);

bool ddnet_net_flush(struct DdnetNet *net, uint64_t peer_index);

/**
 * Keeps a copy of the map under `map_id` for `ddnet_net_send_map`.
 */
bool ddnet_net_set_map(struct DdnetNet *net,
                       uint32_t map_id,
                       const uint8_t *name,
                       size_t name_len,
                       uint32_t crc,
                       const uint8_t (*sha256)[32],
                       const uint8_t *data,
                       size_t data_len);

/**
 * Sends the map to a QUIC peer on a stream of its own.
 */
bool ddnet_net_send_map(struct DdnetNet *net, uint64_t peer_index, uint32_t map_id);

/**
 * Stops a map still going out to the peer.
 */
bool ddnet_net_cancel_map(struct DdnetNet *net, uint64_t peer_index);

bool ddnet_net_connect(struct DdnetNet *net,
                       const char *addr,
                       size_t addr_len,
                       uint64_t *peer_index);

bool ddnet_net_close(struct DdnetNet *net,
                     uint64_t peer_index,
                     const char *reason,
                     size_t reason_len);

bool ddnet_net_send_connless_chunk(struct DdnetNet *net,
                                   const char *addr,
                                   size_t addr_len,
                                   const uint8_t *chunk,
                                   size_t chunk_len);

/**
 * Sends a 0.6 connectionless packet with the extended header.
 */
bool ddnet_net_send_connless_chunk_extended(struct DdnetNet *net,
                                            const char *addr,
                                            size_t addr_len,
                                            const uint8_t (*extra)[4],
                                            const uint8_t *chunk,
                                            size_t chunk_len);

/**
 * The 0.7 token accepted from any address, which a server registers with
 * so the masterserver can challenge it.
 */
bool ddnet_net_global_token7(struct DdnetNet *net, uint32_t *token);

bool ddnet_net_num_peers_in_bucket(struct DdnetNet *net,
                                   const char *addr,
                                   size_t addr_len,
                                   uint32_t *result);

bool ddnet_net_set_logger(void (*log)(int32_t level,
                                      const char *system,
                                      size_t system_len,
                                      const char *message,
                                      size_t message_len));

/**
 * How much the crate logs, in the levels the logger is handed: 0 errors
 * only, up to 4 everything, below 0 nothing. A line above the level costs
 * nothing; it is not even formatted.
 */
void ddnet_net_set_log_level(int32_t level);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  /* NET_NET_H */
