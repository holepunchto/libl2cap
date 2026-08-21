# libl2cap

I/O-agnostic Bluetooth L2CAP channels for Linux.

The library wraps the kernel's `AF_BLUETOOTH`/`SOCK_SEQPACKET` sockets behind
a sans-I/O contract: it owns the protocol state, you own the event loop. Watch
`l2cap_channel_fd()` for the bits `l2cap_channel_events()` asks for, and hand
the results to `l2cap_channel_process()`.

Linux-only by design: other platforms expose L2CAP through entirely different
native APIs.

```c
#include <l2cap.h>

l2cap_addr_t local, peer;
l2cap_addr_init("78:AF:08:C0:40:3A", L2CAP_BDADDR_LE_PUBLIC, &local);
l2cap_addr_init("6C:3E:00:AB:78:1D", L2CAP_BDADDR_LE_RANDOM, &peer);

l2cap_channel_t channel;
l2cap_channel_init(&channel, NULL);
l2cap_channel_connect(&channel, &local, &peer, 0x80, on_connect);

// poll l2cap_channel_fd() for l2cap_channel_events(),
// then l2cap_channel_process(&channel, fired)
```

See `include/l2cap.h` for the full API and `test/` for runnable examples —
the state machine is exercised with plain Unix `SOCK_SEQPACKET` socketpairs,
no Bluetooth adapter required.

## License

Apache-2.0
