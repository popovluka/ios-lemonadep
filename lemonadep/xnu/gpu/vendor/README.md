# Vendored: UniXNU/NativeAdreno + UniXNU/ArmSmmu4K

Both MIT-licensed (see each directory's own `LICENSE`). Vendored rather
than reimplemented because they are real, already-in-production code
(`NativeAdreno`: "GPU commands and the scanout copy run on sheng hardware";
`ArmSmmu4K`: "used by the GPU and storage SMMU paths on sheng") that is
~97% chip- and platform-generic -- see `lemonadep/HANDOFF.md`'s "GPU, Phase
2" section for the survey that established this (only
`NativeAdrenoA740HwInit.h`, intentionally *not* vendored here, is
chip-specific; lemonadep's own equivalent is
`lemonadep/xnu/gpu/LemonadepA660HwInit.h`, derived from real sources, not
copied).

| Source | Commit | Fetched |
| --- | --- | --- |
| `github.com/UniXNU/NativeAdreno` | `b686b9054f57d393c80195b82568a09f8ca106a2` | 2026-10-23 |
| `github.com/UniXNU/ArmSmmu4K` | `4d0d6598b1a36aeec3de01091893fa6660e49388` | 2026-10-23 |

## What's vendored and why

- `NativeAdrenoGpu.{cpp,h}` -- the generic lifecycle orchestrator
  (`reserve -> bootGmu -> programSmmu -> ring attach -> CP start -> IRQ
  mask`). Lemonadep's own board callbacks plug into this unchanged; see
  `lemonadep/xnu/gpu/lemonadep_gpu_board.cpp` (not vendored -- the new,
  lemonadep-specific glue).
- `NativeAdrenoRing.{cpp,h}`, `NativeAdrenoCpInit.{cpp,h}`,
  `NativeAdrenoIrq.{cpp,h}`, `NativeAdrenoTimer.{cpp,h}` -- the CP ring,
  CP init sequence, interrupt mask and GPU-counter timer. Chip-generic
  (take register offsets/IOVAs as parameters, not hardcoded).
- `NativeAdrenoGmuPlatform.{cpp,h}` -- bounds-checked MMIO adapter binding
  real windows to the `ShengGmuIo`-shaped contract
  `lemonadep_gmu_coldstart.c` also follows (see that file; lemonadep wrote
  its own GMU cold-start logic rather than vendoring `ShengGmu.c`, because
  that file's A740-specific control flow is genuinely wrong for A660 in
  several places -- see `HANDOFF.md`).
- `NativeAdrenoSmmuContext.{cpp,h}`, `NativeAdrenoSmmuMemory.{cpp,h}` --
  the IOKit-facing wrapper around `ArmSmmu4K`'s real SMMUv2 programming.
  Lemonadep supplies its own bank/stream-ID values and runtime `NUMCB`
  discovery (`lemonadep_gpu_smmu.cpp`, not vendored); the wrapper itself
  needed no chip-specific changes.
- `NativeAdrenoMemory.{cpp,h}` -- BO/page-table memory bookkeeping.
- `NativeAdrenoABI.h` -- the kernel/user-client wire ABI lemonadep's own
  IOKit user client implements byte-for-byte, so Turnip's real
  `tu_knl_darwin.cc` (see `HANDOFF.md`, "GPU, Phase 1") works against it
  unmodified, once that toolchain work happens.
- `ArmSmmu4K/*` -- the full real SMMUv2 native-4K table/context/stream
  library, used as-is. Includes `ArmSmmu4KAudit.{c,h}`: not in the first
  vendoring pass, added once `ArmSmmu4KTableMemory.h`'s `#include` made it
  a real, required dependency, not an optional extra (caught by actually
  syntax-checking the vendored files, not by reading the file list).
  Syntax-checks clean with `clang --target=aarch64-linux-gnu -fsyntax-only`
  (the Mmio file's own `#error` guard requires little-endian AArch64,
  hence that target, not the host's native one).

## Not vendored

`NativeAdrenoA740HwInit.h`, `NativeAdrenoAccelerator.cpp`,
`NativeAdrenoBoard.cpp/h`, `NativeAdrenoClient.cpp/h`,
`NativeAdrenoFirmware*.{cpp,h}`, `NativeAdrenoGmuConfig.cpp/h`,
`NativeAdrenoGmuContext.cpp/h`, `NativeAdrenoGmuMemory.cpp/h`,
`NativeAdrenoGmuService.cpp/h`, `NativeAdrenoGpuContext.cpp/h`,
`NativeAdrenoKernelTime.cpp/h`, `NativeAdrenoPublish.cpp`,
`NativeAdrenoPwrup*.{cpp,h}`, `NativeAdrenoService.cpp`,
`NativeAdrenoSmmuService.cpp/h`, `NativeAdrenoStatus.h`,
`NativeAdrenoSubmit.cpp/h`, `NativeAdrenoUserClient.cpp/h`,
`NativeAdrenoVm.cpp/h` -- either superseded by lemonadep's own, simpler
equivalents already written (GMU cold-start, GDSC/clock reserve), or not
yet needed (submit/fence, the IOKit publish/user-client glue, which
lemonadep will write fresh against sweet's own `IOAcceleratorES`-style
conventions rather than port UniXNU's own IOKit service/client classes).
