// A queued fire-and-forget write (NULL callback) must not erase the drain
// callback a previous queued write registered: drain means "the queue
// emptied", and the caller who asked for it is still waiting.

#include <assert.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <l2cap.h>

static int drained = 0;

static void
on_drain(l2cap_channel_t *channel) {
  (void) channel;
  drained++;
}

int
main(void) {
  int fds[2];
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) == 0);

  int sndbuf = 4096;
  assert(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0);

  l2cap_channel_t a;
  l2cap_channel_init(&a, NULL);
  assert(l2cap_channel_accept(&a, fds[0]) == 0);

  uint8_t msg[512];
  memset(msg, 0xab, sizeof(msg));

  int r = 0;
  int sent = 0;
  while ((r = l2cap_channel_write(&a, msg, sizeof(msg), on_drain)) == L2CAP_WRITE_SENT) {
    sent++;
    assert(sent < 10000);
  }
  assert(r == L2CAP_WRITE_QUEUED);

  // fire-and-forget on top of the queue: must not erase on_drain
  assert(l2cap_channel_write(&a, msg, sizeof(msg), NULL) == L2CAP_WRITE_QUEUED);

  // alternate: make room on the peer side, flush, until the queue empties
  uint8_t buf[sizeof(msg)];
  int spins = 0;
  while (l2cap_channel_events(&a) & L2CAP_WRITABLE) {
    while (recv(fds[1], buf, sizeof(buf), MSG_DONTWAIT) > 0)
      ;
    assert(l2cap_channel_process(&a, L2CAP_WRITABLE) == 0);
    assert(++spins < 10000);
  }

  assert(drained == 1);

  l2cap_channel_close(&a);
  close(fds[1]);

  return 0;
}
