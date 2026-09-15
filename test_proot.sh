#!/data/data/com.termux/files/usr/bin/bash
#
# test_proot.sh — ipinfo 自测脚本（自动识别运行环境）
#
#   A) 在 Termux 宿主机（非 proot）跑：
#        先在宿主机取原生基准 → 把源码拷进容器 → 容器内用 gcc(glibc) 重编 →
#        断言“除邻居表应为 0 外，其余必须与原生完全一致”
#   B) 在 proot 容器内部跑：
#        跳过与原生比对（在容器里取不到原生基准），只做容器侧的不变量断言：
#        邻居表必须为 0（PRoot 不支持 RTM_GETNEIGH）、-v 必须提示 AF_UNIX 伪装、
#        stderr 为空、JSON 合法、退出码正确、无 if%d 占位符
#
# 之所以要区分：proot-distro 拒绝在 proot 会话里再套一层（nested proot），
# 而在容器内取的“原生基准”只是容器数据，拿来对比没有意义。
#
# 用法:
#   make test-proot                     # 宿主机（推荐）
#   bash test_proot.sh [DISTRO] [USER]  # 宿主机；在容器内则自动切到 B 模式
set -uo pipefail

DISTRO="${1:-ubuntu}"
USERNAME="${2:-he}"
ROOTFS="${IPINFO_ROOTFS:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/$DISTRO/rootfs}"
WORK=/tmp/ipinfo_selftest
SRCDIR="$(cd "$(dirname "$0")" && pwd)"

# ---------- 判断当前是否在 PRoot 会话里 ----------
# PRoot 会伪造 getuid()，但 /proc/self/status 里的 Uid 是内核真值。
FAKE_UID="$(id -u 2>/dev/null || echo '?')"
REAL_UID="$(awk '/^Uid:/{print $2}' /proc/self/status 2>/dev/null || echo '?')"
IN_PROOT=0
if [ "$REAL_UID" != '?' ] && [ "$FAKE_UID" != "$REAL_UID" ]; then
    IN_PROOT=1
fi

[ -f "$SRCDIR/ipinfo.c" ] || { echo "找不到 $SRCDIR/ipinfo.c" >&2; exit 1; }

# 严格计数模式：路由行 "  IPv4 table ..." / 邻居行 "  IPv4 dev ..." / 地址行 "  IPv4: ..."
cnt() { grep -cE "$1" || true; }

if [ "$IN_PROOT" = 1 ]; then
    echo "===== 模式 B：在 PRoot 容器内部运行 ====="
    echo "  getuid()=$FAKE_UID（假）  /proc 里的真 uid=$REAL_UID"
    echo "  说明：容器内拿不到原生基准，因此只做容器侧不变量断言"
    PODIR="$WORK"
    mkdir -p "$PODIR" || { echo "无法创建 $PODIR" >&2; exit 1; }
    cp "$SRCDIR/ipinfo.c" "$PODIR/"
    rm -f "$PODIR/expect.env"
    RUN="bash $WORK/run.sh"
else
    echo "===== 模式 A：Termux 宿主机 ====="
    command -v proot-distro >/dev/null 2>&1 || {
        echo "[错误] 找不到 proot-distro（本模式需要它）" >&2; exit 2; }
    [ -d "$ROOTFS" ] || {
        echo "找不到容器 rootfs: $ROOTFS（可用 IPINFO_ROOTFS=... 覆盖）" >&2; exit 1; }
    [ -x "$SRCDIR/ipinfo" ] || {
        echo "请先 make（$SRCDIR/ipinfo 不存在）" >&2; exit 1; }

    echo "== 1) 取 Termux 原生基准（uid=$REAL_UID）=="
    cd "$SRCDIR" || exit 1
    EXP_IFACES=$(./ipinfo -s | wc -l)
    EXP_ADDR=$(./ipinfo -R | cnt '^  IPv[46]: ')
    EXP_DEF=$(./ipinfo | cnt '^  IPv[46] table ')
    EXP_V4=$(./ipinfo -l -4 | cnt '^  IPv4 table ')
    EXP_V6=$(./ipinfo -l -6 | cnt '^  IPv6 table ')
    printf '  ifaces=%s addr=%s default_routes=%s v4=%s v6=%s neighbors=%s\n' \
           "$EXP_IFACES" "$EXP_ADDR" "$EXP_DEF" "$EXP_V4" "$EXP_V6" "$(./ipinfo -n | cnt '^  IPv[46] dev ')"

    PODIR="$ROOTFS$WORK"
    mkdir -p "$PODIR"
    cp "$SRCDIR/ipinfo.c" "$PODIR/"
    cat > "$PODIR/expect.env" <<EOF
