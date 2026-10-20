CROSS ?= i686-w64-mingw32-
HOST_CC ?= cc
PYTHON ?= python3
TARGET_CC = $(CROSS)gcc
TARGET_LD = $(CROSS)ld
DLLTOOL = $(CROSS)dlltool
OBJDUMP = $(CROSS)objdump
OBJCOPY = $(CROSS)objcopy

WARN = -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion \
	-Wcast-qual -Wcast-align -Wundef -Wpointer-arith -Wwrite-strings -Wvla \
	-Wstrict-prototypes -Wmissing-prototypes
COMMON = -std=c23 $(WARN) -Iinclude
KERNEL = $(COMMON) -Icompat -O2 -march=i486 -mtune=i486 \
	-msoft-float -mno-sse -mno-mmx -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-ident -fno-strict-aliasing -Wstack-usage=1536
PE_FLAGS = -mi386pe --subsystem native --major-subsystem-version 4 \
	--minor-subsystem-version 0 --major-os-version 4 --minor-os-version 0 \
	--entry _DriverEntry@8 --image-base 0x10000 --file-alignment 0x200 \
	--section-alignment 0x1000 --disable-dynamicbase --disable-nxcompat \
	--enable-reloc-section --no-insert-timestamp

.PHONY: all drivers test test-qemu coverage check package floppy test-floppy clean \
	help
all: drivers

drivers: build/r8139cp.sys

build:
	mkdir -p $@

build/ndis-miniport.a: compat/ndis_miniport.def | build
	$(DLLTOOL) --kill-at --input-def $< --output-lib $@

MINIPORT_DEPS = src/ndis/miniport.c compat/ndis_miniport.h compat/portio.h \
	include/rtl8139.h

build/miniport.o: $(MINIPORT_DEPS) | build
	$(TARGET_CC) $(KERNEL) -c $< -o $@

build/core-i486.o: src/core/rtl8139.c include/rtl8139.h | build
	$(TARGET_CC) $(KERNEL) -c $< -o $@

build/r8139cp.sys: build/miniport.o build/core-i486.o build/ndis-miniport.a
	$(TARGET_LD) $(PE_FLAGS) -Map $(@:.sys=.map) -o $@ $^

build/test-core: tests/test_core.c src/core/rtl8139.c include/rtl8139.h | build
	$(HOST_CC) $(COMMON) -O1 -g -fsanitize=address,undefined \
		-fno-omit-frame-pointer tests/test_core.c src/core/rtl8139.c -o $@

MINIPORT_TEST = $(HOST_CC) $(COMMON) -Icompat -Wno-attributes -DRTL_HOST_TEST \
	-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

build/test-miniport: tests/test_miniport.c $(MINIPORT_DEPS) src/core/rtl8139.c | build
	$(MINIPORT_TEST) tests/test_miniport.c src/ndis/miniport.c \
		src/core/rtl8139.c -o $@

test: build/test-core build/test-miniport
	./build/test-core
	./build/test-miniport

build/libcore.so: src/core/rtl8139.c include/rtl8139.h | build
	$(HOST_CC) $(COMMON) -O2 -fPIC -shared $< -o $@

test-qemu: build/libcore.so
	$(PYTHON) tests/test_qemu.py

# GCC static analyzer over the kernel sources (no object is kept).
build/analyzed: src/ndis/miniport.c src/core/rtl8139.c $(MINIPORT_DEPS) | build
	$(TARGET_CC) $(KERNEL) -fanalyzer -c src/ndis/miniport.c -o /dev/null
	$(TARGET_CC) $(KERNEL) -fanalyzer -c src/core/rtl8139.c -o /dev/null
	touch $@

# Line coverage of the host tests; the core object is shared so both test
# programs add to one rtl8139.gcda.
COV = build/coverage
COV_CC = $(HOST_CC) $(COMMON) -Icompat -Wno-attributes -O0 --coverage
coverage: | build
	$(RM) -r $(COV)
	mkdir -p $(COV)
	$(COV_CC) -c src/core/rtl8139.c -o $(COV)/rtl8139.o
	$(COV_CC) -DRTL_HOST_TEST -c src/ndis/miniport.c -o $(COV)/miniport.o
	$(COV_CC) tests/test_core.c $(COV)/rtl8139.o -o $(COV)/test-core
	$(COV_CC) -DRTL_HOST_TEST tests/test_miniport.c $(COV)/miniport.o \
		$(COV)/rtl8139.o -o $(COV)/test-miniport
	./$(COV)/test-core
	./$(COV)/test-miniport
	gcov -n -o $(COV) src/ndis/miniport.c src/core/rtl8139.c
	@for f in src/ndis/miniport.c src/core/rtl8139.c; do \
		gcov -t -o $(COV) $$f 2>/dev/null | \
		sed -n "s|^ *#####: *\([0-9]*\):|$$f:\1: not covered:|p"; \
	done

