/*
 * family_probe.c — 暴露“请求里的 family 参数”对结果的影响
 *
 * 起因：在 proot 里 `ip -6 route show table all` 能拿到 45 条 IPv6 路由，
 *       而 ipinfo（用 AF_UNSPEC 发一次 dump）拿到 0 条。怀疑 PRoot 的合成
 *       netlink 对 AF_UNSPEC 的请求只回 IPv4。
 *
 * 本探针对 RTM_GETADDR / RTM_GETROUTE / RTM_GETNEIGH 各发三次 dump，
 * 分别用 AF_UNSPEC / AF_INET / AF_INET6，打印条目数与返回消息的 family 分布。
 *
 * 构建与用法：
 *   aarch64-linux-android-clang -O2 -Wall -Wextra -o tools/family_probe tools/family_probe.c
 *   ./tools/family_probe
 *
 * 本机实测（VPN 开启、IPv4 175 条 / IPv6 45 条）：
 *   Termux 原生 : ROUTE  UNSPEC=fams 2:175 10:45   ← 内核按 UNSPEC 返回两族
 *                 ROUTE  INET6 =fams 10:45
 *   proot       : ROUTE  UNSPEC=fams 2:156         ← 只回 IPv4！IPv6 整族消失
 *                 ROUTE  INET6 =fams 10:45         ← 显式指定 family 才有
 *   结论：dump 这类请求要**按 family 各发一次**，不要依赖 AF_UNSPEC。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/neighbour.h>

#define MAXFAM 8
struct result {
    int entries;
    int nfam;
    int fam[MAXFAM];     /* 出现过的 family 值 */
    int fam_cnt[MAXFAM];
    int err;
};

static void tally(struct result *r, int family)
{
    for (int i = 0; i < r->nfam; i++)
        if (r->fam[i] == family) { r->fam_cnt[i]++; return; }
    if (r->nfam < MAXFAM) { r->fam[r->nfam] = family; r->fam_cnt[r->nfam] = 1; r->nfam++; }
}

static void run(int type, int family, int payload_len, struct result *r)
{
    memset(r, 0, sizeof(*r));
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) { r->err = errno; return; }

    struct { struct nlmsghdr h; char payload[64]; } req;
    memset(&req, 0, sizeof(req));
    unsigned seq = (unsigned)time(NULL) ^ (unsigned)type ^ (unsigned)family;
    req.h.nlmsg_len   = NLMSG_LENGTH(payload_len);
    req.h.nlmsg_type  = (unsigned short)type;
    req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.h.nlmsg_seq   = seq;
    *((unsigned char *)NLMSG_DATA(&req.h)) = (unsigned char)family;

    struct sockaddr_nl k = { .nl_family = AF_NETLINK };
    if (sendto(fd, &req, req.h.nlmsg_len, 0, (void *)&k, sizeof(k)) < 0) {
        r->err = errno;
        close(fd);
        return;
    }

    char buf[65536];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        int len = (int)n, stop = 0;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
             len >= (int)sizeof(struct nlmsghdr) && (int)h->nlmsg_len <= len;
             len -= (int)NLMSG_ALIGN(h->nlmsg_len),
             h = (struct nlmsghdr *)((char *)h + NLMSG_ALIGN(h->nlmsg_len))) {
            if (h->nlmsg_type == NLMSG_DONE) { stop = 1; break; }
            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = NLMSG_DATA(h);
                r->err = e->error;
                stop = 1;
                break;
            }
            if (h->nlmsg_type == RTM_NEWADDR) {
                r->entries++; tally(r, ((struct ifaddrmsg *)NLMSG_DATA(h))->ifa_family);
            } else if (h->nlmsg_type == RTM_NEWROUTE) {
                r->entries++; tally(r, ((struct rtmsg *)NLMSG_DATA(h))->rtm_family);
            } else if (h->nlmsg_type == RTM_NEWNEIGH) {
                r->entries++; tally(r, ((struct ndmsg *)NLMSG_DATA(h))->ndm_family);
            }
        }
        if (stop) break;
    }
    close(fd);
}

static const char *famname(int f)
{
    switch (f) {
    case AF_UNSPEC: return "AF_UNSPEC";
    case AF_INET:   return "AF_INET  ";
    case AF_INET6:  return "AF_INET6 ";
    default:        return "?        ";
    }
}

static void report(const char *tag, int type, int family, int payload_len)
{
    struct result r;
    run(type, family, payload_len, &r);
    printf("  %-10s family=%-9s entries=%-4d", tag, famname(family), r.entries);
    if (r.err) printf("  [err=%s]", strerror(r.err < 0 ? -r.err : r.err));
    if (r.nfam) {
        printf("  families:");
        for (int i = 0; i < r.nfam; i++) printf(" %d:%d", r.fam[i], r.fam_cnt[i]);
    }
    putchar('\n');
}

int main(void)
{
    unsigned fake = (unsigned)getuid(), real = 0;
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "Uid:\t%u", &real) == 1) break;
        fclose(f);
    }
    printf("== family_probe ==  getuid=%u real=%u in_proot=%s\n",
           fake, real, (fake != real) ? "yes" : "no");

    int fa[3] = { AF_UNSPEC, AF_INET, AF_INET6 };
    puts("  --- RTM_GETADDR ---");
    for (int i = 0; i < 3; i++) report("ADDR", RTM_GETADDR, fa[i], (int)sizeof(struct rtgenmsg));
    puts("  --- RTM_GETROUTE ---");
    for (int i = 0; i < 3; i++) report("ROUTE", RTM_GETROUTE, fa[i], (int)sizeof(struct rtgenmsg));
    puts("  --- RTM_GETNEIGH ---");
    for (int i = 0; i < 3; i++) report("NEIGH", RTM_GETNEIGH, fa[i], (int)sizeof(struct ndmsg));
    return 0;
}
