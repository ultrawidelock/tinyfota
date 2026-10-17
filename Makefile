# tinyfota host build: unit tests, sanitizers and the seam check.
#
# CC and NM are honoured. On a machine whose default cc or nm is not the tool
# you want, pass both:
#   make test CC=/usr/bin/clang NM="xcrun nm"

NM ?= nm
# Byte order for every sort and comm below, whatever the caller's locale.
export LC_ALL := C
BUILD := build/host

CSTD := -std=c11 -O1
SAN := -g -fsanitize=address,undefined -fno-sanitize-recover=all

# Mach-O prefixes every C symbol with an underscore; ELF does not.
SYM_PREFIX := $(if $(filter Darwin,$(shell uname -s)),_,)

# Zephyr's clang warning_base, which is what the source's firmware build
# compiled these files under, plus -Werror.
WARN := -Wall -Wformat -Wformat-security -Wno-format-zero-length \
	-Wno-unused-but-set-variable -Wno-typedef-redefinition \
	-Wno-deprecated-non-prototype -Wdouble-promotion -Wno-pointer-sign \
	-Wpointer-arith -Werror

# The delta-update suite: both halves of the core, the signature seam over the
# recording PSA (psafake/), the SMP image group over the zcbor/mcumgr doubles
# (smpfake/), a scripted detools (dfufake/), and the host port (port/): RAM
# partitions that enforce the nRF word and page alignment rules, and a virtual
# clock. CONFIG_MCUMGR_SMP_LEGACY_RC_BEHAVIOUR is on so the explicit "rc" key a
# legacy client expects is compiled and checked. _DEFAULT_SOURCE because the
# host port names CLOCK_MONOTONIC, which glibc hides under strict -std=c11;
# Darwin ignores it.
# The suite keeps the golden public key beside the vector it belongs to, for
# the reader, while the receiver links its own copy; hence the one waiver.
TEST_WARN := $(WARN) -Wno-unused-const-variable
TEST_DEFS := -DTINYFOTA_PORT_HOST -D_DEFAULT_SOURCE -DCONFIG_TINYFOTA_SMP_IMG=1 \
	-DCONFIG_TINYFOTA_APPLIER_CHUNK=256 -DCONFIG_MCUMGR_GRP_OS_RESET_HOOK=1 \
	-DCONFIG_MCUMGR_GRP_ENUM_DETAILS_NAME=1 -DCONFIG_MCUMGR_SMP_LEGACY_RC_BEHAVIOUR=1
TEST_INCS := -Itests/host -Itests/host/port -Itests/host/dfufake -Itests/host/smpfake \
	-Itests/host/logfake -Itests/host/psafake -Iinclude -Icore -Iports/zephyr
TEST_SRCS := tests/host/test.c tests/host/test_dfu.c tests/host/test_dfu_smp.c \
	tests/host/dfufake/dfufake.c tests/host/smpfake/smpfake.c tests/host/psafake/psafake.c \
	tests/host/port/osal_host.c tests/host/port/flash_host.c \
	core/dfu_crc.c core/dfu_receiver.c core/dfu_applier.c \
	ports/zephyr/crypto_psa.c ports/zephyr/dfu_smp_img.c ports/zephyr/smp_group.c
TEST_HDRS := $(wildcard include/tinyfota/*.h core/*.h ports/zephyr/*.h tests/host/*.h tests/host/*/*.h \
	tests/host/*/*/*.h tests/host/*/*/*/*.h tests/host/*/*/*/*/*.h tests/host/*/*/*/*/*/*.h)

# Behaviour tinyfota added to the imported core, in its own binary so the
# imported suite above stays check-for-check identical to the source's.
ADD_SRCS := tests/host/test.c tests/host/test_tinyfota.c \
	tests/host/dfufake/dfufake.c tests/host/psafake/psafake.c \
	tests/host/port/osal_host.c tests/host/port/flash_host.c \
	core/dfu_crc.c core/dfu_receiver.c core/dfu_applier.c ports/zephyr/crypto_psa.c

.PHONY: test asan seams wire tooling powercut fuzz fuzz-run tinyble clean

