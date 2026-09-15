#!/data/data/com.termux/files/usr/bin/bash
#
# test_proot.sh — ipinfo 自测脚本（自动识别运行环境）
#
#   A) 在 Termux 宿主机（非 proot）跑：取宿主机参考值 → 进容器用 gcc(glibc) 重编 → 跑断言
#   B) 在 proot 容器内部跑：直接对当前环境跑同样的断言
#
# ---------------------------------------------------------------------------
# 重要设计原则：只断言【程序不变量】，环境差异只做【报告】
#
#   程序不变量（无论在哪跑都必须成立，失败即真 bug）：
#     - 退出码 0/1/2 正确
#     - 所有模式下 stderr 为空
#     - JSON 可解析且结构完整
#     - 不出现 if<N> 占位符（路由/邻居引用的接口必须都能解析出真名）
#     - 输出非空（接口数 > 0）
#     - 【诚实性】用户要了邻居表却没有数据时，必须明确说明原因，
#       不能静默空着（这是刚修过的行为，值得锁住）
#
#   环境差异（只打印，不参与判定）：
#     - proot 下邻居表条数（本环境为 0：PRoot 把 AF_NETLINK 换成 AF_UNIX，
#       其回退实现没有邻居表）
#     - -v 里 AF_UNIX 仿真提示出现几次
#     - 容器内与宿主机的接口/路由计数差异
#   这些是“当前测试环境的能力短板”，不是程序应有的性质。若哪天 PRoot 补上了
#   邻居表，断言“必须为 0”反而会误报失败——那就成了惩罚变好。
# ---------------------------------------------------------------------------
set -uo pipefail

DISTRO="${1:-ubuntu}"
USERNAME="${2:-he}"
ROOTFS="${IPINFO_ROOTFS:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/$DISTRO/rootfs}"
WORK=/tmp/ipinfo_selftest
SRCDIR="$(cd "$(dirname "$0")" && pwd)"

# ---------- 判断当前是否在 PRoot 会话里 ----------
# PRoot 伪造 getuid()，但 /proc/self/status 里的 Uid 是内核真值。
FAKE_UID="$(id -u 2>/dev/null || echo '?')"
REAL_UID="$(awk '/^Uid:/{print $2}' /proc/self/status 2>/dev/null || echo '?')"
IN_PROOT=0
[ "$REAL_UID" != '?' ] && [ "$FAKE_UID" != "$REAL_UID" ] && IN_PROOT=1

[ -f "$SRCDIR/ipinfo.c" ] || { echo "找不到 $SRCDIR/ipinfo.c" >&2; exit 1; }

# 严格计数：路由行 "  IPv4 table ..." / 邻居行 "  IPv4 dev ..." / 地址行 "  IPv4: ..."
cnt() { grep -cE "$1" || true; }

if [ "$IN_PROOT" = 1 ]; then
    echo "===== 模式 B：在 PRoot 容器内部运行 ====="
    echo "  getuid()=$FAKE_UID（假）  /proc 里的真 uid=$REAL_UID"
    PODIR="$WORK"
    mkdir -p "$PODIR" || { echo "无法创建 $PODIR" >&2; exit 1; }
    cp "$SRCDIR/ipinfo.c" "$PODIR/"
    rm -f "$PODIR/reference.env"
    RUN="bash $WORK/run.sh"
else
    echo "===== 模式 A：Termux 宿主机 ====="
    command -v proot-distro >/dev/null 2>&1 || {
        echo "[错误] 找不到 proot-distro（本模式需要它）" >&2; exit 2; }
    [ -d "$ROOTFS" ] || {
        echo "找不到容器 rootfs: $ROOTFS（可用 IPINFO_ROOTFS=... 覆盖）" >&2; exit 1; }
    [ -x "$SRCDIR/ipinfo" ] || {
        echo "请先 make（$SRCDIR/ipinfo 不存在）" >&2; exit 1; }

    echo "== 1) 宿主机参考值（uid=$REAL_UID，仅用于对比报告）=="
    cd "$SRCDIR" || exit 1
    REF_IFACES=$(./ipinfo -s | wc -l)
    REF_ADDR=$(./ipinfo -R | cnt '^  IPv[46]: ')
    REF_DEF=$(./ipinfo | cnt '^  IPv[46] table ')
    REF_V4=$(./ipinfo -l -4 | cnt '^  IPv4 table ')
    REF_V6=$(./ipinfo -l -6 | cnt '^  IPv6 table ')
    REF_NB=$(./ipinfo -n | cnt '^  IPv[46] dev ')
    printf '  ifaces=%s addr=%s default_routes=%s v4=%s v6=%s neighbors=%s\n' \
           "$REF_IFACES" "$REF_ADDR" "$REF_DEF" "$REF_V4" "$REF_V6" "$REF_NB"

    PODIR="$ROOTFS$WORK"
    mkdir -p "$PODIR"
    cp "$SRCDIR/ipinfo.c" "$PODIR/"
    cat > "$PODIR/reference.env" <<EOF
