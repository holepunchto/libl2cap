// The state machine does not care that the descriptor is Bluetooth: a Unix
// SOCK_SEQPACKET socketpair has the same semantics (atomic SDUs, EAGAIN, EOF),
// so the whole read/write/close path is testable without an adapter.

#include <assert.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <l2cap.h>

static char received[64];
static size_t received_len = 0;
static int eof_seen = 0;

static void
on_read(l2cap_channel_t *channel, size_t len, const uint8_t *data) {
  (void) channel;

  if (len == 0) {
    eof_seen = 1;
    return;
  }

  assert(len < sizeof(received));
  memcpy(received, data, len);
  received_len = len;
}

static void
pump(l2cap_channel_t *channel) {
  int events = l2cap_channel_events(channel);
  if (events == 0) return;

  struct pollfd p = {l2cap_channel_fd(channel), 0, 0};
  if (events & L2CAP_READABLE) p.events |= POLLIN;
  if (events & L2CAP_WRITABLE) p.events |= POLLOUT;

  int n = poll(&p, 1, 1000);
  assert(n >= 0);
  if (n == 0) return;

  int fired = 0;
  if (p.revents & (POLLIN | POLLHUP)) fired |= L2CAP_READABLE;
  if (p.revents & (POLLOUT | POLLERR)) fired |= L2CAP_WRITABLE;

  int err = l2cap_channel_process(channel, fired);
  assert(err == 0);
}

int
main(void) {
  int fds[2];
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) == 0);

  l2cap_channel_t a, b;
  l2cap_channel_init(&a, NULL);
  l2cap_channel_init(&b, NULL);
  assert(l2cap_channel_accept(&a, fds[0]) == 0);
  assert(l2cap_channel_accept(&b, fds[1]) == 0);

  assert(l2cap_channel_read_start(&b, on_read) == 0);

  const uint8_t msg[] = "hello over l2cap";
  assert(l2cap_channel_write(&a, msg, sizeof(msg), NULL) == L2CAP_WRITE_SENT);

  pump(&b);
  assert(received_len == sizeof(msg));
  assert(memcmp(received, msg, sizeof(msg)) == 0);

  l2cap_channel_close(&a);
  pump(&b);
  assert(eof_seen);

  l2cap_channel_close(&b);
  l2cap_channel_close(&b); // idempotent

  return 0;
}
