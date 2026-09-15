/*
 * ipinfo - 在 Android（Termux / PRoot-Distro）下枚举本机网络信息
 *
 * 背景 / 实测能力（uid=u0_aXXX, untrusted_app_27）:
 *
 *   Termux 原生:
 *     /proc/net、/sys/class/net   -> EACCES
 *     netlink RTM_GETLINK (dump)  -> EACCES（sendto 直接 EACCES，仅此一种请求被拒）
 *     netlink RTM_GETADDR/ROUTE   -> OK，回复中带 IFA_FLAGS/IFA_CACHEINFO/IFA_BROADCAST
 *     SIOCGIFNAME / if_indextoname-> OK
 *     ioctl SIOCGIF*              -> OK（前提是 ifr_name 是真实名字）
 *     SOCK_DGRAM 与 SOCK_RAW 行为完全一致（障碍与 socket 类型无关）
 *
 *   PRoot-Distro（本机的 proot 带自定义补丁）:
 *     socket(AF_NETLINK) 会被静默换成 AF_UNIX！用 getsockopt(SO_DOMAIN) 可看到
 *     domain 从 16 变成 1，proot 日志：
 *       "AF_NETLINK %s denied by host (%s); enabling AF_UNIX fallback for sandbox helpers"
 *     所以所谓“PRoot 下 netlink 可用”是假象：回复由 PRoot 合成，实测会丢掉
 *     IFA_FLAGS/IFA_CACHEINFO/IFA_BROADCAST、ifa_flags 恒为 0x80、消息顺序反转，
 *     并会把 ifindex 27 的 txqlen 报成 1000（adb ifconfig / ioctl 均为 3000）。
 *     但地址/前缀/路由表与真内核结果一致，仍可用；mtu/txqlen 一律改用 ioctl 取真值。
 *     if_indextoname / SIOCGIFNAME -> EACCES（PRoot 拦掉）
 *     netlink 的 IFLA_ADDRESS      -> 长度 0（Android 对第三方应用隐藏 MAC）
 *
 * 三个环境的真实 uid / SELinux 上下文 / capabilities 完全相同
 * （u:r:untrusted_app_27:s0:c104,c258,c512,c768, CapEff 0），
 * 所以 Termux 原生下 RTM_GETLINK 被拒的具体原因不用瞎猜，
 * 也不要归错于 uid / capability / socket 类型。
 *
 * 结论：名字解析走 netlink ∪ ioctl 双路兜底：
 *   1) netlink RTM_GETLINK 提供 name/index/flags/mtu/txqlen/operstate/MAC
 *   2) 名字仍未知时用 if_indextoname() / SIOCGIFNAME 反查（Termux 路径）
 *   3) 再用 ioctl 补齐 mtu/flags/IPv4/netmask/broadcast
 *   4) netmask 永远可以由 prefixlen 算出，不依赖 ioctl
 * netlink 失败时默认静默降级，-v 才打印原因（包括上面那个 AF_UNIX 伪装）。
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/if_addr.h>

#define MAXIF    64
#define MAXV6    16
#define MAXRT    512
#define NL_BUF   (64 * 1024)

/* 自带的 NLMSG_OK/NEXT：内核头里的宏会触发 -Wsign-compare 噪音 */
#define NLOK(nlh, len) ((len) >= (int)sizeof(struct nlmsghdr) &&          \
                        (nlh)->nlmsg_len >= sizeof(struct nlmsghdr) &&    \
                        (int)(nlh)->nlmsg_len <= (len))
#define NLNEXT(nlh, len) ((len) -= (int)NLMSG_ALIGN((nlh)->nlmsg_len),    \
                          (struct nlmsghdr *)((char *)(nlh) +             \
                                              NLMSG_ALIGN((nlh)->nlmsg_len)))

struct v6addr {
    struct in6_addr a;
    int pfx;
    int scope;
    int temporary;      /* IFA_F_TEMPORARY：隐私扩展生成的随机 IID */
    int tentative;
    int deprecated;
};

struct iface {
    char     name[IFNAMSIZ];
    int      name_real;          /* 名字是真名（netlink/if_indextoname/SIOCGIFNAME）
                                    而不是 "if%d" 占位符 */
    int      link_ok;            /* netlink RTM_GETLINK 已给出可用的 32 位 flags */
    int      index;
    int      mtu;
    int      txqlen;
    unsigned int flags;          /* IFF_* */
    int      operstate;          /* -1=未知 0=unknown 1=notpresent ... 6=up */
    int      has_mac;
    int      mac_len;            /* Android 下通常为 0，表示被隐藏 */
    unsigned char mac[32];

    int      has_v4;
    int      v4_pfx;
    struct in_addr v4, mask, brd;

    struct v6addr v6[MAXV6];
    int      v6_n;
};

struct route {
    int  family;
    int  oif;
    int  table;
    int  metric;
    int  dst_len;                /* 0 == default */
    unsigned char dst[16];
    int  has_gw;
    unsigned char gw[16];
};

static struct iface ifs[MAXIF];
static int  ifs_n = 0;

