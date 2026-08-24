// The IDLE guard in l2cap_server_listen() fires before any Bluetooth socket
// is created, so the rejection path is testable with a Unix socket: attach a
// descriptor to leave IDLE, then verify listen() refuses instead of
// clobbering the live descriptor.

#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <l2cap.h>

int
main(void) {
  l2cap_addr_t local;
  assert(l2cap_addr_init("78:AF:08:C0:40:3A", L2CAP_BDADDR_LE_PUBLIC, &local) == 0);

  int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  assert(fd >= 0);

  l2cap_server_t server;
  l2cap_server_init(&server, NULL);
  assert(l2cap_server_attach(&server, fd) == 0);

  assert(l2cap_server_listen(&server, &local, 0x80, 1) == -EINVAL);
  assert(l2cap_server_fd(&server) == fd);

  l2cap_server_close(&server);

  // closed is not idle either: reuse requires a fresh init
  assert(l2cap_server_listen(&server, &local, 0x80, 1) == -EINVAL);

  return 0;
}
