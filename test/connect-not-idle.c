// The IDLE guard in l2cap_channel_connect() fires before any Bluetooth
// socket is created, so the rejection path is testable with a Unix
// socketpair: adopt a descriptor to leave IDLE, then verify connect()
// refuses instead of clobbering the live descriptor.

#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <l2cap.h>

int
main(void) {
  l2cap_addr_t local, peer;
  assert(l2cap_addr_init("78:AF:08:C0:40:3A", L2CAP_BDADDR_LE_PUBLIC, &local) == 0);
  assert(l2cap_addr_init("6C:3E:00:AB:78:1D", L2CAP_BDADDR_LE_RANDOM, &peer) == 0);

  int fds[2];
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) == 0);

  l2cap_channel_t channel;
  l2cap_channel_init(&channel, NULL);
  assert(l2cap_channel_accept(&channel, fds[0]) == 0);

  int fd = l2cap_channel_fd(&channel);
  assert(l2cap_channel_connect(&channel, &local, &peer, 0x80, NULL) == -EINVAL);
  assert(l2cap_channel_fd(&channel) == fd);

  // the rejected connect changed nothing: the channel still writes
  const uint8_t msg[] = "still open";
  assert(l2cap_channel_write(&channel, msg, sizeof(msg), NULL) == L2CAP_WRITE_SENT);

  l2cap_channel_close(&channel);

  // closed is not idle either: reuse requires a fresh init
  assert(l2cap_channel_connect(&channel, &local, &peer, 0x80, NULL) == -EINVAL);

  close(fds[1]);

  return 0;
}