static struct route rts[MAXRT];
static int  rts_n = 0;

struct opts {
    const char *filter;
    int only_up, all, w4, w6, wmac, routes, all_routes, json, summary, verbose, help;
};
static struct opts opt = { NULL, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0 };

/* ---------------- 基础工具 ---------------- */

static void vlog(const char *fmt, ...)
{
    if (!opt.verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("ipinfo: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* 按 ifindex 取条目；name 非空且当前名字还是占位符时补写 */
static struct iface *slot(int index, const char *name)
{
    for (int i = 0; i < ifs_n; i++)
        if (ifs[i].index == index) {
            if (name && (ifs[i].name[0] == 0 || ifs[i].name[0] == '?')) {
                snprintf(ifs[i].name, IFNAMSIZ, "%s", name);
                ifs[i].name_real = 1;
            }
            return &ifs[i];
        }
    if (ifs_n >= MAXIF) {
        vlog("too many interfaces (>%d), ignoring index %d", MAXIF, index);
        return NULL;
    }
    struct iface *p = &ifs[ifs_n++];
    memset(p, 0, sizeof(*p));
    p->index     = index;
    p->operstate = -1;
    snprintf(p->name, IFNAMSIZ, "%s", name && name[0] ? name : "?");
    p->name_real = (name && name[0]) ? 1 : 0;
    return p;
}

static struct iface *by_index(int index)
{
    for (int i = 0; i < ifs_n; i++)
        if (ifs[i].index == index) return &ifs[i];
    return NULL;
}

static int name_known(const struct iface *p)
{
    return p->name_real;
}

/* ---------------- 通用 netlink dump ---------------- */

typedef void (*nl_cb)(struct nlmsghdr *h, void *ctx);

/* 返回 0 成功；-errno 失败 */
static int nl_dump(int type, int family, nl_cb cb, void *ctx)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -errno;

    /* PRoot（本机这个带补丁的构建）在宿主拒绝 AF_NETLINK 时会静默把 socket
     * 换成 AF_UNIX（日志："AF_NETLINK ... denied by host; enabling AF_UNIX
     * fallback for sandbox helpers"），此时“netlink 成功”其实是 PRoot 合成
     * 的回复（实测会缺 IFA_FLAGS/IFA_CACHEINFO/IFA_BROADCAST）。
     * -v 时把这一真相报出来，避免把仿真数据当成内核数据。*/
    if (opt.verbose) {
        int dom = -1;
        socklen_t dl = sizeof(dom);
        if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &dom, &dl) == 0 &&
            dom != AF_NETLINK)
            vlog("socket(AF_NETLINK) is actually domain %d "
                 "(PRoot AF_UNIX fallback?) - netlink replies may be emulated", dom);
    }

    int rcvbuf = 1 << 20;   /* 大路由表时降低被截断的概率 */
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct {
        struct nlmsghdr h;
        struct rtgenmsg g;
    } req;
    memset(&req, 0, sizeof(req));
    unsigned seq = (unsigned)time(NULL) ^ (unsigned)type;
    req.h.nlmsg_len    = NLMSG_LENGTH(sizeof(req.g));   /* 不含尾部对齐填充 */
    req.h.nlmsg_type   = (unsigned short)type;
    req.h.nlmsg_flags  = NLM_F_REQUEST | NLM_F_DUMP;
    req.h.nlmsg_seq    = seq;
    req.g.rtgen_family = (unsigned char)family;

    struct sockaddr_nl k = { .nl_family = AF_NETLINK };
    if (sendto(fd, &req, req.h.nlmsg_len, 0, (void *)&k, sizeof(k)) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }

    char buf[NL_BUF];
    int  rc = 0, done = 0, intr = 0;
    while (!done) {
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_TRUNC);
        if (n == 0) break;                /* 对端关闭：防御性退出，避免死循环 */
        if (n < 0) {
            if (errno == EINTR) continue;
            rc = -errno;
            break;
        }
        if (n > (ssize_t)sizeof(buf)) {   /* 被截断：只能尽力而为 */
            vlog("netlink type %d reply truncated (%zd > %zu)", type, n, sizeof(buf));
            n = sizeof(buf);
        }
        int len = (int)n;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
             NLOK(h, len); h = NLNEXT(h, len)) {
            /* 只校验 seq：内核回复里的 nlmsg_pid 是本 socket 自动绑定的端口号
             * （实测等于本进程 pid），并非文档所说的 0，用 pid!=0 过滤会把
             * 包括 NLMSG_DONE 在内的所有消息全部丢掉，导致 recv 永久阻塞。*/
            if (h->nlmsg_seq != seq) continue;
            if (h->nlmsg_type == NLMSG_DONE) {
                if (h->nlmsg_flags & NLM_F_DUMP_INTR) intr = 1;
                done = 1;
                break;
            }
            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = NLMSG_DATA(h);
                if (e->error) rc = e->error;   /* 负数 */
                done = 1;
                break;
            }
            cb(h, ctx);   /* 回调自行按响应类型过滤 */
        }
    }
    close(fd);
    if (intr) vlog("netlink type %d dump interrupted, result may be partial", type);
    return rc;
}