check: drivers test build/analyzed
	$(PYTHON) tests/check_binary.py --objdump $(OBJDUMP)
	$(PYTHON) tests/check_package.py

package: check
	mkdir -p dist/nt4/docs dist/win95/docs
	cp build/r8139cp.sys packaging/nt4/OEMSETUP.INF dist/nt4/
	cp build/r8139cp.sys packaging/win95/r8139cp.inf dist/win95/
	cp README.md LICENSE dist/nt4/
	cp README.md LICENSE dist/win95/
	cp docs/DRIVER.md docs/PROVENANCE.md dist/nt4/docs/
	cp docs/DRIVER.md docs/PROVENANCE.md dist/win95/docs/
	$(PYTHON) tests/check_package.py --dist
	cd dist && sha256sum nt4/*.* nt4/LICENSE nt4/docs/* \
		win95/*.* win95/LICENSE win95/docs/* > SHA256SUMS

# 1.44 MB FAT12 driver disk: A:\NT4 (OEMSETUP.INF) and A:\WIN95 (r8139cp.inf).
FLOPPY = dist/rtl8139cp-floppy.img
floppy: $(FLOPPY)

# Not bootable: src/boot/floppy.S replaces mtools' boot code, partition
# table and 0x55AA signature (bytes 0x3e-0x1ff of sector 0).
build/floppy-boot.bin: src/boot/floppy.S | build
	$(TARGET_CC) -c $< -o build/floppy-boot.o
	$(OBJCOPY) -O binary -j .text build/floppy-boot.o $@

$(FLOPPY): package build/floppy-boot.bin
	$(RM) $@
	mformat -C -i $@ -f 1440 -v RTL8139CP -N 8139C0DE ::
	dd if=build/floppy-boot.bin of=$@ bs=1 seek=62 count=450 conv=notrunc status=none
	mmd -i $@ ::/NT4 ::/WIN95 ::/DOCS
	mcopy -i $@ -m dist/nt4/OEMSETUP.INF dist/nt4/r8139cp.sys ::/NT4/
	mcopy -i $@ -m dist/win95/r8139cp.inf dist/win95/r8139cp.sys ::/WIN95/
	mcopy -i $@ -m README.md LICENSE ::/
	mcopy -i $@ -m docs/DRIVER.md docs/PROVENANCE.md ::/DOCS/
	$(PYTHON) tests/check_package.py --floppy $@

test-floppy: $(FLOPPY)
	$(PYTHON) tests/test_floppy_boot.py $(FLOPPY)

help:
	@echo 'RTL8139C+ NDIS 3.0 miniport for Windows NT 4.0 and Windows 95'
	@echo
	@echo 'Targets:'
	@echo '  all        (default) same as drivers'
	@echo '  drivers    build/r8139cp.sys (one binary for NT4 and Windows 95)'
	@echo '  test       host core and miniport tests (ASan/UBSan)'
	@echo '  check      drivers, test, GCC analyzer, PE/import/i486 and INF checks'
	@echo '  test-qemu  shared C core against QEMU'"'"'s RTL8139C+ model'
	@echo '             (needs qemu-system-i386)'
	@echo '  coverage   line coverage of the host tests (gcov)'
	@echo '  package    check, then create dist/{nt4,win95}/ and dist/SHA256SUMS'
	@echo '  floppy     package, then create $(FLOPPY) (needs mtools)'
	@echo '  test-floppy  floppy, then boot it in QEMU to verify it is not'
	@echo '             bootable (needs qemu-system-i386)'
	@echo '  clean      remove build outputs and generated packages'
	@echo '  help       show this text'
	@echo
	@echo 'Variables:'
	@echo '  CROSS      cross toolchain prefix (now: $(CROSS))'
	@echo '  HOST_CC    host compiler for tests (now: $(HOST_CC))'
	@echo '  PYTHON     interpreter for checks (now: $(PYTHON))'

clean:
	$(RM) -r build
	$(RM) -r dist/nt4 dist/win95
	$(RM) dist/SHA256SUMS $(FLOPPY)
