# LUS carve stubs (Phase 0 output, orchestrator-owned)

Shadow headers that satisfy libultraship's desktop header leaks when compiling the
carve for 3DS (see docs/research/spike-lus-carve-report.md). Include this directory
BEFORE the vendored LUS headers (port/3ds/vendor/lus/include). Real third-party headers
(nlohmann/json.hpp, BS_thread_pool.hpp, tinyxml2.h) are NOT vendored here — they live in
port/3ds/gfx/third_party/.

No patch step: the carve is vendored at port/3ds/vendor/lus/ with all 38 lus-*.patch —
lus-newlib-portability.patch included — already folded in. See
port/3ds/vendor/lus/PROVENANCE.md.
