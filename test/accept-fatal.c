// A fatal accept failure - here EINVAL from a connected, non-listening
// descriptor - must stop the server: the caller learns it through
// l2cap_server_failed() instead of spinning on a level-triggered fd.

#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <l2cap.h>

static int connections = 0;

static void
on_connection(l2cap_server_t *server) {
  (void) server;
  connections++;
}

int
main(void) {
  int fds[2];
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) == 0);

  l2cap_server_t server;
  l2cap_server_init(&server, NULL);
  assert(l2cap_server_attach(&server, fds[0]) == 0);
  assert(l2cap_server_accept_start(&server, on_connection) == 0);
  assert(l2cap_server_events(&server) == L2CAP_READABLE);
  assert(l2cap_server_failed(&server) == 0);

  l2cap_channel_t channel;
  l2cap_channel_init(&channel, NULL);
  assert(l2cap_server_accept(&server, &channel) == -EINVAL);

  assert(l2cap_server_failed(&server) == 1);
  assert(l2cap_server_events(&server) == 0); // nothing left to poll for

  // Failed is terminal: accepting never resumes
  l2cap_channel_t again;
  l2cap_channel_init(&again, NULL);
  assert(l2cap_server_accept(&server, &again) == -EINVAL);
  assert(connections == 0);

  l2cap_server_close(&server);
  close(fds[1]);

  return 0;
}
