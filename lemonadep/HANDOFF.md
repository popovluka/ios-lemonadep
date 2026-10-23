# lemonadep handoff (2026-10-23 ~00:10) — read first

## TOUCH WORKS ON-DEVICE (2026-10-23)

Confirmed live: Settings app opened, scrolled through its list (Wi-Fi,
Notifications, Sounds, Focus, Screen Time, Control Center, Display &
Brightness, Home Screen, General), all via real finger touches on the
panel. Volume/power keys work at the same time. This is the first time
GUI interaction has worked on lemonadep.

Root cause of the earlier `ENXIO`/`no S3908 (err=6)` failure: the touch
1.8V enable pin (gpio197) is an SM8350 **eGPIO** pad, owned by an island
subsystem until the apps processor explicitly claims it (set bit 12 when
bit 11 reads 1 — see `pinctrl-sm8350.c`/`pinctrl-msm.c`, and
`blahajcoding/op9pro-mainline-poc`, mainline Linux on this exact board,
which hit the identical wall). `touch_gpo()` now sets that claim bit.
Also fixed in the same push: `SweetHIDTouch` no longer hardcodes sweet's
1080x2400 panel — it reads the real boot framebuffer size
(`PE_current_console`) and the digitizer's own extents (ADT /touch
panel-x/panel-y = 1440x3216) and scales between them, instead of clamping
raw digitizer coordinates to iPhone-X-sized bounds. See the dated section
below ("Touch: SE4 is GSI-only...") for the full root-cause writeup and
the commits (`b9cb8c6`, `eacd968`, `e7c623c`).

On-device log confirms every step cleanly:
```
SWEET TOUCH: pwr 2v8 gpio31 cfg=0x200 io=0x3, 1v8 gpio197 cfg=0x1a00 io=0x3
SWEET TOUCH: I2C bit-banged on gpio20/21 (SDA/SCL), SE4 untouched
SWEET TOUCH: bus self-test released sda=1 scl=1, driven-low sda=0 scl=0
SWEET TOUCH: TCM after reset: err=0 code=0x10 len=24 mode=0x1
SWEET TOUCH: TCM bring-up ok, mode=0x1
SWEET TOUCH: selected Synaptics S3908 TCM, poll=10ms
SWEET TOUCH: touch report len 5 -> configured layout, 1 object(s)
SWEET HID: digitizer 1440x3216 -> panel 1080x2412, presented 1125x2436
SWEET HID: touch 0 down at 974,3215 edge
```
Kernel run: https://github.com/popovluka/ios-lemonadep/actions/runs/39858965799
(cpus=4 boot image, boot-args unchanged: `sweettouch=1 sweetkeys=1 sweetusb=0`).

**Next**: M5 is effectively reached (home screen + keys + touch all work
together). Remaining open items: GPU acceleration, USB, WiFi/BT, cellular —
see `GAPS.md`.

## GPU, Phase 0: metal2vulkan reads and compiles real iOS 16.7 AIR (2026-10-23)

Host-only investigation, no phone risk, no CI cost. The plan (and gating
question) is `ios-flame`'s `METAL-NEXT.md`: "Does metal2vulkan read iOS 16.7
AIR (QuartzCore MTLB v2.7)?" Answer: **yes, confirmed directly, not inferred.**

