// netd/netd.c
// Network daemon for MinOS.
// Runs once at boot (spawned by supervisor from services.d/netd.conf).
//
// What it does:
//   1. Creates the host bridge: osBr0 at 10.0.0.1/24
//   2. Enables IP forwarding so container packets can be routed
//   3. Sets up NAT (masquerade) for container outbound internet access
//   4. Exits cleanly — veth pairs are created per-container by the runtime

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include "netd.h"

// Read bridge config from /net.conf
// Format:
//   bridge_name = osBr0
//   bridge_ip   = 10.0.0.1
//   bridge_prefix = 24
//   out_iface   = eth0
static void parse_net_conf(char *bridge_name, char *bridge_ip,
                            int  *bridge_prefix, char *out_iface) {
    // Set defaults
    strncpy(bridge_name,  "osBr0",    63);
    strncpy(bridge_ip,    "10.0.0.1", 63);
    *bridge_prefix = 24;
    strncpy(out_iface,    "eth0",     63);

    FILE *f = fopen("/net.conf", "r");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[64], val[64];
        if (sscanf(line, " %63[^=] = %63s", key, val) != 2) continue;

        if (!strcmp(key, "bridge_name"))   strncpy(bridge_name, val, 63);
        if (!strcmp(key, "bridge_ip"))     strncpy(bridge_ip,   val, 63);
        if (!strcmp(key, "bridge_prefix")) *bridge_prefix = atoi(val);
        if (!strcmp(key, "out_iface"))     strncpy(out_iface,   val, 63);
    }
    fclose(f);
}

int main(void) {
    char bridge_name[64], bridge_ip[64], out_iface[64];
    int  bridge_prefix;

    parse_net_conf(bridge_name, bridge_ip, &bridge_prefix, out_iface);

    // ── Step 1: Create host bridge ─────────────────────────────────────────
    // ip link add osBr0 type bridge
    // ip addr add 10.0.0.1/24 dev osBr0
    // ip link set osBr0 up
    setup_bridge(bridge_name, bridge_ip, bridge_prefix);

    // ── Step 2: Enable IP forwarding ──────────────────────────────────────
    // echo 1 > /proc/sys/net/ipv4/ip_forward
    enable_ip_forwarding();

    // ── Step 3: Set up NAT (masquerade) ──────────────────────────────────
    // nft add table ip nat
    // nft add chain ip nat POSTROUTING { type nat hook postrouting priority 100; }
    // nft add rule  ip nat POSTROUTING oif eth0 masquerade
    setup_masquerade(out_iface);

    // Netd exits cleanly after setup.
    // Per-container veth pairs are created by runtime/spawn.c on container start.
    return 0;
}
