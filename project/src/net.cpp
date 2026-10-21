// Vigilante 8 Arcade: System Link (LAN) play, and internet play through an
// xlive relay (https://github.com/sp00nznet/xlive).
//
// The SDK's XNet layer gives every console the same fake address and stubs QoS
// lookup, so the game can never find or reach another one. These overrides
// give it real addresses and a way to find sessions:
//
//   - XNADDRs carry this machine's real LAN IPv4 address, and a peer table maps
//     the addresses the game sees back to IPs.
//   - A discovery thread broadcasts a beacon on UDP `lan_port` while hosting,
//     and answers probes.
//   - Session create/search (XGI messages through XMsgStartIORequest) go to the
//     relay when `relay_host` is set, so a client finds hosts beyond the LAN.
//   - XNetQosLookup answers at once with plausible stats; the game only needs
//     the lookup to complete before it lists a session.
//   - WSARecvFrom/WSAGetOverlappedResult poll the native socket, because the
//     game receives with overlapped I/O, which the SDK doesn't implement.
//
// Game traffic itself goes peer to peer over the SDK's native sockets. See
// docs/networking.md for the protocol and what has been tested.

#include "vig8_pch.h"
#include "vig8_init.h"
#include "xlive.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

REXCVAR_DEFINE_INT32(lan_port, 3074, "Vig8/Network",
                     "UDP port for System Link session discovery (open it in the firewall)");
REXCVAR_DEFINE_STRING(relay_host, "", "Vig8/Network",
                      "xlive relay server for internet play; empty = LAN only");
REXCVAR_DEFINE_INT32(relay_port, 36000, "Vig8/Network", "xlive relay TCP port");

