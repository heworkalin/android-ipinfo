# ipinfo

**Dig out a device's network information on a restricted Android box — no root, no adb authorization.**

A single-file, dependency-free command-line tool written in plain C. It runs inside Termux
(`untrusted_app`, uid 1xxxx) and uses a **combination of netlink and ioctl** to work around the
information hiding Android applies to ordinary apps: interfaces, IPs, netmasks, broadcast
addresses, gateways, MTU, queue length, interface state and the routing table.

> **中文版: [README.md](README.md)**
>
> **Disclosure**
>
> Code and docs in this repo were produced with an AI coding agent
> ([pi.dev](https://pi.dev), model `deepseek-v4-flash`); the author only provided the requirement.
> Every "measured" figure is real command output from the device and can be reproduced.

---

## 1. What problem does this solve?

Android hides nearly all low-level network information from ordinary apps (`untrusted_app`).
Measured on this device (Android 15):

| Route | Result |
|---|---|
| `/proc/net/*` (dev, route, arp, if_inet6 …) | `EACCES` |
| `/sys/class/net/*` (mtu, address, operstate …) | `EACCES` |
| netlink `RTM_GETLINK` dump (this is what `ip link` uses) | `sendto: EACCES` |
| netlink `RTM_GETADDR` / `RTM_GETROUTE` | **works** |
| ioctl `SIOCGIF*` (with a real interface name) | **works** |

That second column is not "apps can't, but the shell can" — **even `adb shell` (uid=2000,
authorized) is refused**:

```console
$ adb shell id
uid=2000(shell) ... context=u:r:shell:s0
$ adb shell ip link show
request send failed: Permission denied
$ adb shell cat /sys/class/net/wlan0/mtu
cat: /sys/class/net/wlan0/mtu: Permission denied
```

In other words, on this device "looking at a NIC" is **root-only**. But the information you
actually need day to day (the current Wi-Fi IP, netmask, gateway, MTU, routing table) *is*
reachable by an unprivileged process — you just have to take a different path. That is what this
tool does.

## 2. Measured runtime environment

| Item | Value |
|---|---|
| Device / OS | PJE110 / Android 15 (SDK 35) |
| Kernel | `Linux 5.15.167-android13-8-o-01144 aarch64` |
| Runtime | Termux (native) + `proot-distro` Ubuntu 24.04 |
| Process identity | `uid=10616`, `u:r:untrusted_app_27:s0:c104,c258,c512,c768` |
| Capabilities | `CapEff = 0000000000000000` (no root, no `CAP_NET_ADMIN`) |
| Toolchain | `aarch64-linux-android-clang` (NDK r29, bionic target) |
| Reference oracle | `adb shell` (uid=2000, `u:r:shell:s0`, authorized on 127.0.0.1:5555) |

## 3. Results: three-way comparison against a privileged baseline

Same device, same moment, `wlan0`:

```console
# 1) adb shell (uid=2000, adb authorization required)
$ adb shell ifconfig wlan0
wlan0  Link encap:UNSPEC  Driver cnss_pci
       inet addr:192.168.10.2  Bcast:192.168.10.255  Mask:255.255.255.0
       inet6 addr: fe80::8488:9202:9fb3:3e69/64 Scope: Link
       UP BROADCAST RUNNING MULTICAST  MTU:1500  Metric:1
       collisions:0 txqueuelen:3000

$ adb shell ip route show table all | head -1
default via 192.168.10.1 dev wlan0 table 1027 proto static

$ adb shell ip link show          # amusingly, this one fails
request send failed: Permission denied

# 2) ipinfo (uid=10616, zero authorization, native Termux)
$ ./ipinfo -i wlan0
wlan0  (index 27, mtu 1500, qlen 3000, state up)
  flags: UP,BROADCAST,RUNNING,MULTICAST
  MAC : (unavailable - hidden by Android)
  IPv4: 192.168.10.2/24  mask 255.255.255.0  brd 192.168.10.255
  IPv6: fe80::8488:9202:9fb3:3e69%wlan0/64  (link-local)

default routes:
  IPv4 table 1027          dev wlan0    default    gw 192.168.10.1    metric 0
```

The values match the baseline (`192.168.10.2/24`, `brd .255`, `gw 192.168.10.1`, `mtu 1500`,
`txqueuelen 3000`, table `1027`). The MAC is unavailable — **even the shell cannot get it**; that
is a blanket Android policy.

### Full routing table, compared as a set

The route set printed by `ipinfo -l` was compared against the privileged `adb shell` as a set.
After normalizing representation differences (`fe80::` ↔ `fe80::/128`, `multicast` ↔ `ff00::/8`)
they are **identical**:

| | adb `ip [-4/-6] route show table all` | `ipinfo -l` | intersection | diff |
|---|---|---|---|---|
| IPv4 | 14 | 14 | 14 | empty |
| IPv6 | 49 | 49 | 49 | empty |

### Other interfaces and script-friendly output

`ipinfo` can also list every interface and route in one go:

```console
$ ./ipinfo -s            # one line per interface, script friendly
lo ipv4=127.0.0.1/8 ipv6=::1/128 state=up
dummy0 ipv6=fe80::34bb:bdff:fe0d:64ad%dummy0/64 state=up
...
wlan0 ipv4=192.168.10.2/24 ipv6=fe80::8488:9202:9fb3:3e69%wlan0/64 state=up

$ ./ipinfo -j | jq .     # JSON, easy for programs to consume
```

## 4. How it works

It walks two paths and uses whichever is available, then **corrects possibly-emulated data with
kernel-direct ioctls**:

1. **netlink** (`NETLINK_ROUTE`, unbound):
   * `RTM_GETADDR` → addresses + prefix lengths (v4/v6)
   * `RTM_GETROUTE` → default routes, including Android's per-network tables (1027, 1000000027, …)
   * `RTM_GETLINK` → interface name / flags / mtu / operstate / MAC
     (refused in the native environment on this device; under proot PRoot fakes it as a success — see below)
2. **ioctl** (`SIOCGIF*`, needs the real interface name):
   * fills in `mtu`, `tx_queue_len`, `flags`
   * fills in the IPv4 address, `netmask`, `broadcast`
   * `mtu` / `txqlen` **always come from ioctl**: PRoot's emulated netlink reports `wlan0`'s
     txqlen as 1000, while ioctl and `adb shell ifconfig` both say 3000
3. **Three-level name fallback**: netlink `IFLA_IFNAME` → `if_indextoname()` → raw `SIOCGIFNAME`
   ioctl. (The third step is not decoration: glibc's `if_indextoname` returns `EACCES` under
   PRoot, but the raw ioctl works.)
4. **The netmask is recomputed from the prefix length**, so it does not depend on
   `SIOCGIFNETMASK` succeeding.

A failing netlink request degrades silently by default; `-v` prints the reason — including a
notice about PRoot's `AF_NETLINK → AF_UNIX` substitution, so emulated data is not mistaken for
kernel data.

## 5. Usage

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

Exit codes: `0` success, `1` bad option, `2` no matching interface.

```sh
ipinfo                     # everything
ipinfo -i wlan0            # only wlan0 (routes are filtered by interface too)
ipinfo -4 -s               # one line per interface, IPv4 only
ipinfo -l -6               # all IPv6 routes (including connected), not just default
ipinfo -j | jq .           # structured output
```

## 6. Building

```sh
make            # uses Termux's bundled aarch64-linux-android-clang by default
make check      # smoke test
make install    # install to $PREFIX/bin
```

Or manually:

```sh
aarch64-linux-android-clang -O2 -Wall -Wextra -Wpedantic -std=c11 -o ipinfo ipinfo.c
```

Single file, no third-party dependencies, zero warnings under `-Wall -Wextra -Wpedantic`,
clean under ASan/UBSan.

## 7. Known limitations

* **Interfaces with no address at all cannot be enumerated.** `ifconfig -a` lists **30**
  interfaces (it can read `/proc/net/dev`), but `untrusted_app` gets `EACCES` on that file and
  `if_nameindex()` (which goes through `RTM_GETLINK`) is refused as well, so `ipinfo -s` can only
  list the **10** interfaces that carry an address. This is a **hard limitation**, not an
  implementation defect.
* **No MAC address**: Android hides it from third-party apps; `SIOCGIFHWADDR` and netlink
  `IFLA_ADDRESS` both come back empty. `adb shell` cannot get it either.
* **Some fields are emulated under PRoot**: `operstate`, `LOWER_UP` and friends come from PRoot's
  synthesized replies there, which is less trustworthy than native ioctl; `mtu`/`txqlen` have
  been forced over to ioctl to get the real values.

## 8. How it was developed

This project was **developed with the help of an AI coding agent**:

* Tool: [**pi.dev**](https://pi.dev) (`@earendil-works/pi-coding-agent`)
* Model: **`deepseek-v4-flash`** (provider: `deepseek`)

The measurement log, pitfalls and verification methods are collected in
[`PITFALLS.md`](PITFALLS.md) (Chinese). Every "measured" figure in this document comes from real
device command output and is reproducible.

## 9. License

[MIT](LICENSE) © 2026 heworkalin
