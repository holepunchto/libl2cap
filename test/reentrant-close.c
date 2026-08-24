// Closing the channel from inside a read callback must stop the read loop
// immediately: the second queued SDU is never delivered.

#include <assert.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <l2cap.h>

static int reads = 0;

static void
on_read(l2cap_channel_t *channel, size_t len, const uint8_t *data) {
  (void) len;
  (void) data;

  reads++;
  l2cap_channel_close(channel);
}

int
main(void) {
  int fds[2];
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) == 0);

  l2cap_channel_t a;
  l2cap_channel_init(&a, NULL);
  assert(l2cap_channel_accept(&a, fds[0]) == 0);
  assert(l2cap_channel_read_start(&a, on_read) == 0);

  const uint8_t msg[] = "x";
  assert(send(fds[1], msg, sizeof(msg), 0) >= 0);
  assert(send(fds[1], msg, sizeof(msg), 0) >= 0);

  assert(l2cap_channel_process(&a, L2CAP_READABLE) == 0);

  // Two SDUs went in, but the close inside on_read must stop the loop: the
  // second one is never delivered - one read, not two
  assert(reads == 1);
  assert(l2cap_channel_events(&a) == 0);

  close(fds[1]);

  return 0;
}