/* ---------------- RTM_GETLINK：名字/flags/mtu/MAC ---------------- */

static void cb_link(struct nlmsghdr *h, void *ctx)
{
    (void)ctx;
    if (h->nlmsg_type != RTM_NEWLINK) return;
    struct ifinfomsg *m = NLMSG_DATA(h);
    int len = (int)h->nlmsg_len - (int)NLMSG_LENGTH(sizeof(*m));

    struct iface *p = slot(m->ifi_index, NULL);
    if (!p) return;
    p->link_ok = 1;                      /* 链路层属性可信，ioctl 不必再问 */
    p->flags = (unsigned int)m->ifi_flags;

    for (struct rtattr *r = IFLA_RTA(m); RTA_OK(r, len); r = RTA_NEXT(r, len)) {
        switch (r->rta_type) {
        case IFLA_IFNAME:
            snprintf(p->name, IFNAMSIZ, "%s", (char *)RTA_DATA(r));
            p->name_real = 1;
            break;
        case IFLA_MTU:
            if (RTA_PAYLOAD(r) >= 4) memcpy(&p->mtu, RTA_DATA(r), 4);
            break;
        case IFLA_TXQLEN:
            if (RTA_PAYLOAD(r) >= 4) memcpy(&p->txqlen, RTA_DATA(r), 4);
            break;
        case IFLA_OPERSTATE:
            if (RTA_PAYLOAD(r) >= 1) p->operstate = *(unsigned char *)RTA_DATA(r);
            break;
        case IFLA_ADDRESS:
            p->has_mac = 1;
            p->mac_len = (int)RTA_PAYLOAD(r);
            /* Android 第三方应用这里通常拿到 0 长度，或直接不返回该属性 */
            if (p->mac_len > (int)sizeof(p->mac)) p->mac_len = (int)sizeof(p->mac);
            if (p->mac_len > 0) memcpy(p->mac, RTA_DATA(r), (size_t)p->mac_len);
            break;
        default:
            break;
        }
    }
}

/* ---------------- RTM_GETADDR：地址 ---------------- */

static void add_v6(struct iface *p, const struct in6_addr *a, int pfx,
                   int scope, unsigned ifa_flags)
{
    for (int i = 0; i < p->v6_n; i++)
        if (IN6_ARE_ADDR_EQUAL(&p->v6[i].a, a)) return;   /* 去重 */
    if (p->v6_n >= MAXV6) return;
    struct v6addr *v = &p->v6[p->v6_n++];
    v->a          = *a;
    v->pfx        = pfx;
    v->scope      = scope;
    v->tentative  = (ifa_flags & IFA_F_TENTATIVE) != 0;
    v->deprecated = (ifa_flags & IFA_F_DEPRECATED) != 0;
    /* IFA_F_TEMPORARY 与 IFA_F_SECONDARY 同一位(0x01)，IPv6 下即隐私扩展地址 */
    v->temporary  = (ifa_flags & IFA_F_TEMPORARY) != 0;
}

static void cb_addr(struct nlmsghdr *h, void *ctx)
{
    (void)ctx;
    if (h->nlmsg_type != RTM_NEWADDR) return;
    struct ifaddrmsg *m = NLMSG_DATA(h);
    int len = (int)h->nlmsg_len - (int)NLMSG_LENGTH(sizeof(*m));

    struct iface *p = slot(m->ifa_index, NULL);   /* 名字由 cb_link/兜底提供 */
    if (!p) return;

    unsigned ifa_flags = m->ifa_flags;
    struct in_addr local = {0}, addr = {0}, brd = {0};
    int have_local = 0, have_addr = 0, have_brd = 0;

    for (struct rtattr *r = IFA_RTA(m); RTA_OK(r, len); r = RTA_NEXT(r, len)) {
        switch (r->rta_type) {
        case IFA_LOCAL:
            if (m->ifa_family == AF_INET && RTA_PAYLOAD(r) == 4) {
                memcpy(&local, RTA_DATA(r), 4); have_local = 1;
            }
            break;
        case IFA_ADDRESS:
            if (m->ifa_family == AF_INET && RTA_PAYLOAD(r) == 4) {
                memcpy(&addr, RTA_DATA(r), 4); have_addr = 1;
            } else if (m->ifa_family == AF_INET6 && RTA_PAYLOAD(r) == 16) {
                add_v6(p, (const struct in6_addr *)RTA_DATA(r),
                       m->ifa_prefixlen, m->ifa_scope, ifa_flags);
            }
            break;
        case IFA_BROADCAST:
            if (m->ifa_family == AF_INET && RTA_PAYLOAD(r) == 4) {
                memcpy(&brd, RTA_DATA(r), 4); have_brd = 1;
            }
            break;
        case IFA_FLAGS:
            if (RTA_PAYLOAD(r) >= 4) memcpy(&ifa_flags, RTA_DATA(r), 4);
            break;
        default:
            break;
        }
    }

    if (m->ifa_family == AF_INET && (have_local || have_addr)) {
        p->v4     = have_local ? local : addr;  /* 点对点接口 local 才是本端 */
        p->v4_pfx = m->ifa_prefixlen;
        p->has_v4 = 1;
        if (have_brd) p->brd = brd;
    }
}

