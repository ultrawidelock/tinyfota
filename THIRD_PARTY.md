# Third-party code

tinyfota's own code is ISC (see `LICENSE`). The code below keeps its own
license. Each license was read from its file, not assumed.

| Component | Where | License | License file |
|---|---|---|---|
| detools, C applier | `third_party/detools/detools.{c,h}` | BSD-2-Clause | `third_party/detools/LICENSE` |
| heatshrink decoder | `third_party/detools/heatshrink/` | ISC | `third_party/detools/heatshrink/LICENSE` |
| sais (sais-lite 2.4.1) | inside the `detools` pip package, `detools/sais/sais.c`; not vendored here | MIT | the header of that file, and the first lines of `third_party/detools/LICENSE` |

## detools

- Upstream: <https://github.com/eerimoq/detools>.
- Copyright 2003-2005 Colin Percival (original C implementation) and
  Copyright (c) 2019 Erik Moqvist. BSD 2-Clause, from `third_party/detools/LICENSE`.
- Imported unchanged from ultrawidelock@d6e23761
  (`modules/ultrawidelock_dfu/src/detools/`).
- Provenance, checked byte for byte: `detools.c` and `detools.h` are identical
  to upstream commit `074929b6c97f0dbc931fc8876e1a7df83199b0c8` (2023-03-10),
  the commit Espressif's `esp_delta_ota` component pins. Their version string
  reads `0.52.0`, but they are not identical to the `0.52.0` tag: they carry
  upstream's later incremental header parser.
- Patched in tinyfota, each change marked `tinyfota:` in the source. Both fix
  crashes on a corrupt in-place patch, and both are open upstream as of
  2026-10-16:
  - <https://github.com/eerimoq/detools/pull/20>: reject a segment size of 0,
    which was a division by zero.
  - <https://github.com/eerimoq/detools/pull/21>: keep the from offset inside
    the memory, which could overflow an `int` or reach `mem_read()` out of
    range. It adds `memory_size` to `struct detools_apply_patch_in_place_t`,
    as upstream master already has.
- Only the in-place applier is compiled, and only into the bootloader. The
  build switches LZMA, CRLE and file I/O off (see `zephyr/CMakeLists.txt`).

## heatshrink

- Upstream: <https://github.com/atomicobject/heatshrink>, v0.4.1.
- Copyright (c) 2013-2015 Scott Vokes. ISC, from
  `third_party/detools/heatshrink/LICENSE`, which is upstream's v0.4.1 file
  verbatim.
- The four source files are byte-identical to detools' `c/heatshrink/` at the
  commit above. That directory's `README.rst`, which states the ISC license, is
  kept beside them unchanged. The ultrawidelock copy carried neither file; both
  were added from upstream for this repository.

## sais

- sais-lite 2.4.1 by Yuta Mori, Copyright (c) 2008-2010, MIT License.
- Not compiled into firmware and not vendored here. The `detools` pip package
  builds it as a C extension when the host tool is installed, which is one
  reason a fresh machine needs a C compiler (see `tools/tinyfota/README.md`).
