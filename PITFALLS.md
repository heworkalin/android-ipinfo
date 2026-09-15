# 踩坑记录（PITFALLS）

`ipinfo` 开发过程中真实撞到的坑，以及每个坑的**证据**和**修法**。
所有结论都来自本机真机命令输出（PJE110 / Android 15 / kernel 5.15），可复现。

目录

- [0. 先说方法论：三条会让你得出错误结论的测试习惯](#0-先说方法论)
- [1. netlink 的坑](#1-netlink-的坑)
- [2. Android 权限的坑](#2-android-权限的坑)
- [3. PRoot 的坑（最阴的一类）](#3-proot-的坑)
- [4. 代码层面的坑](#4-代码层面的坑)
- [5. 复核清单](#5-复核清单)

---

## 0. 先说方法论

这三个习惯，任何一个漏掉都会让你写出"看起来对、其实错"的结论。

### 0.1 必须在**目标调用方式**下测试

我一开始用 `proot-distro login ubuntu -- <cmd>`（默认 fake root）验证，就宣称"proot 下可用"。
但实际使用方式是：

```sh
proot-distro login ubuntu --user he -- <cmd>
```

这两个身份不同（fake root vs `he`），不测就等于没测。

### 0.2 PRoot 会伪造 `getuid()`，所以要在**被跟踪进程外面**读真实凭据

在 proot 里 `getuid()` 返回 `/etc/passwd` 里的假 uid，`/proc/self/attr/current` 也可能被改写。
唯一可信的办法是：让目标进程自己报出 pid，然后**从 proot 外面**读 `/proc/<pid>/status`：

```console
$ proot-distro login ubuntu --user he -- /tmp/probe_who &   # 打印自己的 pid 后 sleep
SELF pid=30239 uid=10617 euid=10617 ctx=u:r:untrusted_app_27:s0:...
--- 从 proot 外面读 /proc/30239 ---
Uid:	10616	10616	10616	10616          ← 真 uid（假的 10617 只是 PRoot 的谎）
CapEff:	0000000000000000
u:r:untrusted_app_27:s0:c104,c258,c512,c768
```

### 0.3 必须有一个**有权限的对照基准**

否则你无法判断"工具输出的值"是不是真的。本机 `adb` 已授权，就用它当基准：

```console
$ adb shell ip route show table all | head -1
default via 192.168.10.1 dev wlan0 table 1027 proto static
$ adb shell ifconfig wlan0
... MTU:1500 ... txqueuelen:3000
```

**正是这个基准立刻暴露了 PRoot 仿真数据是错的**（见 3.3）。

### 0.4 stdout / stderr 分开管道，输出顺序会骗人
差点害我写错文档的一例：

```sh
./ipinfo -i wlan0 -v                       # 它的 stderr 会输出 “RTM_GETLINK failed”
cp ipinfo <rootfs>/tmp/ipinfo
timeout 60 proot-distro login ubuntu --user he -- /tmp/ipinfo -i wlan0   # 没加 -v
```

因为 stdout 与 stderr 是**两个独立的管道**，native 那一次的 stderr 行会在下一个 `echo` 之后才被
读到，看起来就像属于 proot 那一次，让我一度以为“PRoot 行为不确定”。

**教训**：诊断时把 stdout 与 stderr 分别重定向到文件再读（`> out 2> err`），
不要把两个管道混在一起看顺序。重测后 13/13 次全部稳定为 `AF_UNIX` 替换，并不存在随机性。

### 0.5 计数别用太宽的模式，否则“接口行”会被当成“邻居行”

验证邻居表时我用 `grep -c '^  IPv'` 计数，在 proot 里得到 **14**，一度以为“PRoot 居然支持邻居表”。
实际上这 14 条是**接口的地址行**——它们以 `  IPv4: 192.168.10.2/24 ...` 开头，也是两个空格 + `IPv`：

| 行类型 | 样子 | 正确模式 |
|---|---|---|
| 接口地址 | `  IPv4: 192.168.10.2/24  mask …` | `'^  IPv[46]: '` |
| 路由 | `  IPv4 table 1027  dev …` | `'^  IPv[46] table '` |
| 邻居 | `  IPv4 dev wlan0  192.168.10.1 …` | `'^  IPv[46] dev '` |

第二个错是一次性犯的：计数时给 `ipinfo` 加上了 `-R`（隐藏路由），于是路由数永远是 0。

**教训**：计数给每种行配专属模式（`table` / `dev` / `:`），并且先看一眼原始输出再下结论；
这就是后来把“容器内自测”写成脚本（`test_proot.sh`）的原因——机器断言比人眼盯 grep 可靠。

### 0.6 不要把【环境事实】写成【断言】，那是惩罚变好

自测脚本初版里我写了“邻居表必须为 0”，理由是“PRoot 不支持 `RTM_GETNEIGH`”。
这是错的：

* “当前这个测试环境拿不到邻居表”是**环境的能力短板**，不是程序应有的性质；
* 哪天 PRoot 补上了这个能力，或者换了一台设备/内核能拿到，这条断言就会报 FAIL
  —— **变好了反而算错**。

后来把自测拆成两块：

| | 例子 | 处理 |
|---|---|---|
| 程序不变量 | 退出码、stderr 干净、JSON 结构、无 `if<N>` 占位符、**要了数据却没有时必须说明（不能静默）** | **断言** |
| 环境差异 | proot 下邻居表几条、AF_UNIX 提示几次、与宿主机计数差多少 | **只报告** |

至于“环境差异”本身对不对，用**另一条通道**验证：与有权限的 `adb shell` 做集合级对照
（`cmp_routes.py` / `cmp_neigh.py` / `make compare`），而不是把环境现象固化成断言。

---

## 1. netlink 的坑

### 1.1 用 `nlmsg_pid != 0` 过滤消息 → 进程永久阻塞（最严重）

**现象**：程序在 Termux 原生下 100% CPU 卡死，`timeout` 才能结束。

**原因**：我以为内核回复的 `nlmsg_pid` 是 0（很多文档这么写）。实测**不是**——它是本 socket
自动绑定的端口号，也就是本进程 pid：

```console
ADDR: DONE pid=23336 seq=1789477150 (req seq=1789477150)   # pid 就是自己
```

于是 `if (h->nlmsg_pid != 0) continue;` 把**包括 `NLMSG_DONE` 在内的所有消息全部丢掉**，
`recv()` 再也等不到结束标志，永久阻塞。

**修法**：每个 dump 用独立 socket，只校验 `nlmsg_seq`；并且 `recv() == 0` 时防御性退出。

```c
if (h->nlmsg_seq != seq) continue;   /* 不要校验 pid */
```

### 1.2 `recv()` 返回 0 没处理

即使没有 1.1，`recv() == 0`（对端关闭）若不 `break`，`len = 0` → for 循环不执行 →
`while (!done)` 继续 `recv()` → 空转。已加 `if (n == 0) break;`。

### 1.3 内核头里的 `NLMSG_OK` 宏与 `-Wextra` 冲突

`NLMSG_OK(nlh, len)` 内部把 `len` 与 `__u32` 比较，无论 `len` 是 `int` 还是 `unsigned`
都会触发 `-Wsign-compare`。改用自己的宏：

```c
#define NLOK(nlh, len) ((len) >= (int)sizeof(struct nlmsghdr) && \
                        (nlh)->nlmsg_len >= sizeof(struct nlmsghdr) && \
                        (int)(nlh)->nlmsg_len <= (len))
```

### 1.4 大表截断与 dump 中断

* 用 `recv(..., MSG_TRUNC)` 拿到真实长度，`> sizeof(buf)` 时说明被截断，报出来而不是静默出错。
* `NLMSG_DONE` 上带 `NLM_F_DUMP_INTR` 表示 dump 中途被中断，结果可能不完整。
* `SO_RCVBUF` 调大（1 MiB）降低截断概率。

### 1.5 请求长度：`sizeof(req)` vs `NLMSG_LENGTH`

`sizeof(struct { nlmsghdr h; rtgenmsg g; })` 会含尾部对齐填充（20 字节，而不是
`NLMSG_LENGTH(sizeof(rtgenmsg))` = 17）。规范写法是后者。**实测：本机三种请求、
两个环境下 17 和 20 都可用**，所以改过去没有风险，但更规范。

### 1.6 路由表 id 装不进 8 位时，`rtm_table` 是 252

Android 的 per-network 表 id 是 `1000000000 + netId` 这种大数，`rtm_table`（u8）装不下，
内核就把 `rtm_table` 设为 `RT_TABLE_COMPAT`(252)，真值放在 `RTA_TABLE`：

```console
fam=2 rtm_table=252  RTA_TABLE=1000000027 (0x3b9aca1b) oif=27 dst=192.168.10.0/24
```

坑点：

* 不要因为 `rtm_table == 252` 就显示成 "local"（**252 是 `compat`，`local` 是 255**）。
  一个"改进建议"曾让我把 252 标成 local，用原始 dump 一对照就发现是错的。
* 必须先取 `rtm_table`，再被 `RTA_TABLE` 覆盖。

### 1.7 `-l` 全路由模式

只看默认路由（`rtm_dst_len != 0` 就 return）会漏掉直连/网段路由。支持 `-l` 后需要
额外处理 `RTA_DST`（4 或 16 字节）、并放大 `MAXRT`（原 128 → 512）。
本机实测全路由共 **66** 条（IPv4 14 + IPv6 52），默认路由 5 条。

> 顺带一个坑：早期我写的是“全表 80 条”，那是用 `grep -c '^  IPv'` 数出来的——
> 把 14 行接口地址也数进去了（14 + 66 = 80）。见 §0.5。

### 1.8 邻居表（ARP/NDP）：请求长度不能用 `rtgenmsg`

`RTM_GETNEIGH` 的请求体是 `struct ndmsg`，不是 `struct rtgenmsg`。因此 `nl_dump()` 后来
加了一个 `payload_len` 参数：

```c
nl_dump(RTM_GETNEIGH, AF_UNSPEC, (int)sizeof(struct ndmsg), cb_neigh, NULL);
```

另外解析属性时内核头里没有现成的 `NEIGH_RTA()`（只有 `NDA_RTA()` 在某些头里），
自己算偏移：`(char *)m + NLMSG_ALIGN(sizeof(struct ndmsg))`。

### 1.9 邻居表/路由表可以反推接口（修掉 `if%d` 占位符）

邻居表里会出现根本没有地址、`RTM_GETADDR` 报不出来的接口（本机实测 `if7/if8/if28/if29/if30`）。
它们既然在 `ndm_ifindex` / `RTA_OIF` 里现身，就可以拿去建接口条目再反查名字：

```c
if (m->ndm_ifindex > 0) slot(m->ndm_ifindex, NULL);   /* cb_route 里用 rt.oif 同理 */
```

这里有两个容易踩的点，我各踩了一次：

1. **发现必须在“显示过滤”之前做。** 最初 `slot()` 写在早退语句后面：

   ```c
   if (!opt.neigh_all && (m->ndm_state & NUD_NOARP)) return;   /* 先早退了 */
   if (m->ndm_ifindex > 0) slot(m->ndm_ifindex, NULL);          /* 永远执行不到 */
   ```

   而那些无地址接口的名字**恰好只出现在 NOARP 组播条目里**，于是 `-a` 一个都发现不了。
   把 `slot()` 提到早退之前才修好。
2. **`-a` 自己也要触发邻居 dump。** 否则“显示全部接口”这个诉求在原生环境下不可能实现——
   邻居表是本机唯一会报出无地址接口 ifindex 的来源。现在条件是 `opt.neigh || opt.all`。

配套改动：`resolve_names()` + `ioctl_fill()` 从“ROUTE 之前”移到**所有 dump 之后**，
否则新发现的接口拿不到名字。

效果（本机实测）：默认 **10** 个接口 → `-a` 后 **15** 个，多出来的是
`gretap0`、`erspan0`、`wlan1`、`p2p0`、`wifi-aware0`，全是真名，不再有 `if%d` 占位符。
剩下的 15 个（`gre0`/`sit0`/`tunl0`/`rmnet_data3…6` 等）没有任何路由/邻居条目引用，
仍然无从得知（硬限制）。

### 1.10 邻居表：shell 与非特权应用看到的 lladdr 不同

`ipinfo -N` 与 `adb shell ip neigh show nud all` 条数相同（49 == 49），但默认视图才是
严格对照：**`ipinfo -n` 与 `ip neigh show` 完全相同（10 == 10）**。

全集视图里有 26 条差异，全部是 NOARP 条目上的 lladdr：shell 看得到 `08`（1 字节），
untrusted_app 看不到。这是**调用者身份**差异（uid=2000 vs 10616），不是解析错误。
`cmp_neigh.py` 把默认视图当硬指标，全集差异只做信息输出。

### 1.11 不要被无关进程的 stderr 骗到

调试期间输出里突然多出一行：

```
Cannot bind netlink socket: Permission denied
```

一度以为是自己程序在喷。排查结果：

```console
$ readelf -d ./ipinfo | grep NEEDED
  Shared library: [libdl.so]
  Shared library: [libc.so]
$ strings ./ipinfo | grep -c "Cannot bind netlink"
0
$ grep -rl "Cannot bind netlink socket" /system/lib64/
/system/lib64/libiprouteutil.so
/system/lib64/libnetlink.so
```

这句话来自 **Android 平台自带的 iproute2 库**，是先前 `adb shell ip ...` 那个进程留下的输出，
混进了终端流。`ipinfo` 连续跑 12 次 stderr 均为空。**教训**：先确认二进制有没有依赖/持有那个字符串，
再怀疑自己。

---

### 1.12 “空输出”是最坏的输出（邻居表在容器内静默无输出）

用户在容器里跑 `./ipinfo -N`（nud all，想看 ARP），结果**邻居表什么都不打印**，
一度以为工具坏了。实际上这是 PRoot 的能力缺口（返回 0 条）——但**工具没告诉他是这个原因**。

“空输出”与“请求不支持 / 工具坏了”长得一模一样，这是可用性问题，不是能力问题。
修法：当用户明确要了邻居表（`-n/-N`）却一条都没有时，显式输出：

```
neighbors (nud all): (none)
  note: RTM_GETNEIGH returned 0 entries and netlink here is emulated
        (PRoot replaced AF_NETLINK with AF_UNIX, whose fallback does
        not implement the neighbor table). Run ipinfo outside proot
        (Termux host) to get ARP/NDP entries.
```

为此把 `SO_DOMAIN` 检测从“仅 -v 时做”改成**总是做**，并把结果记到全局 `nl_emulated`：

```c
if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &dom, &dl) == 0 && dom != AF_NETLINK) {
    nl_emulated = 1;
    if (opt.verbose) vlog("... actually domain %d (PRoot AF_UNIX fallback?)", dom);
}
```

这个提示也写进了自测断言：`test_proot.sh` 会检查容器内 `-n` 是否打印了 `(none)`
并说明了“netlink here is emulated”，防止以后又被静默掉。

### 1.13 tmux / 交互式 login 不会改变行为

曾怀疑“在 tmux 里交互式 `proot-distro login` 与 `-- command` 模式不同”。实测（tmux 3.7c）：

```console
$ tmux new-session -d -s t1; tmux send-keys -t t1 'proot-distro login ubuntu --user he' Enter
$ tmux send-keys -t t1 'cd /data/data/com.termux/files/home/android_ip && ./ipinfo -v -N -i wlan0' Enter
ipinfo: socket(AF_NETLINK) is actually domain 1 (PRoot AF_UNIX fallback?) ...   # 4 次
```

结论：两种方式行为完全一致，仍然是 AF_UNIX 伪装、邻居表为空。
注意 `-N` 下提示是 **4 次**而不是 3 次——因为多了一次 `RTM_GETNEIGH` dump（LINK/ADDR/ROUTE/NEIGH）。
`test_proot.sh` 里断言的是不带 `-n` 的 3 次。

## 2. Android 权限的坑

### 2.1 `RTM_GETLINK` 被拒，但 `RTM_GETADDR` / `RTM_GETROUTE` 可用

```console
Termux 原生（uid=10616）:
  GETLINK   sendto DENIED: Permission denied
  GETADDR   sendto OK
  GETROUTE  sendto OK
```

同一个 socket、同一个 uid，只有 `RTM_GETLINK` 这一种**请求类型**被拒。

### 2.2 不要归错因（uid / capability / SELinux / socket 类型都不是原因）

我最初把它归因于"SELinux 拒绝"，写进了文档。**这是错的**，用真实凭据一测就穿帮：

| | Termux 原生 | `proot -0` | `proot-distro --user he` |
|---|---|---|---|
| 真 uid | 10616 | 10616 | 10616 |
| `CapEff` | 0 | 0 | 0 |
| SELinux 上下文 | `u:r:untrusted_app_27:s0:c104,c258,c512,c768` | 同左 | 同左 |

三者**完全相同**，但结果不同 → 原因不在这里。逐个排除过程：

| 假设 | 实验 | 结论 |
|---|---|---|
| socket 类型（`SOCK_RAW`） | 同请求分别用 `SOCK_DGRAM` / `SOCK_RAW` | 两者行为完全一致 → 无关 |
| ptrace | 自己写最小 tracer（`PTRACE_TRACEME` + `PTRACE_CONT`） | 仍被拒 → 无关 |
| `NoNewPrivs` | `prctl(PR_SET_NO_NEW_PRIVS); execv(...)` | 仍被拒 → 无关 |
| seccomp 加速 | `PROOT_NO_SECCOMP=1 proot ...` | 仍 OK → 无关 |
| `LD_PRELOAD` | `env -u LD_PRELOAD ./probe` | 仍被拒 → 无关 |

**结论：原因未确定。** 文档里就写"未确定"，不要编一个听起来合理的理由。

### 2.3 `/proc/net/*`、`/sys/class/net/*` 对应用全 EACCES

```
if_inet6/route/ipv6_route/arp/dev : EACCES
ls /sys/class/net                 : Permission denied
```

后果：`ifconfig -a`（shell）能列出 **30** 个接口，而 `RTM_GETADDR` 只能给出有地址的
**10** 个。不过还能报回 **5** 个（实测 `gretap0`/`erspan0`/`wlan1`/`p2p0`/`wifi-aware0`）——
靠路由表/邻居表里的 ifindex 反推（见 §1.9），`-a` 时共 15 个。
剩下 15 个（`tunl0`/`gre0`/`sit0`/`rmnet_data3…6` 等）没有被任何路由或邻居条目引用，
零授权下无从得知，这是真正的硬限制。

---

## 3. PRoot 的坑

### 3.1 PRoot 会把 `AF_NETLINK` socket 换成 `AF_UNIX`

`proot -v 9` 的原文：

```
sysenter start: socket(0x10, 0x80003, 0x0)      # AF_NETLINK, SOCK_RAW
proot info: AF_NETLINK bind denied by host (Permission denied);
            enabling AF_UNIX fallback for sandbox helpers
sysenter end:   socket(0x1,  0x80002, 0x0)      # 被改成 AF_UNIX, SOCK_DGRAM
```

用 `getsockopt(SO_DOMAIN)` 可以直接看到（`AF_INET`/`AF_UNIX` 不受影响）：

| 请求 | Termux 原生 | proot |
|---|---|---|
| `socket(AF_INET)` | `SO_DOMAIN=2` | `SO_DOMAIN=2` |
| `socket(AF_UNIX)` | `SO_DOMAIN=1` | `SO_DOMAIN=1` |
| `socket(AF_NETLINK)` | `SO_DOMAIN=16` | **`SO_DOMAIN=1`** |

**所以"PRoot 下 netlink 可用"是假象。** 我在 README 里写错过一次，已改成上面这段事实。
现在 `ipinfo -v` 会主动提示这一点：

```
ipinfo: socket(AF_NETLINK) is actually domain 1 (PRoot AF_UNIX fallback?) - netlink replies may be emulated
```

### 3.2 PRoot 的"netlink 回复"是合成的，不是内核原样转发

同一 `RTM_GETADDR` 请求，两边对比：

```
Termux（真内核）: index=18 pfx=64 scope=0 ifaflags=0x0  attrs: t1/16 t6/16 t8/4
proot（合成）   : index=18 pfx=64 scope=0 ifaflags=0x80 attrs: t1/16 t2/16
```

差别：

* 少 `IFA_CACHEINFO`(t6)、`IFA_FLAGS`(t8)
* IPv4 少 `IFA_BROADCAST`(t4)
* `ifa_flags` 恒为 `0x80`（`IFA_F_PERMANENT`）
* 消息顺序反转

### 3.3 仿真数据会**报错值**：txqlen 1000 vs 真值 3000

这是对照 adb 基准才发现的：

```console
$ adb shell ifconfig wlan0 | grep txqueuelen
       collisions:0 txqueuelen:3000          ← 真值
$ proot ... ipinfo -i wlan0
wlan0  (index 27, mtu 1500, qlen 1000, ...)   ← PRoot 仿真给的是错的
$ ./ipinfo -i wlan0                            # 改用 ioctl SIOCGIFTXQLEN 之后
wlan0  (index 27, mtu 1500, qlen 3000, ...)   ← 正确
```

**修法**：`mtu` / `txqlen` 一律用内核直通的 ioctl 取值，不信任可能的仿真 netlink。
（`flags` 仍优先 netlink，因为 ioctl 的 `ifr_flags` 是 16 位，会截掉 `IFF_LOWER_UP`。）

```c
run_ioctl(fd, p->name, SIOCGIFMTU, &ifr);      if (ifr.ifr_name[0]) p->mtu = ifr.ifr_mtu;
run_ioctl(fd, p->name, SIOCGIFTXQLEN, &ifr);   if (ifr.ifr_name[0]) p->txqlen = ifr.ifr_qlen;
```

### 3.4 glibc 的 `if_indextoname` 在 PRoot 下 EACCES，但原始 ioctl 可用

这就是老代码里那句"if_indextoname 被 PRoot 拦掉"的真相——**说法对了一半**：

| | bionic `if_indextoname` | glibc `if_indextoname` | 原始 `SIOCGIFNAME` |
|---|---|---|---|
| Termux 原生 | OK | OK | OK |
| proot `--user he` | OK | **EACCES** | **OK** |

glibc 的实现走了别的路（内部 netlink），在 PRoot 下失败；而直接发的
`ioctl(SIOCGIFNAME)` 是好的。所以"三级名字兜底"是**必要**的：

```
netlink IFLA_IFNAME → if_indextoname() → 原始 SIOCGIFNAME ioctl
```

### 3.5 PRoot 伪造 `getuid()` 与 `/proc/self/*`

见 0.2。任何“验证身份”的代码在 PRoot 下都可能被骗，必须在外面读。

### 3.6 PRoot 不支持 `RTM_GETNEIGH`（带对照组的实测）

单看“邻居表 0 条”无法区分三种可能：PRoot 不实现 / 探针写错 / 内核确实没有条目。
所以仓库里带了一个带**对照组**的探针 [`tools/neigh_probe.c`](tools/neigh_probe.c)：
同一进程、同一时刻把 `RTM_GETADDR` 也跑一遍。ADDR 有数据而 NEIGH 没有，就排除了后两者。

```console
$ make probe                                  # 在 Termux 原生跑
  getuid()=10616  /proc 里的真 uid=10616  => 不在 PRoot 里
  [对照组] RTM_GETADDR
    ADDR   family=AF_UNSPEC SO_DOMAIN=16  DONE=yes entries=15 (报文 16 个)
  [待测]   RTM_GETNEIGH
    NEIGH  family=AF_UNSPEC SO_DOMAIN=16  DONE=yes entries=38 (报文 39 个)
    NEIGH  family=AF_INET   SO_DOMAIN=16  DONE=yes entries=3  (报文 4 个)
    NEIGH  family=AF_INET6  SO_DOMAIN=16  DONE=yes entries=35 (报文 36 个)

$ cp tools/neigh_probe .../rootfs/tmp/ && proot-distro login ubuntu --user he -- /tmp/neigh_probe
  getuid()=10617  /proc 里的真 uid=10616  => 在 PRoot 容器里
  [对照组] RTM_GETADDR
    ADDR   family=AF_UNSPEC SO_DOMAIN=1   DONE=yes entries=15 (报文 16 个)   ← 对照组有数据
  [待测]   RTM_GETNEIGH
    NEIGH  family=AF_UNSPEC SO_DOMAIN=1   DONE=yes entries=0  (报文 1 个)
    NEIGH  family=AF_INET   SO_DOMAIN=1   DONE=yes entries=0  (报文 1 个)
    NEIGH  family=AF_INET6  SO_DOMAIN=1   DONE=yes entries=0  (报文 1 个)
```

三点结论：

1. `SO_DOMAIN` 从 16 变成 1 —— 又是 PRoot 的 AF_UNIX 回退；
2. 对照组 ADDR 在容器里也拿到 **15** 条，与原生完全一样 —— 探针与环境都正常；
3. NEIGH 三种 family 都是 `DONE=yes entries=0`（**不是报错**）—— PRoot 的回退只实现了 ADDR，
   没实现邻居表。

所以“容器里没有 ARP”是 PRoot 的能力缺口，与程序无关；在宿主机上跑即可拿到 ARP/网关 MAC。

> 补充：早期我写过一个临时探针得到同样结论（NEIGHv4/v6 均 0 条），但那个文件在清理
> 临时文件时被删了，导致结论无法复现。现在这个探针连同对照组一起进了仓库
> （`make probe`），结论可随时重做。

顺带一个产物：这份容器内自测现在是个脚本（`test_proot.sh`）：先在 Termux 取参考值、
再进容器用 gcc 重编，然后断言**程序不变量**（退出码、stderr、JSON 结构、无占位符、
“为空必须有说明”），而把接口/路由/邻居的计数差异当**环境差异只做报告**（见 §0.6）。

### 3.7 proot-distro 拒绝嵌套：脚本必须知道自己在哪

在容器内部直接嗂 `make test-proot` 时，第 1 步把容器内的数据当成了“原生基准”，
然后第 3 步直接报：

```
Error: attempted to run proot-distro in a proot session. ...
```

两个问题：容器内的“基准”只是容器数据（对比无意义）；且 proot-distro 不允许嵌套。

**如何知道自己在不在容器里**：PRoot 会伪造 `getuid()`，但 `/proc/self/status` 里的 `Uid:`
是内核真值，两者不等就是在容器里：

```sh
fake=$(id -u)                                     # 10617（假）
real=$(awk '/^Uid:/{print $2}' /proc/self/status) # 10616（真）
[ "$fake" != "$real" ] && echo "在 proot 会话里"
```

`test_proot.sh` 据此分两种模式（同样的手法也曾用来窺探 `CapEff` 与 SELinux 上下文，见 §0.2）：

* **模式 A（宿主机）**：取原生基准 + 进容器对比
* **模式 B（容器内）**：不做对比，只断言容器侧不变量（邻居为 0、AF_UNIX 提示 3 次、
  无 `if%d` 占位符、stderr 为空、JSON/退出码）

另外，基准值全部**运行时现取**：设备网络状态是会变的（本项目开发过程中就从
10 接口/14+52 路由变成了 9 接口/12+41），写死数字必然假失败。

---

## 4. 代码层面的坑

| # | 坑 | 现象 | 修法 |
|---|---|---|---|
| 4.1 | `name_known()` 把兜底名 `if%d` 当成真名 | 首字符是 `'i'`，于是后续拿 `if12` 去问内核，白跑一堆 ioctl | 加 `name_real` 标志，只有真名才做 ioctl |
| 4.2 | `operstate > 0` 漏掉 `IF_OPER_UNKNOWN == 0` | 内核返回的合法值 0 被跳过，退化成用 flags 猜 | 初值用 `-1`，判断改 `>= 0` |
| 4.3 | `IN6_IS_ADDR_LINKLOCAL((struct in6_addr *)r->gw)` | `gw[16]` 是 `unsigned char[]`，对齐为 1，强转是 UB（交叉编译到严格对齐架构可能真炸） | `memcpy` 到局部 `struct in6_addr` 再判断 |
| 4.4 | `flags_str` 里 `strncat(b, s, n - strlen(b) - 1)` | 长度计算脆弱，理论上可下溢 | 改成 `snprintf` 追加 |
| 4.5 | 目录里的 `ipinfo` 二进制与 `ipinfo.c` 不一致 | 二进制是从 `ipinfo.bak.c` 编的，`-h` 显示的选项和源码不一样 | `make` 重新编译，并加 `Makefile` + `make check` |
| 4.6 | `-4 -6` 同时给出 | `w4=w6=0`，什么都不显示 | 两者都为 0 时视为不限制 |
| 4.7 | `nlmsg_len = sizeof(req)` | 含尾部填充（见 1.5） | 用 `NLMSG_LENGTH(sizeof(req.g))` |

---

## 5. 复核清单

改动之后按这个顺序过一遍，基本不会漏：

```sh
make && make check

# 0) 邻居表能力探针（带对照组，用来区分“环境不支持”与“探针写错”）
make probe

# 1) 原生环境：不得卡死（曾经因 nlmsg_pid 阻塞）
timeout 10 ./ipinfo -v -s

# 2) 目标调用方式：真实身份
timeout 60 proot-distro login ubuntu --user he -- /tmp/ipinfo -v -i wlan0

# 3) 有权限基准对照（值必须一致）
adb shell ifconfig wlan0        # mtu / txqueuelen / flags / addr / bcast / mask
adb shell ip route show table all | head

# 4) 真 uid / 上下文（从外面读，防 PRoot 伪造）
timeout 30 proot-distro login ubuntu --user he -- sleep 20 &
grep -E '^(Uid|CapEff)' /proc/<pid>/status

# 5) 路由集合级对照（归一化表示后差集应为空）
#    IPv4: 14/14，IPv6: 49/49
python3 cmp_routes.py     # 归一化 fe80:: ↔ fe80::/128、multicast ↔ ff00::/8

# 6) 邻居表对照（默认视图必须完全相同；nud all 的差异只做信息输出）
python3 cmp_neigh.py      # 默认视图 10 == 10（随环境变化）

# 6.5) 邻居表在容器内为什么是 0：跑探针，看对照组 ADDR 有没有数据
#      make probe  # 原生
#      拷进容器再跑  # 容器内：ADDR 有数据、NEIGH 全 0 → PRoot 的能力缺口

# 7) 进 proot 容器重测（自动识别模式：宿主机做对比，容器内只查不变量）
#    宿主机：make test-proot    容器内：bash test_proot.sh
make test-proot

# 8) socket 真身（防 AF_NETLINK 被换）
./ipinfo -v               # 出现 "actually domain 1" 就说明是 PRoot 仿真

# 9) 内存检查
aarch64-linux-android-clang -O1 -g -fsanitize=address,undefined -o /tmp/ipinfo_asan ipinfo.c
./ipinfo_asan -l -j >/dev/null && python3 -c 'import json;json.load(open("/dev/stdin"))'
```

## 以上内容均为AI的总结，不是本人开发的，本人只是基于一个小的需求需要，所以说才开发的

## Note (English)

This file is intentionally Chinese-only — no hand-written translation is planned. If you need it in
another language, just throw it at an AI (or any other translation tool).

All values quoted in this file are real measurements taken on the device and can be reproduced;
§5 (复核清单) is a step-by-step verification checklist.
