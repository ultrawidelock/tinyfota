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
	-Itests/host/logfake -Itests/host/psafake -Iinclude -Icore
TEST_SRCS := tests/host/test.c tests/host/test_dfu.c tests/host/test_dfu_smp.c \
	tests/host/dfufake/dfufake.c tests/host/smpfake/smpfake.c tests/host/psafake/psafake.c \
	tests/host/port/osal_host.c tests/host/port/flash_host.c \
	core/dfu_crc.c core/dfu_receiver.c core/dfu_applier.c \
	ports/zephyr/crypto_psa.c ports/zephyr/dfu_smp_img.c
TEST_HDRS := $(wildcard include/tinyfota/*.h core/*.h tests/host/*.h tests/host/*/*.h \
	tests/host/*/*/*.h tests/host/*/*/*/*.h tests/host/*/*/*/*/*.h tests/host/*/*/*/*/*/*.h)

# Behaviour tinyfota added to the imported core, in its own binary so the
# imported suite above stays check-for-check identical to the source's.
ADD_SRCS := tests/host/test.c tests/host/test_tinyfota.c \
	tests/host/dfufake/dfufake.c tests/host/psafake/psafake.c \
	tests/host/port/osal_host.c tests/host/port/flash_host.c \
	core/dfu_crc.c core/dfu_receiver.c ports/zephyr/crypto_psa.c

.PHONY: test asan seams wire clean

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

# The browser's copy of the update protocols against the Python tool, byte for
# byte (tests/tooling/fota_wire_check.sh). Needs python3 and node.
wire:
	tests/tooling/fota_wire_check.sh

clean:
	rm -rf build
