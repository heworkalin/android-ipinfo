# ipinfo - Android 网络信息枚举工具
#
# Termux 自带的 aarch64-linux-android-clang 生成的是 bionic/Android 目标，
# 因此在 Termux 与 PRoot-Distro 里都能直接运行。
# 只有 CC 来自 make 内建默认值时才替换，命令行 `make CC=...` 或环境变量仍然生效。

ifeq ($(origin CC),default)
CC := aarch64-linux-android-clang
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
test-proot: ipinfo
	bash test_proot.sh

# 邻居表能力探针（带 RTM_GETADDR 对照组，用来说清“0 条”到底是环境还是探针的问题）
probe: tools/neigh_probe
	./tools/neigh_probe

tools/neigh_probe: tools/neigh_probe.c
	$(CC) $(CFLAGS) -o $@ $<

install: ipinfo
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 ipinfo $(DESTDIR)$(BINDIR)/ipinfo

clean:
	rm -f ipinfo *.o tools/neigh_probe
	rm -rf build

.PHONY: all check compare test-proot probe install clean
