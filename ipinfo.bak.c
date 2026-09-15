#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

struct iface {
    char  name[IFNAMSIZ];
    int   index;
    int   mtu;
    short flags;
    int   has_v4;
    struct in_addr v4, mask, brd;
    struct { struct in6_addr a; int pfx; } v6[8];
    int   v6_n;
};

static struct iface ifs[64];
static int ifs_n = 0;

/* 按 ifindex 去重取条目；没有就新建 */
static struct iface *slot(int index, const char *name)
{
    for (int i = 0; i < ifs_n; i++)
        if (ifs[i].index == index) return &ifs[i];
    if (ifs_n >= 64) return NULL;
    struct iface *p = &ifs[ifs_n++];
    memset(p, 0, sizeof(*p));
    p->index = index;
    snprintf(p->name, IFNAMSIZ, "%s", name);
    return p;
}

/* ---------- netlink RTM_GETADDR（不 bind） ---------- */
static void nl_getaddrs(void)
{
    int fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
    if (fd < 0) return;

    struct {
        struct nlmsghdr  h;
        struct ifaddrmsg m;
    } req;
    memset(&req, 0, sizeof(req));
    req.h.nlmsg_len   = sizeof(req);
    req.h.nlmsg_type  = RTM_GETADDR;
    req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.h.nlmsg_seq   = 1;
    req.m.ifa_family  = AF_UNSPEC;

    struct sockaddr_nl k = { .nl_family = AF_NETLINK };
    if (sendto(fd, &req, sizeof(req), 0, (void *)&k, sizeof(k)) < 0) {
        close(fd);
        return;
    }

    char buf[16384];
    int n, done = 0;
    while (!done && (n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        struct nlmsghdr *h = (struct nlmsghdr *)buf;
        for (; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE ||
                h->nlmsg_type == NLMSG_ERROR) { done = 1; break; }
            if (h->nlmsg_type != RTM_NEWADDR) continue;

            struct ifaddrmsg *m = NLMSG_DATA(h);
            int len = h->nlmsg_len - NLMSG_LENGTH(sizeof(*m));
            struct rtattr *r = IFA_RTA(m);

            char name[IFNAMSIZ] = "?";
            if (if_indextoname(m->ifa_index, name) == NULL)
                snprintf(name, IFNAMSIZ, "if%d", m->ifa_index);

            struct iface *p = slot(m->ifa_index, name);
            if (!p) continue;

            for (; RTA_OK(r, len); r = RTA_NEXT(r, len)) {
                if (r->rta_type != IFA_ADDRESS) continue;
                if (m->ifa_family == AF_INET && RTA_PAYLOAD(r) == 4) {
                    memcpy(&p->v4, RTA_DATA(r), 4);
                    p->has_v4 = 1;
                } else if (m->ifa_family == AF_INET6 &&
                           RTA_PAYLOAD(r) == 16 && p->v6_n < 8) {
                    memcpy(&p->v6[p->v6_n].a, RTA_DATA(r), 16);
                    p->v6[p->v6_n].pfx = m->ifa_prefixlen;
                    p->v6_n++;
                }
            }
        }
    }
    close(fd);
}

/* ---------- ioctl 补 flags / mtu / IPv4 netmask & broadcast ---------- */
static void ioctl_fill(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;

    for (int i = 0; i < ifs_n; i++) {
        struct iface *p = &ifs[i];
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, IFNAMSIZ, "%s", p->name);

        if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) p->flags = ifr.ifr_flags;
        if (ioctl(fd, SIOCGIFMTU,   &ifr) == 0) p->mtu   = ifr.ifr_mtu;

        /* netlink 漏了 IPv4 的话，这里补 */
        if (!p->has_v4 && ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
            struct sockaddr_in *s = (void *)&ifr.ifr_addr;
            p->v4 = s->sin_addr;
            p->has_v4 = 1;
        }
        if (p->has_v4) {
            if (ioctl(fd, SIOCGIFNETMASK, &ifr) == 0) {
                struct sockaddr_in *s = (void *)&ifr.ifr_netmask;
                p->mask = s->sin_addr;
            }
            if (ioctl(fd, SIOCGIFBRDADDR, &ifr) == 0) {
                struct sockaddr_in *s = (void *)&ifr.ifr_broadaddr;
                p->brd = s->sin_addr;
            }
        }
    }
    close(fd);
}

