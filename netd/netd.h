#pragma once
#include <sys/types.h>

// ── Bridge ────────────────────────────────────────────────────────────────────
// Create bridge `name`, assign `ip/prefix_len`, bring it up.
int setup_bridge(const char *name, const char *ip, int prefix_len);

// ── NAT / Forwarding ──────────────────────────────────────────────────────────
// Write 1 to /proc/sys/net/ipv4/ip_forward
void enable_ip_forwarding(void);

// Add nftables MASQUERADE rule for containers leaving through `out_iface`
void setup_masquerade(const char *out_iface);

// ── Veth Pairs ────────────────────────────────────────────────────────────────
// Create veth pair: `host_if` stays on host, `ctr_if` moves into container ns.
int veth_create(const char *host_if, const char *ctr_if);

// Attach `veth` to bridge `bridge`
int veth_add_to_bridge(const char *veth, const char *bridge);

// Move `veth` into the network namespace of process `pid`
int veth_move_to_netns(const char *veth, pid_t pid);

// Enter container netns and configure: assign IP, set default route
int veth_configure_in_netns(pid_t pid, const char *veth,
                             const char *ip, int prefix,
                             const char *gateway);