/* ---------------- RTM_GETROUTE：默认路由 ---------------- */

static void cb_route(struct nlmsghdr *h, void *ctx)
{
    (void)ctx;
    if (h->nlmsg_type != RTM_NEWROUTE) return;
    struct rtmsg *m = NLMSG_DATA(h);
    int len = (int)h->nlmsg_len - (int)NLMSG_LENGTH(sizeof(*m));

    if (m->rtm_family != AF_INET && m->rtm_family != AF_INET6) return;
    if (rts_n >= MAXRT) {
        static int warned = 0;
        if (!warned++) vlog("more than %d routes, extra ones dropped", MAXRT);
        return;
    }

    struct route rt;
    memset(&rt, 0, sizeof(rt));
    rt.family  = m->rtm_family;
    rt.table   = m->rtm_table;
    rt.dst_len = m->rtm_dst_len;      /* 0 即 default，打印时再按 -l 决定 */

    int gw_len = 0;
    for (struct rtattr *r = RTM_RTA(m); RTA_OK(r, len); r = RTA_NEXT(r, len)) {
        switch (r->rta_type) {
        case RTA_DST:
            if (RTA_PAYLOAD(r) == 4 || RTA_PAYLOAD(r) == 16)
                memcpy(rt.dst, RTA_DATA(r), RTA_PAYLOAD(r));
            break;
        case RTA_GATEWAY:
            gw_len = (int)RTA_PAYLOAD(r);
            if (gw_len == 4 || gw_len == 16) memcpy(rt.gw, RTA_DATA(r), (size_t)gw_len);
            break;
        case RTA_OIF:
            if (RTA_PAYLOAD(r) >= 4) memcpy(&rt.oif, RTA_DATA(r), 4);
            break;
        case RTA_PRIORITY:
            if (RTA_PAYLOAD(r) >= 4) memcpy(&rt.metric, RTA_DATA(r), 4);
            break;
        case RTA_TABLE:
            if (RTA_PAYLOAD(r) >= 4) memcpy(&rt.table, RTA_DATA(r), 4);
            break;
        default:
            break;
        }
    }
    rt.has_gw = (gw_len == 4 || gw_len == 16);
    rts[rts_n++] = rt;
}

/* ---------------- 名字兜底：netlink 不可用时用 ioctl 反查 ---------------- */

static void resolve_names(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    for (int i = 0; i < ifs_n; i++) {
        struct iface *p = &ifs[i];
        if (name_known(p)) continue;

        char nm[IFNAMSIZ] = {0};
        if (if_indextoname((unsigned)p->index, nm) == NULL) {
            /* if_indextoname 在 PRoot 下 EACCES，退回 SIOCGIFNAME */
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            ifr.ifr_ifindex = p->index;
            if (ioctl(fd, SIOCGIFNAME, &ifr) == 0)
                snprintf(nm, sizeof(nm), "%s", ifr.ifr_name);
        }
        if (nm[0]) {
            snprintf(p->name, IFNAMSIZ, "%s", nm);
            p->name_real = 1;
        } else {
            /* 反查失败："if%d" 只是展示占位符，name_real 保持 0，
             * 免得 ioctl_fill 拿 "if12" 去问内核对不存在的接口 */
            snprintf(p->name, IFNAMSIZ, "if%d", p->index);
        }
    }
    close(fd);
}

/* ---------------- prefixlen <-> netmask ---------------- */

static struct in_addr pfx2mask(int pfx)
{
    struct in_addr m;
    if (pfx <= 0)       m.s_addr = 0;
    else if (pfx >= 32) m.s_addr = 0xffffffffu;
    else                m.s_addr = htonl(0xffffffffu << (32 - pfx));
    return m;
}

static int mask2pfx(uint32_t mask)
{
    uint32_t h = ntohl(mask);
    int n = 0;
    while (h & 0x80000000u) { n++; h <<= 1; }
    return n;
}

/* ---------------- ioctl 兜底：mtu / flags / addr / mask / brd ---------------- */

static void run_ioctl(int fd, const char *name, unsigned long req, struct ifreq *ifr)
{
    memset(ifr, 0, sizeof(*ifr));
    snprintf(ifr->ifr_name, IFNAMSIZ, "%s", name);
    if (ioctl(fd, req, ifr) < 0) ifr->ifr_name[0] = 0;   /* 标记失败 */
}

