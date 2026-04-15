// netd/veth.c — veth pair creation and container netns configuration
// Fixed: no <net/if.h>, manual ifidx(), _GNU_SOURCE for setns/CLONE_NEWNET

#define _GNU_SOURCE             // must be first — enables setns(), CLONE_NEWNET
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <linux/if.h>           // struct ifreq, IFNAMSIZ, IFF_UP — must come BEFORE net/if.h
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/if_addr.h>
#include <linux/veth.h>          // VETH_INFO_PEER
#include <linux/sockios.h>       // SIOCGIFINDEX
#include <arpa/inet.h>
#include <sched.h>               // setns(), CLONE_NEWNET (needs _GNU_SOURCE)

#include "netd.h"

#define NL_BUF 4096

// ── Helpers (same pattern as bridge.c) ────────────────────────────────────────

static unsigned int ifidx(const char *name) {
    struct ifreq ifr;
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    ioctl(fd, SIOCGIFINDEX, &ifr);
    close(fd);
    return (unsigned int)ifr.ifr_ifindex;
}

static void nl_attr(struct nlmsghdr *n, size_t max, int type,
                    const void *data, size_t len) {
    struct rtattr *rta = (struct rtattr *)((uint8_t *)n + NLMSG_ALIGN(n->nlmsg_len));
    size_t total = RTA_LENGTH(len);
    if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(total) > max) return;
    rta->rta_type = type;
    rta->rta_len  = (unsigned short)total;
    if (len) memcpy(RTA_DATA(rta), data, len);
    n->nlmsg_len = (unsigned int)(NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(total));
}

static struct rtattr *nl_tail(struct nlmsghdr *n) {
    return (struct rtattr *)((uint8_t *)n + NLMSG_ALIGN(n->nlmsg_len));
}

static int nl_talk(struct nlmsghdr *n) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -1;
    struct sockaddr_nl sa = {.nl_family = AF_NETLINK};
    bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    n->nlmsg_flags |= NLM_F_ACK;
    sendto(fd, n, n->nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa));
    char ack[256]; recv(fd, ack, sizeof(ack), 0);
    close(fd);
    return 0;
}

// ── Veth operations ────────────────────────────────────────────────────────────

// ip link add <host_if> type veth peer name <ctr_if>
int veth_create(const char *host_if, const char *ctr_if) {
    char buf[NL_BUF] = {0};
    struct nlmsghdr  *n   = (struct nlmsghdr *)buf;
    struct ifinfomsg *ifi = NLMSG_DATA(n);

    n->nlmsg_len   = NLMSG_LENGTH(sizeof(*ifi));
    n->nlmsg_type  = RTM_NEWLINK;
    n->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL;
    ifi->ifi_family = AF_UNSPEC;

    nl_attr(n, NL_BUF, IFLA_IFNAME, host_if, strlen(host_if) + 1);

    // IFLA_LINKINFO → IFLA_INFO_KIND="veth" → IFLA_INFO_DATA → VETH_INFO_PEER
    struct rtattr *li = nl_tail(n);
    nl_attr(n, NL_BUF, IFLA_LINKINFO, NULL, 0);
    nl_attr(n, NL_BUF, IFLA_INFO_KIND, "veth", strlen("veth") + 1);

    struct rtattr *li_data = nl_tail(n);
    nl_attr(n, NL_BUF, IFLA_INFO_DATA, NULL, 0);

    struct rtattr *peer = nl_tail(n);
    nl_attr(n, NL_BUF, VETH_INFO_PEER, NULL, 0);

    // Peer requires an embedded ifinfomsg before its attributes
    struct ifinfomsg peer_ifi = {.ifi_family = AF_UNSPEC};
    if (n->nlmsg_len + sizeof(peer_ifi) <= NL_BUF) {
        memcpy((uint8_t *)buf + n->nlmsg_len, &peer_ifi, sizeof(peer_ifi));
        n->nlmsg_len += sizeof(peer_ifi);
    }
    nl_attr(n, NL_BUF, IFLA_IFNAME, ctr_if, strlen(ctr_if) + 1);

    // Back-fill nested rtattr lengths
    peer->rta_len    = (unsigned short)((uint8_t *)nl_tail(n) - (uint8_t *)peer);
    li_data->rta_len = (unsigned short)((uint8_t *)nl_tail(n) - (uint8_t *)li_data);
    li->rta_len      = (unsigned short)((uint8_t *)nl_tail(n) - (uint8_t *)li);

    return nl_talk(n);
}

