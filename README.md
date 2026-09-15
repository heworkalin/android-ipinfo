# ipinfo

**在 Android 受限环境（无 root、无 adb 授权）下，把本机网络信息尽量挖出来。**

一个纯 C、零第三方依赖的单文件命令行工具，跑在 Termux（`untrusted_app`，uid 1xxxx）里，
用 **netlink + ioctl 的组合**绕过 Android 对普通应用的隐藏，拿到接口、IP、掩码、广播、网关、
MTU、队列长度、接口状态和路由表。

> **声明 / Disclosure**
>
> 本仓库的代码与文档由 AI 编程助手生成与整理（[pi.dev](https://pi.dev)，模型 `deepseek-v4-flash`），
> 作者只提出需求。文中所有“实测”数据均来自真机命令输出，可自行复现验证。
>
> Code and docs in this repo were produced with an AI coding agent
> ([pi.dev](https://pi.dev), model `deepseek-v4-flash`); the author only provided the requirement.
> Every "measured" figure is real command output from the device and can be reproduced.

---

## 1. 这个工具解决什么问题

Android 对普通应用（`untrusted_app`）隐藏了几乎全部网络底层信息。实测本机（Android 15）：

| 途径 | 结果 |
|---|---|
| `/proc/net/*`（dev、route、arp、if_inet6 …） | `EACCES` |
| `/sys/class/net/*`（mtu、address、operstate …） | `EACCES` |
| netlink `RTM_GETLINK` dump（`ip link` 走的就是它） | `sendto: EACCES` |
| netlink `RTM_GETADDR` / `RTM_GETROUTE` | **可用** |
| ioctl `SIOCGIF*`（真实接口名） | **可用** |

注意第二列不是"应用不行、shell 就行"——**连 `adb shell`（uid=2000，需要授权）也被拒**：

```console
$ adb shell id
uid=2000(shell) ... context=u:r:shell:s0
$ adb shell ip link show
request send failed: Permission denied
$ adb shell cat /sys/class/net/wlan0/mtu
cat: /sys/class/net/wlan0/mtu: Permission denied
```

也就是说，在这台设备上"看网卡"这件事**只有 root 能做**。而日常真正需要的信息
（当前 Wi-Fi 的 IP、掩码、网关、MTU、路由表）都是可以被非特权程序拿到的，
只是需要换一条路。本工具做的就是这件事。

## 2. 实测运行环境

| 项 | 值 |
|---|---|
| 设备 / 系统 | PJE110 / Android 15 (SDK 35) |
| 内核 | `Linux 5.15.167-android13-8-o-01144 aarch64` |
| 运行环境 | Termux（原生）+ `proot-distro` Ubuntu 24.04 |
| 进程身份 | `uid=10616`，`u:r:untrusted_app_27:s0:c104,c258,c512,c768` |
| 能力 | `CapEff = 0000000000000000`（无 root、无 `CAP_NET_ADMIN`） |
| 工具链 | `aarch64-linux-android-clang`（NDK r29，bionic 目标） |
| 对比基准 | `adb shell`（uid=2000，`u:r:shell:s0`，已授权 127.0.0.1:5555） |

## 3. 效果：和"有权限"的基准三方对照

同一台设备，同一时刻，`wlan0`：

```console
# ① adb shell（uid=2000，需要 adb 授权）
$ adb shell ifconfig wlan0
wlan0  Link encap:UNSPEC  Driver cnss_pci
       inet addr:192.168.10.2  Bcast:192.168.10.255  Mask:255.255.255.0
       inet6 addr: fe80::8488:9202:9fb3:3e69/64 Scope: Link
       UP BROADCAST RUNNING MULTICAST  MTU:1500  Metric:1
       collisions:0 txqueuelen:3000

$ adb shell ip route show table all | head -1
default via 192.168.10.1 dev wlan0 table 1027 proto static

$ adb shell ip link show          # 有趣的是这条反而失败
request send failed: Permission denied

# ② ipinfo（uid=10616，零授权，Termux 原生）
$ ./ipinfo -i wlan0
wlan0  (index 27, mtu 1500, qlen 3000, state up)
  flags: UP,BROADCAST,RUNNING,MULTICAST
  MAC : (unavailable - hidden by Android)
  IPv4: 192.168.10.2/24  mask 255.255.255.0  brd 192.168.10.255
  IPv6: fe80::8488:9202:9fb3:3e69%wlan0/64  (link-local)

default routes:
  IPv4 table 1027          dev wlan0    default    gw 192.168.10.1    metric 0
```

结果与基准一致（`192.168.10.2/24`、`brd .255`、`gw 192.168.10.1`、`mtu 1500`、
`txqueuelen 3000`、table `1027`）。MAC 拿不到——这一条**连 shell 也拿不到**，是 Android 的统一策略。

### 全量路由表对照（集合级）

`ipinfo -l` 输出的路由集与有权限的 `adb shell` 做过集合级对比，归一化表示差异
（`fe80::` ↔ `fe80::/128`、`multicast` ↔ `ff00::/8`）后**完全相同**：

| | adb `ip [-4/-6] route show table all` | `ipinfo -l` | 交集 | 差集 |
|---|---|---|---|---|
| IPv4 | 14 | 14 | 14 | 空 |
| IPv6 | 49 | 49 | 49 | 空 |

### 其余接口与脚本化输出

`ipinfo` 还能一次列出全部接口和路由：

```console
$ ./ipinfo -s            # 每个接口一行，脚本友好
lo ipv4=127.0.0.1/8 ipv6=::1/128 state=up
dummy0 ipv6=fe80::34bb:bdff:fe0d:64ad%dummy0/64 state=up
...
wlan0 ipv4=192.168.10.2/24 ipv6=fe80::8488:9202:9fb3:3e69%wlan0/64 state=up

$ ./ipinfo -j | jq .     # JSON，便于程序消费
```

## 4. 它是怎么做到的

同时走两条路，谁可用用谁，并**用内核直通的 ioctl 校正可能的仿真数据**：

1. **netlink**（`NETLINK_ROUTE`，不 bind）：
   * `RTM_GETADDR` → 地址 + 前缀长度（v4/v6）
   * `RTM_GETROUTE` → 默认路由，含 Android 的 per-network 表（1027、1000000027…）
   * `RTM_GETLINK` → 接口名 / flags / mtu / operstate / MAC
     （本机原生环境此请求被拒，proot 下会被 PRoot 伪造成"成功"，见下）
2. **ioctl**（`SIOCGIF*`，需要真实接口名）：
   * 补 `mtu`、`tx_queue_len`、`flags`
   * 补 IPv4 地址、`netmask`、`broadcast`
   * `mtu`/`txqlen` **一律以 ioctl 为准**：实测 PRoot 的仿真 netlink 把 `wlan0`
     的 txqlen 报成 1000，而 ioctl 与 `adb shell ifconfig` 都是 3000
3. **名字三级兜底**：netlink `IFLA_IFNAME` → `if_indextoname()` → 原始 `SIOCGIFNAME` ioctl。
   （第三步不是摆设：glibc 的 `if_indextoname` 在 PRoot 下会 `EACCES`，但原始 ioctl 可用。）
4. **netmask 由 prefixlen 反算**，不依赖 `SIOCGIFNETMASK` 是否成功。

任一 netlink 请求失败时默认静默降级，`-v` 才输出原因——包括提示 PRoot 的
`AF_NETLINK → AF_UNIX` 伪装，避免把仿真数据当内核数据。

## 5. 用法

```
usage: ipinfo [options]
  -i IFACE   only this interface (name or ifindex)
  -4         IPv4 only
  -6         IPv6 only
  -u         only interfaces that are IFF_UP
  -a         also show interfaces without any address
  -m         show MAC (default)
  -M         hide MAC
  -r         show routes (default)
  -R         hide routes
  -l         all routes (connected/prefix routes too), not just default
  -j         JSON output
  -s         compact one-line-per-interface output (no routes)
  -v         verbose: also print fallback/failure reasons to stderr
  -h         show this help
```

退出码：`0` 正常，`1` 参数错误，`2` 没有匹配的接口。

```sh
ipinfo                     # 完整信息
ipinfo -i wlan0            # 只看 wlan0（路由也会按接口过滤）
ipinfo -4 -s               # 每接口一行，只要 IPv4
ipinfo -l -6               # 全部 IPv6 路由（含直连），不只是 default
ipinfo -j | jq .           # 结构化输出
```

## 6. 构建

```sh
make            # 默认用 Termux 自带的 aarch64-linux-android-clang
make check      # 冒烟测试
make install    # 装到 $PREFIX/bin
```

也可以手动：

```sh
aarch64-linux-android-clang -O2 -Wall -Wextra -Wpedantic -std=c11 -o ipinfo ipinfo.c
```

单文件、无第三方依赖、`-Wall -Wextra -Wpedantic` 零警告、ASan/UBSan 干净。

## 7. 已知限制

* **枚举不出"完全没有地址"的接口**。`ifconfig -a` 能列出 **30** 个接口（它读得到 `/proc/net/dev`），
  而 `untrusted_app` 对该文件 `EACCES`，`if_nameindex()`（走 `RTM_GETLINK`）也被拒，
  所以 `ipinfo -s` 只能列出有地址的 **10** 个接口。这是**硬限制**，不是实现缺陷。
* **MAC 地址拿不到**：Android 对第三方应用隐藏，`SIOCGIFHWADDR` 与 netlink `IFLA_ADDRESS`
  都返回空；这一条 `adb shell` 同样拿不到。
* **PRoot 下部分字段是仿真的**：`operstate`、`LOWER_UP` 等在 proot 来自 PRoot 的合成回复，
  不如原生 ioctl 可靠；`mtu`/`txqlen` 已强制改用 ioctl 取真值。

## 8. 开发方式

本项目**借助 AI 编程工具开发**：

* 工具：[**pi.dev**](https://pi.dev)（`@earendil-works/pi-coding-agent`）
* 模型：**`deepseek-v4-flash`**（provider: `deepseek`）

开发过程中的实测记录、踩坑与验证方法整理在 [`PITFALLS.md`](PITFALLS.md)。
本文档中所有"实测""实测环境"的数据都来自真机命令输出，可复现。