- **GPU firmware pulled from this phone's own `/vendor`** (dynamic partition,
  mounted read-only from TWRP): `a660_gmu.bin`, `a660_sqe.fw`,
  `a660_zap.{mdt,b00,b01,b02,elf}` -- the literal Adreno 660 firmware set
  (same names `blahajcoding/op9pro-mainline-poc`'s dts node and
  `oosexgang/hackios-xnu-kernel`'s README both document for this GPU family).
  Saved to `lemonadep/fw-gpu/` (git-ignored; proprietary, never committed),
  reproducible with `lemonadep/fw-gpu/pull.sh`.
- **Mesa has dedicated Turnip support for Adreno 660**, not an approximation
  of a neighboring chip: `freedreno/turnip: add support for a660`
  (gitlab.freedesktop.org/mesa/mesa/-/merge_requests/10892); the Linux msm
  driver groups it as its own family, `ADRENO_6XX_GEN4 /* a660 family */`.
- **A real `.metallib` extracted from our own built iOS root**
  (`lemonadep/m2-artifacts/ios16-root.simg`, HFSX -- `simg2img` then `7z l/e`,
  which has a native HFS/HFSX codec; `hpmount`/hfsutils could not open this
  image, 7z could): `System/Library/PrivateFrameworks/RenderBox.framework/
  default.metallib`, real `MTLB` header, 46 real functions (`plane__vertex`,
  `uber_fragment`, `stroke_lines_vertex`, ...).
- **`metal2vulkan` (steelbrain/metal2vulkan, commit a2c440b -- the same
  commit `ios-flame`'s own investigation pinned) built clean** with Rust
  1.98 (its stated minimum) in WSL -- no Apple toolchain needed for the
  translator itself, unlike the kernel or the eventual iOS-side bridge.
- **Compiled a real pipeline to SPIR-V**: `plane__vertex` + `plane_color_fragment`
  against a floor Vulkan 1.2 profile (`vk12-floor.json`, bundled in the repo;
  not yet an Adreno-660-specific profile) -- `out.vert.spv` (3336 bytes),
  `out.frag.spv` (4776 bytes). **Independently validated with Khronos's own
  `spirv-val`** (apt `spirv-tools`, a separate tool from metal2vulkan's own
  check): clean exit 0, no errors, both stages.

This directly answers the one open question that would have sunk the whole
GPU plan before it started. What is still unconfirmed: a real Adreno-660
Vulkan device profile (needs Turnip actually running, e.g. via Linux
drm-shim, to get real `vulkaninfo --json` limits), the darwin-KMD Mesa patch
building against our kernel ABI, and everything kernel-side (new GPU user
client, BO alloc/submit/fence) -- all of which need the Apple cross-toolchain
and real CI time, which is why this stayed host-only for now.

## GPU, Phase 1: the real source chain is public (2026-10-23)

`ios-flame`'s own METAL-NEXT.md names its reference as "UniXNU (sheng, Adreno
740)" via a local clone it never redistributes. The actual UniXNU project
(github.com/UniXNU, device repos for many Qualcomm SoCs) is public, and the
pieces we need are tracked files, not something to reconstruct secondhand:

- **`NativeAdrenoABI.h`** (UniXNU/NativeAdreno): the exact, versioned,
  `static_assert`-pinned kernel/user-client wire ABI (Query, BoAlloc, BoMap,
  BoImport, Submit, WaitFence, GetDeviceInfo, CpuMap/Unmap, queues). This is
  what our kernel driver implements; Turnip's darwin backend assumes it
  byte-for-byte.
- **`patches/mesa/turnip-darwin-kmd.patch`** (UniXNU/UniXNU, 1437 lines): the
  real `tu_knl_darwin.cc` plus the `tu_knl.cc`/`tu_device.cc`/
  `wsi_common_metal_layer.m` hookup. Confirms the IOKit discovery path:
  `IOServiceGetMatchingService` for `$TU_DARWIN_SERVICE` (default
  `"IOAcceleratorES"` -- **confirmed by the patch's own comment to be the
  real Apple class name "the original Metal discovery finds", not something
  any of these projects invented**), open type `$TU_DARWIN_OPEN_TYPE`
  (default `0x4147`); `"NativeAdrenoGpuPublished"` as the service name selects
  the unsandboxed direct form (external-method selector == ABI method
  number), the simpler path for us since sweet's own sandbox MAC policy is
  already permissive. **Correction**: an earlier version of this note said
  sweet already has a `SweetAccelerator.cpp` using this name -- checked and
  that file does not exist in our tree (matches the README's own "no GPU
  driver" statement). There is nothing to extend; the GPU driver is new.
- **`scripts/data/adreno-compiler-inputs.json`** (UniXNU/UniXNU): the exact
  pinned Mesa commit (`85fda3b36405bcaea8bffc64aa4f2651dc90770d`) and the full
  ordered patch list (9 patches: `turnip-darwin-kmd`, `ir3-darwin-build-id`,
  `ir3-portable-asm-roundtrip`, `turnip-empty-scissor`,
  `turnip-builtin-shader-pack`, `turnip-graphics-cache-feedback`,
  `turnip-host-import-images`, `offline-adreno-compiler-probe`,
  `freedreno-tools-nir`), plus a sparse-checkout list that avoids cloning all
  of Mesa.
- **Ruled out, not assumed**: checked our own `xnu-1005` tree for an existing
  `sweet_a6xx.c` (the file `ios-flame`'s notes analyze) -- **we don't have
  one**. It is `ios-flame`'s own addition for their fork, not something
  upstream `ios-sweet` ships. The kernel-side GPU driver is genuinely new
  work here, informed by this real source, not extended from anything
  already in the tree.

Not yet done: the actual kernel driver (GMU/CP bring-up for Adreno 660
specifically -- UniXNU's own hwinit derivation, `NativeAdrenoA740HwInit.h`,
is for the 740 and chip-generation-specific, so 660's sequence needs its own
sourcing from the real Linux `msm` driver, not reuse), and the Apple
cross-toolchain build of Turnip + the frontend bridge.

## GPU, Phase 2: real A660 register table derived; found the real GMU algorithm (2026-10-23)

**Derived `lemonadep/xnu/gpu/LemonadepA660HwInit.h`**, the A660 equivalent of
UniXNU's own `NativeAdrenoA740HwInit.h`: same method (`derive-a660-hwinit.py`,
adapted from their `scripts/derive-a740-hwinit.py`), not hand-guessed values.
Source: `ianchb/sm8550-mainline` @ `42f3b40c` -- the exact commit UniXNU's own
`scripts/data/adreno-inputs.json` pins, sha256-verified before deriving
anything. Four registers in UniXNU's own A740 list don't exist for A660 (found
while deriving, not assumed): `CP_BV_APRIV_CNTL`, `CP_BV_RB_RPTR_ADDR`,
`CP_LPAC_APRIV_CNTL`, `GRAS_NC_MODE_CNTL` -- checked against the real
`a6xx_gpu.c hw_init()`, all four are written only inside
`if (adreno_is_a7xx(adreno_gpu))`, and the register XML tags them
`variants="A7XX"` with no A6XX entry at all. A660 doesn't have that hardware
(the BV/visibility ring and LPAC async-compute block are A7XX-only).
Result: 50 registers, 49 hwcg entries, 41 protect writes. Reproducible:
`lemonadep/xnu/gpu/fetch-and-derive.sh`.

**Found where the real GMU cold-start algorithm lives**: not in
`UniXNU/NativeAdreno` itself (that repo's `NativeAdrenoGmuPlatform.cpp` is
pure MMIO plumbing, binding IOKit-mapped windows to an external `ShengGmuIo`
interface) -- the actual state machine is `UniXNU-sheng/package/ShengGmu.c`
(282 lines), copied and adapted by several other device forks for their own
SoCs (`UniXNU-kebab`, `UniXNU-uke`, `UniXNU-braveark`, `UniXNU-komodo`), the
same pattern this project already follows for touch/buttons. Read it in
full. It implements, with real register names matching the pinned Linux
`a6xx_gmu.c`/`a6xx_hfi.c` I already have: reset/idle precondition checks,
RSC/PDC setup writes, TCM firmware download with full readback verification
(every word, not spot-checked), HFI memory zeroing, CM3 release, and polling
`FW_INIT_RESULT`/`HFI_CTRL_STATUS` with both elapsed-time and iteration
bounds. Its own header comments are explicit about the stakes: "No
substituted ACK exists", "A device allows one attempt", "Any failure after
mutation attempts reset assertion and poisons the session" -- matches the
risk I already flagged (same failure class as the SE4-DMA crash), and
confirms the state machine itself treats a bad attempt as unrecoverable
*within one boot*, not silently retryable; our own recovery stays the same
(temporary `fastboot boot`, reboot and try again).

**What's genuinely chip-and-board-specific, not reusable, and not yet
derived**: `ShengGmuSource.h` -- the companion file `ShengGmu.c` includes,
which `UniXNU-sheng` deliberately does **not** commit ("Generated only from
exact pinned XML/catalog/DT/firmware policy; ignored. In particular, no
vendor firmware or sequencer payload is stored in Git"). It holds: the
RSC/PDC register-setup sequence (board-specific RPMh/PDC resource wiring,
the same kind of cmd-db lookup our own touch/keys work already did for
`ldoc3`/`ldoc11`, not shareable across boards even on the same chip), the
pinned firmware's exact byte count and sha256 (ours: the real
`a660_gmu.bin` already pulled into `lemonadep/fw-gpu/`), and the five GMU
MMIO window physical addresses/sizes (GMU core, RSCC, PDC, GPU register
window, CX misc). This is the next real derivation, same rigor as the
hwinit table, not a copy.

## GPU, Phase 2 continued: A660's real GMU/RSC control flow traced (2026-10-23)

Read `a6xx_gmu_fw_start()` and `a6xx_gmu_rpmh_init()` in the pinned
`a6xx_gmu.c` directly, branch by branch, rather than porting `ShengGmu.c`'s
A7XX-targeted sequence with substituted addresses. Two real divergences
found this way, both confirmed against the actual conditionals (not
assumed):

1. **TCM retention is NOT a `CX_MISC` write on A660.** `ShengGmu.c` writes
   `CX_MISC_TCM_RET_CNTL=1` (an A7XX-only register -- `a6xx.xml` tags it
   `variants="A7XX-"`, no A6XX entry at all). The real code:
   `if (adreno_is_a7xx(...)) CX_MISC_TCM_RET_CNTL=1; else if
   (!adreno_is_a8xx(...)) GMU_GENERAL_7=1` (`a6xx_gmu.c:949-952`). A660 is
   neither -- it writes `GMU_GENERAL_7=1` in the GMU core window instead.
   The earlier FAL10-veto step (`FALNEXT_INTF`/`FAL_INTF=1`) *is* shared with
   A660: `adreno_is_a650_family()` (`family == ADRENO_6XX_GEN3 ||
   ADRENO_6XX_GEN4`) includes A660's `ADRENO_6XX_GEN4`, confirmed at
   `adreno_gpu.h:521-528`.
2. **A660's RSC/PDC handshake takes the "PDC configured in AOP" path**,
   same as A7XX (`a6xx_gmu.c:688-695`: `pdc_in_aop = true` whenever
   `adreno_is_a650_family() || adreno_is_a7xx()`). This skips the entire
   PDC sequencer-microcode and TCS1/TCS3 command block
   (`a6xx_gmu.c:735-772`) via a `goto setup_pdc`; only the RSC side still
   programs: `RSC_STATUS0_DRV0 |= BIT(24)` (disable SDE clock gating),
   `PDC_SLAVE_ID_DRV0=1`, the `HIDDEN_TCS_CMD0` zero/zero/0x80000000 triplet
   (`0x80000000`, not A740's `0x80000021` -- `adreno_is_a740_family()` is
   `ADRENO_7XX_GEN2 || ADRENO_7XX_GEN3`, A660 is neither, confirmed at
   `adreno_gpu.h:565-570`), `OVERRIDE_START_ADDR=0`,
   `PDC_SEQ_START_ADDR=0x4520`, `PDC_MATCH_VALUE_LO=0x4510`,
   `PDC_MATCH_VALUE_HI=0x4514`, the RSC sequencer microcode (A660 uses the
   a650/a7xx values `{0xeaaae5a0, 0xe1a1ebab, 0xa2e0a581, 0xecac82e2,
   0x0020edad}` at `RSCC_SEQ_MEM_0_DRV0[0..4]`, not the plain-a6xx values),
   then jumps straight to `PDC_GPU_SEQ_START_ADDR=0`,
   `PDC_GPU_ENABLE_PDC=0x80000001`.

This is why `ShengGmu.c` can't be ported with substituted register values
alone -- the control flow itself branches per generation in at least these
two places, and probably more in the parts not yet read (HFI queue setup,
`a6xx_hfi.c`).

**Done**: all the register offsets above, plus the rest of `fw_start()`
(HFI table pointer, fence range, CM3 config, chip ID/log registration,
GBIF QoS), are now resolved and committed in
`lemonadep/xnu/gpu/LemonadepA660HwInit.h` (`derive-a660-hwinit.py`, same
sha256-verified pinned source). Tracing the rest of `fw_start()` caught a
second real bug before it happened: ShengGmu.c's A740 fence-range value
(`0x32<<18|0x8a0`) and chip-ID/log registers (`GMU_GENERAL_10`/`_8`) are
genuinely wrong for A660, not just differently addressed -- the real
plain-a6xx branch (confirmed against the actual conditionals) uses fence
range `0xa<<18|0xa0` and writes chip ID/log to `GMU_HFI_SFR_ADDR` /
`GPU_GMU_CX_GMU_PWR_COL_CP_MSG` instead. One piece that *does* carry over
unchanged: the GBIF QoS writes (`a640_gbif`, confirmed shared by A660 in
the catalog) are the same four `0x71620` writes ShengGmu.c already does --
the tracing finds what's genuinely shared, not just what diverges.

Still to do: read `a6xx_hfi.c` for the HFI queue setup and message
protocol (likely more such branches), then write `LemonadepGmuSource.h`
(the board-specific pieces: real firmware hash, MMIO window physical
addresses, RPMh/cmd-db GPU power-level votes) and the actual kernel code.

## GPU, Phase 2 done: all sourcing complete, ready to write the driver (2026-10-23)

Read `a6xx_hfi.c` (1102 lines) fully. Findings:

- **HFI queue table layout is generic** (`a6xx_hfi_queue_init`/`a6xx_hfi_init`,
  `a6xx_hfi.h`'s `struct a6xx_hfi_queue_table_header`/`a6xx_hfi_queue_header`):
  one table header + 3 fixed queues at 4K offsets within the HFI memory
  region, no chip-generation branch at all.
- **A660's real BW table is already hand-written per-chip in Linux**
  (`a660_build_bw_table()`) -- transcribed verbatim into
  `LemonadepA660HwInit.h`, not derived; confirmed it's the one actually
  dispatched (`adreno_is_a660()` is an exact `chip_id==660` match, and our
  catalog entry has no `.bcms` override that would select a generated
  table instead).
- **ACD and IFPC are both real no-ops for a first bring-up**: gated by
  `acd_table->enable_by_level` and `idle_level != GMU_IDLE_STATE_IFPC`,
  both unset unless explicitly requested. Deliberately skipping both for
  v1, the same "simplest correct path first" choice already used for
  touch/buttons -- not a corner cut, a documented, justified scope cut.
- **Firmware loading is a real divergence from `ShengGmu.c` too**: A660
  takes the non-legacy, self-describing block-header format
  (`struct block_header{addr,size,type,value}`, `a6xx_gmu.c:843-917`), not
  `ShengGmu.c`'s fixed 3-region `AdrenoFirmwarePlanGmu` assumption (a
  different, A740-specific format). Parsed the real `a660_gmu.bin` already
  pulled from this phone with the actual algorithm
  (`lemonadep/xnu/gpu/verify-gmu-firmware.py`): 4 real payload blocks (ITCM,
  icache, 2x DTCM), header+payload bytes sum to exactly the file size --
  confirms the real on-phone firmware genuinely follows this format.

Wrote `lemonadep/xnu/gpu/LemonadepGmuSource.h`: the five real MMIO window
addresses (`qcom,adreno-660.1` node, mainline `sm8350.dtsi`; confirmed
`cx_mem`, not `cx_dbgc`, is the real `CX_MISC` window by reading
`a6xx_gpu.c`'s `msm_ioremap(pdev, "cx_mem")` call), GPUCC/SMMU addresses,
the `pil_gpu_mem` zap region (cross-checked against the existing
`HWMAP_lemonadep.md` entry), GIC SPIs, and the firmware segment table.
**Caught a real mistake before it shipped**: hand-typed the firmware
segment table's file offsets from memory instead of the verified parse
output -- wrong. Fixed, and `verify-gmu-firmware.py` now automatically
cross-checks the header against a live firmware parse every run, so this
can't silently drift again.

**All the real data needed for a correct GMU cold-start is now sourced,
verified and committed**: `LemonadepA660HwInit.h` (register table + RSC/PDC
sequence + fw_start sequence + BW table), `LemonadepGmuSource.h` (MMIO
windows + firmware), `NativeAdrenoABI.h` (the kernel user-client ABI to
implement). Next: write the actual kernel driver (new IOKit user client
published as `IOAcceleratorES`, the GMU cold-start state machine, CP ring,
submit/fence), then the Apple cross-toolchain build of Turnip + the
frontend bridge -- both need real CI time.

## GPU, Phase 3: the GMU cold-start driver is written and host-tested (2026-10-23)

`lemonadep/xnu/gpu/lemonadep_gmu_coldstart.c`: real code, not a sketch --
reset/idle checks, the RSC/PDC handshake, firmware load, `fw_start`,
`power_config`, and the real CM3 reset toggle, built entirely from
`LemonadepA660HwInit.h`/`LemonadepGmuSource.h`. No XNU headers (injected
I/O, same host-testable shape as `sweet_touch.c`'s own tests).

Adapting `UniXNU-sheng`'s real `ShengGmu.c` turned out to need more than
substituting register values -- writing this surfaced two further real
divergences, found and fixed, not shipped blind:

1. **`GMU_BYTE_OFFSET`'s `mmio_offset` correction, missed entirely at
   first.** The real `a6xx_gmu.c` subtracts `(GMU core base -
   kgsl_3d0_reg_memory base)` from every plain `gmu_write()`/`gmu_read()`
   offset. `RSCC`/`PDC` don't need it (confirmed: they use their own
   mapped base directly). This affected every Core-window register,
   including three (`GPU_GMU_AO_GMU_CGC_*`) that were in the very first
   `LemonadepA660HwInit.h` derivation and had been silently wrong since
   then. **Found by the host test's mock register bank crashing on an
   out-of-window array index** -- not by inspection. `derive-a660-hwinit.py`
   now applies this correction with one helper that also asserts every
   offset actually falls inside the real window, so it fails loudly
   instead of shipping wrong a second time.
2. **A missing step**: `a6xx_gmu_power_config()`'s RPMH_CTRL GPU-client
   vote enable and `a6xx_gmu_start()`'s real CM3 reset toggle, found only
   by continuing to trace past where an earlier draft of this file
   stopped.

`test_gmu_coldstart.c`: a mock register bank (not a stub that always
succeeds) runs the real function against the real `a660_gmu.bin` already
on the phone. Clean under ASan/UBSan, verifying specific sourced values
actually land on the wire and that the firmware payload reaches the right
ITCM/DTCM offsets.

**Not yet written**: SMMU context programming, the CP ring and submit/fence
(`NativeAdrenoRing`/`NativeAdrenoCpInit`), and the IOKit user client
implementing `NativeAdrenoABI.h`. Then, separately, the Apple cross-toolchain
build of Turnip + the Metal frontend bridge -- both need real CI time this
repo hasn't spent yet. (**Metal is explicitly de-scoped for now** -- the
near-term goal is proving the GPU wakes up and executes a real command,
which needs none of the Mesa/Turnip toolchain at all.)

## GPU, Phase 4: CX GDSC + GMU clocks written and host-tested; real SMMU stream/bank semantics traced (2026-10-23)

**`lemonadep_gpu_reserve.c`**: real CX GDSC enable (`SW_COLLAPSE`/`PWR_ON`
bit poll, `drivers/clk/qcom/gdsc.c`'s real algorithm) and the seven real
clock branches the GMU needs, sourced from `drivers/clk/qcom/
{gpucc,gcc}-sm8350.c`. GX GDSC needs no separate step -- confirmed earlier,
its real power-up is the GMU/RSC sequence already written, not a software
toggle. Host-tested with a mock register bank that starts in the real
cold-reset state (everything gated/collapsed) and proves the one
`BRANCH_HALT_SKIP` branch (`gcc_ddrss_gpu_axi_clk`) is genuinely never
polled, by leaving its `CLK_OFF` bit deliberately stuck and checking the
function still returns success.

**SMMU stream/bank semantics, traced from the real `arm-smmu.c` (not
guessed, not left as a TODO)**: the device tree's `iommus = <&adreno_smmu
SID MASK>` cells are literal -- confirmed directly in
`arm_smmu_of_xlate()`'s `FIELD_PREP(ARM_SMMU_SMR_ID, args->args[0])`/
`FIELD_PREP(ARM_SMMU_SMR_MASK, args->args[1])` -- so GPU uses SIDs 0 and 1
(two contexts), GMU uses SID 5, mask `0x400` on all three (architecturally,
an SMR mask bit of 1 means "don't care" for that ID bit; since 0/1/5 differ
in the bits the mask does *not* cover, this unambiguously separates the
three routes regardless of bit 10's value at runtime).

**What's genuinely not a DT-sourceable constant**: the real ARM SMMU
*context bank* number each stream lands on. On Linux this is picked
dynamically by the IOMMU core's own allocator at domain-attach time, not a
fixed hardware wiring -- there is nothing to "source" here the way a
register offset can be. Since our driver will exclusively own this SMMU
instance (no Linux allocator sharing it), the correct approach is to read
the real hardware capability register (`ID1`'s `NUMCB` field) at runtime
and pick free banks ourselves, not hardcode a guessed number. This is the
next piece of code to write, not more header-reading.

## GPU, Phase 5: vendored NativeAdreno/ArmSmmu4K/AdrenoPM4; found the CP ring/IRQ vendoring was wrong (2026-10-23)

Vendored `NativeAdreno` (minus `NativeAdrenoGmuPlatform.cpp/h`, dropped
after finding it's tightly bound to `ShengGmu.h`'s own types and redundant
with `lemonadep_gmu_coldstart.c`), `ArmSmmu4K` (full, including
`ArmSmmu4KAudit.{c,h}` and `ArmSmmu4KLive.{c,h}`, found to be real required
dependencies only by actually trying to syntax-check the include chain,
not by reading the file list) and `AdrenoPM4`. Full provenance (exact
commits, what's included and why) in `lemonadep/xnu/gpu/vendor/README.md`.
Verified GMU's real memory-region sizes for A660 specifically
(`a6xx_gmu_init()`'s own A660-specific comment: "A660 now requires
handling 'prealloc requests'... hardcode allocations based on the known
firmware") -- they turn out to match `ShengGmu.c`'s A740 values exactly,
because A660 takes the identical real branches.

**Caught a real, more serious problem before writing more on top of it**:
`NativeAdrenoRing.cpp`/`NativeAdrenoCpInit.cpp`/`NativeAdrenoIrq.cpp` are
**not chip-generic the way the headers and the earlier file-size survey
suggested** -- their actual submission, fence and interrupt mechanics are
tuned for A7XX specifically, not just differently addressed:

- `NativeAdrenoCpInit.cpp` implements `a7xx_cp_init()` (its own comment
  says so: "The payload values are a7xx_cp_init in a6xx_gpu.c"). The real
  dispatch is `adreno_is_a7xx(adreno_gpu) ? a7xx_cp_init(gpu) :
  a6xx_cp_init(gpu)` (`a6xx_gpu.c:1480`) -- A660 takes `a6xx_cp_init()`, a
  genuinely different packet: no `CP_THREAD_CONTROL` prefix (that disables
  concurrent binning, an A7XX-only feature), and an 8-word `CP_ME_INIT`
  payload with a different mask/layout, not the power-up-reglist-carrying
  7-word A7XX one (`a6xx_gpu.c:895-920` vs `924-968`).
- `NativeAdrenoRing.cpp`'s fence write itself uses `CP_EVENT_WRITE7`,
  its own comment says "on A7XX"; the SMMU table-switch cache handling is
  explicitly A7XX-only ("event 0x31 is only CACHE_CLEAN on A7XX"); the BR/BV
  thread framing in `submitFramed()` is a hardware feature A660 (not a7xx)
  doesn't have at all.
- `NativeAdrenoIrq.cpp`'s decode table is, by its own comment, "every A7XX
  status bit", including "A7XX LPAC/BV replicas" in the CP error mask.

Only `NativeAdrenoTimer.cpp` (confirmed: both counters are tagged
`A6XX-A7XX` in the real register XML, genuinely shared) and the
SMMU/memory pieces hold up as vendored. **Wrote `lemonadep_cp_init.cpp`**:
the real `a6xx_cp_init()` 8-word `CP_ME_INIT` sequence, new code (not
adapted from a vendored file), reusing only what's confirmed generic --
`NativeAdrenoRing::submit()`/`wait()` for the ring mechanics and
`AdrenoPM4Type7()` for packet-header encoding (`CP_ME_INIT = 0x48`,
untagged in `adreno_pm4.xml`, confirmed shared). `NativeAdrenoGpu::start()`
itself can't be used unmodified either -- it hardcodes a call into the
A7XX `CpInit` internally -- so the actual orchestration will be lemonadep's
own function with the same proven *shape* (reserve -> bootGmu ->
programSmmu -> ring attach -> CP init -> wait -> IRQ/timer attach), not a
call into the vendored class.

**Resolved, and it's worse than "needs checking": `NativeAdrenoRing::submit()`
is genuinely wrong for A660, confirmed, not just suspected.** The real
`a6xx_submit()` (`a6xx_gpu.c:416-430`, generic across the whole a6xx/a7xx
family *except this one piece*) writes the fence as `CP_SCRATCH(2)=seqno`,
then `CP_EVENT_WRITE` (opcode `0x46`, same number for both generations --
confirmed in `adreno_pm4.xml`, `CP_EVENT_WRITE` tagged `A2XX-A6XX` and
`CP_EVENT_WRITE7` tagged `A7XX-`, same opcode byte, different payload
*field layout* per generation) with event `CACHE_FLUSH_TS` (`0x04`) and the
IRQ bit, 4 plain payload words. `NativeAdrenoRing.cpp`'s `emitFence()`
instead builds event `0x31` (`CACHE_CLEAN`) plus an extra "enable" bit with
no equivalent in the classic format -- the A7XX `CP_EVENT_WRITE7` payload
shape, not A6XX's. Same opcode number, genuinely different, incompatible
payload. This is the A7XX GPU-generation protocol itself, not a chip
variant of an address.

**Conclusion: the CP ring and interrupt layer cannot be vendored for A660
at all** -- `NativeAdrenoRing.cpp` and `NativeAdrenoIrq.cpp` are protocol-
level A7XX implementations, not generically-addressed A6XX-family code
like `NativeAdrenoTimer`/`NativeAdrenoSmmuContext`/`NativeAdrenoMemory`
turned out to be. Submission and interrupt handling need the same
from-scratch treatment the GMU cold-start got: real `a6xx_gpu.c` source,
lemonadep's own code, not a vendored class. This is a real scope change
from the original vendoring plan, found by reading function bodies rather
than assuming headers and file sizes told the whole story -- worth stating
plainly rather than quietly working around.

## GPU, Phase 5 continued: real A660 CP ring written and host-tested (2026-10-23)

**`lemonadep_ring.c`**: the real `a6xx_submit()` sequence (`a6xx_gpu.c:
395-430`) -- `CP_SCRATCH(2)=seqno`, then `CP_EVENT_WRITE` with
`event=CACHE_FLUSH_TS(0x04)|IRQ` and the real 4-word payload. `CP_RB_CNTL
= 0x20c`, derived from the real `MSM_GPU_RB_CNTL_DEFAULT` formula in
`msm_gpu.h` (`BUFSZ=ilog2(32KiB/8)=12`, `BLKSZ=ilog2(32/8)=2`) and
confirmed A660 takes the `HAS_HW_APRIV` branch (no `NO_UPDATE` bit) from
its real catalog quirks. Deliberately simplified for this first bring-up,
the same "simplest correct path first" choice as ACD/IFPC: no RPTR-shadow
setup (legitimate given `HAS_HW_APRIV`) and completion is CPU-polled on
the fence memory instead of IRQ-driven, so `NativeAdrenoIrq.cpp`'s own
A7XX-specific interrupt decode doesn't need fixing for this milestone.

**Caught a real mistake in my own first draft, before testing**: hand-rolled
the PM4 packet header encoding instead of calling the real vendored
`AdrenoPM4Type4`/`AdrenoPM4Type7` functions, and got it wrong (missing the
real format's odd-parity bits entirely). Fixed by calling the real
functions instead of re-deriving the format.

**`lemonadep_cp_init.c`**: rewritten from an earlier `.cpp` draft to sit
directly on `lemonadep_ring.c` instead of the vendored (A7XX) `NativeAdrenoRing`
class, removing an IOKit dependency this logic never actually needed and
making it host-testable.

Both have real host tests (`test_ring.c`, `test_cp_init.c`) built around a
mock GPU that actually *executes* submitted packets (scans the ring for
the real packet header and writes its own fence) rather than a stub that
always reports success -- `wait()` observes a genuine completion, and the
tests assert the exact real payload bytes landed, including an explicit
check that no `CP_THREAD_CONTROL` packet (A7XX-only) is ever emitted.
Clean under ASan/UBSan.

Checked `NativeAdrenoMemory.cpp`/`NativeAdrenoSmmuContext.cpp` for the
same A7XX coupling that broke Ring/CpInit/Irq: clean, no chip-generation
references at all -- consistent with them being genuinely architectural
(SMMU table management, BO bookkeeping), not Adreno-generation-specific.
Safe to build the memory-allocation and SMMU-programming glue on top of
these as originally planned.

**Still to do**: the orchestrator tying `reserve -> bootGmu -> programSmmu
-> ring attach -> CP init` together, real memory allocation
(`IOBufferMemoryDescriptor`, the real pattern already used in
`SweetSurface.cpp`) for the GMU regions and ring/fence buffers, SMMU
context/table programming with the real bank/SID values already traced,
and the IOKit service publishing this as `IOAcceleratorES`. Then a boot-arg
gate, an overlay patch, and a real CI kernel build.

## GPU, Phase 6: memory/SMMU glue drafted; due diligence on "are we reinventing the wheel" (2026-10-23)

**Sourced the real SMMU output address size (`OAS`) instead of guessing
one.** `lemonadep_gpu_memory.cpp`'s first draft used `output_bits=32` with
a TODO -- wrong on its face (would cap physical addressing at 4 GiB).
`ID2` (`0x28`), bits `[7:4]`, is a 4-bit index into a fixed architectural
table, confirmed from the real `arm_smmu_id_size_to_bits()`
(`arm-smmu.c:1723-1738`): not a direct bit count. Added
`lemonadep_smmu_discover_output_bits()` next to the existing `NUMCB`
discovery (same ID1/ID2 register pair, same "read the real hardware
capability at runtime" pattern), host-tested against every real table
entry.

**`lemonadep_gpu_memory.cpp`**: allocates the GMU regions and ring/fence
buffers (`NativeAdrenoMemory::allocate`, confirmed chip-generic) and
programs two real SMMU contexts (`NativeAdrenoSmmuContext`, also confirmed
chip-generic) -- GMU on stream ID 5, GPU on stream IDs 0/1, the real
traced binding. **Honestly incomplete, flagged in the code, not hidden**:
only `icache` is actually mapped into the GMU context so far; `dummy`/
`debug`/`log`/`hfi` are allocated but not yet mapped (needs
`ArmSmmu4K`'s Live incremental-update API, not yet used). Can't be
syntax-checked locally -- same Apple-SDK sysroot limitation as every other
IOKit C++ file in this project.

**Checked whether this is being duplicated elsewhere, concretely, not by
assumption**: the two UniXNU device repos that actually target SM8350
(`UniXNU-haydn`, Xiaomi K40 Pro; `UniXNU-zeta`, Surface Duo 2) have no GPU
work at all -- `zeta`'s own handoff doc shows it's stuck before `launchd`
even starts successfully (`EBADARCH`: it targets real arm64e iOS 26,
needing pointer authentication hardware our iOS 16 arm64 approach
sidesteps entirely), and `haydn`'s `package/` directory has nothing but a
config file. A third, unrelated project (`lechaukha12/xnu-xzs`, Sony
Xperia XZs, Adreno 530) claims a hardware-verified GPU accelerator in its
README; reading the actual submit path found it submits a `CP_NOP` packet
with a marker string and reports success unconditionally, with no real
command execution or fence wait -- a stub that convinces userland a GPU
exists, not working acceleration (its own source comments reference
IDA-style `sub_0x...` names, and its doc links are local
`file:///Users/.../Desktop/...` paths). No shortcut was missed; the
low-level work in this project is genuinely new.

## GPU, Phase 6 continued: GMU region mapping completed via ArmSmmu4K Live update (2026-10-23)

**The TODO from Phase 6 was more than "map 4 more regions" -- the GMU
context's `prepare()` call itself was wrong.** `ArmSmmu4KLive`'s tables are
a fixed topology: `prepare()`'s `iova`/`bytes` span is reserved once and
nothing outside it can ever be mapped later, no matter how much table
budget is left (confirmed by reading `ArmSmmu4KLive.c`'s `layout()`/`walk()`,
not assumed from the header comment alone). The Phase 6 draft called
`prepare()` with only icache's own range (`[0x4000, 0x1000000)`), so
`dummy`/`debug`/`log`/`hfi` -- clustered past `0x60000000` -- were outside
the prepared aperture and could never have been mapped there, TODO or not.
Fixed by widening the aperture to span all five regions at once
(`LEMONADEP_GMU_APERTURE_IOVA`/`_BYTES`, `LemonadepGmuSource.h`, derived
from the real IOVAs/sizes already in that file, not hand-typed -- this
project has twice already shipped a hand-typed-wrong constant this exact
way).

**The aperture/table-budget math was verified empirically, not hand-computed.**
Widening the aperture to ~1.5 GiB (icache near 0 to hfi's end past
`0x60400000`) changes `ArmSmmu4KLive`'s required intermediate+leaf table
page count and the minimum `leaf_budget` (its `layout()` rejects a budget
smaller than the whole aperture's page count, not just the mapped
portion) -- both nontrivial LPAE-level arithmetic. Rather than trust hand
arithmetic (the class of mistake this project already caught twice: the
GMU `mmio_offset` correction, the firmware segment `file_offset` typos),
`test_gpu_memory.c` calls the real `ArmSmmu4KLivePrepare()` with a
generous trial budget and asserts the actual required page count: 774
pages, confirmed by the test run, comfortably under the chosen 1024-page
`LEMONADEP_GMU_TABLE_BUDGET_PAGES`. `LEMONADEP_GMU_LEAF_BUDGET` is a
derived macro (`APERTURE_BYTES/4096 + margin`), correct by construction
rather than guessed.

**`lemonadep_gpu_smmu_map.c`** (new, plain C, no IOKit -- host-testable
like `lemonadep_gpu_smmu.c`): `lemonadep_gmu_map_regions()` calls the real
vendored `ArmSmmu4KLiveUpdate()` once per region, growing the "before"
list each time exactly as that function's own contract requires. No new
mapping logic was written -- this is sequencing around the real vendored
call, the "translation layer vs. real work" line the user asked about
back in Phase 4 still holds here: there is no existing XNU SMMU driver to
sit this on top of, so this sequencing *is* the needed code.

**`test_gpu_memory.c`** (new): drives the *real* `ArmSmmu4KLivePrepare` ->
`ArmSmmu4KLiveAudit` -> `ArmSmmu4KProgramDisabledContext` ->
`ArmSmmu4KAttachStreams` -> `lemonadep_gmu_map_regions` (5x
`ArmSmmu4KLiveUpdate`) -> `ArmSmmu4KLiveAudit` sequence end to end against
a mock SMMU-500 register file, under ASan/UBSan. The mock only reports
enabled/attached once the real sequence actually wrote the right bits (not
a stub that always succeeds) -- same discipline as `test_gpu_reserve.c`'s
GDSC mock. One real hardware-semantics bug caught by the test itself: the
mock's first draft stored writes to the context bank's fault-status
register (FSR, `cb+0x58`) verbatim; real hardware clears fault bits
written as 1 (W1C), and `ArmSmmu4KProgramDisabledContext`'s own
UINT32_MAX "clear any stale fault" write depends on exactly that
semantics -- a verbatim-store mock made the function spuriously see a
fault and fail. Fixed in the mock, not the real driver.

**`NativeAdrenoSmmuContext.h`**: added three accessors
(`bank()`/`pollBudget()`/`visibilityHandle()`), alongside the
`arenaHandle()`/`stateHandle()`/`configuration()`/`ioHandle()` it already
had -- non-functional, no logic changed, just exposing state the class
already stores so `lemonadep_gpu_smmu_program()` can drive
`ArmSmmu4KLiveUpdate()` directly after `attach()`.

**`lemonadep_gpu_memory.cpp`**: `lemonadep_gpu_smmu_program()` now widens
the GMU `prepare()` call to the full aperture and maps all five regions
via `lemonadep_gmu_map_regions()`. GPU ring/fence context (stream IDs 0/1)
is unchanged. Still can't be syntax-checked locally (Apple SDK sysroot),
same limitation as every other IOKit C++ file in this project; the plain-C
mapping logic it calls into is fully host-tested above.

## GPU, Phase 7: top-level boot sequencing written and host-verified end to end (2026-10-23)

**`lemonadep_gpu_boot.c`** (new): sequences the plain-C pieces already
written and unit-tested individually -- `lemonadep_gpu_reserve()` (power/
clocks) -> `lemonadep_gmu_cold_start()` (GMU firmware up) ->
`lemonadep_ring_attach()` -> `lemonadep_a6xx_cp_init()` (the real
CP_ME_INIT) -- via direct `#include` of each, the same convention
`lemonadep_cp_init.c` already uses for `lemonadep_ring.c`. No new
hardware logic: this file is sequencing, not reinvention. SMMU context
setup and buffer allocation (`lemonadep_gpu_memory.cpp`, C++/IOKit) and
the real MMIO io-struct wiring are one layer up, in the not-yet-written
IOKit service; this function takes them already built.

**`test_gpu_boot.c`** (new): combines the three existing per-domain mocks
(`test_gpu_reserve.c`'s power/clock bank, `test_gmu_coldstart.c`'s GMU
register bank, `test_cp_init.c`'s ring mock that actually executes
submitted packets) and drives the *entire* sequence under ASan/UBSan.
Result: `status=LemonadepBootOk fence=1` -- a submitted CP_ME_INIT
command reaches a real completed fence through the exact call order a
device boot will use. This is the host-verifiable form of the milestone
the user asked for back in Phase 0 ("at least working GPU/acceleration
ASAP"); the remaining gap to an on-device log line is wiring real MMIO/
SMMU/memory instead of mocks (the not-yet-written IOKit service, boot-arg
gate and CI build -- see the open plan in this session).

## GPU, Phase 7 continued: GMU firmware delivery solved via the existing payload-segment mechanism (2026-10-23)

**Real blocker found before writing the IOKit service**: `lemonadep_gmu_cold_start()` needs the real
`a660_gmu.bin` bytes (pulled from the phone, gitignored, never touches CI) in kernel memory before
`load_firmware()` runs -- and IOKit matching happens well before UFS root is mounted, so there was no
existing way to get it there. Asked the user; chose "shim places it at a fixed PA, kernel reads via
boot-arg" (the same shape as `xnu/patches/1001`'s early framebuffer, which the real ABL-populated
`boot_args->Video` struct carries -- there is no equivalent ABI field for an arbitrary blob, so this
uses the free-text cmdline instead).

**No shim changes needed.** `tools/mkpayload.py`'s existing `--ramdisk` option already does exactly
this shape of thing: append an arbitrary blob as one more payload segment right after the kernel,
advance `kernel_end` past it, compute its real physical address from the kernel's own `virt_base`/
`phys_base` -- the loader (`shim/loader.c`) copies every segment generically, so a new segment needs
zero loader code. Generalized it as `--extra-blob NAME:FILE` (repeatable): same placement, but instead
of an ADT `RAMDisk` entry, appends `{NAME}_pa=0x.. {NAME}_bytes=0x..` to the cmdline boot-args for the
kernel driver to `PE_parse_boot_argn` and `ml_io_map`.

**Verified against a real downloaded kernel binary** (`lemonadep/m2-artifacts/kernel-bitbang/kernel.development.sweet`,
from an earlier CI run), not just read for plausibility: a 55256-byte test blob placed with
`--extra-blob gmufw:FILE` landed at PA `0x1024cc000` -- inside the documented kernel RAM window
(`0x1_00000000` + `0xfcc00000`, HWMAP_lemonadep.md section "DRAM banks"), right after the kernel's own
last segment, exactly the same "behind the kernel, below topOfKernelData" region the loader already
treats as its own scratch space for the ADT and `boot_args`. Not a new, unverified carve-out: the PA is
computed at build time from the real kernel image, not hand-picked. Confirmed the packed cmdline
actually contains `gmufw_pa=0x1024cc000 gmufw_bytes=0xd7d8`.

Also tightened `mkpayload.py`'s cmdline packing: it used to silently truncate a cmdline longer than
`BOOT_LINE_LENGTH` (608 bytes); now it asserts instead, since a silently-truncated boot-arg string
(now that `--extra-blob` tokens are appended automatically) would be a much harder bug to notice than a
build-time failure.

Next: the boot-build script for this milestone will need
`--extra-blob gmufw:lemonadep/fw-gpu/a660_gmu.bin` added to its `mkpayload.py` invocation, and
`SweetAccelerator.cpp` reads `gmufw_pa`/`gmufw_bytes` via `PE_parse_boot_argn`.

## GPU, Phase 8: first real patch into the xnu build -- power/clock + GMU cold start (2026-10-23)

**No `SweetAccelerator`/`IOAcceleratorES` IOKit service after all.** Reading how touch/keys/usb
actually integrate (`xnu/patches/0060`, `0072`) showed the real pattern: a plain-C board driver
(`bsd/dev/sweet_touch.c` etc.) called from one hook in `bsd/dev/sweet_ufs.c`'s `sweet_ufs_rootdev()`,
with IOKit only used for the user-visible HID event classes those drivers already have (`SweetHIDButtons`/
`SweetHIDTouch`, from the base `0058` patch). GPU bring-up needs no user-visible events for this
milestone, so `lemonadep_gpu_init()` follows that exact same shape, called right after
`sweet_usb_probe()`.

**Scope cut, on the user's call**: wiring the full GPU/GMU SMMU context and real IOVA-backed memory
(`lemonadep_gpu_memory.cpp`, C++/IOKit) into the build is its own patch. This first one only wires
`lemonadep_gpu_reserve.c` (CX GDSC + GMU clocks) and `lemonadep_gmu_coldstart.c` (real GMU firmware
load and cold start) -- real register writes, nothing that depends on the CP ring or SMMU being
correct. `lemonadep_gpu_init()` (new, `bsd/dev/lemonadep_gpu.c`) deliberately **stops right after GMU
cold start reports ready**, before the ring/CP_ME_INIT: submitting a command to the real CP through an
unmapped/untranslated SMMU context is not a risk worth taking before that context actually exists.
Off by default (`sweetgpu=1` to enable) -- unlike touch/keys/usb's opt-out default, every one of these
register writes is unverified on real hardware.

**New patch `lemonadep/xnu/patches/1007-lemonadep-gpu-bringup.patch`**: injects
`bsd/dev/lemonadep_gpu.c` (the glue -- boot-arg gate, `ml_io_map`s the GPUCC/GCC/five-GMU-window
physical ranges from `LemonadepGmuSource.h`, reads the firmware via `phystokv(gmufw_pa)`, calls
`lemonadep_gpu_reserve()` then `lemonadep_gmu_cold_start()`, logs each stage) plus
`bsd/dev/lemonadep/{LemonadepGmuSource.h,LemonadepA660HwInit.h,lemonadep_gpu_reserve.c,
lemonadep_gmu_coldstart.c}`, a `bsd/conf/files` entry, and the `sweet_ufs.c` extern+call-site hook --
generated as real unified diffs (not hand-transcribed) from the actual files already written and
host-tested this session, to avoid transcription mistakes in a ~1100-line patch.

**Actually verified, not just plausible-looking**:
- `lemonadep_gpu_init()`'s symbol/type usage (window enums, io-struct field names, status enums) was
  grep-cross-checked against the real `lemonadep_gpu_reserve.c`/`lemonadep_gmu_coldstart.c` definitions.
- Syntax-checked clean against minimal stub headers standing in for `pexpert.h`/`kern/clock.h`/etc
  (only `%llx` vs the host's native `uint64_t` width differs -- a host-only artifact: `uint64_t` really
  is `unsigned long long` on the actual arm64 Darwin target).
- **Ran the project's own `check-overlay.sh` against the real cloned `xnu-8796.141.3` base plus the
  full existing patch series plus this new one**: applies cleanly (not in the SKIP list; the two SKIPs
  that do print, `0031`/`0041`, are pre-existing and expected -- the script's own comment says so), and
  the patched tree's `bsd/dev/lemonadep_gpu.c`, `bsd/dev/lemonadep/*`, `bsd/conf/files` entry and
  `sweet_ufs.c` hook are all exactly as intended.
- Full real-SDK-header compilation isn't possible locally (`libkern/_OSByteOrder.h` isn't in the open
  xnu tree) -- the same limitation every other IOKit/BSD-adjacent file in this project has always had;
  real compilation happens on the next CI kernel build.

## GPU, Phase 8 continued: booted on real hardware -- power/clock reserve succeeded, GMU cold start timed out (2026-10-23)

**First real on-device result.** Built `shim/out/lemonadep/m3-gpu-boot.img` (via new `lemonadep/m3-gpu-build.sh`,
mirroring `m1b-build.sh`) with `sweetgpu=1` and the real `a660_gmu.bin` placed via `--extra-blob`, and
`fastboot boot`ed it (temporary, nothing flashed). Needed `unentitled_ios_sim_launch=1 ignition_level=1
launchd_unsecure_cache=1` too -- without them boot panics at `launchd` before the console shows enough to
read (a real, pre-existing, fully unrelated issue: `NOTES.md:171`, from 2026-10-15, long before any GPU
code existed -- `namespace 9 code 0xe` = `EXEC_EXIT_REASON_WRONG_PLATFORM`).

Read the actual console output by extracting frames from a screen-recording video (`ffmpeg -vf fps=2`,
then scanned the frames) rather than trying to time a photo -- the relevant lines scroll past in under a
second among everything else IOKit matching prints:

```
SWEET GPU: firmware at pa=0x1024d0000 bytes=0xd7d8
SWEET GPU: power/clock reserve status=0
SWEET GPU: GMU cold start status=3
```

`status=0` on power/clock reserve is a real success: CX GDSC enable + all 7 clock branches
(`lemonadep_gpu_reserve.c`) actually came up on real Adreno 660 silicon -- the first real GPU-adjacent
hardware success in this project. `status=3` (`LemonadepGmuTimeout`) on GMU cold start is a genuine,
well-defined failure (not a crash, not a corrupted kernel, not a hang) somewhere inside
`lemonadep_gmu_cold_start()`'s several real timeout-capable polls -- but the single status code alone
couldn't say which one.

**Added per-phase tracking** (`lemonadep_gmu_phase`, new enum in `lemonadep_gmu_coldstart.c`): an
optional `lemonadep_gmu_phase *phase` out-parameter, set just before each of the real timeout-capable
steps (RSC wake-ack poll, RSC sequencer-idle loop, PDC handshake writes, firmware load, fw_start
register writes, power_config, the GMU-start probe read, and the final FW_INIT_RESULT ready-poll), so a
future `LemonadepGmuTimeout` pairs with exactly where it happened instead of requiring another guess.
Threaded through `lemonadep_gpu_boot()` and the `bsd/dev/lemonadep_gpu.c` glue (now logs
`phase=N` alongside `status=N`); `test_gmu_coldstart.c`/`test_gpu_boot.c` updated and re-passing
(`phase=10` = `LemonadepGmuPhaseDone` on the real firmware, both host tests still green). Non-functional
otherwise -- no register write or poll logic changed, only which step is recorded as "current" when a
`TRY()` returns early.

The `mount[3] exited ... handshake structure not initialized` panic later in boot is confirmed the same
pre-existing, unrelated `NOTES.md:171` issue (reaching a userspace `mount` process at all means kernel-
level root mount -- and `lemonadep_gpu_init()`, called from that exact same `sweet_ufs_rootdev()` hook --
already completed without corrupting anything).

Next: rebuild with this patch, boot again, read which phase the timeout happened at.

## GPU, Phase 8 continued: found and fixed the real bug -- RSC wake-poll was never supposed to run (2026-10-23)

**Booted overlay 1007 again with phase tracking: `SWEET GPU: GMU cold start status=3 phase=2`** --
`LemonadepGmuPhaseRscWakeAck`, the very first poll in `rsc_pdc_handshake()` (right after writing
`RSCC_CONTROL_REQ = bit1`, waiting for `RSCC_CONTROL_ACK` bit 1). Read the frame via the same
video-frame-extraction approach as the first readable capture.

**Root cause, confirmed by reading the real `a6xx_gmu.c` directly (not assumed), from the pinned
clone already in `/root/gpuwork/`**: `a6xx_rpmh_start()` (the function this wake-ack poll comes from)
starts with:

```c
if (!test_and_clear_bit(GMU_STATUS_PDC_SLEEP, &gmu->status))
	return 0;
```

`GMU_STATUS_PDC_SLEEP` is set *only* by `a6xx_rpmh_stop()` (the suspend path, `a6xx_gmu.c:660`) and
checked/cleared only here (`:612`). `gmu->status` is zero-initialized at probe. So on a genuine first
cold boot -- never previously suspended -- the real driver's `a6xx_rpmh_start()` takes that early
`return 0` and **never touches `RSCC_CONTROL_REQ`/`RSCC_CONTROL_ACK`/`RSCC_SEQ_BUSY_DRV0` at all.**
Our code performed that wake request + poll unconditionally on every cold start -- exactly the bug: on
real cold silicon that was never previously put to sleep, nothing ever acks it, because nothing is
*supposed* to be woken.

Separately confirmed `a6xx_gmu_rpmh_init()` (the PDC/RSC-sequencer-microcode writes, what our
`rsc_pdc_handshake()` does *after* the now-removed wake poll) is called **exactly once, from GMU probe**
(`a6xx_gmu.c:2464`), never from the per-boot `fw_start` path -- for this driver's first and only
bring-up, "once at probe" and "once here" are the same call, so those writes were already correctly
placed; only the wake-poll before them was wrong.

**Fix**: removed the `RSCC_CONTROL_REQ` wake write, the `RSCC_CONTROL_ACK` poll and the
`RSCC_SEQ_BUSY_DRV0` idle loop from `rsc_pdc_handshake()` entirely (not gated/skipped -- deleted, with
a comment citing the real source, since this driver has no suspend/resume path that would ever need
them), going straight to the PDC-init writes. `LemonadepGmuPhaseRscWakeAck`/`RscSeqIdle` removed from
the phase enum (dead states). Host tests (`test_gmu_coldstart.c`, `test_gpu_boot.c`) updated -- their
mocks modeled the now-removed registers too -- and re-passing: 174 writes logged (2 fewer, exactly the
two removed `RSCC_CONTROL_REQ` writes), `phase=8`/`LemonadepGmuPhaseDone`.

This is the kind of mistake the project's own sourcing discipline exists to catch, caught the way it's
supposed to be caught: not by guessing a fix from the symptom, but by reading the real function the
failing poll came from and finding the actual gate condition. Next: rebuild, boot again, see whether
cold start now actually completes (phase=8/status=0) or hits a different, now-unmasked step.

## GPU, Phase 8 continued: a hard SoC-level fault, a due-diligence check, and staged on-device testing (2026-10-23)

**The fix above wasn't the end of it.** Rebuilding and booting again (same `sweetgpu=1`, now running the
untested firmware-load/fw_start/SYSRESET tail for the first time ever on real hardware) produced a
**hard SoC-level fault** -- the device dropped into Qualcomm's own `XBLRamDumpMain`/`phoenix:PHOENIX_XBL_RAMDUMP`
crash-collector log, not a kernel panic. That's a platform-level reset (likely a NOC/bus fault), a step
below anything our kernel's own panic handler can even catch.

**Due diligence first, per the user's explicit ask, before touching more code**: searched for any
existing project -- UniXNU's own flagship (A740/Xiaomi Pad 6S Pro, self-reported, unverified, a
different chip generation we already draw from), `UniXNU-sweet` (SM7150/Adreno 618, no bootable image),
`ios-guacamole` (SM8150/Adreno 640, GPU explicitly "roadmap", its own author notes Turnip/Freedreno/KGSL
all assume a Linux kernel interface that doesn't exist here -- the same wall), `ios-topaz` (claims
working GPU accel via unpublished patches, unverifiable) -- **nothing solves this for us; we are
genuinely at the frontier here**, confirmed by real searching, not assumed. Also checked the real
upstream commit history for *why* Linux does an initial DDR bandwidth vote and sets explicit clock
rates before GMU bring-up: the bandwidth vote is a **soft** reliability improvement (`a6xx_gmu_set_initial_bw()`
silently skips if its OPP lookup fails, GMU starts on default bus settings anyway) and the clock-rate
set is about determinism, not documented anywhere as crash-preventing. **Neither is a confirmed cause**
of the fault -- real but not a smoking gun, so not implemented yet on a guess.

**Instead: staged, boot-arg-selected testing**, so each further step can be tried on real hardware one
at a time without a new CI kernel build per attempt (`sweetgpu` is now an integer stage, not a boolean;
`lemonadep_gmu_cold_start()` gained a `stop_after` parameter and a finer-grained phase split -- a new
`LemonadepGmuPhaseGbifCx` isolates the GBIF QoS writes, the *only* writes in the whole function through
the GPU register window instead of the GMU Core window, and therefore the most plausible single point
where an unpowered/unclocked GX rail could produce exactly this kind of hard fault):

```
sweetgpu=1: power reserve + PDC handshake only
sweetgpu=2: + firmware load (ITCM/DTCM writes)
sweetgpu=3: + fw_start's HFI/fence/chipid Core-window writes
sweetgpu=4: + the GBIF QoS writes (GPU-window)
sweetgpu=5: + power_config
sweetgpu=6: + the GMU-start DTCM probe read (still before SYSRESET)
sweetgpu=7+: full run, including the SYSRESET toggle and ready-poll
```

`lemonadep_gpu_boot()`'s own GMU cold-start call is unaffected (hardcoded `stop_after=None`): a partial
GMU with the ring/CP_ME_INIT still attempted after it would make no sense, and that path doesn't reach
hardware yet regardless. New host test in `test_gmu_coldstart.c` proves `stop_after` actually halts
where asked (PDC writes present, firmware-load/fw_start/GBIF writes absent) rather than just returning
a plausible-looking status. All host tests still pass (174 writes / phase=9/Done on a full run, matching
the new phase count after inserting `GbifCx`).

Next: boot with `sweetgpu=1` (lowest-risk increment: PDC handshake only, the exact step that crashed
*before* the wake-poll fix and has never been exercised cleanly on its own) and work up one stage at a
time, rather than re-running the whole untested tail again.

Branch `lemonadep`, pushed to `fork` = github.com/popovluka/ios-sweet. Phone: LE2123 8 GB, Evolution X 16 + Magisk, slot b,
`fastboot boot` of our v3 shim works. Backups in `backup/partitions` (23, SHA-256 verified). Tools: `C:\platform-tools`,
WSL distro `LemonadepLab` (aarch64 gcc, dtc, qemu-user, AOSP mkbootimg at `/opt/aosp-mkbootimg`).

## Proven on device (M1a shim runs 1-6)
- Display: splash 0xE4D00000 1080x2412 stride 4320 32bpp; ABL keeps autorefresh on -> text visible.
- EL1, PSCI 1.1, vtimer INTID 27 works, SGI works, CPU_ON starts all 7 secondaries.
- UFS: UFSHCI 3.0 left running by ABL, reads OK, userdata = LUN0 LBA 0x2BE848.
- Hypervisor restrictions: `msr cntp_*` undefined at EL1. vGIC (IIDR 0x47000070): **GICR IPRIORITYR writes fault
  unpredictably** (test 6: SETTLE=0 but XNU SEQ fault=0x1FE: all 8 priority writes faulted, IGROUPR0 and ISENABLER0 never
  fault). RWP never sets; polling it is useless.

## Done since
- Overlay 1002 rewritten: skip GICR priority writes (`no-gicr-priority-writes`) + skip CNTP_CTL (`no-el1-physical-timer`);
  RWP wait dropped. mkadt lemonadep sets both flags. Pushed with `[kernel]` (watch the lemonadep.yml run).

## Next step
1. Kernel artifact `xnu-kernel-lemonadep` from that run: downloading needs GitHub sign-in (no `gh` here), so the user
   downloads the zip from the run page into `D:\ios port\` (or installs `gh` and runs `gh auth login`).
2. M1b (GAPS.md checklist): mkadt `--board lemonadep` (dram default set), mkpayload with boot-args
   `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 sweettouch=0 sweetkeys=0 sweetusb=0
   wdt=-1 wdt_test=no_user_checkin` (NO ufsrw=1 yet), shim build with OS_VERSION=16.0.0 OS_PATCH_LEVEL=2026-08,
   `fastboot boot`, user photographs. Expect a panic at root mount (userdata still Android).

## M1b kernel ready (2026-10-22 ~05:35)
- gh CLI installed + signed in as popovluka; can now watch/download CI runs directly.
- Kernel built: run 39716740124, both 1001+1002 applied (not skipped), only pre-existing
  C99-style warnings in touched files. Downloaded to xnu/out-1002/kernel.development.sweet
  (SHA256 0b52714352465715d64bec93d66f032106ed992458da45265fc12f0cd9034a5c).
- M1b boot image built via lemonadep/m1b-build.sh: shim/out/lemonadep/m1b-boot.img
  (SHA256 2312f619141d416ffeb532951503b79517ccfa1223d4f7423acc3f3f7ae23df7), cpus=1,
  read-only (no ufsrw=1). NOT YET BOOTED ON DEVICE.

## Next: M1b on-device test
`fastboot boot shim/out/lemonadep/m1b-boot.img` (nothing flashed). Expect: shim loader lines,
then XNU kernel console text (white on screen, keepsyms=1), likely ending in a panic about the
root device (userdata still Android f2fs) -- that is a PASS for M1b. User photographs the
final screen. If it hangs at "XNU jumping" with no kernel text, photo the bottom edge (1001
stage bands). Any crash before that point needs the ESR/ELR/FAR lines.


## M1 MILESTONE REACHED (2026-10-22 ~05:40) — kernel boots on real hardware

M1b ran on the phone (`fastboot boot shim/out/lemonadep/m1b-boot.img`, SHA256 2312f619...,
kernel SHA256 0b52714352..., cpus=1, read-only). Full XNU boot sequence succeeded:

- shim -> kernel handoff OK
- SWEET GIC init with overlay 1002 (priority writes skipped, CNTP skipped): **no abort**
- SWEET CPU: cpu=0 came alive, SGI1 OK
- SWEET FB: AppleCLCD 1080x2412 stride 4320 at 0xe4d00000, matches shim-measured geometry exactly
- MAC Framework initialized; zone/VM bootstrap completed (85 maps, 165 entries)
- Reached BSD root mount: `ufsroot0` recognized, UFS reads happening, correctly fails to mount
  because userdata still has Android's filesystem (`hfs_ValidateHFSPlusVolumeHeader: unknown
  Volume Signature : 749c`) -> infinite retry loop (mount(14) through mount(21)+), not a crash/panic.
  User power-cycled back to fastboot to break the loop (kernel has no PSCI reset in this path).

**This confirms the GIC/timer hypervisor workarounds in overlay 1002 are correct and the full
shim+kernel boot chain works on actual lemonadep (OnePlus 9 Pro) hardware**, through CPU/GIC/timer/
display/VM/BSD init, down to attempting (and correctly failing) root mount.

## Next: M2 (destructive — build iOS root image, flash to userdata)

Per GAPS.md checklist M2:
1. Build iOS root image per README.md Sec 2 (iPhone X 16.7.16/20H392, mkiosroot.sh) — needs the
   IPSW, not yet downloaded.
2. **DESTRUCTIVE: erases all Android user data.** Needs explicit user confirmation + step 0.5
   backups already done (23 partitions, backup/partitions/, SHA256 verified).
3. `fastboot -S 512M flash userdata ios16-root.simg`
4. Reboot kernel with `ufsrw=1 rootrw=1` (now safe to add since real root exists).
5. Pull persistent log via adb-root (Android still has Magisk root at this point, works fine;
   log pull method for later once Android is gone needs sorting per GAPS.md D10).

NOT STARTED: no IPSW downloaded, no root image built yet. This is a much bigger undertaking
than M1 (large download, apfs-fuse/hfsprogs tooling in WSL, sweet-root-tweaks.py changes to
review for lemonadep applicability).

## M2 root image BUILT (2026-10-22 ~08:16)

Built on a real Linux box (luka-macbookpro, EndeavourOS/Arch) via SSH, not WSL2 (whose
kernel has zero HFS+ support -- confirmed dead end, do not retry there). Script:
lemonadep/m2-build-macbook.sh (pushed, includes two fixes found along the way:
CMAKE_POLICY_VERSION_MINIMUM=3.5 for apfs-fuse's old CMakeLists.txt against CMake 4.x,
and -include cstdint to fix 48 files relying on a transitive include GCC 16 dropped).

Result: /mnt/m2work/build/ios16-root.simg on the external DataTraveler USB drive
(formatted ext4, mounted at /mnt/m2work on the Mac), 12,145,230,136 bytes (~11.3 GiB),
SHA256 46a7ad2676f44be5aa4cd6323fc92f3904a0cf932da0c57ff6e4f39bfa07dc35.
Built from iPhone X 16.7.16 (20H392), OS+SystemOS+AppOS dmgs (checksums in the script),
real D22AP device tree extracted (research/ipsw-x/d22ap.adt, gitignored).

NEXT: get ios16-root.simg onto the Windows machine (USB drive physically, or scp --
check which is faster/already done), then checklist M2 step 3: DESTRUCTIVE
`fastboot -S 512M flash userdata ios16-root.simg` -- NEEDS EXPLICIT USER CONFIRMATION,
backups already verified (23 partitions, backup/partitions/, done 2026-10-22 early).
Then boot kernel with ufsrw=1 rootrw=1 (lemonadep/m1b-build.sh CPUS arg reusable, just
add rootrw to BOOTARGS), pull log via adb-root on Android (Magisk still present at this
point) before/after, per GAPS.md D10 caveat about recovery access once Android's gone.

## M3 MILESTONE REACHED (2026-10-22 ~14:17) — LOCK SCREEN ON REAL HARDWARE

After fixing the boot-args (unentitled_ios_sim_launch=1 + ignition_level=1, see commit
above), booted shim/out/lemonadep/m2-boot.img (SHA256 c85599b9397cb0724b199074a12ebb29ba294b4e0bd10b4f870d46c9ff572359)
with the real iOS root now flashed to userdata. Result: full boot through to the
**iOS 16 lock screen rendering on the OnePlus 9 Pro display** -- time display, status
bar, slide-to-unlock bar, home indicator, all visible and correctly positioned. Took
roughly 4 minutes on the Apple logo (matches sweet's own documented boot time) plus
some minutes of daemon crash-loop noise (bluetoothd/fairplayd.H2 -- expected, no BT/SEP
hardware, same known gap sweet has and still reaches home screen through) before
SpringBoard settled and the lock screen appeared.

This reaches M3 directly from M1 (skipped needing overlay 1003 panel-geometry fix --
the native FHD 1080x2412 mode rendered correctly as-is, no centering/scaling needed).

Current boot-args (shim/out/lemonadep/m2-boot.img):
debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0
ufsrw=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin
launchd_unsecure_cache=1 wdt=-1 sweettouch=0 sweetkeys=0 sweetusb=0

touch and buttons are both still disabled (sweettouch=0 sweetkeys=0), so the lock
screen cannot be interacted with yet -- that needs overlay 1004 (keys: PMK8350 PON +
PM8350 GPIO6, mapped in HWMAP_lemonadep.md) and 1005 (touch: Synaptics S3908 on
GENI SE4, also mapped) per OVERLAY_PATCHES.md. Neither has been written yet.

NEXT: M4 (touch) / M5 (home screen + keys) -- write overlay 1004/1005, or just try
cpus=8 first to confirm multi-core boot still works now that userland is live (higher
risk of exposing a race condition overlay 1002 didn't need to handle with cpus=1).

Related projects noted (2026-10-22, not yet investigated): github.com/UniXNU/UniXNU
(UEFI-based iOS/macOS on non-Apple ARM64, different approach) and
github.com/oosexgang (hackios-xnu-kernel, hackios-xnu-userland -- new org, similar
bring-up effort, possibly worth cross-referencing for lemonadep-specific techniques).

## Overlays 1004 (keys) + 1005 (touch) written, kernel built, first on-device boot queued (2026-10-22 ~15:40)

GitHub's macOS Actions runners stopped being assigned for a few hours today
(queued jobs sat with no runner, then auto-cancelled after ~15 min -- a
transient capacity issue, not a quota/billing one: this repo is public, so
Actions minutes including macOS are free and unlimited regardless). Verified
by diffing `runner_name` across today's runs: real runner at 00:16, none
from ~12:13 onward, real runner again at 13:31.

Wrote overlay 1005 (`lemonadep/xnu/patches/1005-lemonadep-touch-tcm.patch`):
Synaptics S3908 touch over the standard TCM v1/v2 protocol, GENI SE4 (not
sweet's SE7), selected at runtime from a new ADT `/touch` node (so sweet is
byte-for-byte unchanged). Every address came from real source, not guesses
-- see HWMAP_lemonadep.md section 6: the exact driver match (Syna_tcm_oncell,
via its own `of_device_id compatible="synaptics-s3908"`, ruling out the
similarly-named S3706/Syna_tcm_S3910), the resolved TLMM base (the earlier
"disagreement" was never real -- vendor dtsi's raw IP base + the driver's own
pin-array offset fold to exactly mainline's pre-folded address), and the
full SE4/wrap0 GCC clock bring-up register map (vote reg/mask, 5 halt regs,
RCG), derived from `gcc-lahaina.c` the same way sweet's own SE7/wrap1
`touch_clocks()` is built.

Also added `lemonadep/xnu/test/test_touch_tcm.c` (+ `gen_touch.py`, extracting
the real functions verbatim, same technique `gen.py` uses for 1001): a host
ASan/UBSan + aarch64/qemu test of the TCM message-framing logic against a
scripted mock transport, wired into `check-overlay.sh`. It caught two real
bugs before they ever reached a kernel build: `touch_tcm_write` sent a bare
command twice, and `touch_tcm_read` left a message's trailing padding byte
unread when the whole payload fit in the initial header read, which would
have desynced the next transaction's framing. Both fixed and now pinned by
`write_zero_len` / `read_one_byte` test cases.

Manually triggered the kernel build (`gh workflow run lemonadep.yml -f
build_kernel=true`) once a real macOS runner was confirmed available (run
https://github.com/popovluka/ios-sweet/actions/runs/39786869668). **Both
overlays compiled and linked successfully** -- `kernel.development.sweet`,
SHA256 `7686e006b3d724ff63a18b7794156843cd568eee3f0d9c26d9510d235ff140dd`,
only the pre-existing declaration-after-statement style warnings (already
present before these patches, explicitly non-fatal via
`-Wno-error=declaration-after-statement`), no compile errors. The overlay
verification step confirmed `1001 + 1002 + 1004 + 1005 applied`.

Built the M2 boot image with **both touch and keys enabled together** for
the first time (`sweettouch=1 sweetkeys=1 sweetusb=0`, via
`lemonadep/m2-boot-build.sh`): `shim/out/lemonadep/m2-boot.img`, SHA256
`89f68b78dc39bdfab9a783bef9d46d115fe5098ed2fa892a0dff84fcde778034`.

**NEXT**: `fastboot boot` this image (temporary, RAM-only, safe to repeat)
and check the kernel console for `SWEET KEYS:` / `SWEET TOUCH:` lines (keys
now logs `pon-ppid=... kpdpwr-bit=... resin-bit=...`; touch now logs
`lemonadep=1` then, on a successful TCM bring-up, `TCM bring-up ok,
mode=...`), then physically try the power/volume buttons and a swipe on the
lock screen. Neither overlay has been tested against the real PMK8350/S3908
silicon yet -- this is the first real signal on whether the sourced register
map and protocol sequence actually match the hardware. If something's off,
the boot-args can disable either independently (`sweetkeys=0` or
`sweettouch=0`) to bisect.

## Review caught a real bug before the on-device test; rebuilt (2026-10-22 ~17:07)

A fresh adversarial review of overlays 1004/1005 (requested explicitly, before
the planned first on-device test) found a real bug outside anything the TCM
framing tests exercise: `sweet_touch.c`'s lemonadep path calls
`sweet_rpmh_vreg_enable("ldoc3", 3008)` to vote the touch rail, but that
function (`bsd/dev/sweet_rpmh.c`, added by the base sweet series) has a
hardcoded safety gate accepting only the literal string `"ldoc11"` --
sweet's own resource. The call would have returned `EINVAL` immediately,
aborting touch bring-up before any I2C/TCM access. The underlying mechanism
(`rh_lookup`/`rh_send`) is a genuine runtime cmd-db search, not a hardcoded
address, so the fix just widens the explicit allow-list to the one
additional resource now reasoned through: PM8350C L3. Appended to
`lemonadep/xnu/patches/1005-lemonadep-touch-tcm.patch`, commit `ed90ba2`.

Everything else survived the review: re-verified 1004's PPID math and
confirmed the RT_STS/SUBTYPE offsets are genuinely universal (only
`keys_read`'s low byte matters, no override needed there); re-verified
1005's DFS register offsets (`rcg+0x14`, `rcg+0x1c`) against the actual
generic `drivers/clk/qcom/clk-rcg2.c` (`SE_CMD_DFSR_OFFSET`/`SE_PERF_DFSR`),
not just "same IP generation" reasoning as before.

Rebuilt the kernel (run
https://github.com/popovluka/ios-lemonadep/actions/runs/39794051333, after
the first attempt hit the same transient macOS-runner queue issue and
needed one rerun) -- `kernel.development.sweet`, SHA256
`462f55a5ec6c9290b6163a9d977c1a926229520435b87f524ce3f30b2569d3f1` --
and the boot image: `shim/out/lemonadep/m2-boot.img`, SHA256
`fcda58a9eeff2560eda0ec17ad2002628639f20c10be97051b9e6e1155410c5a`,
same boot-args (`sweettouch=1 sweetkeys=1 sweetusb=0`). **This is the
image to actually test on-device** -- the earlier one (SHA256
`89f68b78...`) would have failed touch bring-up at the RPMh vote step.

## Touch: SE4 is GSI-only, so the bus is now bit-banged (2026-10-22 evening)

Observed on-device (persistent log via TWRP, `tools/sweet-log.py`), cpus=4:

- **Buttons work**: power blanks the screen, volume works. The vol keys were swapped;
  fixed in 1004 from the retail dtsi (RESIN = KEY_VOLUMEUP, PM8350 GPIO6 =
  KEY_VOLUMEDOWN) via ADT `swap-volume-keys`.
- **GCC**: `S_AHB` read `0x80000002` (CLK_OFF|HWCG). m_ahb/s_ahb are HW-clock-
  gated, so the halt check is skipped for them, as in Linux
  `clk_branch_in_hwcg_mode`. This was not a vote race: a retry made no difference.
- **SE4 reads FIFO_IF_DISABLE (0x64 bit0) = 1** and keeps that value after
  clearing 0xe18, so FIFO transfers time out. The vendor dtsi gives it
  `dmas = <&gpi_dma0 ...>`, which makes it a GSI-mode engine.
- **Driving SE4's SE-DMA engine crashed the SoC** into an XBL ramdump screen
  (commit 1f036eb). Don't try that again.
- **Retraction**: an earlier note that "touch works at cpus=1" was never backed by a
  log. The nav pill moving was most likely iOS's own hint animation.

Fix (commit b9cb8c6): **bit-bang I2C on TLMM gpio20 (SDA) / gpio21 (SCL)**.
It is ported from zoe-vb/ios-sargo `xnu/patches/0087`; that patch is the same
wall on the Pixel 3a, whose touch SE is also GSI-only. Lemonadep never touches
SE4 now, and sweet's FIFO path is unchanged. BB_HALF_NS = 2500 (~200 kHz).

TCM protocol bugs fixed in the same commit, against the vendor
`Syna_tcm_oncell` driver:

- Commands are `[cmd, len_lo, len_hi, payload]`. Before this fix, no length bytes were sent.
- `ENABLE_REPORT` takes a 1-byte payload (the report type, 0x11).
- After reset, bring-up waits for the unsolicited `REPORT_IDENTIFY` (0x10) and
  falls back to `CMD_IDENTIFY`. A non-application mode gets `RUN_APP_FIRMWARE`.

New test `lemonadep/xnu/test/test_touch_bb.c` (+ `gen_touch_bb.py`). The real
`sweet_touch.c` drives a simulated open-drain bus with a mock S3908 at
0x4B. The test checks:

- bring-up
- the exact report-config bytes the IC receives
- the enable payload
- a two-finger report followed by a lift, mapped to HID down/up events

It runs in `check-overlay.sh` on host ASan/UBSan and on aarch64 qemu, and passes.
The simulator can't prove the real IC's timing, pull-up strength, or the
exact `REPORT_TOUCH` format for an all-lifted frame. On-device log lines to
look for:

- `I2C bit-banged on gpio20/21`
- `TCM after reset: ... code=0x10 ... mode=0x1`
- `TCM bring-up ok`
- `report err=` (only if polls fail)

Kernel run: https://github.com/popovluka/ios-lemonadep/actions/runs/39853877374
(cpus=4 boot image built from it by `m2-boot-build.sh ... 4`).