namespace {

constexpr uint32_t kTitleId = 0x584108A8;  // Vigilante 8 Arcade

// Discovery packets: magic, type, xnkid[8], xnaddr[36], then for a beacon a
// big-endian u16 length and the host's QoS data.
constexpr uint8_t kMagic = 0xD8, kBeacon = 0x01, kProbe = 0x02;
constexpr int kHeaderLen = 46;
constexpr int kMaxQos = 512;

#pragma pack(push, 1)
struct XNADDR {
  uint32_t ina;        // IPv4, network order
  uint32_t inaOnline;  // same
  uint16_t wPortOnline;
  uint8_t abEnet[6];   // derived from the IP, so it's unique per machine
  uint8_t abOnline[20];
};
#pragma pack(pop)
static_assert(sizeof(XNADDR) == 36);

struct Peer {
  XNADDR xnaddr;
  uint8_t xnkid[8];
  bool connected;
};

XNADDR g_local{};
uint32_t g_local_ip = 0;
uint8_t* g_base = nullptr;  // guest memory, saved for the discovery thread

std::vector<Peer> g_peers;
std::shared_mutex g_peers_mutex;

struct {
  bool active;
  uint8_t xnkid[8];
  uint8_t data[kMaxQos];
  uint16_t data_len;
} g_qos;
std::mutex g_qos_mutex;

// A QoS result whose "pending" count is cleared a little later (see
// XNetQosLookup), and a search completion likewise held back.
struct Delayed {
  uint32_t addr, result;
  std::chrono::steady_clock::time_point at;
};
std::vector<Delayed> g_qos_done, g_search_done;
std::mutex g_delayed_mutex;

SOCKET g_disc = INVALID_SOCKET;
std::thread g_disc_thread;
std::atomic<bool> g_disc_running{false};

struct PendingRecv {
  uint32_t socket, buf, buf_len, bytes_ptr, flags_ptr, from_ptr, fromlen_ptr;
};
std::unordered_map<uint32_t, PendingRecv> g_recvs;  // by overlapped address
std::mutex g_recvs_mutex;

auto later(int ms) { return std::chrono::steady_clock::now() + std::chrono::milliseconds(ms); }

XNADDR AddrFromIp(uint32_t ip) {
  XNADDR a{};
  a.ina = a.inaOnline = ip;
  auto* b = reinterpret_cast<uint8_t*>(&ip);
  uint8_t mac[6] = {0x00, 0x50, b[0], b[1], b[2], b[3]};
  std::memcpy(a.abEnet, mac, 6);
  return a;
}

Peer* FindPeer(uint32_t ip) {
  for (auto& p : g_peers)
    if (p.xnaddr.ina == ip) return &p;
  return nullptr;
}

void AddPeer(const XNADDR& addr, const uint8_t* xnkid) {
  std::unique_lock lock(g_peers_mutex);
  Peer* p = FindPeer(addr.ina);
  if (!p) p = &g_peers.emplace_back(Peer{addr, {}, false});
  p->xnaddr = addr;
  if (xnkid) std::memcpy(p->xnkid, xnkid, 8);
}

uint32_t ReadIp(uint8_t* base, uint32_t ptr) {
  uint32_t ip;
  std::memcpy(&ip, base + ptr, 4);
  return ip;
}

// The first private (10/8, 172.16/12, 192.168/16) address, else any non-loopback
// one that isn't Tailscale's 100.64/10, else Tailscale. A VPN address first would
// beacon to a network the other console isn't on.
uint32_t PickLanIp() {
  char host[256] = {};
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  if (gethostname(host, sizeof(host)) != 0 || getaddrinfo(host, nullptr, &hints, &res) != 0)
    return htonl(INADDR_LOOPBACK);
  uint32_t best = 0, other = 0, tailscale = 0;
  for (auto* ai = res; ai; ai = ai->ai_next) {
    uint32_t ip = reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr.s_addr, h = ntohl(ip);
    if ((h >> 24) == 127) continue;
    bool priv = (h >> 24) == 10 || (h & 0xFFF00000u) == 0xAC100000u || (h >> 16) == 0xC0A8;
    if (priv && !best) best = ip;
    else if ((h & 0xFFC00000u) == 0x64400000u) { if (!tailscale) tailscale = ip; }
    else if (!other) other = ip;
  }
  freeaddrinfo(res);
  return best ? best : other ? other : tailscale ? tailscale : htonl(INADDR_LOOPBACK);
}

void SendDisc(uint8_t type, const uint8_t* xnkid, const sockaddr_in& to, const uint8_t* qos,
              uint16_t qos_len) {
  uint8_t buf[kHeaderLen + 2 + kMaxQos];
  buf[0] = kMagic;
  buf[1] = type;
  std::memcpy(buf + 2, xnkid, 8);
  std::memcpy(buf + 10, &g_local, 36);
  int n = kHeaderLen;
  if (type == kBeacon) {
    buf[46] = uint8_t(qos_len >> 8);
    buf[47] = uint8_t(qos_len);
    std::memcpy(buf + 48, qos, qos_len);
    n = 48 + qos_len;
  }
  sendto(g_disc, reinterpret_cast<char*>(buf), n, 0, reinterpret_cast<const sockaddr*>(&to),
         sizeof(to));
}

void SendBeacon(const sockaddr_in& to) {
  std::lock_guard lock(g_qos_mutex);
  if (g_qos.active) SendDisc(kBeacon, g_qos.xnkid, to, g_qos.data, g_qos.data_len);
}

sockaddr_in DiscAddr(uint32_t ip) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(uint16_t(REXCVAR_GET(lan_port)));
  a.sin_addr.s_addr = ip;
  return a;
}

