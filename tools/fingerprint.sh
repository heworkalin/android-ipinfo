#!/bin/bash
#
# fingerprint.sh — 打印当前环境下 ipinfo 的“能力指纹”，便于跨环境对比
#
# 用法:
#   bash tools/fingerprint.sh [包含 ipinfo 与 neigh_probe 的目录]
#
# 输出一行块：os / 真伪 uid / selinux / socket 真身 / 对照组 ADDR / 待测 NEIGH /
#             接口与路由计数 / 邻居计数
#
# 对比时建议在【同一时刻】分别在本机、proot-distro、TMOE 等环境各跑一次，
# 因为设备网络状态是会变的（Wi-Fi/蜂窝/VPN 切换会让接口与路由数量大幅变化）。
set -u

DIR="${1:-$(cd "$(dirname "$0")" && pwd)}"
IP="$DIR/ipinfo"
NP="$DIR/neigh_probe"
[ -x "$NP" ] || NP="$DIR/tools/neigh_probe"   # 仓库布局下探针在 tools/

[ -x "$IP" ] || { echo "找不到可执行的 $IP" >&2; exit 1; }

real_uid="$(awk '/^Uid:/{print $2}' /proc/self/status 2>/dev/null || echo '?')"
fake_uid="$(id -u 2>/dev/null || echo '?')"
in_proot=no
[ "$fake_uid" != "$real_uid" ] && in_proot=yes

os="$( (. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME") || echo "Android $(uname -r | grep -o 'android[0-9]*' || echo unknown)" )"
sel="$(tr -d '\0' < /proc/self/attr/current 2>/dev/null || echo '?')"

echo "== ipinfo fingerprint =="
printf '  os       : %s\n' "$os"
printf '  kernel   : %s\n' "$(uname -r)"
printf '  uid      : getuid=%s (假)   real=%s   in_proot=%s\n' "$fake_uid" "$real_uid" "$in_proot"
printf '  selinux  : %s\n' "$sel"

if [ -x "$NP" ]; then
    out="$("$NP" 2>/dev/null)"
    printf '  SO_DOMAIN: '
    printf '%s' "$out" | awk '
        { tag=$1; for (i=1;i<=NF;i++) if ($i ~ /^SO_DOMAIN=/) { sub(/SO_DOMAIN=/,"",$i); printf "%s=%s ", tag, $i } }
        END { print "" }'
    printf '  ADDR     : '   # 对照组
    printf '%s' "$out" | awk '$1=="ADDR" && /family=/{ for(i=1;i<=NF;i++) if ($i ~ /^entries=/) { sub(/entries=/,"",$i); print "entries=" $i } }'
    printf '  NEIGH    : '   # 待测
    printf '%s' "$out" | awk '$1=="NEIGH" && /family=/{ for(i=1;i<=NF;i++) if ($i ~ /^entries=/) { sub(/entries=/,"",$i); printf "entries=%s ", $i } } END { print "" }'
else
    echo "  (没有 neigh_probe，跳过 socket/邻居探测)"
fi

cnt() { grep -cE "$1" || true; }
printf '  ipinfo   : ifaces=%s addr=%s default_routes=%s v4_routes=%s v6_routes=%s neighbors(-n)=%s neighbors(-N)=%s\n' \
    "$("$IP" -s | wc -l)" \
    "$("$IP" -R | cnt '^  IPv[46]: ')" \
    "$("$IP" | cnt '^  IPv[46] table ')" \
    "$("$IP" -l -4 | cnt '^  IPv4 table ')" \
    "$("$IP" -l -6 | cnt '^  IPv6 table ')" \
    "$("$IP" -n | cnt '^  IPv[46] dev ')" \
    "$("$IP" -N | cnt '^  IPv[46] dev ')"
