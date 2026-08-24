#ifndef L2CAP_H
#define L2CAP_H

// Linux-only: this library wraps the kernel's AF_BLUETOOTH sockets and its
// constants are raw Linux ABI values. Other platforms expose L2CAP through
// entirely different APIs (CoreBluetooth, BluetoothSocket) and are out of
// scope.

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#define L2CAP_BDADDR_LE_PUBLIC 0x01
#define L2CAP_BDADDR_LE_RANDOM 0x02

/**
 * Readiness bits exchanged with the caller's event loop: `l2cap_channel_events()`
 * says which ones the channel currently waits for, `l2cap_channel_process()`
 * takes the ones that fired.
 */
#define L2CAP_READABLE 0x1
#define L2CAP_WRITABLE 0x2

/** `l2cap_channel_write()` handed the SDU to the kernel. */
#define L2CAP_WRITE_SENT 0

/** `l2cap_channel_write()` queued the SDU; the drain callback fires once the
 * queue empties. */
#define L2CAP_WRITE_QUEUED 1

/**
 * Levels for `l2cap_channel_set_security()` and `l2cap_server_set_security()`,
 * matching the kernel's BT_SECURITY_* values.
 */
#define L2CAP_SECURITY_LOW    0x01
#define L2CAP_SECURITY_MEDIUM 0x02
#define L2CAP_SECURITY_HIGH   0x03
#define L2CAP_SECURITY_FIPS   0x04

typedef struct l2cap_addr_s l2cap_addr_t;
typedef struct l2cap_channel_s l2cap_channel_t;
typedef struct l2cap_chunk_s l2cap_chunk_t;
typedef struct l2cap_server_s l2cap_server_t;

/**
 * The result of a `l2cap_channel_connect()`. `status` is 0 on success or a
 * negated errno on failure; a failed channel must still be closed. The
 * callback fires exactly once.
 */
typedef void (*l2cap_connect_cb)(l2cap_channel_t *channel, int status);

/**
 * An inbound SDU. `len == 0` means the channel ended: it is delivered once
 * and reading stops until `l2cap_channel_read_start()` is called again. The
 * buffer is owned by the channel and only valid for the duration of the call.
 *
 * Caveat: SOCK_SEQPACKET cannot distinguish a zero-length SDU from EOF; both
 * are reported as `len == 0`.
 */
typedef void (*l2cap_read_cb)(l2cap_channel_t *channel, size_t len, const uint8_t *data);

/**
 * The write queue drained after `l2cap_channel_write()` returned
 * `L2CAP_WRITE_QUEUED`. Never fires after `l2cap_channel_close()`.
 */
typedef void (*l2cap_drain_cb)(l2cap_channel_t *channel);

struct l2cap_addr_s {
  uint8_t bdaddr[6]; // Little-endian, as the kernel stores it
  uint8_t type;      // L2CAP_BDADDR_LE_*
};

/**
 * A connection-oriented channel. Allocate it yourself and initialise it with
 * `l2cap_channel_init()` before any other call; every field except `data` is
 * private.
 */
struct l2cap_channel_s {
  void *data;

  // Private
  int _fd;
  int _state;
  int _reading;
  uint8_t _security;
  uint16_t _psm;
  uint16_t _rcv_mtu;
  uint16_t _snd_mtu;
  l2cap_addr_t _peer;
  uint8_t *_read_buf;
  l2cap_connect_cb _on_connect;
  l2cap_read_cb _on_read;
  l2cap_drain_cb _on_drain;
  l2cap_chunk_t *_write_head;
  l2cap_chunk_t *_write_tail;
};

/**
 * Parse "AA:BB:CC:DD:EE:FF" into an address of the given type.
 */
int
l2cap_addr_init (const char *str, uint8_t type, l2cap_addr_t *addr);

/**
 * Format an address back into `str`, which must hold at least 18 bytes.
 */
void
l2cap_addr_to_string (const l2cap_addr_t *addr, char *str);

/**
 * Initialise a channel. `data` is the caller's opaque pointer, available on
 * every callback via `channel->data`.
 */
void
l2cap_channel_init (l2cap_channel_t *channel, void *data);

/**
 * Require a security level for the link, applied when the socket is created —
 * call it between `l2cap_channel_init()` and `l2cap_channel_connect()`.
 * Without it the kernel default (low) applies. Channels accepted from a
 * server inherit the server's level.
 *
 * Low stays the default on purpose: medium and above require pairing, so
 * forcing them would refuse every unauthenticated peer. The caller knows
 * whether its use case can pair; the library does not.
 */
int
l2cap_channel_set_security (l2cap_channel_t *channel, uint8_t level);

/**
 * Start a non-blocking connect to `peer` on `psm`, bound to the `local`
 * adapter address. The channel must be initialised and idle — anything else
 * is -EINVAL. Poll `l2cap_channel_fd()` for `l2cap_channel_events()`
 * and feed the results to `l2cap_channel_process()`; `cb` fires when the
 * connect settles.
 */
int
l2cap_channel_connect (l2cap_channel_t *channel, const l2cap_addr_t *local, const l2cap_addr_t *peer, uint16_t psm, l2cap_connect_cb cb);

/**
 * Adopt an already-connected SOCK_SEQPACKET descriptor into an initialised,
 * idle channel — anything else is -EINVAL. The channel takes ownership of
 * `fd` in every case: on failure the descriptor is closed, on success it is
 * switched to non-blocking, close-on-exec mode.
 */