static void ioctl_fill(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;

    for (int i = 0; i < ifs_n; i++) {
        struct iface *p = &ifs[i];
        if (!name_known(p)) continue;
        struct ifreq ifr;

        /* mtu / txqlen 一律以内核直通的 ioctl 为准。
         * 实测：PRoot 的仿真 netlink 把 wlan0 的 txqlen 报成 1000，
         * 而 `adb shell ifconfig` 与 ioctl SIOCGIFTXQLEN 都是 3000。
         * ioctl 在 Termux 原生与 proot 下都可用，所以它比“netlink”可信。
         * flags 则优先用 netlink：ioctl 的 ifr_flags 是 16 位，会截掉 IFF_LOWER_UP。*/
        run_ioctl(fd, p->name, SIOCGIFMTU, &ifr);
        if (ifr.ifr_name[0]) p->mtu = ifr.ifr_mtu;
        run_ioctl(fd, p->name, SIOCGIFTXQLEN, &ifr);
        if (ifr.ifr_name[0]) p->txqlen = ifr.ifr_qlen;
        if (!p->link_ok) {
            run_ioctl(fd, p->name, SIOCGIFFLAGS, &ifr);
            if (ifr.ifr_name[0]) p->flags = (unsigned short)ifr.ifr_flags;
        }
        if (!p->has_v4) {
            run_ioctl(fd, p->name, SIOCGIFADDR, &ifr);
            if (ifr.ifr_name[0]) {
                p->v4 = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
                p->has_v4 = 1;
                /* ioctl 不返回 prefixlen，用 netmask 反推 */
                struct ifreq m2;
                run_ioctl(fd, p->name, SIOCGIFNETMASK, &m2);
                if (m2.ifr_name[0]) {
                    struct in_addr m = ((struct sockaddr_in *)&m2.ifr_netmask)->sin_addr;
                    p->v4_pfx = mask2pfx(m.s_addr);
                }
            }
        }
        if (p->has_v4) {
            if (p->mask.s_addr == 0) {
                if (p->v4_pfx > 0) {
                    p->mask = pfx2mask(p->v4_pfx);
                } else {
                    run_ioctl(fd, p->name, SIOCGIFNETMASK, &ifr);
                    if (ifr.ifr_name[0])
                        p->mask = ((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr;
                }
            }
            if (p->brd.s_addr == 0) {
                run_ioctl(fd, p->name, SIOCGIFBRDADDR, &ifr);
                if (ifr.ifr_name[0]) {
                    struct in_addr b = ((struct sockaddr_in *)&ifr.ifr_broadaddr)->sin_addr;
                    if (b.s_addr) p->brd = b;
                }
            }
        }
    }
    close(fd);
}

/* ---------------- 展示辅助 ---------------- */

static void cat_flags(char *b, size_t n, const char *s)
{
    size_t l = strlen(b);
    if (l + 1 >= n) return;
    snprintf(b + l, n - l, "%s%s", l ? "," : "", s);
}

static void flags_str(unsigned int f, char *b, size_t n)
{
    b[0] = 0;
#define A(bit, s) do { if ((f) & (bit)) cat_flags(b, n, s); } while (0)
    A(IFF_UP,          "UP");
    A(IFF_BROADCAST,   "BROADCAST");
    A(IFF_LOOPBACK,    "LOOPBACK");
    A(IFF_POINTOPOINT, "P2P");
    A(IFF_RUNNING,     "RUNNING");
    A(IFF_MULTICAST,   "MULTICAST");
#ifdef IFF_LOWER_UP
    A(IFF_LOWER_UP,    "LOWER_UP");
#endif
#ifdef IFF_NOARP
    A(IFF_NOARP,       "NOARP");
#endif
#undef A
}

static const char *opstate_str(int s)
{
    switch (s) {
    case 0: return "unknown";
    case 1: return "not-present";
    case 2: return "down";
    case 3: return "lowerlayerdown";
    case 4: return "testing";
    case 5: return "dormant";
    case 6: return "up";
    default: return "?";
    }
}

/* operstate 优先，netlink 拿不到时用 flags 推断。
 * 注意 IF_OPER_UNKNOWN==0 是内核返回的合法值，所以用 >=0 而非 >0。*/
static const char *state_str(const struct iface *p)
{
    if (p->operstate >= 0) return opstate_str(p->operstate);
    if (p->flags & IFF_UP)
        return (p->flags & IFF_RUNNING) ? "up" : "unknown";
    return "down";
}

static const char *v6_kind(const struct in6_addr *a)
{
    if (IN6_IS_ADDR_LOOPBACK(a))  return "loopback";
    if (IN6_IS_ADDR_LINKLOCAL(a)) return "link-local";
    if (IN6_IS_ADDR_SITELOCAL(a)) return "site-local";
    if (IN6_IS_ADDR_MULTICAST(a)) return "multicast";
    if (IN6_IS_ADDR_V4MAPPED(a))  return "v4-mapped";
    if ((a->s6_addr[0] & 0xfe) == 0xfc) return "unique-local";
    return "global";
}

static int v6_needs_scope(const struct iface *p, const struct in6_addr *a)
{
    (void)p;
    return IN6_IS_ADDR_LINKLOCAL(a) || IN6_IS_ADDR_MC_LINKLOCAL(a);
}

/* "fe80::1%wlan0" */
static void v6_ntop(const struct iface *p, const struct in6_addr *a, char *out, size_t n)
{
    inet_ntop(AF_INET6, a, out, n);
    if (v6_needs_scope(p, a)) {
        size_t l = strlen(out);
        snprintf(out + l, n - l, "%%%s", p->name);
    }
}

static const char *table_name(int t)
{
    switch (t) {
    /* Linux 内核保留值（linux/rtnetlink.h） */
    case 252: return "compat";
    case 253: return "default";
    case 254: return "main";
    case 255: return "local";
    case 0:   return "unspec";
    /* Android netd 保留表（system/netd） */
    case 97:  return "local_network";
    case 98:  return "legacy_system";
    case 99:  return "legacy_network";
    default:  return NULL;
    }
}

static void mac_str(const struct iface *p, char *b, size_t n)
{
    b[0] = 0;
    if (!p->has_mac || p->mac_len <= 0) return;
    for (int i = 0; i < p->mac_len; i++) {
        size_t l = strlen(b);
        if (l + 4 >= n) break;
        snprintf(b + l, n - l, "%02x%s", p->mac[i], i + 1 < p->mac_len ? ":" : "");
    }
}

/* ---------------- 选择 / 排序 ---------------- */

static int is_number(const char *s)
{
    if (!s || !*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

static int selected(const struct iface *p)
{
    if (opt.filter) {
        int hit = strcmp(p->name, opt.filter) == 0 ||
                  (is_number(opt.filter) && atoi(opt.filter) == p->index);
        if (!hit) return 0;
        return 1;   /* 明确点名了就显示，即使没有地址 */
    }
    if (opt.only_up && !(p->flags & IFF_UP)) return 0;
    if (opt.all) return 1;
    return (p->has_v4 && opt.w4) || (p->v6_n > 0 && opt.w6);
}

static int cmp_iface(const void *a, const void *b)
{
    const struct iface *x = a, *y = b;
    return (x->index > y->index) - (x->index < y->index);
}

static int cmp_route(const void *a, const void *b)
{
    const struct route *x = a, *y = b;
    if (x->family != y->family) return x->family - y->family;
    if (x->dst_len != y->dst_len) return x->dst_len - y->dst_len;
    if (x->metric != y->metric) return x->metric - y->metric;
    return (x->table > y->table) - (x->table < y->table);
}

/* 路由同样尊重 -4/-6/-i/-R/-l */
static int route_selected(const struct route *r)
{
    if (!opt.routes) return 0;
    if (!opt.all_routes && r->dst_len != 0) return 0;
    if (r->family == AF_INET  && !opt.w4) return 0;
    if (r->family == AF_INET6 && !opt.w6) return 0;
    if (opt.filter) {
        struct iface *p = by_index(r->oif);
        int hit = (p && strcmp(p->name, opt.filter) == 0) ||
                  (is_number(opt.filter) && atoi(opt.filter) == r->oif);
        if (!hit) return 0;
    }
    return 1;
}

/* ---------------- 输出：文本 ---------------- */

static void print_one(const struct iface *p)
{
    char fs[128], mac[64];
    flags_str(p->flags, fs, sizeof(fs));

    printf("%s  (index %d", p->name, p->index);
    if (p->mtu > 0)     printf(", mtu %d", p->mtu);
    if (p->txqlen > 0)  printf(", qlen %d", p->txqlen);
    printf(", state %s)\n", state_str(p));
    printf("  flags: %s\n", fs[0] ? fs : "-");

    if (opt.wmac) {
        if (p->flags & IFF_LOOPBACK) {
            printf("  MAC : (loopback)\n");
        } else {
            mac_str(p, mac, sizeof(mac));
            printf("  MAC : %s\n", mac[0] ? mac : "(unavailable - hidden by Android)");
        }
    }

    if (opt.w4 && p->has_v4) {
        char a[INET_ADDRSTRLEN], m[INET_ADDRSTRLEN], b[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &p->v4,   a, sizeof(a));
        inet_ntop(AF_INET, &p->mask, m, sizeof(m));
        inet_ntop(AF_INET, &p->brd,  b, sizeof(b));
        printf("  IPv4: %s/%d  mask %s  brd %s\n", a, p->v4_pfx, m, b);
    }
    if (opt.w6) {
        for (int i = 0; i < p->v6_n; i++) {
            char a[INET6_ADDRSTRLEN + IFNAMSIZ];
            v6_ntop(p, &p->v6[i].a, a, sizeof(a));
            printf("  IPv6: %s/%d  (%s)%s%s%s\n", a, p->v6[i].pfx,
                   v6_kind(&p->v6[i].a),
                   p->v6[i].temporary  ? " [temporary]"  : "",
                   p->v6[i].tentative  ? " [tentative]"  : "",
                   p->v6[i].deprecated ? " [deprecated]" : "");
        }
    }
    printf("\n");
}

static void print_routes(void)
{
    int any = 0;
    for (int i = 0; i < rts_n; i++) if (route_selected(&rts[i])) any = 1;
    if (!any) return;

    printf("%s:\n", opt.all_routes ? "routes" : "default routes");
    for (int i = 0; i < rts_n; i++) {
        if (!route_selected(&rts[i])) continue;
        struct route *r = &rts[i];
        char gw[INET6_ADDRSTRLEN + IFNAMSIZ] = "-";
        char dst[INET6_ADDRSTRLEN + 8] = "default";
        char tb[24];
        const char *tn = table_name(r->table);
        if (tn) snprintf(tb, sizeof(tb), "%s(%d)", tn, r->table);
        else    snprintf(tb, sizeof(tb), "%d", r->table);

        struct iface *p = by_index(r->oif);
        char dev[IFNAMSIZ];
        if (p) snprintf(dev, sizeof(dev), "%s", p->name);
        else   snprintf(dev, sizeof(dev), "if%d", r->oif);

        if (r->dst_len) {
            char d[INET6_ADDRSTRLEN];
            inet_ntop(r->family, r->dst, d, sizeof(d));
            snprintf(dst, sizeof(dst), "%s/%d", d, r->dst_len);
        }
        if (r->has_gw) {
            inet_ntop(r->family, r->gw, gw, sizeof(gw));
            /* IPv6 链路本地网关离开接口就没意义，补 %dev。
             * 用 memcpy 而不是强转：gw[] 对齐为 1，
             * 直接当 struct in6_addr* 用是 UB。*/
            if (r->family == AF_INET6) {
                struct in6_addr tmp;
                memcpy(&tmp, r->gw, sizeof(tmp));
                if (IN6_IS_ADDR_LINKLOCAL(&tmp)) {
                    size_t l = strlen(gw);
                    snprintf(gw + l, sizeof(gw) - l, "%%%s", dev);
                }
            }
        }
        printf("  %-4s table %-13s dev %-14s %-32s gw %-45s metric %d\n",
               r->family == AF_INET ? "IPv4" : "IPv6", tb, dev, dst, gw, r->metric);
    }
    printf("\n");
}

/* 一行一个接口，便于脚本消费 */
static void print_summary_line(const struct iface *p)
{
    printf("%s", p->name);
    if (p->has_v4 && opt.w4) {
        char a[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &p->v4, a, sizeof(a));
        printf(" ipv4=%s/%d", a, p->v4_pfx);
    }
    if (opt.w6) {
        for (int i = 0; i < p->v6_n; i++) {
            char a[INET6_ADDRSTRLEN + IFNAMSIZ];
            v6_ntop(p, &p->v6[i].a, a, sizeof(a));
            printf(" ipv6=%s/%d", a, p->v6[i].pfx);
        }
    }
    printf(" state=%s", state_str(p));
    if (opt.wmac && !(p->flags & IFF_LOOPBACK)) {
        char mac[64];
        mac_str(p, mac, sizeof(mac));
        if (mac[0]) printf(" mac=%s", mac);
    }
    printf("\n");
}

/* ---------------- 输出：JSON ---------------- */

static void json_string(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { putchar('\\'); putchar(*s); }
        else if ((unsigned char)*s < 0x20) printf("\\u%04x", (unsigned char)*s);
        else putchar(*s);
    }
    putchar('"');
}

static void print_json(void)
{
    int first = 1;
    printf("{\n  \"interfaces\": [");
    for (int i = 0; i < ifs_n; i++) {
        struct iface *p = &ifs[i];
        if (!selected(p)) continue;
        char fs[128], mac[64];
        flags_str(p->flags, fs, sizeof(fs));
        mac_str(p, mac, sizeof(mac));

        printf("%s\n    {\"name\": ", first ? "" : ",");
        json_string(p->name);
        printf(", \"index\": %d, \"mtu\": %d, \"txqlen\": ", p->index, p->mtu);
        if (p->txqlen > 0) printf("%d", p->txqlen); else printf("null");
        printf(", \"state\": ");
        json_string(state_str(p));
        printf(", \"flags\": \"%s\"", fs);
        if (opt.wmac) {
            printf(", \"mac\": ");
            if (mac[0]) json_string(mac); else printf("null");
        }
        printf(", \"ipv4\": [");
        if (opt.w4 && p->has_v4) {
            char a[INET_ADDRSTRLEN], m[INET_ADDRSTRLEN], b[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &p->v4,   a, sizeof(a));
            inet_ntop(AF_INET, &p->mask, m, sizeof(m));
            inet_ntop(AF_INET, &p->brd,  b, sizeof(b));
            printf("{\"address\": \"%s\", \"prefixlen\": %d, \"netmask\": \"%s\", "
                   "\"broadcast\": \"%s\"}", a, p->v4_pfx, m, b);
        }
        printf("], \"ipv6\": [");
        for (int k = 0; opt.w6 && k < p->v6_n; k++) {
            char a[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &p->v6[k].a, a, sizeof(a));
            printf("%s{\"address\": \"%s\", \"prefixlen\": %d, \"scope\": ",
                   k ? ", " : "", a, p->v6[k].pfx);
            json_string(v6_kind(&p->v6[k].a));
            if (v6_needs_scope(p, &p->v6[k].a)) {
                printf(", \"scope_id\": ");
                json_string(p->name);
            }
            if (p->v6[k].temporary)  printf(", \"temporary\": true");
            if (p->v6[k].tentative)  printf(", \"tentative\": true");
            if (p->v6[k].deprecated) printf(", \"deprecated\": true");
            putchar('}');
        }
        printf("]}");
        first = 0;
    }
    printf("\n  ],\n  \"routes\": [");
    first = 1;
    for (int i = 0; i < rts_n; i++) {
        if (!route_selected(&rts[i])) continue;
        struct route *r = &rts[i];
        char gw[INET6_ADDRSTRLEN + IFNAMSIZ] = {0};
        struct iface *p = by_index(r->oif);
        const char *tn = table_name(r->table);
        if (r->has_gw) inet_ntop(r->family, r->gw, gw, sizeof(gw));
        printf("%s\n    {\"family\": \"%s\", \"table\": %d, \"table_name\": ",
               first ? "" : ",", r->family == AF_INET ? "ipv4" : "ipv6", r->table);
        if (tn) json_string(tn); else printf("null");
        printf(", \"destination\": ");
        if (r->dst_len) {
            char d[INET6_ADDRSTRLEN];
            inet_ntop(r->family, r->dst, d, sizeof(d));
            printf("{\"address\": \"%s\", \"prefixlen\": %d}", d, r->dst_len);
        } else {
            printf("null");
        }
        printf(", \"dev\": ");
        json_string(p ? p->name : "?");
        printf(", \"gateway\": ");
        if (gw[0]) json_string(gw); else printf("null");
        printf(", \"metric\": %d}", r->metric);
        first = 0;
    }
    printf("\n  ]\n}\n");
}

/* ---------------- 使用说明 ---------------- */

static void usage(const char *argv0)
{
    printf(
        "usage: %s [options]\n"
        "  -i IFACE   only this interface (name or ifindex)\n"
        "  -4         IPv4 only\n"
        "  -6         IPv6 only\n"
        "  -u         only interfaces that are IFF_UP\n"
        "  -a         also show interfaces without any address\n"
        "  -m         show MAC (default)\n"
        "  -M         hide MAC\n"
        "  -r         show routes (default)\n"
        "  -R         hide routes\n"
        "  -l         all routes (connected/prefix routes too), not just default\n"
        "  -j         JSON output\n"
        "  -s         compact one-line-per-interface output (no routes)\n"
        "  -v         verbose: also print fallback/failure reasons to stderr\n"
        "  -h         show this help\n",
        argv0);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    int optc;
    while ((optc = getopt(argc, argv, "i:46uamMrRljsvh")) != -1) {
        switch (optc) {
        case 'i': opt.filter  = optarg; break;
        case '4': opt.w6      = 0;      break;
        case '6': opt.w4      = 0;      break;
        case 'u': opt.only_up = 1;      break;
        case 'a': opt.all     = 1;      break;
        case 'm': opt.wmac    = 1;      break;
        case 'M': opt.wmac    = 0;      break;
        case 'r': opt.routes  = 1;      break;
        case 'R': opt.routes  = 0;      break;
        case 'l': opt.all_routes = 1;   break;
        case 'j': opt.json    = 1;      break;
        case 's': opt.summary = 1;      break;
        case 'v': opt.verbose = 1;      break;
        case 'h': opt.help    = 1;      break;
        default:
            usage(argv[0]);
            return 1;
        }
    }
    if (opt.help) { usage(argv[0]); return 0; }
    if (!opt.w4 && !opt.w6) { opt.w4 = opt.w6 = 1; }   /* -4 -6 视为不限制 */

    /* 顺序很重要：先 LINK 建表（名字），再 ADDR 填地址；
       之后才做名字/ioctl 兜底，最后才是 ROUTE。 */
    int r;
    if ((r = nl_dump(RTM_GETLINK, AF_UNSPEC, cb_link, NULL)) < 0)
        vlog("RTM_GETLINK failed: %s (falling back to if_indextoname/ioctl)",
             strerror(-r));
    if ((r = nl_dump(RTM_GETADDR, AF_UNSPEC, cb_addr, NULL)) < 0)
        vlog("RTM_GETADDR failed: %s", strerror(-r));

    resolve_names();
    ioctl_fill();

    if ((r = nl_dump(RTM_GETROUTE, AF_UNSPEC, cb_route, NULL)) < 0)
        vlog("RTM_GETROUTE failed: %s", strerror(-r));

    qsort(ifs, (size_t)ifs_n, sizeof(ifs[0]), cmp_iface);
    qsort(rts, (size_t)rts_n, sizeof(rts[0]), cmp_route);

    if (opt.json) {
        print_json();
        return 0;
    }

    int shown = 0;
    for (int i = 0; i < ifs_n; i++) {
        if (!selected(&ifs[i])) continue;
        if (opt.summary) print_summary_line(&ifs[i]);
        else             print_one(&ifs[i]);
        shown++;
    }

    if (!shown) {
        fprintf(stderr, "ipinfo: no matching interface\n");
        return 2;
    }
    if (!opt.summary) print_routes();
    return 0;
}