/* ---------- 展示辅助 ---------- */
static void flags_str(short f, char *b, size_t n)
{
    b[0] = 0;
    #define A(bit, s) do { if (f & (bit)) { \
        if (b[0]) strncat(b, ",", n - strlen(b) - 1); \
        strncat(b, s, n - strlen(b) - 1); } } while (0)
    A(IFF_UP, "UP");
    A(IFF_RUNNING, "RUNNING");
    A(IFF_LOOPBACK, "LOOPBACK");
    A(IFF_BROADCAST, "BROADCAST");
    A(IFF_MULTICAST, "MULTICAST");
    A(IFF_POINTOPOINT, "P2P");
    #undef A
}

static const char *v6_kind(const struct in6_addr *a)
{
    if (IN6_IS_ADDR_LOOPBACK(a))  return "loopback";
    if (IN6_IS_ADDR_LINKLOCAL(a)) return "link-local";
    if (IN6_IS_ADDR_SITELOCAL(a)) return "site-local";
    if (IN6_IS_ADDR_MULTICAST(a)) return "multicast";
    if (IN6_IS_ADDR_V4MAPPED(a))  return "v4-mapped";
    return "global";
}

static void print_one(struct iface *p, int w4, int w6)
{
    char fs[64];
    flags_str(p->flags, fs, sizeof(fs));

    printf("%s  (index %d, mtu %d)\n", p->name, p->index, p->mtu);
    printf("  flags: %s\n", fs[0] ? fs : "-");

    if (w4 && p->has_v4) {
        char a[INET_ADDRSTRLEN], m[INET_ADDRSTRLEN], b[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &p->v4,   a, sizeof(a));
        inet_ntop(AF_INET, &p->mask, m, sizeof(m));
        inet_ntop(AF_INET, &p->brd,  b, sizeof(b));
        printf("  IPv4: %s  mask %s  brd %s\n", a, m, b);
    }
    if (w6) {
        for (int i = 0; i < p->v6_n; i++) {
            char a[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &p->v6[i].a, a, sizeof(a));
            printf("  IPv6: %s/%d  (%s)\n",
                   a, p->v6[i].pfx, v6_kind(&p->v6[i].a));
        }
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    const char *filter = NULL;
    int w4 = 1, w6 = 1, only_up = 0, opt;

    while ((opt = getopt(argc, argv, "i:46u")) != -1) {
        switch (opt) {
        case 'i': filter  = optarg; break;
        case '4': w6      = 0;      break;
        case '6': w4      = 0;      break;
        case 'u': only_up = 1;      break;
        default:
            fprintf(stderr,
                "usage: %s [-i iface] [-4] [-6] [-u]\n"
                "  -i iface   只看指定接口\n"
                "  -4         只看 IPv4\n"
                "  -6         只看 IPv6\n"
                "  -u         只看 UP 的接口\n", argv[0]);
            return 1;
        }
    }

    nl_getaddrs();
    ioctl_fill();

    int shown = 0;
    for (int i = 0; i < ifs_n; i++) {
        struct iface *p = &ifs[i];
        if (filter && strcmp(p->name, filter)) continue;
        if (only_up && !(p->flags & IFF_UP)) continue;

        int has_v4 = p->has_v4 && w4;
        int has_v6 = p->v6_n > 0 && w6;
        if (!has_v4 && !has_v6) continue;

        print_one(p, w4, w6);
        shown++;
    }

    if (!shown) {
        fprintf(stderr, "no matching interface\n");
        return 2;
    }
    return 0;
}