int
l2cap_channel_accept (l2cap_channel_t *channel, int fd);

int
l2cap_channel_fd (const l2cap_channel_t *channel);

/**
 * The caller's side of the event-loop contract: register the returned bits
 * as the poll interest for `l2cap_channel_fd()`, and re-query after every
 * call that changes the channel (connect, accept, process, write,
 * read_start/stop, close). Returns 0 when the descriptor needs no watching
 * at all.
 */
int
l2cap_channel_events (const l2cap_channel_t *channel);

/**
 * Advance the channel after the caller's loop reported `events`. Invokes the
 * registered callbacks synchronously. Returns 0, or a negated errno on a
 * fatal socket error, after which the channel must be closed.
 */
int
l2cap_channel_process (l2cap_channel_t *channel, int events);

int
l2cap_channel_read_start (l2cap_channel_t *channel, l2cap_read_cb cb);

int
l2cap_channel_read_stop (l2cap_channel_t *channel);

/**
 * Send one SDU of at most `l2cap_channel_snd_mtu()` bytes. Returns
 * `L2CAP_WRITE_SENT`, `L2CAP_WRITE_QUEUED`, or a negated errno. `cb` may be
 * NULL; when several writes queue, the last non-NULL `cb` is the one that
 * fires on drain.
 */
int
l2cap_channel_write (l2cap_channel_t *channel, const uint8_t *data, size_t len, l2cap_drain_cb cb);

uint16_t
l2cap_channel_psm (const l2cap_channel_t *channel);

uint16_t
l2cap_channel_rcv_mtu (const l2cap_channel_t *channel);

uint16_t
l2cap_channel_snd_mtu (const l2cap_channel_t *channel);

const l2cap_addr_t *
l2cap_channel_peer (const l2cap_channel_t *channel);

/**
 * Close the descriptor and free the channel's internal buffers. Synchronous
 * and idempotent. Pending queued writes are discarded and the drain callback
 * never fires after close — the caller settles its own bookkeeping. The
 * struct is the caller's to reclaim; reusing it requires a fresh
 * `l2cap_channel_init()`.
 */
void
l2cap_channel_close (l2cap_channel_t *channel);

/**
 * At least one incoming connection is ready. The callback must make
 * progress: accept at least one connection, stop accepting, or close the
 * server — the descriptor is level-triggered, so a callback that does none
 * of those fires again on every `l2cap_server_process()`. Accepting only
 * one is fine: the descriptor stays readable and this fires again.
 */
typedef void (*l2cap_connection_cb)(l2cap_server_t *server);

/**
 * A listening endpoint. Same contract as the channel: allocate and
 * `l2cap_server_init()` it yourself, poll `l2cap_server_fd()` for
 * `l2cap_server_events()`, feed the results to `l2cap_server_process()`.
 */
struct l2cap_server_s {
  void *data;

  // Private
  int _fd;
  int _state;
  int _accepting;
  uint8_t _security;
  uint16_t _psm;
  l2cap_addr_t _local;
  l2cap_connection_cb _on_connection;
};

void
l2cap_server_init (l2cap_server_t *server, void *data);

/**
 * Require a security level for accepted links, applied when the socket is
 * created — call it between `l2cap_server_init()` and `l2cap_server_listen()`.
 */
int
l2cap_server_set_security (l2cap_server_t *server, uint8_t level);

/**
 * Bind to the `local` adapter address and listen on `psm`. The server must
 * be initialised and idle — anything else is -EINVAL. Pass `psm` 0 to
 * let the kernel assign one from the LE dynamic range; read it back with
 * `l2cap_server_psm()`. Nothing is accepted until
 * `l2cap_server_accept_start()`.
 */
int
l2cap_server_listen (l2cap_server_t *server, const l2cap_addr_t *local, uint16_t psm, int backlog);

/**
 * Adopt an already-listening SOCK_SEQPACKET descriptor (socket activation,
 * tests). The server takes ownership of `fd` in every case: on failure the
 * descriptor is closed, on success it is switched to non-blocking,
 * close-on-exec mode.
 */
int
l2cap_server_attach (l2cap_server_t *server, int fd);

int
l2cap_server_fd (const l2cap_server_t *server);

int
l2cap_server_events (const l2cap_server_t *server);

int
l2cap_server_process (l2cap_server_t *server, int events);

int
l2cap_server_accept_start (l2cap_server_t *server, l2cap_connection_cb cb);

int
l2cap_server_accept_stop (l2cap_server_t *server);

/**
 * Accept one pending connection into `channel`, which must be initialised
 * and idle. Returns 0, or a negated errno — -EAGAIN when nothing is pending.
 */
int
l2cap_server_accept (l2cap_server_t *server, l2cap_channel_t *channel);

uint16_t
l2cap_server_psm (const l2cap_server_t *server);

const l2cap_addr_t *
l2cap_server_local (const l2cap_server_t *server);

/**
 * Close the listening descriptor. Synchronous and idempotent; accepted
 * channels live on independently. Reusing the struct requires a fresh
 * `l2cap_server_init()`.
 */
void
l2cap_server_close (l2cap_server_t *server);

#ifdef __cplusplus
}
#endif

#endif // L2CAP_H
