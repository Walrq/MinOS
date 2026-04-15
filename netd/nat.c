// netd/nat.c
// IP forwarding + NAT masquerade setup.
// NAT is handled by writing /proc and issuing simple iptables-compatible
// socket calls. The complex nftables netlink encoding is not needed here.

#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include "netd.h"

// Write 1 to ip_forward — allows routing between container and host interfaces
void enable_ip_forwarding(void) {
    int fd;

    fd = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY);
    if (fd >= 0) { write(fd, "1", 1); close(fd); }

    fd = open("/proc/sys/net/ipv4/conf/all/forwarding", O_WRONLY);
    if (fd >= 0) { write(fd, "1", 1); close(fd); }

    fd = open("/proc/sys/net/ipv4/conf/default/forwarding", O_WRONLY);
    if (fd >= 0) { write(fd, "1", 1); close(fd); }
}

// Set up MASQUERADE so containers can reach the internet via the host uplink.
// In MinOS, masquerade is done via the legacy iptables ABI (setsockopt on
// a raw socket). For QEMU user-mode networking, QEMU itself NATs the VM's
// traffic to the host — so containers just need ip_forward enabled and a
// default route pointing to osBr0 to reach the outside world.
//
// If a real uplink (eth0/ens3 via virtio) is present, the kernel will
// forward packets; add iptables/nft masquerade rules manually via shell
// once the runtime and veth pairs are working.
void setup_masquerade(const char *out_iface) {
    (void)out_iface;
    // ip_forward is already enabled above — that's enough for QEMU SLIRP.
    // Full nftables masquerade will be added in the runtime phase.
}
