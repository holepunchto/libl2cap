// EOF must be delivered exactly once. Without disarming, a peer that closes
// leaves the descriptor readable forever: events() keeps asking for READABLE,
// recv() keeps returning 0, and a caller that does not close inside the
// callback spins at 100% CPU re-receiving the same EOF.

#include <assert.h>
#include <sys/socket.h>
#include <unistd.h>

#include <l2cap.h>

static int eof_count = 0;

static void
on_read (l2cap_channel_t *channel, size_t len, const uint8_t *data) {
  (void) channel;
  (void) data;

  assert(len == 0);
  eof_count++;
}

int
main (void) {
  int fds[2];
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) == 0);

  l2cap_channel_t channel;
  l2cap_channel_init(&channel, NULL);
  assert(l2cap_channel_accept(&channel, fds[1]) == 0);
  assert(l2cap_channel_read_start(&channel, on_read) == 0);

  close(fds[0]); // peer goes away

  assert(l2cap_channel_events(&channel) & L2CAP_READABLE);
  assert(l2cap_channel_process(&channel, L2CAP_READABLE) == 0);
  assert(eof_count == 1);

  // the channel stops asking for READABLE...
  assert((l2cap_channel_events(&channel) & L2CAP_READABLE) == 0);

  // ...and even a spurious READABLE does not re-deliver the EOF
  assert(l2cap_channel_process(&channel, L2CAP_READABLE) == 0);
  assert(eof_count == 1);

  // read_start() re-arms explicitly: the EOF is delivered again
  assert(l2cap_channel_read_start(&channel, on_read) == 0);
  assert(l2cap_channel_process(&channel, L2CAP_READABLE) == 0);
  assert(eof_count == 2);

  l2cap_channel_close(&channel);

  return 0;
}