void DiscoveryLoop() {
  auto next_beacon = std::chrono::steady_clock::now();
  while (g_disc_running.load()) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(g_disc, &fds);
    timeval tv{0, 100000};
    if (select(int(g_disc) + 1, &fds, nullptr, nullptr, &tv) > 0) {
      uint8_t buf[2048];
      sockaddr_in from{};
      socklen_t fromlen = sizeof(from);
      int n = recvfrom(g_disc, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                       reinterpret_cast<sockaddr*>(&from), &fromlen);
      XNADDR sender;
      if (n >= kHeaderLen && buf[0] == kMagic) {
        std::memcpy(&sender, buf + 10, 36);
        if (sender.ina != g_local_ip) {
          if (buf[1] == kProbe) SendBeacon(DiscAddr(from.sin_addr.s_addr));
          else if (buf[1] == kBeacon) AddPeer(sender, buf + 2);
        }
      }
    }
    {
      std::lock_guard lock(g_delayed_mutex);
      auto now = std::chrono::steady_clock::now();
      std::erase_if(g_qos_done, [&](const Delayed& d) {
        if (now < d.at) return false;
        std::memset(g_base + d.addr + 4, 0, 4);  // cxnqosPending = 0

        return true;
      });
    }
    if (std::chrono::steady_clock::now() >= next_beacon) {
      SendBeacon(DiscAddr(INADDR_BROADCAST));
      next_beacon = later(500);
    }
  }
}

// ---- XGI session messages (app 251) ------------------------------------------
// Buffer layouts read from the game's calls to XMsgStartIORequest.

// XGISessionCreateImpl, 0x000B0010: +0x08 public slots, +0x14 XSESSION_INFO out.
uint32_t XgiCreate(uint8_t* base, uint32_t buf, uint32_t len) {
  if (!buf || len < 0x18) return 0x80004005;
  uint32_t info = REX_LOAD_U32(buf + 0x14), slots = REX_LOAD_U32(buf + 0x08);
  uint8_t xnkid[8];
  uint32_t seed = g_local_ip ^ uint32_t(std::time(nullptr));
  for (auto& b : xnkid) b = uint8_t((seed = seed * 1664525u + 1013904223u) >> 24);
  if (info) {
    std::memcpy(base + info, xnkid, 8);
    std::memcpy(base + info + 8, &g_local, 36);
  }
  if (xlive::IsConnected()) xlive::RegisterSession(xnkid, nullptr, 0, uint8_t(slots));
  REXLOG_INFO("[net] hosting session {:02X}{:02X}{:02X}{:02X}.. ({} slots, {})", xnkid[0],
              xnkid[1], xnkid[2], xnkid[3], slots, xlive::IsConnected() ? "relay" : "LAN only");
  return 0;
}

// XGISessionSearch, 0x000B001C: +0x08 max results, +0x1C output buffer: a u32
// count, 4 reserved bytes, then 1326-byte XSESSION_SEARCHRESULTs (XNKID, XNADDR,
// open/filled slot counts at +44/+46).
uint32_t XgiSearch(uint8_t* base, uint32_t buf, uint32_t len) {
  if (!buf || len < 0x20) return 0x80004005;
  uint32_t max = REX_LOAD_U32(buf + 0x08), out = REX_LOAD_U32(buf + 0x1C);
  if (!out || !max) return 0;
  xlive::Session s[8];
  int count = xlive::IsConnected() ? xlive::SearchSessions(s, int(std::min(max, 8u))) : 0;
  count = std::max(count, 0);
  for (int i = 0; i < count; i++) {
    uint32_t e = out + 8 + uint32_t(i) * 1326;
    std::memset(base + e, 0, 1326);
    std::memcpy(base + e, s[i].xnkid, 8);
    std::memcpy(base + e + 8, s[i].host_xnaddr, 36);
    base[e + 44] = s[i].max_players > s[i].current_players
                       ? uint8_t(s[i].max_players - s[i].current_players) : 0;
    base[e + 46] = s[i].current_players;
    XNADDR host;
    std::memcpy(&host, s[i].host_xnaddr, 36);
    AddPeer(host, s[i].xnkid);
  }
  REX_STORE_U32(out, uint32_t(count));
  REXLOG_INFO("[net] session search: {} found", count);
  return 0;
}

