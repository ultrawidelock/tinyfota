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

TEST_SRCS := $(wildcard tests/host/*.c)

.PHONY: test asan seams clean

test:
	@if [ -z "$(TEST_SRCS)" ]; then echo "make test: no host tests yet"; fi

asan:
	@if [ -z "$(TEST_SRCS)" ]; then echo "make asan: no host tests yet"; fi

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

clean:
	rm -rf build
