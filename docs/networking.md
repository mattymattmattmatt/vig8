# Networking: System Link and the xlive relay

Code: [`project/src/net.cpp`](../project/src/net.cpp). Settings are cvars in the `Vig8/Network`
group of the F4 settings overlay (saved to `project\build\vig8.toml`):

| cvar | Default | |
| --- | --- | --- |
| `lan_port` | `3074` | UDP port for session discovery. Open it in the firewall on every machine. |
| `relay_host` | empty | An [xlive](https://github.com/sp00nznet/xlive) relay for play beyond the LAN. Empty = LAN only. |
| `relay_port` | `36000` | The relay's TCP port. |

## Why the overrides exist

The SDK's XNet layer is Xenia's: every console reports the same placeholder address, QoS lookup
is a stub, and overlapped socket receives aren't implemented. A game can create a System Link
session but no other machine can find or reach it. `net.cpp` replaces those imports
(`REX_EXTERN(__imp__NetDll_...)`, linked over the SDK's with `/force:multiple`).

## How a session is found

1. **Addresses.** `XNetGetTitleXnAddr` returns an XNADDR holding this machine's LAN IPv4 address
   (private ranges first; a Tailscale `100.64/10` address only if nothing else exists, because a
   VPN address would beacon to a network the other machine isn't on). The MAC field is derived
   from the IP so each machine's is unique.
2. **Hosting.** When the game calls `XNetQosListen` it starts beaconing: a background thread
   broadcasts a packet every 500 ms on UDP `lan_port`.
3. **Relay.** With `relay_host` set, the XGI session messages the game sends through
   `XMsgStartIORequest` go to the relay: create (`0x000B0010`) registers the session, search
   (`0x000B001C`) fills the game's result buffer from the relay's list.
4. **QoS.** `XNetQosLookup` answers at once with good figures (30 ms RTT, 10 Mbit/s). The game
   only needs the lookup to complete before it lists a session.
5. **Play.** Game traffic goes peer to peer over the SDK's native sockets.
   `WSARecvFrom`/`WSAGetOverlappedResult` poll the native socket for the game's overlapped
   receives.

## Discovery packets

UDP, broadcast to `255.255.255.255:lan_port` or sent back to a prober:

| Offset | Size | |
| --- | --- | --- |
| 0 | 1 | magic `0xD8` |
| 1 | 1 | `0x01` beacon, `0x02` probe |
| 2 | 8 | XNKID (session id) |
| 10 | 36 | sender's XNADDR |
| 46 | 2 | beacon only: QoS data length, big-endian |
| 48 | n | beacon only: the host's QoS data, as given to `XNetQosListen` |

## Timing workarounds, and why

The game stores a callback object in its session struct (`+8` of the overlapped) **after** the
call that starts an operation returns. If the operation completes inside that call, its
completion callback (`sub_8218A068`) reads garbage and faults. So:

- session search completes 150 ms later, from the next `XMsgStartIORequest` call (a guest thread;
  the discovery thread has no kernel state to complete it from);
- other XGI messages complete deferred rather than immediately;
- a QoS result stays "pending" for 300 ms;
- and `sub_8218A068` skips the callback outright if the object or its vtable pointer is near null.

The relay session isn't removed on `XGISessionDelete`: the game deletes its Live session right
after creating it while still waiting in the lobby. It goes when the relay connection closes.

## What has been tested

The last test on the 0.1 SDK (2026-03-03, two instances and a relay on one LAN) got this far: the
client connected to the relay, searched, and found the host's session:

```
[XGI] SEARCH result[0]: xnkid=E62E.. ip=C0A8xxxx
[XGI] SEARCH found 1 sessions
```

It then crashed with a null read (`Access address: 0x0000000000000020 (READ)`) before listing the
session. The timing workarounds above were committed later that day (`3f3e87f`) and were never
run against a host. Nothing has been tested on the 0.10 SDK yet, and no two-machine race has been
played. See ROADMAP.md.
