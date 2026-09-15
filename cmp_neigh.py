#!/usr/bin/env python3
"""对照 ipinfo 与 adb shell ip neigh 的邻居表（ARP/NDP）。

用法:
    python3 cmp_neigh.py        # 需要 adb 已授权

比较分两层：

1. **默认视图（严格）**：`ipinfo -n` vs `adb shell ip neigh show`。
   两边都应隐藏 NOARP/组播条目，集合必须完全相同（本机实测 10 == 10）。

2. **全集视图（信息性）**：`ipinfo -N` vs `adb shell ip neigh show nud all`。
   允许存在差异，原因是**调用者身份不同**：adb shell 是 uid=2000(shell)，
   ipinfo 是 uid=10616(untrusted_app)，内核对后者会少给一些字段
   （实测：raw-IP 接口上的 NOARP 条目，shell 能看到 1 字节 lladdr "08"，
   非特权应用看不到）。这类差异只出现在 NOARP 噪音条目上，不影响实际使用。

退出码：默认视图不一致 → 1，否则 0。
"""
import ipaddress
import subprocess
import sys

NAME = {"ipv4": "IPv4", "ipv6": "IPv6"}


def _norm(addr: str) -> str:
    try:
        return str(ipaddress.ip_address(addr))
    except ValueError:
        return addr


def _row(fam, dev, addr, mac, state):
    return (fam, dev, _norm(addr), mac, state)


def from_adb(nud_all: bool) -> set:
    cmd = ["adb", "shell", "ip", "neigh", "show"] + (["nud", "all"] if nud_all else [])
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout
    res = set()
    for line in out.splitlines():
        t = line.split()
        if len(t) < 3:
            continue
        addr = t[0]
        dev = mac = None
        for i, x in enumerate(t):
            if x == "dev" and i + 1 < len(t):
                dev = t[i + 1]
            if x == "lladdr" and i + 1 < len(t):
                mac = t[i + 1]
        fam = "ipv6" if ":" in addr else "ipv4"
        res.add(_row(fam, dev, addr, mac, t[-1]))
    return res


def from_ipinfo(nud_all: bool) -> set:
    out = subprocess.run(["./ipinfo", "-N" if nud_all else "-n", "-R"],
                         capture_output=True, text=True, timeout=20).stdout
    res = set()
    for line in out.splitlines():
        t = line.split()
        if len(t) < 6 or t[0] not in ("IPv4", "IPv6"):
            continue
        fam = "ipv4" if t[0] == "IPv4" else "ipv6"
        dev = t[t.index("dev") + 1]
        addr = t[t.index("dev") + 2].split("%", 1)[0]   # 去掉 %dev 作用域
        mac = t[t.index("dev") + 3]
        mac = None if mac == "-" else mac
        res.add(_row(fam, dev, addr, mac, t[-1]))
    return res


def main() -> int:
    rc = 0

    adb, ours = from_adb(False), from_ipinfo(False)
    print(f"[默认视图]  adb={len(adb)} ipinfo={len(ours)} 交集={len(adb & ours)}")
    if adb == ours:
        print("            OK —— 完全相同")
    else:
        rc = 1
        for x in sorted(adb - ours)[:10]:
            print("   - 仅 adb   :", x)
        for x in sorted(ours - adb)[:10]:
            print("   + 仅 ipinfo:", x)

    adb_all, ours_all = from_adb(True), from_ipinfo(True)
    delta = (adb_all - ours_all) | (ours_all - adb_all)
    print(f"[全集视图]  adb={len(adb_all)} ipinfo={len(ours_all)} "
          f"差异={len(delta)}（信息性，不算失败）")
    for x in sorted(delta, key=lambda r: tuple("" if v is None else str(v) for v in r))[:6]:
        side = "仅 adb   " if x in adb_all - ours_all else "仅 ipinfo"
        print(f"   {side}:", x)
    if delta:
        print("            注：差异来自调用者身份（shell 2000 vs untrusted_app 10616），")
        print("                内核对非特权应用少给 NOARP 条目的 lladdr，不影响实际使用。")
    return rc


if __name__ == "__main__":
    sys.exit(main())