REF_IFACES=$REF_IFACES
REF_ADDR=$REF_ADDR
REF_DEF=$REF_DEF
REF_V4=$REF_V4
REF_V6=$REF_V6
REF_NB=$REF_NB
EOF
    echo "== 2) 准备容器内 $WORK =="
    RUN="proot-distro login $DISTRO --user $USERNAME -- bash $WORK/run.sh"
fi

# ---------- 在容器内执行的测试体 ----------
cat > "$PODIR/run.sh" <<'EOS'
#!/bin/bash
cd /tmp/ipinfo_selftest || exit 1
fail=0
ok()   { printf '  OK   %s\n' "$1"; }
bad()  { printf '  FAIL %s\n' "$1"; fail=1; }
cnt()  { grep -cE "$1" || true; }

echo "-- 环境 --"
echo "  getuid=$(id -u)  真 uid=$(awk '/^Uid:/{print $2}' /proc/self/status)  selinux=$(tr -d '\0' < /proc/self/attr/current 2>/dev/null)"

echo "-- 编译（容器内 gcc / glibc）--"
gcc -O2 -Wall -Wextra -Wpedantic -std=c11 -o ipinfo_glibc ipinfo.c || exit 1
echo "  OK   $(ls -l ipinfo_glibc | awk '{print $5}') bytes, $(readelf -l ipinfo_glibc | grep -o 'ld-linux-aarch64.so.1' | head -1)"
b=./ipinfo_glibc

# ======================= 程序不变量 =======================
echo
echo "===== 程序不变量（与环境无关，失败即真 bug）====="

echo "-- 输出非空 --"
n_if=$($b -s | wc -l); n_addr=$($b -R | cnt '^  IPv[46]: ')
[ "$n_if"   -gt 0 ] && ok "接口数 $n_if > 0"        || bad "接口数为 0"
[ "$n_addr" -gt 0 ] && ok "地址行 $n_addr > 0"      || bad "地址行为 0"

echo "-- stderr 必须为空（不带 -v 时）--"
for a in "-s" "-n" "-N" "-l -j" "-a"; do
    e=$($b $a 2>&1 >/dev/null)
    [ -z "$e" ] && ok "stderr($a) 空" || bad "stderr($a): $e"
done

 echo "-- -v 的 stderr 只能出现诊断行（以 'ipinfo: ' 开头）--"
e=$($b -v -s 2>&1 >/dev/null | grep -vc '^ipinfo: ' || true)
[ "$e" = 0 ] && ok "-v 的 stderr 全是 'ipinfo: ' 诊断行" || bad "-v 的 stderr 有 $e 行非诊断输出"

echo "-- 不能出现 if<N> 占位符（路由/邻居引用的接口都该能解析出真名）--"
n=$($b -a -s | awk '{print $1}' | grep -c '^if[0-9]' || true)
[ "$n" = 0 ] && ok "接口名 0 个占位符" || bad "接口名有 $n 个 if<N> 占位符"
n=$($b -l -N | grep -cE 'dev if[0-9]' || true)
[ "$n" = 0 ] && ok "路由/邻居 0 个占位符" || bad "路由/邻居有 $n 处 dev if<N>"

echo "-- JSON 结构与内容 --"
if $b -j -l -N > out.json 2>/dev/null; then
    if python3 - <<'PY'
import json, sys
d = json.load(open('/tmp/ipinfo_selftest/out.json'))
assert set(('interfaces', 'routes', 'neighbors')) <= set(d), d.keys()
for i in d['interfaces']:
    assert set(('name','index','mtu','state','flags','ipv4','ipv6')) <= set(i), i.keys()
for r in d['routes']:
    assert set(('family','table','dev','destination','gateway','metric')) <= set(r), r.keys()
for n in d['neighbors']:
    assert set(('family','dev','address','lladdr','state')) <= set(n), n.keys()
