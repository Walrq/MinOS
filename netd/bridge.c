// netd/bridge.c — create osBr0, assign 10.0.0.1/24, bring it up
// Uses RTNETLINK only. No <net/if.h> (conflicts with <linux/if.h>).

#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if.h>           // struct ifreq, IFNAMSIZ, IFF_UP — must come BEFORE net/if.h
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/if_addr.h>
#include <linux/sockios.h>      // SIOCGIFINDEX
#include <arpa/inet.h>

#include "netd.h"

#define NL_BUF 4096

// ── Helpers ────────────────────────────────────────────────────────────────────

// Get interface index by name using ioctl (avoids <net/if.h> conflict)
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

// ── Bridge operations ──────────────────────────────────────────────────────────

static int bridge_create(const char *name) {
    char buf[NL_BUF] = {0};
    struct nlmsghdr  *n   = (struct nlmsghdr *)buf;
    struct ifinfomsg *ifi = NLMSG_DATA(n);

    n->nlmsg_len   = NLMSG_LENGTH(sizeof(*ifi));
    n->nlmsg_type  = RTM_NEWLINK;
    n->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL;
    ifi->ifi_family = AF_UNSPEC;

    nl_attr(n, NL_BUF, IFLA_IFNAME, name, strlen(name) + 1);

    struct rtattr *li = nl_tail(n);
    nl_attr(n, NL_BUF, IFLA_LINKINFO, NULL, 0);
    nl_attr(n, NL_BUF, IFLA_INFO_KIND, "bridge", strlen("bridge") + 1);
    li->rta_len = (unsigned short)((uint8_t *)nl_tail(n) - (uint8_t *)li);

    return nl_talk(n);
}

static int if_set_up(const char *name) {
    char buf[NL_BUF] = {0};
    struct nlmsghdr  *n   = (struct nlmsghdr *)buf;
    struct ifinfomsg *ifi = NLMSG_DATA(n);

    n->nlmsg_len   = NLMSG_LENGTH(sizeof(*ifi));
    n->nlmsg_type  = RTM_NEWLINK;
    n->nlmsg_flags = NLM_F_REQUEST;
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index  = (int)ifidx(name);
    ifi->ifi_flags  = IFF_UP;
    ifi->ifi_change = IFF_UP;

    return nl_talk(n);
}

static int if_add_addr(const char *name, const char *ip_str, int prefix) {
    uint32_t ip;
    if (inet_pton(AF_INET, ip_str, &ip) != 1) return -1;

    char buf[NL_BUF] = {0};
    struct nlmsghdr  *n   = (struct nlmsghdr *)buf;
    struct ifaddrmsg *ifa = NLMSG_DATA(n);

    n->nlmsg_len   = NLMSG_LENGTH(sizeof(*ifa));
    n->nlmsg_type  = RTM_NEWADDR;
    n->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE;
    ifa->ifa_family    = AF_INET;
    ifa->ifa_prefixlen = (unsigned char)prefix;
    ifa->ifa_index     = ifidx(name);
    ifa->ifa_scope     = RT_SCOPE_UNIVERSE;

    nl_attr(n, NL_BUF, IFA_LOCAL,   &ip, 4);
    nl_attr(n, NL_BUF, IFA_ADDRESS, &ip, 4);

    return nl_talk(n);
}

int setup_bridge(const char *name, const char *ip, int prefix_len) {
    bridge_create(name);
    if_add_addr(name, ip, prefix_len);
    if_set_up(name);
    return 0;
}
