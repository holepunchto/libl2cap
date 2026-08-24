// An SDU larger than the send MTU must be rejected up front with EMSGSIZE,
// never queued and never fatal to the channel.

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <l2cap.h>

int
main (void) {
  int fds[2];
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) == 0);

  l2cap_channel_t a;
  l2cap_channel_init(&a, NULL);
  assert(l2cap_channel_accept(&a, fds[0]) == 0);

  uint16_t mtu = l2cap_channel_snd_mtu(&a);
  assert(mtu > 0);

  uint8_t *msg = malloc(mtu + 1);
  assert(msg != NULL);
  memset(msg, 0xab, mtu + 1);

  assert(l2cap_channel_write(&a, msg, mtu + 1, NULL) == -EMSGSIZE);
  assert((l2cap_channel_events(&a) & L2CAP_WRITABLE) == 0); // nothing queued

  // The channel survives and still accepts a conforming SDU
  assert(l2cap_channel_write(&a, msg, mtu, NULL) == L2CAP_WRITE_SENT);

  free(msg);
  l2cap_channel_close(&a);
  close(fds[1]);

  return 0;
}
