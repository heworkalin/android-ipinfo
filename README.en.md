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
| Runtime | Termux native; `proot-distro` Ubuntu 24.04; **TMOE** `tmoe proot ubuntu noble arm64` (Ubuntu 24.04.5) |
| Process identity | `uid=10616`, `u:r:untrusted_app_27:s0:c104,c258,c512,c768` |
| Capabilities | `CapEff = 0000000000000000` (no root, no `CAP_NET_ADMIN`) |
| Toolchain | `aarch64-linux-android-clang` (NDK r29, bionic target) |
| Reference oracle | `adb shell` (uid=2000, `u:r:shell:s0`, authorized on 127.0.0.1:5555) |

Both proot flavors (proot-distro / TMOE) behave identically here (same Termux `proot`):
`AF_NETLINK` is replaced by `AF_UNIX`, the neighbor table is empty and part of the routing table is
missing; TMOE's fake root is uid=0. TMOE's container also lacks `/linkerconfig`, so running a bionic
binary makes the Android linker print `failed to find generated linker configuration` on stderr —
unrelated to this program (a minimal hello-world prints it too).

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

### Neighbor (ARP/NDP) table — including the **gateway's MAC**

The local NIC MAC is hidden by Android, but peers' MACs are obtainable from the neighbor table:

```console
$ ./ipinfo -n -R -i wlan0 | sed -n '/^neighbors/,$p'
neighbors (NOARP/multicast hidden, use -N for all):
  IPv4 dev wlan0          192.168.10.1                               f4:bf:bb:bb:02:b9  REACHABLE
  IPv4 dev wlan0          192.168.10.3                               a4:4b:d5:9a:c7:83  STALE
  IPv4 dev wlan0          192.168.10.18                              -                  FAILED
  IPv6 dev wlan0          fe80::7793:7ee:e499:703a%wlan0             a4:4b:d5:9a:c7:83  STALE
```

Against the baseline (`make compare`):

| | adb `ip neigh show` | `ipinfo -n` | Result |
|---|---|---|---|
| default view (NOARP/multicast hidden) | 10 | 10 | **identical** |
| `nud all` | 49 | 49 | same count; on 26 NOARP entries `shell` can additionally see a 1-byte bogus lladdr `08` that an unprivileged app cannot (see PITFALLS) |

`RTM_GETNEIGH` works in native Termux; **under proot PRoot does not support this request
(returns 0 entries)**. Rather than printing nothing, it says so explicitly:

```console
neighbors (nud all): (none)
  note: RTM_GETNEIGH returned 0 entries and netlink here is emulated
        (PRoot replaced AF_NETLINK with AF_UNIX, whose fallback does
        not implement the neighbor table). Run ipinfo outside proot
        (Termux host) to get ARP/NDP entries.
```

`-a` also issues this request once, to discover interfaces that carry no address
(see [§7 Known limitations](#7-known-limitations)).

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
   * `RTM_GETNEIGH` → the neighbor / ARP table (shown with `-n`/`-N`; also sent once with `-a`
     for interface discovery; the only way to obtain peer MACs)
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
  -a         also show interfaces without an address (discovers some from routes/neighbors)
  -m         show MAC (default)
  -M         hide MAC
  -r         show routes (default)
  -R         hide routes
  -l         all routes (connected/prefix routes too), not just default
  -n         show the ARP/NDP neighbor table (NOARP/multicast hidden)
  -N         like -n, but include NOARP/multicast entries too
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
ipinfo -n                  # neighbor table (ARP/NDP); reveals the gateway MAC
ipinfo -j | jq .           # structured output
```

## 6. Building

```sh
make            # uses Termux's bundled aarch64-linux-android-clang by default
make check      # smoke test
make compare    # set-level comparison against a privileged adb shell (needs adb)
make test-proot # rebuild with the container's gcc (glibc) and run the full self-test inside proot
make install    # install to $PREFIX/bin
```

`make test-proot` (i.e. `bash test_proot.sh`) **detects where it is running** (by comparing
`getuid()` with the real uid in `/proc/self/status`, since PRoot fakes the former):

* **Mode A, on the Termux host**: take host reference values → copy the source into the container →
  build with the **container's own gcc** → run the assertions
* **Mode B, inside the container**: run the same assertions against the current environment

The script keeps two categories strictly apart:

| | Content | Treatment |
|---|---|---|
| **Program invariants** | exit codes 0/1/2; empty stderr without `-v`; with `-v` stderr contains only `ipinfo: ` diagnostics; complete JSON structure; no `if<N>` placeholders; non-empty output; **an empty neighbor table must be explained, never silent** | **asserted** — a failure is a real bug |
| **Environment differences** | neighbor count under proot, number of AF_UNIX notices, host-vs-container count differences | **reported only**, never judged |

This split matters: PRoot lacking the neighbor table is a **capability gap of the current test
environment**, not a property the program should have. Asserting "neighbors must be 0" would make
the test fail the day PRoot gains that capability — punishing an improvement.
All reference values are taken **at runtime**, never hard-coded, so network state changes cannot
cause false failures.

Or manually:

```sh
aarch64-linux-android-clang -O2 -Wall -Wextra -Wpedantic -std=c11 -o ipinfo ipinfo.c
```

Single file, no third-party dependencies, zero warnings under `-Wall -Wextra -Wpedantic`,
clean under ASan/UBSan.

## 7. Known limitations

* **Not all interfaces can be enumerated.** `ifconfig -a` lists **30** interfaces (it can read
  `/proc/net/dev`), but `untrusted_app` gets `EACCES` on that file and `if_nameindex()` (which goes
  through `RTM_GETLINK`) is refused. What this tool can do:
  * **10** interfaces with addresses, from `RTM_GETADDR` (the default view)
  * with `-a`, **5** more address-less interfaces are recovered by resolving the ifindexes seen in
    the **route and neighbor tables** (measured here: `gretap0`, `erspan0`, `wlan1`, `p2p0`,
    `wifi-aware0`) — **15** in total
  * the remaining **15** (`gre0`, `sit0`, `tunl0`, `ip_vti0`, `rmnet_data3…6`, …) are **referenced by
    no route and no neighbor entry**, so there is no way to learn they exist — a genuine hard limit.
* **No MAC address**: Android hides it from third-party apps; `SIOCGIFHWADDR` and netlink
  `IFLA_ADDRESS` both come back empty. `adb shell` cannot get it either.
* **Under PRoot the data is emulated and may be incomplete**: `operstate`, `LOWER_UP` and friends
  come from PRoot's synthesized replies, and the routing table can be missing entries — measured at
  the same moment: native **175 IPv4 + 45 IPv6** routes versus **156 + 45** inside the container
  (the container's own `ip -4 route` also reports 156, so this is a PRoot capability gap).
  `mtu`/`txqlen` have been forced over to ioctl for real values; **run on native Termux for exact data**.

> Diagnostics: `make probe` runs two controlled probes (`tools/neigh_probe`, `tools/family_probe`)
> that tell apart "environment limitation" from "program bug" for the empty neighbor table and for
> the `AF_UNSPEC` request returning IPv4 only.

## 8. How it was developed

This project was **developed with the help of an AI coding agent**:

* Tool: [**pi.dev**](https://pi.dev) (`@earendil-works/pi-coding-agent`)
* Model: **`deepseek-v4-flash`** (provider: `deepseek`)

The measurement log, pitfalls and verification methods are collected in
[`PITFALLS.md`](PITFALLS.md) (Chinese). Every "measured" figure in this document comes from real
device command output and is reproducible.

## 9. License

[MIT](LICENSE) © 2026 heworkalin