EXP_IFACES=$EXP_IFACES
EXP_ADDR=$EXP_ADDR
EXP_DEF=$EXP_DEF
EXP_V4=$EXP_V4
EXP_V6=$EXP_V6
EOF
    echo "== 2) 准备容器内 $WORK =="
    RUN="proot-distro login $DISTRO --user $USERNAME -- bash $WORK/run.sh"
fi

# ---------- 容器内执行的测试体 ----------
cat > "$PODIR/run.sh" <<'EOS'
#!/bin/bash
cd /tmp/ipinfo_selftest || exit 1
fail=0
chk() {  # chk 名称 期望 实际
    if [ "$2" = "$3" ]; then printf '  OK   %-22s %s\n' "$1" "$3"
    else printf '  FAIL %-22s 期望=%s 实际=%s\n' "$1" "$2" "$3"; fail=1; fi
}
cnt() { grep -cE "$1" || true; }
HAVE_EXP=0
[ -f ./expect.env ] && { . ./expect.env; HAVE_EXP=1; }

echo "-- 环境 --"
echo "  getuid=$(id -u)  真 uid=$(awk '/^Uid:/{print $2}' /proc/self/status)  selinux=$(tr -d '\0' < /proc/self/attr/current 2>/dev/null)"
echo "  真实 socket: 见下面 -v 输出"

echo "-- 编译（容器内 gcc / glibc）--"
gcc -O2 -Wall -Wextra -Wpedantic -std=c11 -o ipinfo_glibc ipinfo.c || exit 1
echo "  OK   $(ls -l ipinfo_glibc | awk '{print $5}') bytes, $(readelf -l ipinfo_glibc | grep -o 'ld-linux-aarch64.so.1' | head -1)"
b=./ipinfo_glibc

if [ "$HAVE_EXP" = 1 ]; then
    echo "-- 与原生基准比对（必须完全一致）--"
    chk "接口数"      "$EXP_IFACES" "$($b -s | wc -l)"
    chk "地址行"      "$EXP_ADDR"   "$($b -R | cnt '^  IPv[46]: ')"
    chk "默认路由"    "$EXP_DEF"    "$($b | cnt '^  IPv[46] table ')"
    chk "全路由 IPv4" "$EXP_V4"     "$($b -l -4 | cnt '^  IPv4 table ')"
    chk "全路由 IPv6" "$EXP_V6"     "$($b -l -6 | cnt '^  IPv6 table ')"
else
    echo "-- 无原生基准（容器内直接运行）：只做自洽性检查 --"
    n=$($b -s | wc -l)
    [ "$n" -gt 0 ] && echo "  OK   接口数             $n (>0)" || { echo "  FAIL 接口数为 0"; fail=1; }
    [ "$($b -l -4 | cnt '^  IPv4 table ')" -gt 0 ] \
        && echo "  OK   全路由 IPv4        $($b -l -4 | cnt '^  IPv4 table ')" \
        || { echo "  FAIL 全路由 IPv4 为 0"; fail=1; }
fi

echo "-- PRoot 能力缺口：邻居表必须为 0 --"
chk "邻居默认(-n)"  0 "$($b -n | cnt '^  IPv[46] dev ')"
chk "邻居全集(-N)"  0 "$($b -N | cnt '^  IPv[46] dev ')"

echo "-- 邻居表为空时必须显式说明，而不是静默 --"
if $b -n -R | grep -q 'neighbors (.*): (none)'; then
    echo "  OK   打印了 (none)"
else
    echo "  FAIL 空输出没任何说明（用户无法区分“表为空”和“工具坏了”）"; fail=1
fi
if $b -n -R | grep -q 'netlink here is emulated'; then
    echo "  OK   说明了是 PRoot 仿真 netlink 不支持"
else
    echo "  FAIL 未说明原因"; fail=1
fi

echo "-- -v 必须提示 AF_NETLINK 被换成 AF_UNIX（3 次 dump）--"
n=$($b -v -s -i wlan0 2>&1 >/dev/null | grep -c 'actually domain 1')
chk "AF_UNIX 提示次数" 3 "$n"

echo "-- 输出不得出现 if%d 占位符 --"
n=$($b -a -s | awk '{print $1}' | grep -c '^if[0-9]' || true)
chk "if<N> 占位符" 0 "$n"

echo "-- stderr 必须为空 --"
for a in "-s" "-n" "-N" "-l -j" "-a"; do
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