// ip link set <veth> master <bridge>  +  bring host-side up
int veth_add_to_bridge(const char *veth, const char *bridge) {
    char buf[NL_BUF] = {0};
    struct nlmsghdr  *n   = (struct nlmsghdr *)buf;
    struct ifinfomsg *ifi = NLMSG_DATA(n);

    n->nlmsg_len   = NLMSG_LENGTH(sizeof(*ifi));
    n->nlmsg_type  = RTM_NEWLINK;
    n->nlmsg_flags = NLM_F_REQUEST;
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index  = (int)ifidx(veth);
    ifi->ifi_flags  = IFF_UP;
    ifi->ifi_change = IFF_UP;

    uint32_t master = ifidx(bridge);
    nl_attr(n, NL_BUF, IFLA_MASTER, &master, sizeof(master));

    return nl_talk(n);
}

// ip link set <veth> netns <pid>
int veth_move_to_netns(const char *veth, pid_t pid) {
    char buf[NL_BUF] = {0};
    struct nlmsghdr  *n   = (struct nlmsghdr *)buf;
    struct ifinfomsg *ifi = NLMSG_DATA(n);

    n->nlmsg_len   = NLMSG_LENGTH(sizeof(*ifi));
    n->nlmsg_type  = RTM_NEWLINK;
    n->nlmsg_flags = NLM_F_REQUEST;
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index  = (int)ifidx(veth);

    uint32_t ns_pid = (uint32_t)pid;
    nl_attr(n, NL_BUF, IFLA_NET_NS_PID, &ns_pid, sizeof(ns_pid));

    return nl_talk(n);
}

// Enter container netns, assign IP, set default route, write resolv.conf
int veth_configure_in_netns(pid_t pid, const char *veth,
                             const char *ip_str, int prefix,
                             const char *gw_str) {
    char ns_path[64];
    snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/net", (int)pid);
    int ns_fd = open(ns_path, O_RDONLY | O_CLOEXEC);
    if (ns_fd < 0) return -1;

    pid_t child = fork();
    if (child < 0) { close(ns_fd); return -1; }

    if (child == 0) {
        setns(ns_fd, CLONE_NEWNET);
        close(ns_fd);

        // Assign IP address to veth inside container
        uint32_t ip;
        inet_pton(AF_INET, ip_str, &ip);

        char buf[NL_BUF] = {0};
        struct nlmsghdr  *n   = (struct nlmsghdr *)buf;
        struct ifaddrmsg *ifa = NLMSG_DATA(n);
        n->nlmsg_len   = NLMSG_LENGTH(sizeof(*ifa));
        n->nlmsg_type  = RTM_NEWADDR;
        n->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE;
        ifa->ifa_family    = AF_INET;
        ifa->ifa_prefixlen = (unsigned char)prefix;
        ifa->ifa_index     = ifidx(veth);
        ifa->ifa_scope     = RT_SCOPE_UNIVERSE;
        nl_attr(n, NL_BUF, IFA_LOCAL,   &ip, 4);
        nl_attr(n, NL_BUF, IFA_ADDRESS, &ip, 4);
        nl_talk(n);

        // Bring veth up inside namespace
        memset(buf, 0, NL_BUF);
        n = (struct nlmsghdr *)buf;
        struct ifinfomsg *ifi = NLMSG_DATA(n);
        n->nlmsg_len   = NLMSG_LENGTH(sizeof(*ifi));
        n->nlmsg_type  = RTM_NEWLINK;
        n->nlmsg_flags = NLM_F_REQUEST;
        ifi->ifi_family = AF_UNSPEC;
        ifi->ifi_index  = (int)ifidx(veth);
        ifi->ifi_flags  = IFF_UP;
        ifi->ifi_change = IFF_UP;
        nl_talk(n);

        // Default route via gateway
        uint32_t gw;
        inet_pton(AF_INET, gw_str, &gw);

        memset(buf, 0, NL_BUF);
        n = (struct nlmsghdr *)buf;
        struct rtmsg *rtm = NLMSG_DATA(n);
        n->nlmsg_len   = NLMSG_LENGTH(sizeof(*rtm));
        n->nlmsg_type  = RTM_NEWROUTE;
        n->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE;
        rtm->rtm_family   = AF_INET;
        rtm->rtm_table    = RT_TABLE_MAIN;
        rtm->rtm_protocol = RTPROT_STATIC;
        rtm->rtm_scope    = RT_SCOPE_UNIVERSE;
        rtm->rtm_type     = RTN_UNICAST;
        nl_attr(n, NL_BUF, RTA_GATEWAY, &gw, 4);
        nl_talk(n);

        // DNS
        int rfd = open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (rfd >= 0) { write(rfd, "nameserver 8.8.8.8\n", 19); close(rfd); }

        _exit(0);
    }

    close(ns_fd);
    int status;
    waitpid(child, &status, 0);
    return 0;
}
