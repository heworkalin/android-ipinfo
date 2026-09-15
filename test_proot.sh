#!/data/data/com.termux/files/usr/bin/bash
#
# test_proot.sh — 在 proot-distro 容器【内部】跑完整自测
#
# 用法:
#   bash test_proot.sh [DISTRO] [USER]        # 默认 ubuntu he
#
# 做法与判据:
#   1. 先在 Termux 原生跑一遍，取到基准计数
#   2. 把 ipinfo.c 拷进容器的 /tmp/ipinfo_selftest，用容器里的 gcc(glibc) 编译
#   3. 在容器内部运行，断言：
#        - 接口数 / 地址行 / 默认路由 / 全路由 计数  == 原生基准（应当完全相同）
#        - 邻居表(-n 与 -N) == 0（PRoot 的合成 netlink 不支持 RTM_GETNEIGH）
#        - stderr 为空、JSON 合法、退出码正确、-h 可用
#
# 之所以要断言“邻居 == 0”，是因为这正是 PRoot 的能力缺口：PRoot 会把
# socket(AF_NETLINK) 悄悄换成 AF_UNIX，其合成回复里没有邻居表。
set -uo pipefail

DISTRO="${1:-ubuntu}"
USERNAME="${2:-he}"
ROOTFS="${IPINFO_ROOTFS:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/$DISTRO/rootfs}"
WORK=/tmp/ipinfo_selftest

if [ ! -d "$ROOTFS" ]; then
    echo "找不到容器 rootfs: $ROOTFS" >&2
    echo "（可用 IPINFO_ROOTFS=... 覆盖）" >&2
    exit 1
fi
if [ ! -x ./ipinfo ]; then
    echo "请先 make（当前目录没有 ./ipinfo）" >&2
    exit 1
fi

# ---------- 严格计数模式 ----------
# 路由行: "  IPv4 table ..."   邻居行: "  IPv4 dev ..."   地址行: "  IPv4: ..."
c_route()  { ./ipinfo "$@" 2>/dev/null | grep -cE '^  IPv4 table ' || true; }
c_route6() { ./ipinfo "$@" 2>/dev/null | grep -cE '^  IPv6 table ' || true; }
c_allrt()  { ./ipinfo "$@" 2>/dev/null | grep -cE '^  IPv[46] table ' || true; }
c_neigh()  { ./ipinfo "$@" 2>/dev/null | grep -cE '^  IPv[46] dev ' || true; }
c_addr()   { ./ipinfo "$@" 2>/dev/null | grep -cE '^  IPv[46]: ' || true; }

echo "===== 1) Termux 原生基准（uid=$(id -u)）====="
EXP_IFACES=$(./ipinfo -s | wc -l)
EXP_ADDR=$(c_addr -R)
EXP_DEF=$(c_allrt)
EXP_V4=$(c_route -l -4)
EXP_V6=$(c_route6 -l -6)
printf '  ifaces=%s addr=%s default_routes=%s v4=%s v6=%s neighbors=%s\n' \
       "$EXP_IFACES" "$EXP_ADDR" "$EXP_DEF" "$EXP_V4" "$EXP_V6" "$(c_neigh -n)"

# ---------- 准备容器内工作目录 ----------
echo "===== 2) 准备 $DISTRO 容器内 $WORK ====="
mkdir -p "$ROOTFS$WORK"
cp ipinfo.c "$ROOTFS$WORK/"
cat > "$ROOTFS$WORK/expect.env" <<EOF
EXP_IFACES=$EXP_IFACES
EXP_ADDR=$EXP_ADDR
EXP_DEF=$EXP_DEF
EXP_V4=$EXP_V4
EXP_V6=$EXP_V6
EOF

cat > "$ROOTFS$WORK/run.sh" <<'EOS'
#!/bin/bash
# 在容器内部执行
cd /tmp/ipinfo_selftest || exit 1
. ./expect.env
fail=0
chk() {  # chk 名称 期望 实际
    if [ "$2" = "$3" ]; then printf '  OK   %-22s %s\n' "$1" "$3"
    else printf '  FAIL %-22s 期望=%s 实际=%s\n' "$1" "$2" "$3"; fail=1; fi
}
cnt() { grep -cE "$1" || true; }

echo "-- 容器内身份 --"
echo "  uid=$(id -u) (假)  selinux=$(tr -d '\0' < /proc/self/attr/current 2>/dev/null)"

echo "-- 容器内编译 (gcc/glibc) --"
gcc -O2 -Wall -Wextra -Wpedantic -std=c11 -o ipinfo_glibc ipinfo.c || exit 1
echo "  OK   $(ls -l ipinfo_glibc | awk '{print $5}') bytes, $(readelf -l ipinfo_glibc | grep -o 'ld-linux-aarch64.so.1' | head -1)"
b=./ipinfo_glibc

echo "-- 与原生基准比对 --"
chk "接口数"        "$EXP_IFACES" "$($b -s | wc -l)"
chk "地址行"        "$EXP_ADDR"   "$($b -R | cnt '^  IPv[46]: ')"
chk "默认路由"      "$EXP_DEF"    "$($b | cnt '^  IPv[46] table ')"
chk "全路由 IPv4"   "$EXP_V4"     "$($b -l -4 | cnt '^  IPv4 table ')"
chk "全路由 IPv6"   "$EXP_V6"     "$($b -l -6 | cnt '^  IPv6 table ')"

echo "-- PRoot 能力缺口：邻居表应为 0 --"
chk "邻居默认(-n)"  0 "$($b -n | cnt '^  IPv[46] dev ')"
chk "邻居全集(-N)"  0 "$($b -N | cnt '^  IPv[46] dev ')"

echo "-- -v 应提示 AF_UNIX 伪装（3 次 netlink dump）--"
n=$($b -v -s -i wlan0 2>&1 >/dev/null | grep -c 'actually domain 1')
chk "伪装提示次数"  3 "$n"

echo "-- stderr 必须为空 --"
for a in "-s" "-n" "-N" "-l -j"; do
    e=$($b $a 2>&1 >/dev/null)
    if [ -z "$e" ]; then echo "  OK   stderr($a) 空"
    else echo "  FAIL stderr($a): $e"; fail=1; fi
done

echo "-- JSON 合法性 --"
if $b -j -l -n > out.json 2>/dev/null && python3 - <<'PY'
import json
d = json.load(open('/tmp/ipinfo_selftest/out.json'))
print("  OK   json: ifaces=%d routes=%d neighbors=%d"
      % (len(d["interfaces"]), len(d["routes"]), len(d["neighbors"])))
PY
then :; else echo "  FAIL JSON 解析"; fail=1; fi

echo "-- 退出码 --"
$b -i nosuch >/dev/null 2>&1; [ $? -eq 2 ] && echo "  OK   no-match=2" || { echo "  FAIL no-match"; fail=1; }
$b -Z        >/dev/null 2>&1; [ $? -eq 1 ] && echo "  OK   bad-opt=1"  || { echo "  FAIL bad-opt";  fail=1; }
$b -h        >/dev/null 2>&1; [ $? -eq 0 ] && echo "  OK   help=0"     || { echo "  FAIL help";     fail=1; }

echo
[ $fail -eq 0 ] && echo "===== 容器内自测：全部通过 =====" || echo "===== 容器内自测：有失败 ====="
exit $fail
EOS

# ---------- 在容器内部执行 ----------
echo "===== 3) 在 $DISTRO 容器内部执行（--user $USERNAME）====="
proot-distro login "$DISTRO" --user "$USERNAME" -- bash "$WORK/run.sh"
rc=$?
echo "===== 退出码: $rc ====="
exit $rc
