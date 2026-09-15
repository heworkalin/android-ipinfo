# ipinfo - Android 网络信息枚举工具
#
# 编译器自动选择：
#   Termux 宿主：aarch64-linux-android-clang（bionic 目标，可在 proot 里直跑）
#   容器内部  ：没有 Android 工具链，退到自带的 gcc（glibc 目标，同样能测）
# 只有 CC 来自 make 内建默认值时才自动选，命令行 `make CC=...` 仍然生效。
ifeq ($(origin CC),default)
  ifneq ($(shell command -v aarch64-linux-android-clang 2>/dev/null),)
    CC := aarch64-linux-android-clang
  else
    CC := gcc
  endif
endif

CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11
PREFIX  ?= /data/data/com.termux/files/usr
BINDIR  ?= $(PREFIX)/bin

all: ipinfo

ipinfo: ipinfo.c
	$(CC) $(CFLAGS) -o $@ $<

# 冒烟测试：必须在 Termux 原生和 PRoot 里都不挂、不报错
check: ipinfo
	./ipinfo -s >/dev/null
	./ipinfo -j -4 >/dev/null
	./ipinfo -j -6 >/dev/null
	./ipinfo -h >/dev/null
	@echo "check OK"

# 与有权限的 adb shell 做路由集合级对照（需 adb 已授权）
compare: ipinfo
	python3 cmp_routes.py
	python3 cmp_neigh.py

# 在 proot-distro 容器【内部】用 gcc/glibc 重编并跑完整自测
# 在 proot 容器内部/宿主机都能跑：先尝试构建，构建不了（容器内无 gcc）也不报错，
# 交给脚本自己选择“gcc 重编 / 用现有二进制 / 给出可操作的提示”。
test-proot:
	@if command -v "$(CC)" >/dev/null 2>&1; then $(MAKE) --no-print-directory ipinfo; \
	else echo "[i] 当前环境没有编译器（$(CC)），跳过构建，由 test_proot.sh 自行处理"; fi
	bash test_proot.sh

# 能力探针（用来说清“能力缺失”到底是环境问题还是程序问题）
#   neigh_probe  : 带 RTM_GETADDR 对照组，验证邻居表
#   family_probe : 对 ADDR/ROUTE/NEIGH 各用 AF_UNSPEC/AF_INET/AF_INET6 发一次，
#                  暴露“AF_UNSPEC 在 PRoot 下只回 IPv4”这类问题
probe: tools/neigh_probe tools/family_probe
	./tools/neigh_probe
	./tools/family_probe

tools/neigh_probe: tools/neigh_probe.c
	$(CC) $(CFLAGS) -o $@ $<

tools/family_probe: tools/family_probe.c
	$(CC) $(CFLAGS) -o $@ $<

install: ipinfo
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 ipinfo $(DESTDIR)$(BINDIR)/ipinfo

clean:
	rm -f ipinfo *.o tools/neigh_probe tools/family_probe
	rm -rf build

.PHONY: all check compare test-proot probe install clean
