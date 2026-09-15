#!/usr/bin/env python3
"""对照 ipinfo -l 与 adb shell ip route show table all 的路由集合。

用法:
    python3 cmp_routes.py            # 需要 adb 已授权且 adb shell ip 可用

两者表示法不同，归一化后再比：
    fe80::            <-> fe80::/128
    multicast         <-> ff00::/8
    table local/main  <-> local(255) / main(254)
差集为空才算通过。本机（PJE110/Android 15）实测：
    IPv4 14 == 14，IPv6 49 == 49，差集均为空。
"""
import ipaddress
import re
import subprocess
import sys

NAME2ID = {"local": 255, "main": 254, "default": 253, "unspec": 0, "compat": 252}
KEYWORDS = {"local", "broadcast", "unreachable", "anycast", "throw", "nat"}


def _norm_dst(dst: str) -> str:
    if dst == "multicast":
        return "ff00::/8"
    if "/" not in dst:
        suffix = "/128" if ":" in dst else "/32"
        try:
            return str(ipaddress.ip_network(dst + suffix, strict=False))
        except ValueError:
            return dst
    try:
        return str(ipaddress.ip_network(dst, strict=False))
    except ValueError:
        return dst


def _table_id(tok: str) -> int:
    return int(tok) if tok.isdigit() else NAME2ID.get(tok, 0)


def from_adb(v6: bool) -> set:
    fam = "-6" if v6 else "-4"
    out = subprocess.run(["adb", "shell", "ip", fam, "route", "show", "table", "all"],
                         capture_output=True, text=True, timeout=30).stdout
    res = set()
    for line in out.splitlines():
        t = line.split()
        if not t:
            continue
        dst = t[1] if t[0] in KEYWORDS and len(t) > 1 else t[0]
        dev = next((t[i + 1] for i, x in enumerate(t) if x == "dev"), "-")
        tbl = next((t[i + 1] for i, x in enumerate(t) if x == "table"), "main")
        res.add((_table_id(tbl), dev, _norm_dst(dst)))
    return res


def from_ipinfo(v6: bool) -> set:
    out = subprocess.run(["./ipinfo", "-l", "-6" if v6 else "-4"],
                         capture_output=True, text=True, timeout=20).stdout
    res = set()
    for line in out.splitlines():
        t = line.split()
        if len(t) < 8 or t[0] not in ("IPv4", "IPv6"):
            continue
        tbl = re.sub(r"\(.*", "", t[t.index("table") + 1])
        dev = t[t.index("dev") + 1]
        dst = t[t.index("dev") + 2]
        res.add((_table_id(tbl), dev, _norm_dst(dst)))
    return res


def main() -> int:
    ok = True
    for v6 in (False, True):
        label = "IPv6" if v6 else "IPv4"
        adb, ours = from_adb(v6), from_ipinfo(v6)
        only_adb, only_ours = sorted(adb - ours), sorted(ours - adb)
        status = "OK" if not only_adb and not only_ours else "DIFF"
        print(f"[{label}] adb={len(adb)} ipinfo={len(ours)} "
              f"交集={len(adb & ours)} 仅adb={len(only_adb)} 仅ipinfo={len(only_ours)}  {status}")
        for x in only_adb[:5]:
            print("   - 仅 adb   :", x)
        for x in only_ours[:5]:
            print("   + 仅 ipinfo:", x)
        ok = ok and not only_adb and not only_ours
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