// Copy one received datagram into the guest's WSABUF and out-params.
void DeliverRecv(uint8_t* base, const PendingRecv& r, const uint8_t* data, int n,
                 const sockaddr_in& from, uint32_t overlapped) {
  if (r.buf && uint32_t(n) <= r.buf_len) std::memcpy(base + r.buf, data, n);
  if (r.bytes_ptr) REX_STORE_U32(r.bytes_ptr, uint32_t(n));
  if (r.flags_ptr) REX_STORE_U32(r.flags_ptr, 0);
  if (r.from_ptr) {  // Xbox sockaddr_in: BE family, then port and addr as sent
    REX_STORE_U16(r.from_ptr, AF_INET);
    std::memcpy(base + r.from_ptr + 2, &from.sin_port, 2);
    std::memcpy(base + r.from_ptr + 4, &from.sin_addr, 4);
    std::memset(base + r.from_ptr + 8, 0, 8);
  }
  if (r.fromlen_ptr) REX_STORE_U32(r.fromlen_ptr, 16);
  if (overlapped) {
    REX_STORE_U32(overlapped + 0, 0);  // InternalLow: complete
    REX_STORE_U32(overlapped + 4, uint32_t(n));
  }
}

// Non-blocking receive on the guest socket behind `handle`; <= 0 if nothing.
int TryRecv(uint32_t handle, uint32_t max, uint8_t* data, sockaddr_in& from) {
  auto sock = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(handle);
  if (!sock) return -2;
  auto native = SOCKET(sock->native_handle());
#ifdef _WIN32
  u_long nb = 1;
  ioctlsocket(native, FIONBIO, &nb);
#else
  fcntl(native, F_SETFL, fcntl(native, F_GETFL, 0) | O_NONBLOCK);
#endif
  socklen_t fromlen = sizeof(from);
  return recvfrom(native, reinterpret_cast<char*>(data), int(std::min<uint32_t>(max, 65536)), 0,
                  reinterpret_cast<sockaddr*>(&from), &fromlen);
}

}  // namespace

void NetInit() {
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  g_local_ip = PickLanIp();
  g_local = AddrFromIp(g_local_ip);
  g_local.wPortOnline = htons(uint16_t(REXCVAR_GET(lan_port)));
  char ip[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &g_local_ip, ip, sizeof(ip));

  g_disc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  int on = 1;
  setsockopt(g_disc, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char*>(&on), sizeof(on));
  setsockopt(g_disc, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&on), sizeof(on));
  sockaddr_in any = DiscAddr(INADDR_ANY);
  if (g_disc == INVALID_SOCKET ||
      bind(g_disc, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0) {
    REXLOG_ERROR("[net] can't bind UDP port {}; System Link discovery is off",
                 REXCVAR_GET(lan_port));
    if (g_disc != INVALID_SOCKET) closesocket(g_disc);
    g_disc = INVALID_SOCKET;
  } else {
    g_disc_running = true;
    g_disc_thread = std::thread(DiscoveryLoop);
  }
  REXLOG_INFO("[net] System Link on {}:{}", ip, REXCVAR_GET(lan_port));

  const auto& relay = REXCVAR_GET(relay_host);
  if (!relay.empty()) {
    bool ok = xlive::Connect(relay.c_str(), uint16_t(REXCVAR_GET(relay_port)), kTitleId,
                             reinterpret_cast<const uint8_t*>(&g_local), "Player");
    REXLOG_INFO("[net] relay {}:{} {}", relay, REXCVAR_GET(relay_port),
                ok ? "connected" : "unreachable");
  }
}

void NetShutdown() {
  if (xlive::IsConnected()) xlive::Disconnect();
  g_disc_running = false;
  if (g_disc_thread.joinable()) g_disc_thread.join();
  if (g_disc != INVALID_SOCKET) closesocket(g_disc);
  g_disc = INVALID_SOCKET;
}

