/*
 * neigh_probe.c — 验证 RTM_GETNEIGH（ARP/NDP 邻居表）到底能不能拿到
 *
 * 为什么要单独写这个探针：
 *   ipinfo 的 -n/-N 在 proot 里得到 0 条邻居，但这可能是
 *     (a) PRoot 的合成 netlink 不实现邻居表，或
 *     (b) 探针本身有问题 / 内核真的没有条目
 *   靠一个“0 条”的结论分不清这三者。所以这里带一个**对照组**：
 *     RTM_GETADDR 在同一进程、同一环境、同一时刻也跑一遍。
 *   若 ADDR 有数据而 NEIGH 没有，说明探针与环境都是好的，缺的只是 NEIGH 这一能力。
 *
 * 同时打印每个 socket 的真实 SO_DOMAIN：PRoot 会把 AF_NETLINK(16) 换成 AF_UNIX(1)，
 * 用 getsockopt 就能看到（PRoot 不伪造它）。
 *
 * 构建与用法：
 *   aarch64-linux-android-clang -O2 -Wall -Wextra -o tools/neigh_probe tools/neigh_probe.c
 *   ./tools/neigh_probe                                  # Termux 原生
 *   cp tools/neigh_probe $PREFIX/var/lib/proot-distro/containers/ubuntu/rootfs/tmp/
 *   proot-distro login ubuntu --user he -- /tmp/neigh_probe   # 容器内
 *
 * 本机实测（2026-09，Android 15 / kernel 5.15.167 / proot 5.1.107.92）：
 *   Termux 原生: ADDR entries=13      NEIGH entries=12 (v4) / 37 (v6)
 *   proot      : ADDR entries=13(对照) NEIGH entries=0  （SO_DOMAIN 16 -> 1）
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

struct result {
    int domain;        /* SO_DOMAIN：16=AF_NETLINK，1=AF_UNIX（被 PRoot 换掉） */
    int sendto_errno;
    int nl_err;        /* NLMSG_ERROR 里的 error 值 */
    int msgs;
    int entries;       /* RTM_NEW* 条数 */
    int done;
};

static void run_one(int type, int family, int payload_len, struct result *r)
{
    memset(r, 0, sizeof(*r));
    r->domain = -1;

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) { r->sendto_errno = errno; return; }

    socklen_t dl = sizeof(r->domain);
    getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &r->domain, &dl);

    struct { struct nlmsghdr h; char payload[64]; } req;
    memset(&req, 0, sizeof(req));
    unsigned seq = (unsigned)time(NULL) ^ (unsigned)type ^ (unsigned)family;
    req.h.nlmsg_len    = NLMSG_LENGTH(payload_len);
    req.h.nlmsg_type   = (unsigned short)type;
    req.h.nlmsg_flags  = NLM_F_REQUEST | NLM_F_DUMP;
    req.h.nlmsg_seq    = seq;
    *((unsigned char *)NLMSG_DATA(&req.h)) = (unsigned char)family;

    struct sockaddr_nl k = { .nl_family = AF_NETLINK };
    if (sendto(fd, &req, req.h.nlmsg_len, 0, (void *)&k, sizeof(k)) < 0) {
        r->sendto_errno = errno;
        close(fd);
        return;
    }

    char buf[65536];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        int len = (int)n, stop = 0;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
             len >= (int)sizeof(struct nlmsghdr) &&
                 (int)h->nlmsg_len <= len;
             len -= (int)NLMSG_ALIGN(h->nlmsg_len),
             h = (struct nlmsghdr *)((char *)h + NLMSG_ALIGN(h->nlmsg_len))) {
            r->msgs++;
            if (h->nlmsg_type == NLMSG_DONE) { r->done = 1; stop = 1; break; }
            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = NLMSG_DATA(h);
                r->nl_err = e->error;
                stop = 1;
                break;
            }
            if (h->nlmsg_type == RTM_NEWADDR || h->nlmsg_type == RTM_NEWNEIGH)
                r->entries++;
        }
        if (stop) break;
    }
    close(fd);
}

static void show(const char *tag, int type, int family, int payload, const char *famname)
{
    struct result r;
    run_one(type, family, payload, &r);

    char detail[160];
    if (r.sendto_errno)
        snprintf(detail, sizeof(detail), "sendto 失败: %s", strerror(r.sendto_errno));
    else if (r.nl_err)
        snprintf(detail, sizeof(detail), "NLMSG_ERROR: %s", strerror(-r.nl_err));
    else
        snprintf(detail, sizeof(detail), "DONE=%s entries=%d (报文 %d 个)",
                 r.done ? "yes" : "no", r.entries, r.msgs);

    printf("    %-6s family=%-9s SO_DOMAIN=%-2d  %s\n",
           tag, famname, r.domain, detail);
}

int main(void)
{
    char ctx[128] = "";
    FILE *f = fopen("/proc/self/attr/current", "r");
    if (f) { if (fgets(ctx, sizeof(ctx), f)) { ctx[strcspn(ctx, "\n")] = 0; } fclose(f); }

    unsigned fake = (unsigned)getuid();
    unsigned real = 0;
    f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "Uid:\t%u", &real) == 1) break;
        fclose(f);
    }

    printf("== neigh_probe ==\n");
    printf("  getuid()=%u  /proc 里的真 uid=%u  => %s\n", fake, real,
           (fake != real) ? "在 PRoot 容器里" : "不在 PRoot 里");
    printf("  selinux=%s\n", ctx);

    puts("  [对照组] RTM_GETADDR —— 用来证明“探针本身工作正常”");
    show("ADDR", RTM_GETADDR, AF_UNSPEC, (int)sizeof(struct rtgenmsg), "AF_UNSPEC");

    puts("  [待测]   RTM_GETNEIGH —— ARP/NDP 邻居表");
    show("NEIGH", RTM_GETNEIGH, AF_UNSPEC, (int)sizeof(struct ndmsg), "AF_UNSPEC");
    show("NEIGH", RTM_GETNEIGH, AF_INET,   (int)sizeof(struct ndmsg), "AF_INET");
    show("NEIGH", RTM_GETNEIGH, AF_INET6,  (int)sizeof(struct ndmsg), "AF_INET6");

    puts("  判读：若对照组 ADDR 有条目、而 NEIGH 全为 entries=0，"
         "则说明环境与探针都正常，缺的只是邻居表这一能力。");
    return 0;
}