$(BUILD)/host_test_dfu: $(TEST_SRCS) $(TEST_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(TEST_WARN) $(TEST_DEFS) $(TEST_INCS) $(TEST_SRCS) -o $@

$(BUILD)/host_test_dfu_asan: $(TEST_SRCS) $(TEST_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(TEST_WARN) $(SAN) $(TEST_DEFS) $(TEST_INCS) $(TEST_SRCS) -o $@

$(BUILD)/host_test_tinyfota: $(ADD_SRCS) $(TEST_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(TEST_WARN) $(TEST_DEFS) $(TEST_INCS) $(ADD_SRCS) -o $@

$(BUILD)/host_test_tinyfota_asan: $(ADD_SRCS) $(TEST_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(TEST_WARN) $(SAN) $(TEST_DEFS) $(TEST_INCS) $(ADD_SRCS) -o $@

test: $(BUILD)/host_test_dfu $(BUILD)/host_test_tinyfota
	$(BUILD)/host_test_dfu
	$(BUILD)/host_test_tinyfota

asan: $(BUILD)/host_test_dfu_asan $(BUILD)/host_test_tinyfota_asan
	$(BUILD)/host_test_dfu_asan
	$(BUILD)/host_test_tinyfota_asan

# The update channel on tinyble (ports/zephyr/dfu_tinyble.c) and the SMP
# request decoder (smp_decode.c), fed real SMP request bytes and checked
# against the bytes they notify, over the REAL zcbor. zcbor is not vendored
# here: ZCBOR names a checkout, and every NCS or Zephyr workspace has one at
# modules/lib/zcbor. tinyble and the work queue are doubles (tests/host/tbfake).
# zcbor is compiled as shipped, with only its own warnings silenced.
ZCBOR ?=
TB := $(BUILD)/tinyble
TB_DEFS := -DTINYFOTA_PORT_HOST -D_DEFAULT_SOURCE -DCONFIG_TINYFOTA_SMP_IMG=1 \
	-DCONFIG_TINYFOTA_TINYBLE=1 -DCONFIG_TINYFOTA_TINYBLE_SMP_BUF=384
TB_INCS := -Itests/host -Itests/host/tbfake -Itests/host/port -Itests/host/dfufake \
	-Itests/host/logfake -Itests/host/psafake -Iinclude -Icore -Iports/zephyr -I$(ZCBOR)/include
TB_SRCS := tests/host/test.c tests/host/test_tinyble.c tests/host/tbfake/tbfake.c \
	tests/host/dfufake/dfufake.c tests/host/psafake/psafake.c \
	tests/host/port/osal_host.c tests/host/port/flash_host.c \
	core/dfu_crc.c core/dfu_receiver.c ports/zephyr/crypto_psa.c \
	ports/zephyr/smp_group.c ports/zephyr/smp_decode.c ports/zephyr/dfu_tinyble.c
ZCBOR_SRCS := $(addprefix $(ZCBOR)/src/,zcbor_common.c zcbor_decode.c zcbor_encode.c)

tinyble:
	@test -n "$(ZCBOR)" -a -f "$(ZCBOR)/src/zcbor_decode.c" || \
		{ echo "make tinyble needs ZCBOR=<a zcbor checkout>, e.g. <workspace>/modules/lib/zcbor"; exit 1; }
	@mkdir -p $(TB)/plain $(TB)/asan
	@for f in $(ZCBOR_SRCS); do \
		$(CC) $(CSTD) -w -I$(ZCBOR)/include -c $$f -o $(TB)/plain/$$(basename $$f .c).o || exit 1; \
		$(CC) $(CSTD) -w $(SAN) -I$(ZCBOR)/include -c $$f -o $(TB)/asan/$$(basename $$f .c).o || exit 1; \
	done
	$(CC) $(CSTD) $(TEST_WARN) $(TB_DEFS) $(TB_INCS) $(TB_SRCS) $(TB)/plain/*.o \
		-o $(TB)/host_test_tinyble
	$(CC) $(CSTD) $(TEST_WARN) $(SAN) $(TB_DEFS) $(TB_INCS) $(TB_SRCS) $(TB)/asan/*.o \
		-o $(TB)/host_test_tinyble_asan
	$(TB)/host_test_tinyble
	$(TB)/host_test_tinyble_asan

# The symbols the core leaves for a port to provide: everything a core object
# references that no core or vendored object defines. The vendored detools is
# compiled alongside so its entry points count as defined. Every optional
# surface is on (the SMP upload path, the logging seam), so the list is the
# whole contract. libc is printed but is not a seam: which libc names appear
# is the compiler's choice (bzero for a zeroing memset, __stack_chk_* for the
# stack protector). The rest must match tests/seams.expected.
DETOOLS := third_party/detools
DETOOLS_DEFS := -DDETOOLS_CONFIG_FILE_IO=0 -DDETOOLS_CONFIG_COMPRESSION_LZMA=0 \
	-DDETOOLS_CONFIG_COMPRESSION_CRLE=0 -DDETOOLS_CONFIG_COMPRESSION_NONE=1 \
	-DDETOOLS_CONFIG_COMPRESSION_HEATSHRINK=1
CORE_INCS := -Iinclude -Icore -I$(DETOOLS) -I$(DETOOLS)/heatshrink
SEAM_DEFS := -DTINYFOTA_PORT_LOG=1 -DCONFIG_TINYFOTA_SMP_IMG=1 -DCONFIG_TINYFOTA_APPLIER_CHUNK=256 $(DETOOLS_DEFS)
SEAM_SRCS := $(wildcard core/*.c) $(wildcard $(DETOOLS)/detools.c $(DETOOLS)/heatshrink/heatshrink_decoder.c)
LIBC_RE := ^_*((mem[a-z]*|str[a-z]*|bzero)(_chk)?|stack_chk_(fail|guard))$$

seams:
	@mkdir -p $(BUILD)/seams
	@for f in $(SEAM_SRCS); do \
		$(CC) $(CSTD) $(SEAM_DEFS) $(CORE_INCS) -c $$f -o $(BUILD)/seams/$$(basename $$f .c).o || exit 1; \
	done
	@objs="$(addprefix $(BUILD)/seams/,$(notdir $(SEAM_SRCS:.c=.o)))"; \
	if [ -n "$(SEAM_SRCS)" ]; then \
		$(NM) -A -u $$objs | awk '{ print $$NF }' | sed 's/^$(SYM_PREFIX)//' | sort -u > $(BUILD)/seams/undef.txt; \
		$(NM) -A -g --defined-only $$objs | awk '{ print $$NF }' | sed 's/^$(SYM_PREFIX)//' | sort -u > $(BUILD)/seams/def.txt; \
	else : > $(BUILD)/seams/undef.txt; : > $(BUILD)/seams/def.txt; fi; \
	comm -23 $(BUILD)/seams/undef.txt $(BUILD)/seams/def.txt > $(BUILD)/seams/all.txt; \
	grep -E -v '$(LIBC_RE)' $(BUILD)/seams/all.txt > $(BUILD)/seams/seams.txt || true; \
	cat $(BUILD)/seams/seams.txt; \
	echo "libc, not a seam: $$(grep -E '$(LIBC_RE)' $(BUILD)/seams/all.txt | tr '\n' ' ')"; \
	diff -u tests/seams.expected $(BUILD)/seams/seams.txt && \
	echo "make seams: $$(wc -l < $(BUILD)/seams/seams.txt | tr -d ' ') symbols, matches tests/seams.expected"

# The power-cut simulator (tests/powercut/powercut.c): the real receiver and
# applier over the vendored detools, with the bootloader's detools settings,
# on a NOR flash model; the power is cut at every flash operation of a real
# update between two synthetic images. Vendored code is compiled as shipped and
# only its own warnings are silenced (-w on its objects, never on ours).
PC := $(BUILD)/pc
PC_FIXTURES := tests/powercut/fixtures
PC_DEFS := $(DETOOLS_DEFS) -DTINYFOTA_PORT_LOG=1 -DCONFIG_TINYFOTA_APPLIER_LOG=1 \
	-DCONFIG_TINYFOTA_APPLIER_CHUNK=256
PC_SRCS := tests/powercut/powercut.c core/dfu_receiver.c core/dfu_applier.c core/dfu_crc.c

$(PC)/detools.o: $(DETOOLS)/detools.c $(DETOOLS)/detools.h
	@mkdir -p $(@D)
	$(CC) -std=c11 -O2 -w $(DETOOLS_DEFS) $(CORE_INCS) -c $< -o $@

$(PC)/heatshrink_decoder.o: $(DETOOLS)/heatshrink/heatshrink_decoder.c
	@mkdir -p $(@D)
	$(CC) -std=c11 -O2 -w $(DETOOLS_DEFS) $(CORE_INCS) -c $< -o $@

$(BUILD)/powercut: $(PC_SRCS) $(TEST_HDRS) $(PC)/detools.o $(PC)/heatshrink_decoder.o
	$(CC) -std=c11 -O2 $(WARN) $(PC_DEFS) $(CORE_INCS) $(PC_SRCS) \
		$(PC)/detools.o $(PC)/heatshrink_decoder.o -o $@

powercut: $(BUILD)/powercut
	$(BUILD)/powercut $(PC_FIXTURES)/base.bin $(PC_FIXTURES)/target.bin $(PC_FIXTURES)/update.wdfu

# libFuzzer targets (tests/fuzz/): the header parsers, the receiver's frame and
# SMP upload paths, and patch application on garbage. Apple's clang ships no
# libFuzzer; LLVM's does. On macOS with Homebrew's LLVM, whose clang finds
# the wrong ld by default:
#   make fuzz-run FUZZ_CC=$$(brew --prefix llvm)/bin/clang FUZZ_LDFLAGS=--ld-path=/usr/bin/ld
# fuzz-run gives each target FUZZ_SECONDS, starting from the committed seeds
# (tests/fuzz/gen_seeds.py) and growing its corpus under build/fuzz/; a crash
# is saved as build/fuzz/crash-<target>-<hash> and stops that target.
FUZZ_CC ?= clang
FUZZ_LDFLAGS ?=
FUZZ_SECONDS ?= 600
FUZZ := build/fuzz
FUZZ_TARGETS := header frame upload apply
FUZZ_SAN := -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
FUZZ_DEFS := -DTINYFOTA_PORT_HOST -D_DEFAULT_SOURCE -DTINYFOTA_PORT_LOG=1 \
	-DCONFIG_TINYFOTA_SMP_IMG=1 -DCONFIG_TINYFOTA_APPLIER_CHUNK=256 $(DETOOLS_DEFS)
FUZZ_INCS := -Itests/fuzz -Itests/host/port $(CORE_INCS)
FUZZ_SRCS := tests/fuzz/fuzz_port.c tests/host/port/flash_host.c tests/host/port/osal_host.c \
	core/dfu_crc.c core/dfu_receiver.c core/dfu_applier.c
FUZZ_VENDOR := $(FUZZ)/detools.o $(FUZZ)/heatshrink_decoder.o

$(FUZZ)/detools.o: $(DETOOLS)/detools.c $(DETOOLS)/detools.h
	@mkdir -p $(@D)
	$(FUZZ_CC) -std=c11 -w $(FUZZ_SAN) -fsanitize=fuzzer-no-link $(DETOOLS_DEFS) $(CORE_INCS) -c $< -o $@

$(FUZZ)/heatshrink_decoder.o: $(DETOOLS)/heatshrink/heatshrink_decoder.c
	@mkdir -p $(@D)
	$(FUZZ_CC) -std=c11 -w $(FUZZ_SAN) -fsanitize=fuzzer-no-link $(DETOOLS_DEFS) $(CORE_INCS) -c $< -o $@

$(FUZZ)/fuzz_%: tests/fuzz/fuzz_%.c $(FUZZ_SRCS) $(TEST_HDRS) tests/fuzz/fuzz_port.h $(FUZZ_VENDOR)
	$(FUZZ_CC) -std=c11 $(WARN) $(FUZZ_SAN) -fsanitize=fuzzer $(FUZZ_DEFS) $(FUZZ_INCS) \
		$< $(FUZZ_SRCS) $(FUZZ_VENDOR) $(FUZZ_LDFLAGS) -o $@

fuzz: $(addprefix $(FUZZ)/fuzz_,$(FUZZ_TARGETS))

fuzz-run: fuzz
	@for t in $(FUZZ_TARGETS); do \
		mkdir -p $(FUZZ)/corpus-$$t; \
		echo "fuzz_$$t: $(FUZZ_SECONDS) s"; \
		$(FUZZ)/fuzz_$$t $(FUZZ)/corpus-$$t tests/fuzz/corpus/$$t \
			-max_total_time=$(FUZZ_SECONDS) -timeout=10 -print_final_stats=1 \
			-artifact_prefix=$(FUZZ)/crash-$$t- > $(FUZZ)/fuzz_$$t.log 2>&1; \
		echo "  exit $$?: $$(grep -E '^(Done|==[0-9]+==ERROR|SUMMARY)' $(FUZZ)/fuzz_$$t.log | tail -2)"; \
	done

# The browser's copy of the update protocols against the Python tool, byte for
# byte (tests/tooling/fota_wire_check.sh). Needs python3 and node.
wire:
	tests/tooling/fota_wire_check.sh

# The host tool's own guarantees: the release index (python -m tinyfota.index
# --self-test), the release fan (tests/tooling/release_check.py), and the
# sample's NCS and upstream flash maps agreeing (partition_map_check.py). PY must
# be a Python with tools/tinyfota installed, e.g. make tooling PY=.venv/bin/python
PY ?= python3
tooling:
	$(PY) -m tinyfota.index --self-test
	$(PY) tests/tooling/release_check.py
	$(PY) tests/tooling/partition_map_check.py

clean:
	rm -rf build