// Session create and search go to our handlers; everything else to the SDK's
// app manager as before.
REX_EXTERN(__imp__XMsgStartIORequest) {
  uint32_t app = ctx.r3.u32, msg = ctx.r4.u32, overlapped = ctx.r5.u32;
  uint32_t buf = ctx.r6.u32, len = ctx.r7.u32;

  // Search completions are fired here because this runs on a guest thread;
  // the discovery thread has no kernel state to complete them from.
  std::vector<Delayed> due;
  {
    std::lock_guard lock(g_delayed_mutex);
    auto now = std::chrono::steady_clock::now();
    std::erase_if(g_search_done, [&](const Delayed& d) {
      return now >= d.at ? (due.push_back(d), true) : false;
    });
  }
  for (auto& d : due) REX_KERNEL_STATE()->CompleteOverlappedImmediate(d.addr, d.result);

  uint32_t result;
  if (app == 251 && msg == 0x000B0010) {
    result = XgiCreate(base, buf, len);
  } else if (app == 251 && msg == 0x000B001C) {
    // Completed 150 ms later: completing at once runs the game's callback
    // (sub_8218A068) before it has stored the callback object at
    // overlapped+8, and the callback reads garbage there. Completed
    // immediately rather than deferred, because the SDK never saw this request.
    result = XgiSearch(base, buf, len);
    if (overlapped) {
      std::lock_guard lock(g_delayed_mutex);
      g_search_done.push_back({overlapped, result, later(150)});
      ctx.r3.u64 = 0x103;  // X_ERROR_IO_PENDING
      return;
    }
    ctx.r3.u64 = result;
    return;
  } else if (app == 251 && msg == 0x000B0011) {
    // XGISessionDelete. The relay entry is kept: the game deletes its Live
    // session right after creating it while still waiting in the lobby, and
    // unregistering then would hide it from every client. It goes when the
    // relay connection closes.
    result = 0;
  } else {
    result = REX_KERNEL_STATE()->app_manager()->DispatchMessageAsync(app, msg, buf, len);
  }
  if (overlapped) {
    // Deferred, for the same reason as the search delay above.
    REX_KERNEL_STATE()->CompleteOverlappedDeferred([] {}, overlapped, result);
    ctx.r3.u64 = 0x103;
  } else {
    ctx.r3.u64 = result;
  }
}

// ---- XNet addresses ----------------------------------------------------------
// NetDll_* functions take the caller in r3; the real arguments start at r4.

REX_EXTERN(__imp__NetDll_XNetGetTitleXnAddr) {
  if (ctx.r4.u32) std::memcpy(base + ctx.r4.u32, &g_local, 36);
  ctx.r3.u64 = 4;  // XNET_GET_XNADDR_STATIC
}

REX_EXTERN(__imp__NetDll_XNetXnAddrToInAddr) {
  uint32_t xnaddr = ctx.r4.u32, xnkid = ctx.r5.u32, out = ctx.r6.u32;
  if (!xnaddr || !out) {
    ctx.r3.u64 = 0x80070057;
    return;
  }
  XNADDR a;
  std::memcpy(&a, base + xnaddr, 36);
  uint8_t kid[8] = {};
  if (xnkid) std::memcpy(kid, base + xnkid, 8);
  AddPeer(a, kid);
  std::memcpy(base + out, &a.ina, 4);
  ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__NetDll_XNetInAddrToXnAddr) {
  uint32_t in = ctx.r4.u32, xnaddr_out = ctx.r5.u32, xnkid_out = ctx.r6.u32;
  if (!in) {
    ctx.r3.u64 = 0x80070057;
    return;
  }
  uint32_t ip = ReadIp(base, in);
  std::shared_lock lock(g_peers_mutex);
  Peer* p = FindPeer(ip);
  XNADDR a = p ? p->xnaddr : AddrFromIp(ip);
  if (xnaddr_out) std::memcpy(base + xnaddr_out, &a, 36);
  if (xnkid_out) {
    if (p) std::memcpy(base + xnkid_out, p->xnkid, 8);
    else std::memset(base + xnkid_out, 0, 8);
  }
  ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__NetDll_XNetConnect) {
  if (ctx.r4.u32) {
    uint32_t ip = ReadIp(base, ctx.r4.u32);
    std::unique_lock lock(g_peers_mutex);
    Peer* p = FindPeer(ip);
    if (!p) p = &g_peers.emplace_back(Peer{AddrFromIp(ip), {}, false});
    p->connected = true;
  }
  ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__NetDll_XNetGetConnectStatus) {
  bool up = false;
  if (ctx.r4.u32) {
    std::shared_lock lock(g_peers_mutex);
    Peer* p = FindPeer(ReadIp(base, ctx.r4.u32));
    up = p && p->connected;
  }
  ctx.r3.u64 = up ? 4 /*CONNECTED*/ : 0 /*IDLE*/;
}