print("  OK   json: ifaces=%d routes=%d neighbors=%d" % (len(d['interfaces']), len(d['routes']), len(d['neighbors'])))
PY
    then ok "JSON 合法且键完整"; else bad "JSON 结构不符"; fi
else
    bad "JSON 输出失败"
fi

echo "-- 退出码 --"
$b -i nosuch >/dev/null 2>&1; [ $? -eq 2 ] && ok "no-match=2" || bad "no-match 退出码不为 2"
$b -Z        >/dev/null 2>&1; [ $? -eq 1 ] && ok "bad-opt=1"  || bad "bad-opt 退出码不为 1"
$b -h        >/dev/null 2>&1; [ $? -eq 0 ] && ok "help=0"     || bad "help 退出码不为 0"

echo "-- 诚实性：要了邻居表却没有数据时必须说明原因，不能静默 --"
n_nb=$($b -n | cnt '^  IPv[46] dev ')
if [ "$n_nb" -gt 0 ]; then
    $b -n | grep -q '^neighbors (' && ok "有邻居($n_nb 条)且打印了表头" || bad "有邻居但没打印表头"
else
    $b -n -R | grep -q 'neighbors (.*): (none)' && ok "邻居为空时打印了 (none)" || bad "邻居为空却静默无输出"
    if $b -v -s -i wlan0 2>&1 >/dev/null | grep -q 'actually domain'; then
        $b -n -R | grep -q 'netlink here is emulated' \
            && ok "netlink 被仿真时说明了原因" || bad "netlink 被仿真但没说明原因"
    fi
fi

# ======================= 环境差异（只报告） =======================
echo
echo "===== 环境差异（仅报告，不参与判定）====="
o_if=$($b -s | wc -l)
o_addr=$($b -R | cnt '^  IPv[46]: ')
o_def=$($b | cnt '^  IPv[46] table ')
o_v4=$($b -l -4 | cnt '^  IPv4 table ')
o_v6=$($b -l -6 | cnt '^  IPv6 table ')
o_nb=$($b -n | cnt '^  IPv[46] dev ')
o_nba=$($b -N | cnt '^  IPv[46] dev ')
o_emu=$($b -v -s -i wlan0 2>&1 >/dev/null | grep -c 'actually domain' || true)
printf '  ifaces=%s addr=%s default_routes=%s v4=%s v6=%s neighbors(-n)=%s neighbors(-N)=%s AF_UNIX_notices=%s\n' \
       "$o_if" "$o_addr" "$o_def" "$o_v4" "$o_v6" "$o_nb" "$o_nba" "$o_emu"
if [ -f ./reference.env ]; then
    . ./reference.env
    printf '  宿主机参考: ifaces=%s addr=%s default_routes=%s v4=%s v6=%s neighbors=%s\n' \
           "$REF_IFACES" "$REF_ADDR" "$REF_DEF" "$REF_V4" "$REF_V6" "$REF_NB"
    same=1
    [ "$o_if" = "$REF_IFACES" ] && [ "$o_addr" = "$REF_ADDR" ] && \
    [ "$o_def" = "$REF_DEF" ] && [ "$o_v4" = "$REF_V4" ] && [ "$o_v6" = "$REF_V6" ] || same=0
    if [ "$same" = 1 ]; then
        echo "  与宿主机一致：接口/地址/路由计数完全相同"
    else
        echo "  ⚠ 与宿主机存在计数差异（可能是环境能力差异或采集瞬间网络变化，"
        echo "    不作为失败；宿主机上可用 make compare 与 adb 做集合级对照）"
    fi
    [ "$o_nb" = 0 ] && [ "$REF_NB" != 0 ] && \
        echo "  ⚠ 邻居表：宿主机 $REF_NB 条，容器内 $o_nb 条 —— PRoot 的 AF_UNIX 回退不实现邻居表"
else
    echo "  （容器内直接运行，没有宿主机参考值）"
fi

echo
[ $fail -eq 0 ] && echo "===== 程序不变量：全部通过 =====" || echo "===== 程序不变量：有失败 ====="
exit $fail
EOS

# ---------- 执行 ----------
if [ "$IN_PROOT" = 1 ]; then
    cd "$SRCDIR" || exit 1
    bash "$WORK/run.sh"
else
    echo "== 3) 在 $DISTRO 容器内部执行（--user $USERNAME）=="
    $RUN
fi
rc=$?
echo "===== 退出码: $rc ====="
exit $rc
