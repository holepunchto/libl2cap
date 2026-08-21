// Fill the kernel buffer until a write gets queued, drain the peer, and check
// the drain callback fires exactly once.

#include <assert.h>
#include <poll.h>
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

  // Write until the kernel pushes back and the chunk gets queued. The bound
  // only guards against EAGAIN never arriving: the buffers involved hold a
  // few KB, orders of magnitude less than the bound allows.
  int r = 0;
  int sent = 0;
  while ((r = l2cap_channel_write(&a, msg, sizeof(msg), on_drain)) == L2CAP_WRITE_SENT) {
    sent++;
    assert(sent < 10000);
  }
  assert(r == L2CAP_WRITE_QUEUED);
  assert(drained == 0); // queued, not yet drained
  assert(l2cap_channel_events(&a) & L2CAP_WRITABLE);

  // Drain the peer side, then let the channel flush
  uint8_t buf[sizeof(msg)];
  while (drained == 0) {
    while (recv(fds[1], buf, sizeof(buf), MSG_DONTWAIT) > 0);

    // The peer is already drained, so POLLOUT is immediate: no need to block
    struct pollfd p = {l2cap_channel_fd(&a), POLLOUT, 0};
    assert(poll(&p, 1, 0) >= 0);
    assert(l2cap_channel_process(&a, L2CAP_WRITABLE) == 0);
  }

  assert(drained == 1);
  assert((l2cap_channel_events(&a) & L2CAP_WRITABLE) == 0);

  l2cap_channel_close(&a);
  close(fds[1]);

  return 0;
}