REX_EXTERN(__imp__NetDll_XNetUnregisterInAddr) {
  if (ctx.r4.u32) {
    uint32_t ip = ReadIp(base, ctx.r4.u32);
    std::unique_lock lock(g_peers_mutex);
    std::erase_if(g_peers, [ip](const Peer& p) { return p.xnaddr.ina == ip; });
  }
  ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__NetDll_XNetGetEthernetLinkStatus) {
  (void)base;
  ctx.r3.u64 = 0x01 | 0x04 | 0x08;  // active, 100 Mbps, full duplex
}

// ---- QoS ---------------------------------------------------------------------

// XNetQosListen(xnkid r4, data r5, len r6, bits/s r7, flags r8). Flag 1 stops
// listening; 2 (listen) or 4 (set data) starts beaconing that data.
REX_EXTERN(__imp__NetDll_XNetQosListen) {
  uint32_t xnkid = ctx.r4.u32, data = ctx.r5.u32, len = ctx.r6.u32, flags = ctx.r8.u32;
  std::lock_guard lock(g_qos_mutex);
  if (flags & 1) {
    g_qos.active = false;
  } else if (flags & 6) {
    g_qos.active = true;
    if (xnkid) std::memcpy(g_qos.xnkid, base + xnkid, 8);
    if (data && len) {
      g_qos.data_len = uint16_t(std::min<uint32_t>(len, kMaxQos));
      std::memcpy(g_qos.data, base + data, g_qos.data_len);
    }
  }
  ctx.r3.u64 = 0;
}

// XNetQosLookup(cxna r4, ...): the 13th argument, the XNQOS** to fill, is on the
// stack at r1+116 (the 9th argument is at r1+84). The result is an XNQOS: u32
// cxnqos, u32 cxnqosPending, then 24-byte XNQOSINFOs. It's filled at once with
// good stats but left pending for 300 ms (cleared by the discovery thread),
// because the game reads the callback object it stores after this returns.
REX_EXTERN(__imp__NetDll_XNetQosLookup) {
  g_base = base;
  uint32_t cxna = ctx.r4.u32, pp = REX_LOAD_U32(ctx.r1.u32 + 116);
  if (!cxna || !pp) {
    ctx.r3.u64 = 0;
    return;
  }
  auto* mem = REX_KERNEL_MEMORY();
  if (uint32_t prev = REX_LOAD_U32(pp)) mem->SystemHeapFree(prev);
  uint32_t size = 8 + cxna * 24, q = mem->SystemHeapAlloc(size, 0x10);
  if (!q) {
    REX_STORE_U32(pp, 0);
    ctx.r3.u64 = 0x8007000E;
    return;
  }
  std::memset(base + q, 0, size);
  REX_STORE_U32(q + 0, cxna);
  REX_STORE_U32(q + 4, cxna);
  for (uint32_t i = 0; i < cxna; i++) {
    uint32_t e = q + 8 + i * 24;
    REX_STORE_U8(e + 0, 0x03);  // COMPLETE | TARGET_CONTACTED
    REX_STORE_U16(e + 2, 4);    // probes sent
    REX_STORE_U16(e + 4, 4);    // probes received
    REX_STORE_U16(e + 12, 30);  // median RTT, ms
    REX_STORE_U16(e + 14, 20);  // minimum RTT, ms
    REX_STORE_U32(e + 16, 10000000);  // up, bits/s
    REX_STORE_U32(e + 20, 10000000);  // down, bits/s
  }
  REX_STORE_U32(pp, q);
  {
    std::lock_guard lock(g_delayed_mutex);
    g_qos_done.push_back({q, 0, later(300)});
  }
  ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__NetDll_XNetQosRelease) {
  (void)base;
  if (ctx.r4.u32) REX_KERNEL_MEMORY()->SystemHeapFree(ctx.r4.u32);
  ctx.r3.u64 = 0;
}

// ---- Overlapped receive ------------------------------------------------------

// WSARecvFrom(socket r4, wsabufs r5, count r6, bytes r7, flags r8, from r9,
// fromlen r10, overlapped at r1+84). Only the first WSABUF {u32 len, u32 buf}
// is used.
REX_EXTERN(__imp__NetDll_WSARecvFrom) {
  uint32_t bufs = ctx.r5.u32, overlapped = REX_LOAD_U32(ctx.r1.u32 + 84);
  PendingRecv r{ctx.r4.u32, bufs ? REX_LOAD_U32(bufs + 4) : 0, bufs ? REX_LOAD_U32(bufs) : 0,
                ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  static thread_local uint8_t data[65536];
  sockaddr_in from{};
  int n = TryRecv(r.socket, r.buf_len, data, from);
  if (n == -2) {
    ctx.r3.u64 = uint32_t(-1);
  } else if (n > 0) {
    DeliverRecv(base, r, data, n, from, overlapped);
    ctx.r3.u64 = 0;
  } else {
    if (overlapped) {
      std::lock_guard lock(g_recvs_mutex);
      g_recvs[overlapped] = r;
      REX_STORE_U32(overlapped, 0x103);  // STATUS_PENDING
    }
    ctx.r3.u64 = uint32_t(-1);  // SOCKET_ERROR, WSA_IO_PENDING
  }
}

// WSAGetOverlappedResult(socket r4, overlapped r5, bytes r6, wait r7, flags r8):
// polls the receive that WSARecvFrom left pending. Returns TRUE when done.
REX_EXTERN(__imp__NetDll_WSAGetOverlappedResult) {
  uint32_t overlapped = ctx.r5.u32, bytes = ctx.r6.u32, flags = ctx.r8.u32;
  std::lock_guard lock(g_recvs_mutex);
  auto it = g_recvs.find(overlapped);
  if (it == g_recvs.end()) {
    bool done = overlapped && REX_LOAD_U32(overlapped) == 0;
    if (done && bytes) REX_STORE_U32(bytes, REX_LOAD_U32(overlapped + 4));
    if (done && flags) REX_STORE_U32(flags, 0);
    ctx.r3.u64 = done;
    return;
  }
  static thread_local uint8_t data[65536];
  sockaddr_in from{};
  int n = TryRecv(it->second.socket, it->second.buf_len, data, from);
  if (n == -2) {
    g_recvs.erase(it);
    ctx.r3.u64 = 0;
  } else if (n > 0) {
    DeliverRecv(base, it->second, data, n, from, overlapped);
    if (bytes) REX_STORE_U32(bytes, uint32_t(n));
    if (flags) REX_STORE_U32(flags, 0);
    g_recvs.erase(it);
    ctx.r3.u64 = 1;
  } else {
    ctx.r3.u64 = 0;
  }
}

// sub_8218A068 runs a session's completion callback (r7 == 0) through a vtable
// at *(r3+12). If the session ends before the game has stored the callback
// object, r3 or that vtable pointer is near-null; skip the call (r7 = 1) rather
// than fault.
REX_EXTERN(sub_8218A068) {
  if (ctx.r7.u32 == 0 && (ctx.r3.u32 < 0x10000 || REX_LOAD_U32(ctx.r3.u32 + 12) < 0x10000))
    ctx.r7.u64 = 1;
  __imp__sub_8218A068(ctx, base);
}
