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

## GPU, Phase 9: stages 1-6 clean on-device; the GMU gets real memory and an SMMU context (2026-10-23)

**How logs are read now -- never film the screen.** Hold Power+VolDown to get to fastboot, then
`fastboot boot twrp-3.7.1_12-0-lemonadep.img`, wait for `adb shell true`, then
`MSYS_NO_PATHCONV=1 tools/sweet-log-pull.sh logs/X.txt`, then `adb reboot bootloader` to return to
fastboot with no buttons. The test boot **must have `ufsrw=1`**, or the kernel never writes the log ring
(sweet patch 0047): the first pull after the stage-1 run returned a stale M2 log for exactly that reason.
`m3-gpu-build.sh` now sets `ufsrw=1`, `sweetfb_noconsole=1` (console stays in the log, off the screen;
panics still draw), touch/keys on, `cpus=4`, and takes the stage from `SWEETGPU=N`.

**On-device, kernel run 39967799617 (`kernel-1007e`), all `status=0`, boot to a working home screen each
time** (logs `logs/lemonadep-gpu-stage{1b,2,3,4,6}.txt`, gitignored):

| stage | stopped after | phase |
| --- | --- | --- |
| 1 | PDC handshake | 2 |
| 2 | firmware load (ITCM/DTCM) | 3 |
| 3 | fw_start Core-window writes | 4 |
| 4 | GBIF QoS GPU-window writes | 5 |
| 6 | power_config + start probe read | 7 |

So the earlier XBL ramdump lies in what only the full run did: the SYSRESET release and what the GMU
does next. **Most likely cause, by reading our own code against the firmware layout:** of
`a660_gmu.bin`'s four blocks, the 54476-byte main block targets GMU address 0x4000, the icache, which the
GMU reaches through the Adreno SMMU. `load_firmware()` deliberately left that block "to the board glue",
but the glue never copied it, and no GMU SMMU context existed. Released from reset, the GMU's ITCM stub
would jump into unmapped memory. That's the classic trigger for a platform-level (XPU/NoC) fault.

**Prior art, rechecked at the user's request:** `joranv1/ios-sailfish` (and its 9 forks) is real and
deep, with incremental GPU bring-up up to presenting frames, but it's for Adreno 530 (a5xx), which has
**no GMU** at all. Nothing in it maps onto A660's GMU, though its working power-up does program RCG
rates explicitly, not just enable bits (a second real source for that gap; not implemented yet).

**`lemonadep_gmu_mem.c`** (new, plain C; `test_gmu_mem.c` host test, ASan/UBSan, real firmware):
- `lemonadep_hfi_table_init()`: word for word what `a6xx_hfi_init()`/`a6xx_hfi_queue_init()` write: a
  6-word table header (`size` is assigned the byte count 168 in the real code despite its "dwords"
  comment), then 3 queue headers (ids 0/1/2, A660 isn't legacy), and queues at hfi+4K/+8K/+12K.
- `lemonadep_gmu_icache_load()`: the icache-target blocks to `icache + (addr - 0x4000)`, as
  `a6xx_gmu_fw_load()` memcpys them.
- `lemonadep_smmu_probe()`: **read-only** SMMU-500 survey. It reads sCR0, ID0-2 and sGFSR, picks the
  first disabled stage-1 bank at or above NUMS2CB that no live stream translates through (Linux's
  allocator starts stage-1 banks there too), and the first FAULT, not-live stream slots, exactly what
  `ArmSmmu4KAttachStreams`' preflight demands.
- `lemonadep_gmu_smmu_setup()`: LivePrepare, Audit, publish tables, ProgramDisabledContext (ASID =
  bank, as Linux), AttachStreams, then `lemonadep_gmu_map_regions()` for icache/dummy/debug/log/hfi. It
  attaches **SIDs 5 and 0x405**, because mainline `sm8350.dtsi` has `iommus = <&adreno_smmu 5 0x400>`
  (SMR mask bit 10) and the kit installs only exact matches. The SMMU node is `dma-coherent`, so walks are
  coherent and mappings write-back. If sCR0 isn't what attach requires, it returns Unsupported with zero
  writes. The test proves that, and also proves the probe skips an enabled bank and a bank a live stream
  uses, and leaves both untouched after setup.

**Memory:** one 0x1414000-byte region the shim reserves below topOfKernelData, via
`mkpayload --extra-blob gpumem:zero:SIZE`: a segment with vmsize but no file bytes, which `shim/loader.c`
already zero-fills, so the boot image doesn't grow. The layout (tables, icache, dummy, debug, log, hfi)
is fixed in `lemonadep_gmu_mem.c`, with static asserts against overlap. The CPU writes it through
`phystokv()` and cleans it to PoC (`dc cvac`, line size from CTR_EL0) before the GMU or the SMMU can
see it.

**New stages** (same kernel, boot-arg only): **7** adds memory prep plus the read-only probe (logged:
`smmu scr0=... id0=...`, `bank=... slots=...`), still before SYSRESET. **8** adds the SMMU context; if
setup fails it stops *before* the GMU cold start, so the GMU is never released into unmapped memory.
**9+** is the full start.

Overlay 1007 is now generated by `lemonadep/xnu/gpu/gen-1007.py` from the tested sources: 20 new files,
6 build entries (the glue TU plus the 5 SMMU kit .c files, separate TUs because their static helper
names collide). `check-glue.sh` syntax-checks the glue and the kit for aarch64. check-overlay: ok.

Kernel CI run 39994848477: green, so the SMMU kit and GMU memory code compile in the real kernel
(release `lemonadep-2026-10-23`). **GPU work is paused here on purpose (2026-10-23):** another developer
has Metal and the GPU working on this phone, and we're splitting work with him rather than duplicating it,
so stages 7-9 stay built but un-run until we've compared approaches. If they're picked up again: run
`SWEETGPU=7` first (it reads the SMMU's real state before writing anything), then 8, then 9.

## Wi-Fi, stage 1: QCA6490 power + PCIe0 link, written and host-tested (2026-10-23)

The collaborator suggested we take Wi-Fi, BT, speaker and mic. Wi-Fi goes first. The chip is a QCA6490
(17cb:1103) on PCIe RC0. It's MHI-based, unlike ios-sailfish's QCA6174 (that repo's Apple80211/en0 layer is
the prior art for the top half later). Every fact comes from the live DT (`logs/lemonadep-live.dts`,
gitignored) and LineageOS kernel sources at commit 187d13c (`lemonadep/tools/fetch-los-k.sh`).

- `lemonadep/xnu/wlan/LemonadepWlanSource.h` holds every cited constant:
  - cnss rails, in order and without HPM: smpb10, ldoe7, smpe2, smpb11, smpc1, smpb12.
  - BT_EN off and 100 ms, then WLAN_EN on GPIO 64 and 10 ms.
  - PHY rails ldob5/ldob6 (HPM), GDSC, clocks, BCRs, PARF/DBI/iATU offsets.
  - PERST on GPIO 94.
- `LemonadepPcie0Phy.h` is generated by `gen-pcie0-phy.py`. It is the lahaina-v2.1 `&pcie0` override: 75 writes, checked identical to the live DT. The base dtsi's 77 writes are wrong for this phone.
- `lemonadep_wlan_pcie.c` is the bring-up. `test_wlan_pcie.c` passes under ASan/UBSan, covering:
  - a full run to ID 0x110317cb
  - stop-after
  - a failed rail
  - a dead chip
- `sweet_rpmh.c` now has a per-rail allow table and `sweet_rpmh_vreg_vote(res, mv, hpm)`. The touch path is unchanged.
- Overlay 1008 is generated by `gen-1008.py` against a patched tree. check-overlay is ok, and `check-glue.sh` syntax-checks the result.

**On-device:** `SWEETWLAN=N lemonadep/m4-wlan-build.sh KERNEL` builds the image. Set N as follows:
- 1: rails
- 2: WLAN_EN
- 3: PHY rails
- 4: GDSC
- 5: clocks
- 6: resets
- 7: PARF
- 8: PHY ready
- 9: PERST
- 10: link
- 11 or higher: read the config ID

Read the `SWEET WLAN:` lines from the TWRP log pull.

**Observed on hardware (2026-10-23):**
- **Run 1** (kernel CI 40002251006): smpb10 and ldoe7 voted OK. It stopped at smpe2 because this phone's cmd-db lists smpe2 twice at the same address (0x40500), and our lookup required exactly one match. It now accepts an identical repeat, like Linux's cmd_db (commit after 52b3e06).
- **Run 2** (kernel CI 40007775869, `sweetwlan=11`, `logs/lemonadep-wlan2.txt`): **the chip answered.**
  - All 8 rails voted with status 0. GDSC 0xf822f001 (on).
  - PHY ready on the first poll (status 0x86).
  - The link trained on the second poll: LTSSM 0x11 = L0, link status 0x30110008.
  - **Config ID 0x110317cb**, i.e. vendor 0x17cb, device 0x1103 (QCA6490). Class/rev 0x02800001: network controller, rev 1.
  - The boot then continued normally to SpringBoard.

Next: MHI and firmware download (the firmware stays on the phone and is never committed), then QMI, WMI and en0.

## Wi-Fi, stage 2: BAR0, MHI registers, SMMU probe, SBL over BHI (2026-10-23, host-tested)

**Prior art.**
- ios-sailfish 0170 (QCA6174, ath10k: no MHI): BAR0 at a fixed address in the RC memory window, raw-PA DMA, an SMMU register dump, firmware via the DT. Their SoC's PCIe SMMU let the DMA through untranslated.
- Here, the apps SMMU (`qcom,skip-init`, behind the hypervisor) is probed first, not assumed.

**Sequence.** It follows LOS-K step by step:
- `cnss_pci_enable_bus`
- pci-msm `msm_pcie_enumerate`: memory and bus master on the RP. iATU region 0 for config only; memory requests go out untranslated.
- `mhi_async_power_up`
- `mhi_fw_load_handler` / `mhi_fw_load_bhi`: STATUS=0, IMGADDR, IMGSIZE, TXDB, then poll.
- `mhi_ready_state_transition`

**Facts.** Each one is cited in `lemonadep/xnu/wlan/LemonadepMhiSource.h`:
- Firmware selection, cnss2 `cnss_pci_update_fw_name`: SOC_HW_VERSION (BAR0+0x224) major 2 → amss20.bin, otherwise amss.bin.
- From `cnss_pci_register_mhi`: SBL = the first 512 KiB, `fbc_download`.
- DMA mask: 36 bits.
- Live DT: `mhi,timeout` 10 s. The SMMU stream is 0x1c01, from `iommu-map`.
- The firmware lives in the modem partition under `image/`: amss.bin (5,178,848 B), amss20.bin (5,169,152 B), m3.bin.
  - Pulled with `lemonadep/tools/pull-wlan-fw.sh` into `lemonadep/fw-wlan/`, which is gitignored and never committed.
  - mkpayload embeds both as `wlanfw1`/`wlanfw2`. The chip DMAs directly from that physical address.

**Stages.** sweetwlan=N, continuing stage 1's numbering:
- 12: BAR0 sized and assigned (first aligned address in 0x60300000+), RP bus numbers and memory window set, memory and bus master on both ends.
- 13: read MHI/BHI registers, SOC version and EE, plus a read-only SMMU probe (sCR0, then SMR/S2CR matching 0x1c01).
- 14: SBL over BHI. **This only runs if the SMMU passes 0x1c01 untranslated:**
  - CLIENTPD set, or
  - the matching S2CR is bypass, or
  - nothing matches and USFCFG=0.
  Otherwise it stops with `SmmuNotBypass`. No stream table is ever written.
- 15+: wait for MHI READY, log the EE (expect SBL).

`test_wlan_mhi.c` passes under ASan/UBSan. The mock chip cooperates only when the sequence is right. It covers:
- full run; picking amss vs amss20
- stopping after the register read
- SMR translate vs bypass, and USFCFG fault
- no blob; wrong EE
- BHI error; BHI timeout

Not done yet: MHI context and rings, M0, BHIe (rest of the image), interrupts, the PCIe→DDR interconnect vote. If BHI times out with the SMMU in bypass, that vote is the first suspect.

**Run 1 of stage 2 (kernel CI 40013326022, sweetwlan=15, 26 MB image with both firmware blobs): no log at all.**
- The pulled ring was byte-identical to the previous boot's, flush #13 again. The new boot never flushed once.
- Flushes come every `ufslogsecs` (10 s by default), so the SoC stopped, or the boot never got that far, within 10 s of the log starting.
- Not yet known which of these it was:
  - the bigger payload
  - stage 1 (unchanged and proven, so unlikely)
  - BAR0 sizing
  - the first BAR0 read
  - the apps SMMU GR0 read (the hypervisor may trap it)
  - the BHI doorbell

Fix for the next run:
- Stage 2 now runs on its own thread after stage 1.
- `wlan_checkpoint` logs `SWEET WLAN: next: <access>` and waits 2.5 s before each risky access.
- The build sets `ufslogsecs=1`.
- One boot now names the access that kills the SoC, and the stage-1 lines show whether the payload booted.

**Root cause, found by bisecting on hardware (2026-10-24):** it was the kernel, not the chip.
- Every boot of the new kernel left no log: `sweetwlan` 13 and 11, with or without firmware blobs or `ufslogsecs`.
- The old kernel-1008b image logged fine (`logs/lemonadep-good2.txt`).
- The only kernel change is the window mapping. XNU's `ml_io_map` allocates from `io_submap`, which is **8 MiB for the whole kernel** (`osfmk/arm/io_map.c` `IO_MAP_SIZE`), with `KMA_NOFAIL`.
- Stage 2's 8 MiB PCIe memory window can never fit there, so the kernel dies at the mapping, before stage 1 even runs.
- Fix: all Wi-Fi windows now use `ml_io_map_unmappable(pa, size, VM_WIMG_IO)`, which allocates from kernel_map.
- **Rule for every driver here:** big MMIO windows never go through `ml_io_map`.

**Run with the io_map fix + auto-reboot (kernel CI 40094527584, sweetwlan=15, sweetreboot=150; `logs/lemonadep-wlan9.txt`), observed on the phone:**
- **The auto-reboot works.** The phone booted at 14:43:58 and was back in fastboot by itself at 14:47:12. The SDAM APID is 89, owner 0.
- **BAR0** is 2 MiB at 0x60400000. RC and EP command registers are both 0x100006 (memory + bus master).
- **MHI registers:**

  | Register | Value | Meaning |
  |---|---|---|
  | REGLEN | 0x100 | |
  | CFG | 0x800080 | |
  | BHIOFF | 0x100 | |
  | BHIEOFF | 0x224 | |
  | SOC_HW_VERSION | 0x400c0110 | major 1, so the chip needs **amss.bin** |
  | EE | 0 | PBL |
  | MHIVER | 0xdeadbeef | probably not readable before wake; to be checked |

- **SMMU:** sCR0 = 0x270406, so USFCFG is set and unmatched streams fault. ID0 = 0x4c017e59 (89 SMRs). No SMR matches 0x1c01.
  - The guard stopped before BHI (status 7). No DMA was attempted.
- New stage 14 handles this case. It writes a bypass entry (S2CR bypass + NSH, then SMR valid|0x1c01, mask 0) into the first free slot, reads both back, and stops if either was refused. This mirrors arm-smmu.c `arm_smmu_write_sme` for a bypass domain; LOS-K uses S2CR bypass for those.
- BHI is now stage 15 and READY is 16.

**Run with stage 14 (kernel CI 40095843825, `logs/lemonadep-wlan10.txt`):**
- The SMR write stuck: entry 8 = 0x80001c01.
- **S2CR bypass did not stick:** 0x10300 was written and 0x20300 (fault) read back. The guard stopped before BHI (status 9).
- This is the Qualcomm firmware quirk that mainline handles in `arm-smmu-qcom.c qcom_smmu_cfg_probe` (v6.6). Its comment says "writing BYPASS will end up written as FAULT".
- The fix follows mainline. The last context bank gets SCTLR = 0 and CBAR type S1_TRANS_S2_BYPASS, and the stream's S2CR becomes TRANS → that bank.
- First we check that nobody uses that bank: SCTLR.M must be 0 and no valid entry may already translate to it. If it is in use, the step stops (`SmmuCbInUse`).
- The geometry comes from ID1 (pgshift, numpage, NUMCB), and the whole 1 MiB SMMU window is now mapped.

Two other findings from the bisect:
- `adb exec-out dd` can stall part-way on Windows, leaving a truncated log that looks stale. `tools/sweet-log-pull.sh` now runs dd into the recovery's /tmp and then `adb pull`s the file.
- The 26 MB image (full firmware) was probably fine. Only the SBL slices are embedded now anyway; BHIe will read the rest from disk.

## Auto-reboot to fastboot: overlay 1010 (2026-10-24, host-tested, not yet seen on the phone)

The user asked for this so test boots don't need someone to hold Power+Vol Down. Boot-arg `sweetreboot=<seconds>[,recovery]` (absent = off).

**Mechanism** (`lemonadep/xnu/reboot/`):
- The persistent-log thread polls the deadline. PixeliOS 0083/0086 found that a sleeping thread of its own "never fired in 3 of 4 runs".
- The first tick past the deadline logs. The next tick resets, so that line has reached the disk.
- It also sets `PE_halt_restart`, so a panic or restart lands in fastboot instead of sitting there.

**Reset**, as the vendor kernel's `reboot bootloader` (LOS-K `qcom-reboot-reason.c` + `msm-poweroff.c`):
1. The reason byte goes into PMK8350 SDAM 0x7148, bits 7:1: bootloader 0x02, recovery 0x01. This is the live DT's `reboot_reason` nvmem cell.
   - It is written over SPMI through the arbiter's owner channel: the APID owned by EE 0, v5 command format (`spmi-pmic-arb.c`).
   - The old byte is read first through the observer channel, so bit 0 stays as it was.
2. The IMEM `restart_reason` (0x146bf65c) is set to 0x77665500 (or 0x77665502 for recovery).
3. PSCI SYSTEM_RESET via smc.

All windows are mapped when the timer is armed, from kernel_map. `test_reboot.c` passes under ASan/UBSan.

**Host side:** `lemonadep/tools/test-cycle.sh IMAGE OUT.txt` boots the image, waits for fastboot, boots TWRP and pulls the log. `m4-wlan-build.sh` sets `sweetreboot=300` by default (`SWEETREBOOT=0` turns it off).

Hard SoC crashes (XBL ramdump) still need a button press.

## Overlay 1009: four UI fixes from PixeliOS (2026-10-23)

PixeliOS ([PixOS-Project/pixelios-kernel](https://github.com/PixOS-Project/pixelios-kernel)) is iOS 16 on the Pixel 6a, built on the same ios-sweet base. The four patches below touch files that are byte-identical to ours, so `lemonadep/xnu/gen-1009.sh` applies them unchanged at their commit 41198ee. check-overlay: ok.

| Their patch | What it fixes |
|---|---|
| 0099 | A stand-in for `IONetworkStack`. configd's InterfaceNamer waits 300 s for it. **This is our Settings → About freeze.** Wi-Fi/en0 needs it as well. |
| 0084 | Removes the IOKit busy hold that waits for kernelmanagerd (which iOS doesn't have), and adds an empty `IOAcceleratorES` so QuartzCore stops polling for 30 s per backboardd start. `sweetaccel=0` turns the stand-in off; check it against the collaborator's Metal work. |
| 0085 | Sandbox op 4 returns a container path for app extensions. Without it, PlugInKit aborts every `.appex`. |
| 0101 | Vsync now reports the refresh period, which was 0, so CoreAnimation saw every frame deadline as already missed. |

They observed all four on hardware. On our phone they are not yet observed.

PixeliOS's Wi-Fi (BCM4389, Broadcom) and GPU (Mali) don't carry over to our Qualcomm parts. Their USB CDC-ECM live log (UDP) is worth porting later as a faster log channel.

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

## GPU, Phase 18: HFI transport written as stage 10 (2026-10-29)

Stage 10 = stage 9's cold start, then the HFI handshake and the messages that
need no tables. `sweetgpu=10`.

**What stage 9 still owed.** `a6xx_gmu_fw_start()` ends with `a6xx_gmu_hfi_start()`:
`GMU_HFI_CTRL_INIT = 1`, then poll `GMU_HFI_CTRL_STATUS` bit 0. This driver never did
that — the `hfi_status=0x1` Phase 17 reported was the firmware's own doing, not ours. Stage
10 does it first, in the same place upstream does (right after the cold start reaches Done).

**Transport.** `lemonadep_hfi_send()` is `a6xx_hfi_send_msg` + `queue_write` +
`wait_for_ack` without an interrupt handler:

- Header dword is `(seqnum << 20) | (HFI_MSG_CMD << 16) | (dwords << 8) | id`, and
  `HFI_MSG_CMD` is 0, so the type field contributes nothing.
- Write into command queue 0, wrapping on the 1024-dword size, then pad to the next
  4-dword boundary with `0xfafafafa` — upstream's "cookify" step, which is how the
  firmware tells padding from a message.
- Ring `HOST2GMU_INTR_SET = 1`, poll `GMU2HOST_INTR_INFO` for MSGQ, clear via
  `GMU2HOST_INTR_CLR`. Upstream allows a **full second** here, far beyond `poll_core()`'s
  hard 10 ms cap, so this polls directly instead of reusing it.
- Read reply queue 1: the dword at `read_index` is the response header and counts
  towards the size in its `[15:8]`; `read_index` is then realigned to 4 dwords.

Two things verified rather than assumed:

- **The queue table was already right.** I suspected `lemonadep_hfi_table_init()` was wrong
  (3 queues of 4 KiB, `tx_request=0`) and nearly "fixed" it. Current upstream is
  `HFI_MAX_QUEUES 3`, three `SZ_4K` queues at 4/8/12 KiB, `tx_request = 0`, type
  `10 << 8 | id` with ids 0/1/2 — exactly what it does, 168-byte table and all. Left alone.
- **The interrupt mask needs no write.** `A6XX_HFI_IRQ_MASK` is `CM3_FAULT` alone, so MSGQ
  stays *masked* while upstream polls `INTR_INFO` for it. INTR_INFO is therefore raw status,
  and this driver never touches `GMU2HOST_INTR_MASK`. That avoids a risky write.

**Cache maintenance: a new `lemonadep_flush_from_poc()` (`dc civac`).** Phase 16 noted the
kernel had only a clean (`dc cvac`) and no invalidate, which made a GMU-memory dump a
false-negative risk. Reading the reply queue has exactly that problem, so this is no longer
optional. `civac` writes back anything of ours still dirty before invalidating, which matters
because the GMU owns `write_index` while we own `read_index` in the *same cache line*.

**What is deliberately not sent.** `PERF_TABLE` and `BW_TABLE` come first in upstream's
order and are both skipped. `PERF_TABLE` needs per-level ARC votes read out of RPMh's
cmd-db (`a6xx_gmu_rpmh_votes_init()`), which this driver has no code for, and sending a
half-made table is worse than sending none. `a660_build_bw_table()`'s data is already
transcribed here as `LemonadepA660BwTable_`, so BW_TABLE is the cheaper of the two to add
next. **So `CORE_FW_START` and `START` may well be refused as out of order — and that is
still a success for this stage, because a refusal is a reply, and a reply is what is being
tested.** Each message is reported separately for that reason.

Also confirmed while reading: `probe` is `GMU_CORE_FW_VERSION` (Phase 16), `a660`'s
`gbif_cx = a640_gbif` so the GBIF QSB writes belong in fw_start, and `gmu_chipid` is absent
from the a660 catalog entry so upstream computes `0x06060100` — our constant.

**Not yet run on hardware.** It builds and passes `lemonadep/tools/syntax-check-gpu.sh`
(see below), nothing more.

### New local tooling (commit `1517017`)

- `lemonadep/tools/check-patch-hunks.py` — validates every `@@` header against its body.
  It was re-derived from scratch across sessions; it is in the repo now.
- `lemonadep/tools/extract-patch-tree.py` + `syntax-check-gpu.sh` — materialise the patch's
  new files and run `clang -fsyntax-only` over `lemonadep_gpu.c` with stub XNU headers.
  Catches typos, bad types and bad printf formats in seconds. It caught a real breakage in
  this very change: an edit left an orphaned `void` and made `lemonadep_gpu_init` static
  although it is declared `extern`. Stub `uint64_t` is `unsigned long` on a Linux host, so
  the three `%llx` warnings it prints are expected and are on pre-existing lines.

### Reading the next log

```
SWEET GPU: HFI ctrl_init status=0 ctrl_status=0x1
SWEET GPU: HFI fw_version status=0 intr=0x1 hdr=... ret=... err=0x0 payload=...
SWEET GPU: HFI core_fw_start ...
SWEET GPU: HFI start ...
SWEET GPU: HFI is up -- every message acked
```

- `fw_version status=0 err=0x0` is the real milestone: queue layout, header encoding,
  doorbell, interrupt and reply read are all correct.
- `status=3` (Timeout) means no MSGQ within a second — the doorbell or the queue table is
  wrong. `status=4` (StateMismatch) means MSGQ fired but the queue was empty.
- `err` non-zero, or `hdr & 0xff == 100` (`F2H_MSG_ERROR`), means the transport works and
  the firmware rejected the message — most likely the missing tables, which is expected for
  `core_fw_start`/`start` and would be a surprise for `fw_version`.

## GMU MILESTONE REACHED (2026-10-29) — the GMU executes its firmware and reports ready

Commit `936ac6e` (pipeline 24, kernel `a458ac3e`, image `3daea6fb`), on real hardware:

```
SWEET GPU: gpucc gmu_rcg cmd=0x0 cfg=0x602 cx_gmu=0x6221 ahb=0x0 gdsc_cx=0xf8222000
SWEET GPU: GMU cold start status=0 phase=9
SWEET GPU: GMU start-ready probe=0x30010008 -> mask=0x1ff want=0x100, FW_INIT_RESULT=0x100
SWEET GPU: GMU m3 fw_busy=0x0 sysreset=0x0 fence_status=0x0 seq_busy=0x0 general0=0x30000009 general1=0x0 hfi_status=0x1 hfi_ver=0x20010000
SWEET GPU: GMU reports ready. Stopping before CP ring/CP_ME_INIT
```

**`status=0 phase=9`** is `LemonadepGmuPhaseDone` — the entire cold start completed. The
firmware wrote `FW_INIT_RESULT=0x100`, exactly the `want` the DTCM probe selected, and three
firmware-written registers are now non-zero where every one of them read `0x0` before:
`general0=0x30000009`, `hfi_status=0x1`, `hfi_ver=0x20010000`. The GMU is executing.

**The blocker was the clock, nothing else.** `cfg=0x602` confirms the RCG took the 200 MHz
entry, and that single change turned a 10 ms timeout into a ready report. For the record, at
19.2 MHz the firmware did not merely run slowly — it never set a single status bit in 10 ms,
so this was a hard requirement and not a margin problem.

`fw_busy=0x0` even now, which retroactively confirms the Phase 16 caveat: `FW_BUSY` does not
assert during init, so it was never usable as an "is the M3 alive" signal. `hfi_status` and
`hfi_ver` are the trustworthy ones.

### What this is and is not

This is **not** GPU acceleration, and nothing on screen changes. The GMU is the A660's power
management microcontroller; having it alive is the precondition for touching the 3D core, not
rendering. The UI is still CPU-composited. Stage 9 deliberately stops here, as its own log line
says, before the CP ring and `CP_ME_INIT`.

### The road from here, in order

1. **HFI message sequence** (`a6xx_hfi_start()`): `perf_table`, `bw_table`
   (`LemonadepA660BwTable_`), `core_fw_start`, `send_start`. `HFI_QTBL_ADDR`/`QTBL_INFO` are
   already programmed and `hfi_status=0x1` says the firmware accepted the table, so this is the
   natural next stage. ACD and IFPC stay skipped.
2. **GX rail + the 3D core** (`a6xx_hw_init()`): CP ring allocation, `CP_ME_INIT`, the
   `a660_protect` register-protection table, `a660_hwcg`, `prim_fifo_threshold 0x00300200`.
3. **The zap shader** (`a660_zap.mdt`), which needs the SoC's secure world — likely the hardest
   remaining piece and worth researching before committing to an approach.

### Open threads carried forward

- Context bank 0 collides with the 3D core's context 0; furangelz use context 1. Revisit before
  the 3D core goes up, not now.
- `fsr=0x400` at setup is the boot loader's, present before we touch anything. Benign.
- `ahb=0x0`: `gpu_cc_ahb_clk`'s CLK_ENABLE reads back clear though set, while CLK_OFF is clear
  and GPUCC access works — apparently hardware-controlled on this part.
- The whole firmware load path (ITCM, DTCM, icache) is verified byte-for-byte on hardware. Do
  not re-investigate it.
- `a6xx_rpmh_start()` stays skipped; see Phase 15 for why re-adding it is known to fail.

## GPU, Phase 17: ITCM is perfect, the GMU clock was at 19.2 MHz — set the RCG to 200 MHz (2026-10-29)

Commit `e63dea9` (pipeline 23, kernel `9aee9098`, image `f6b225ec`) answered both questions:

```
SWEET GPU: gpucc gmu_rcg cmd=0x0 cfg=0x0 cx_gmu=0x6221 ahb=0x0 gdsc_cx=0xf8222000
SWEET GPU: GMU itcm=0x10007f80,0x105,0x145,0x159 (want 0x10007f80,0x105,0x145,0x159)
```

**ITCM is byte-perfect** — the expected Cortex-M3 vector table, exactly. Together with the
DTCM proof in Phase 16, the *entire* firmware load path (ITCM, DTCM, icache) is now verified
on real hardware. The M3 has valid code at its reset vector. That closes the hypothesis that
outranked everything else; do not revisit the TCM load.

**The GMU core clock was wrong.** `cfg=0x0` decodes as src_sel 0 (`P_BI_TCXO`) with divider 1,
i.e. **19.2 MHz**, where `a6xx_gmu_resume()` does `clk_set_rate(gmu->core_clk, 200000000)`
before enabling anything. Supporting reads all healthy, which is what makes the rate the
outlier rather than a symptom:

- `cmd=0x0` — ROOT_OFF (bit 31) clear, so the root really is running.
- `cx_gmu=0x6221` — CLK_ENABLE (bit 0) set, CLK_OFF (bit 31) clear: the branch is live.
- `gdsc_cx=0xf8222000` — PWR_ON (bit 31) set, SW_COLLAPSE (bit 0) clear: the rail is on.

Also cleared along the way: the HWCG escape hatch in `enable_branch()` is **not** silently
accepting a halted GMU branch. It is gated on a per-branch `hwcg_capable` argument that only
the two GCC branches pass, so `cx_gmu` is polled strictly and genuinely cleared CLK_OFF.

**The change.** Program `gpu_cc_gmu_clk_src` (`cmd_rcgr 0x1120`) to the table's 200 MHz entry
before the branch enables, in upstream's order (rate, then enable):

1. Enable `gcc_gpu_gpll0_div_clk_src` — GCC `0x52000` bit 16, `BRANCH_HALT_DELAY`, so set and
   delay 10 µs rather than poll (gcc-sm8350.c:1625). **This has to come first**: src 6 is
   `P_GPLL0_OUT_MAIN_DIV`, and switching the mux to a source that is not running would leave
   the GMU with no clock at all — worse than a slow one. Its parent is `gcc_gpll0_out_even`,
   a postdiv of the SoC's main PLL, already up on any booted device.
2. `clk_rcg2_configure()` with `mnd_width 0`: rewrite `CFG_RCGR` src_sel `[10:8]` = 6 and
   src_div `[4:0]` = 2 (`2 * 1.5 - 1`, `hid_width 5`) → `cfg = 0x602`, then set `CMD_RCGR`
   bit 0 (UPDATE) and poll it clear, as `update_config()` does. The mux is glitch-free, so
   reprogramming it while the branch is parked on TCXO is safe.

`0x52000` is inside the GCC window this driver already maps (`0x100000 + 0x72000`), so no new
`ml_io_map` — which matters, see [[xnu-io-map-limit]].

Reading the next log: `cfg=0x602` confirms the rate took. If `FW_INIT_RESULT` moves off zero,
the clock was the blocker. If it stays zero with `cfg=0x602`, the GMU sequence itself is now a
faithful match to upstream on every point checked so far, and the next candidates are the HFI
programming (`HFI_QTBL_ADDR`/`QTBL_INFO`, which point at IOVAs the GMU must translate) and the
`PWR_COL_CP_MSG` log-buffer pointer.

One known cosmetic oddity, not worth a boot by itself: `ahb=0x0` means `gpu_cc_ahb_clk`'s
CLK_ENABLE bit reads back clear even though `enable_branch()` set it. CLK_OFF is also clear, so
the clock runs (GPUCC register access demonstrably works), which suggests that branch is
hardware-controlled on this part. Noted in case it matters later.

## GPU, Phase 16: the M3 never executes — ITCM and the GMU clock RCG are the two unchecked paths (2026-10-29)

Commit `6a3dc4a` (pipeline 22, kernel `3d9e3eb4`, image `e2a9ab01`) on-device:

```
SWEET GPU: GMU m3 fw_busy=0x0 sysreset=0x0 fence_status=0x0 seq_busy=0x0 general0=0x0 general1=0x0 hfi_status=0x0 hfi_ver=0x0
```

Every read zero, and each zero rules something out:

- `sysreset=0x0` — our `1 -> 0` toggle **did** take; the M3 is not held in reset.
- `fence_status=0x0` — the AHB fence refused nothing, so the `0xa`/`0xa0` fence range is fine.
- `seq_busy=0x0` — the RSC sequencer is not stuck.
- `general0/1`, `hfi_status`, `hfi_ver` all zero — nothing firmware-written has been written.

Reads through that window work (the probe returns `0x30010008` in the same sequence), so these
are real values, not a dead window. The M3 is out of reset and is doing nothing.

**The poll is not too short.** `poll_core()` has a hard 10 ms wall-clock cap and a 100 µs
sleep, which is exactly upstream's `gmu_poll_timeout(…, 100, 10000)`. Ruled out; do not revisit.

**The firmware image is intact and the DTCM path is byte-perfect.** Decoding `a660_gmu.bin`
locally against the block table in `LemonadepGmuSource.h`:

| block | addr | bytes | first words |
|---|---|---|---|
| ITCM | `0x00000000` | 476 | `0x10007f80 0x00000105 0x00000145 0x00000159` |
| icache | `0x00004000` | 54476 | `0xbd1fb51f 0xbd10b510 …` |
| DTCM | `0x10007f98` | 40 | all zero |
| DTCM | `0x10007fdc` | 24 | `0xffffffff 0x30010008 0xffffffff 0x30000009 0x0 0x20010000` |

ITCM word 0 is a Cortex-M3 initial SP (`0x10007f80`, landing in DTCM just below the two blocks
loaded there) and word 1 a Thumb reset vector (`0x105`) — a sane vector table. The icache block
opens with real Thumb (`b51f push` / `bd1f pop`). And the word at `0x10007fe0` is `0x30010008`,
**exactly** what the on-device probe reads back, which proves the DTCM write path end to end.

**So two paths remain unverified, and this commit instruments both in one boot.**

1. *ITCM.* The M3 reset-fetches its vector table from ITCM word 0, and nothing has ever read
   ITCM back. `A660_GMU_CM3_ITCM_START` (`0x3000`) is almost certainly right — DTCM at `0x7000`
   is confirmed twice over (independently from `CORE_FW_VERSION = DTCM + 0xff8`, dword
   `0x1d3f8 - 0xff8 = 0x1c400`), and `0x3000`/`0x7000` are exactly 16 KiB apart, matching
   `SZ_16K` ITCM — but "almost certainly" is what this project keeps paying for. New line:
   `SWEET GPU: GMU itcm=…,…,…,… (want 0x10007f80,0x105,0x145,0x159)`.

2. *The GMU clock rate.* `gpu_cc_cx_gmu_clk` (`0x1098`, matching gpucc-sm8350.c exactly) is
   enabled and its `CLK_OFF` polls clear, so a clock does arrive — and the HWCG escape hatch in
   `enable_branch()` is **not** implicated, because it is gated on a per-branch `hwcg_capable`
   flag that only the two GCC branches set. But this driver never programs the RCG that feeds
   the branch: `gpu_cc_gmu_clk_src`, `cmd_rcgr 0x1120`, whose table is 19.2 MHz from TCXO
   (src 0), 200 MHz from `P_GPLL0_OUT_MAIN_DIV` (src 6, div 1.5 → cfg `0x602`) and 500 MHz from
   PLL1 (src 3). Upstream calls `clk_set_rate(gmu->core_clk, 200000000)` before enabling; we
   inherit whatever the boot loader left. New line:
   `SWEET GPU: gpucc gmu_rcg cmd=… cfg=… cx_gmu=… ahb=… gdsc_cx=…`, with ROOT_OFF in
   `cmd` bit 31 and src_sel `[10:8]` / `(2*div - 1)` `[4:0]` in `cfg`.

Reading the rest of the next log: `cfg=0x602` means the rate is already right and the clock is
not the problem; `cfg=0x0` means TCXO, and programming the RCG to 200 MHz becomes the next
change. ITCM reading back zero outranks both — it would mean the M3 has nothing to execute and
every symptom above follows from it.

Note `FW_BUSY` is weaker evidence than it looks: it may only assert while the M3 processes an
HFI message rather than during init, so `fw_busy=0` alone does not prove non-execution. The
ITCM read-back and the firmware-written registers are the trustworthy signals.

## GPU, Phase 15: the FAL10 veto lands but changes nothing; instrumenting the M3 (2026-10-29)

Commit `4eed8d2` (pipeline 21, kernel `6e6f607f`, image `63b8c2f4`) on-device:

```
SWEET GPU: GMU cold start status=3 phase=8
SWEET GPU: FAL10 veto fal=0x1 falnext=0x1
SWEET GPU: GMU start-ready probe=0x30010008 -> mask=0x1ff want=0x100, FW_INIT_RESULT=0x0
SWEET GPU: GMU context FSR=0x400 FAR=0x1dcefcf932aa9
```

The veto **took** — both registers read back `0x1`, which also independently confirms the
`byte = (dword << 2) - 0x6a000` window arithmetic on a register this driver had never touched.
It is correct to have (upstream does it unconditionally for A650-family) but it is **not** the
blocker. Still `FW_INIT_RESULT=0x0`, and still no SMMU fault.

**Why "no fault" is ambiguous.** In Phase 13 the fault at `FAR=0x4000` was *positive evidence*
that the M3 was fetching. Now there is no fault at all, which could mean the fetches succeed —
or that the M3 never issues any. Nothing in the current log distinguishes those, and the PTE
Access Flag is useless as evidence because `descriptor()` pre-sets AF (bit 10) itself.

So this commit adds reads, not behaviour. After the StartReady poll, whatever it decided:

```
SWEET GPU: GMU m3 fw_busy=… sysreset=… fence_status=… seq_busy=… general0=… general1=… hfi_status=… hfi_ver=…
```

- `CM3_FW_BUSY` (dword `0x1f81a` → `0x14068`) is the M3's own busy flag. **Non-zero ⇒ it is
  executing** and the problem is downstream of the icache; zero with no SMMU fault ⇒ it never ran.
- `AHB_FENCE_STATUS` (`0x23b13` → `0x24c4c`) latches an AHB access the fence refused. The fence
  range is programmed a few writes earlier, so a wrong range shows here instead of as silence.
- `GENERAL_0`/`GENERAL_1` (`0x1f9c5`/`0x1f9c6` → `0x14714`/`0x14718`) and `HFI_CTRL_STATUS`/
  `HFI_VERSION_INFO` (`0x1f980`/`0x1f981` → `0x14600`/`0x14604`) are firmware-written: anything
  non-zero is proof of execution.
- `CM3_SYSRESET` read back, to confirm our `1 -> 0` toggle actually left reset deasserted.
- `RSCC_SEQ_BUSY_DRV0` (RSCC window, `0x404`), in case the RSC sequencer is stuck.

The dword→byte conversion was validated before use: it reproduces six constants already in the
driver exactly — `CM3_SYSRESET 0x1f800`, `CM3_CFG 0x1f82d`, `GENERAL_7 0x1f9cc`,
`RPMH_CTRL 0x1f8e8`, `AHB_FENCE_RANGE_0 0x23b11`, and `SEQ_BUSY_DRV0 0x0101` in its own window.

**Deliberately not changed:** `a6xx_rpmh_start()` is still skipped, and that remains right. A
previous session performed its `RSCC_CONTROL_REQ` wake-poll unconditionally and
`RSCC_CONTROL_ACK` bit 1 never set on real silicon — which is exactly what the real driver's
`test_and_clear_bit(GMU_STATUS_PDC_SLEEP, …)` gate predicts for a device never previously slept
(see "GPU, Phase 8 continued"). Do not re-add it without new evidence.

A note against repeating work: a GMU log/debug memory dump looked attractive here and was left
out on purpose. The kernel has `lemonadep_clean_to_poc()` but no matching invalidate, so a CPU
read of those regions can return the zeros we wrote rather than what the GMU wrote — a false
negative. Registers are the stronger signal; add an invalidate first if a memory dump is wanted.

## GPU, Phase 14: the permission fault is FIXED; next suspect is the missing FAL10 veto (2026-10-29)

Commit `d6ed11b` (CircleCI pipeline 20, kernel `362af471…`, boot image `864453ac…`, boot-args
`cpus=8 … sweetgpu=9`) **cleared the fault**:

```
SWEET GPU: GMU context FSR=0x400 FAR=0x1dcefcf132aa9     (was FSR=0x80000408 FAR=0x000004000)
```

PF (bit 3) and MULTI (bit 31) are both gone; `0x400` is the bit already present at setup time,
before any GMU access, i.e. the boot loader's, and FAR is stale garbage because no fault is
recorded. **The GMU's instruction fetch at IOVA `0x4000` now succeeds.** Phase 13's diagnosis —
AP[1]=1 forces PXN regardless of bit 53 — was correct.

What is left is no longer downstream of a fault, so it is now a problem in its own right:

```
SWEET GPU: GMU cold start status=3 phase=8      (Timeout in StartReady)
SWEET GPU: GMU start-ready probe=0x30010008 -> mask=0x1ff want=0x100, FW_INIT_RESULT=0x0
```

**What this run rules out.** Four things previously unverified are now positively confirmed:

- *The TCM load works.* `probe` is read from `CM3_DTCM_START + 0xff8` (dwords) = byte `0x3fe0`,
  which is inside the firmware block we write at `0x10007fdc`. A plausible non-zero value
  (`0x30010008`) coming back means our ITCM/DTCM register writes land where intended.
- *`probe` is `GMU_CORE_FW_VERSION`.* a6xx_gmu.xml puts that register at dword `0x1d3f8`, and
  `DTCM_START` is dword `0x1c400`; the difference is exactly `0xff8`. Linux's "DTCM probe" is
  literally a firmware-version read, so we are already logging it — no extra diagnostic needed.
- *`probe` takes the right branch.* `0x30010008 > 0x20010004`, so `mask=0x1ff want=0x100`
  (rather than `0xffffffff`/`0xbabeface`) is what upstream would choose too.
- *The GBIF CX writes belong here.* a6xx_catalog.c gives the a660 entry (chip_id `0x06060001`)
  `.gbif_cx = a640_gbif`, so writing `GBIF_QSB_SIDE0..3` inside fw_start is correct for this
  part and not a downstream invention. Likewise `gmu_chipid` is absent from that entry, so
  upstream computes it — and the formula yields `0x06060100`, exactly our constant.

**The gap found.** `a6xx_gmu_fw_start()` opens, on any A650-family part, with

```c
	/* Vote veto for FAL10 */
	if (adreno_is_a650_family(adreno_gpu) || adreno_is_a7xx(adreno_gpu)) {
		gmu_write(gmu, REG_A6XX_GPU_GMU_CX_GMU_CX_FALNEXT_INTF, 1);
		gmu_write(gmu, REG_A6XX_GPU_GMU_CX_GMU_CX_FAL_INTF, 1);
	}
```

A660 is A650-family (`adreno_is_a650_family()` is `family >= ADRENO_6XX_GEN3`; A660 is GEN4),
and **this driver has never written either register** — `grep -i fal` over patch 1007 found
nothing. Without the veto, FAL10 is not blocked on the CX rail while the GMU boots. Patch 1007
now writes both first, before the `GENERAL_7` retention write, and logs a read-back
(`SWEET GPU: FAL10 veto fal=… falnext=…`) so a `0x0` read tells us the window is wrong rather
than that the veto was refused.

Offsets, derived not guessed: a6xx_gmu.xml numbers registers in dwords from the GPU base, and
every constant in this driver is `byte = (dword << 2) - 0x6a000`. Checked against a register
the driver already used — `GPU_GMU_CX_GMU_PWR_COL_CP_MSG`, dword `0x1f900` → `0x14400`, which
matches `A660_GPU_GMU_CX_GMU_PWR_COL_CP_MSG`. So `FAL_INTF` dword `0x1f8f0` → `0x143c0` and
`FALNEXT_INTF` dword `0x1f8f1` → `0x143c4`.

Still unverified in the fw_start sequence, in the order I would check them next if the veto is
not enough: whether `rsc_pdc_handshake()` really matches `a6xx_rpmh_start()` (upstream polls
`RSCC_SEQ_BUSY_DRV0`, we have a `RSC_STATUS0_DRV0` constant — not obviously the same register),
and whether `A660_GMU_RPMH_CTRL`'s vote-enable mask is right.

Operational note: `tools/sweet-log-pull.sh` does not run under Git Bash — it rewrites
`/dev/block/sda20` and `/tmp/...` into Windows paths and `blockdev` then rejects its argument.
Export `MSYS_NO_PATHCONV=1` (and `MSYS2_ARG_CONV_EXCL='*'`) first, or run the `dd`/`adb pull`
by hand. Same for every `wsl.exe -- bash /mnt/...` invocation.

## GPU, Phase 13: the icache fault is root-caused — the mapping is unprivileged (2026-10-29)

The stage-9 log from commit `6621fef` (built and booted 2026-10-29, log pulled today) is
the first run with the `ArmSmmu4KExecute` fix on the icache. It did **not** clear the fault:

```
SWEET GPU: GMU SMMU setup status=0 step=7 sids=2 tables=774 leaves=4111 program_phase=5 fsr=0x400 attach_phase=4
SWEET GPU: GMU cold start status=3 phase=8
SWEET GPU: GMU start-ready probe=0x30010008 -> mask=0x1ff want=0x100, FW_INIT_RESULT=0x0
SWEET GPU: GMU context FSR=0x80000408 FAR=0x000004000
```

`FAR=0x4000` is `LEMONADEP_GMU_ICACHE_IOVA` exactly — the GMU's very first instruction
fetch — and FSR bit 3 is still PF. So Execute was necessary but not sufficient.

**Why.** `descriptor()` in the vendored ArmSmmu4K sets `attrs |= 1u<<6` — AP[1], the
"unprivileged access permitted" bit — whenever `ArmSmmu4KPrivileged` is absent, which
mirrors io-pgtable-arm's `ARM_LPAE_PTE_AP_UNPRIV`. In the AArch64 stage-1 EL1&0 regime an
AP[1]=1 page is treated as **PXN regardless of descriptor bit 53**, so clearing PXN had no
effect while the page stayed EL0-accessible, and the GMU's fetches are privileged.

Upstream `a6xx_gmu_memory_alloc()` confirms the intended split: every GMU buffer with a
**fixed** IOVA is mapped `MSM_BO_MAP_PRIV` (its own comment reads *"use IOMMU_PRIV for
icache/dcache"*) — dummy `0x60000000`, debug `0x60400000`, icache `0x4000` — while the
dynamically placed log and hfi buffers get no PRIV. Downstream kgsl does the same for its
`GMU_CACHED_CODE` region (`IOMMU_READ | IOMMU_WRITE | IOMMU_PRIV`). Our region table
already has exactly that fixed/dynamic split, so patch 1007 now derives `Privileged` from
it (`fixed_iova`) and keeps Execute on the icache alone.

Dropping AP[1] on the three fixed regions cannot break their data accesses: AP[1]=0 denies
EL0, not EL1, and nothing but the GMU reads them through the SMMU — the kernel fills the
icache through the direct map and cleans to PoC.

Secondary notes from the same log, both downstream of the fault and not yet separate
problems: `probe=0x30010008` against `want=0x100` and `FW_INIT_RESULT=0x0` are simply what
a GMU that never executed reports, and the `fsr=0x400` already present at setup time
(bit 10, before any GMU access) is the boot loader's, not ours.

Not yet built. Next: a CircleCI run on this patch, then a stage-9 boot, then read
`FSR`/`FAR` again — if PF is gone the next thing to watch is `FW_INIT_RESULT`.

## HakiOS port started: a lemonadep kernel builds and runs user space (2026-10-29 ~01:50)

**Where it is.** `~/hakios/hakios-xnu-kernel` inside the **LemonadepLab WSL distro** (not /mnt/d:
D: is 96% full with 41 GB, WSL's own filesystem has 918 GB, and native-FS builds avoid the 9p
tax). Branch **`hakios`**, not `main` — `main` is still the pre-restructure layout with no
`configs/` and no `hakios-device.py`, so following the docs against it will not work.

**The device repository exists**: `devices/lemonadep` (its own git repo, 2 commits), made with
`tools/hakios-device.py new lemonadep --soc sm8350 --soc-title "Snapdragon 888 (SM8350,
lahaina)" --title "OnePlus 9 Pro" --vendor OnePlus --fdt <the phone's tree>`. It read **13 of
16 `soc_*` values straight out of lemonadep's own device tree**; the three it missed are now
filled in and `hakios-device.py check` passes:
- `soc_cmd_db=0x80860000` — the scan looks for a reserved-memory node *named* `cmd_db`;
  lemonadep calls it `reserved-memory@80860000` (`compatible = "qcom,cmd-db"`). This is also
  what ios-sweet read at runtime (`SWEET RPMH: cmd-db ... pa=0x80860000`), so tree and
  hardware agree;
- `soc_mpss_mem=0x8b800000` (`modem_region@8b800000`), `modem_ipa_fw_at=0x8b500000`
  (`pil_ipa_fw_region@8b500000`).
Two of the auto-read values also match ios-sweet's own findings: `soc_rsc=0x18220000`
(= `SWEET RPMH: rsc=0x18220000`) and `soc_apps_smmu=0x15000000` (= patch 1008's constant).

**It builds on Linux. No macOS, no Xcode, no CI.** theos + its Linux cctools + the
`iPhoneOS16.5.sdk` (theos's own installer pins `swift-toolchain-linux` **v2.3.0 /
swift-5.8-ubuntu20.04**, not the newer tags), then `xnu-deps/setup.sh` lays out the rest.
`make doctor` is green apart from the IPSW and the phone's firmware.

**Two environment gaps cost a build each, neither in `make doctor`:**
1. `uuid/uuid.h` not found — XNU's host SETUP tools need **`uuid-dev`**;
2. then `ld: library not found for -lfirehose_kernel`. The Makefile guard is
   `[ -e xnu-deps/host/bin/ld64 ] || xnu-deps/setup.sh`, and stage 1 had already created
   `ld64` before the uuid failure, so setup was skipped from then on and **stage 2 never
   ran** — it is what builds `libfirehose_kernel.a` and `libpthread_kext.a` against the
   kernel's exported headers. `xnu-deps/setup.sh --stage2` fixes it. (Same library that
   needed `arm64` added to darwin-xnu-build's `ARCHS` in ios-sweet.)

**Result: `hakios-kernel`, 14,365,016 bytes, Mach-O 64-bit arm64 (NOUNDEFS|PIE), Darwin
Kernel Version 22.5.0 / xnu-8796.121.2.** `make qemu-open` boots it with no Apple software
at all and it reaches user space:

```
hakios-kernel: 4 drivers built in (.config): virtio_sound virtio_input virtio_net virtio_blk
hakios-kernel: version 2.1.13-hakios-darwin22.5-sm8350-g7867dc14
load_init_program: attempting to load /sbin/launchd
Hello from user space on Darwin!   I am process 1, uid 0, console fd 0.
pid 1 exited (signal 0, exit 0)    panic: initproc failed to start
```

That panic **is the expected end** of `qemu-open`: mockfs presents `userland/hello` as
`/sbin/launchd`, it prints and exits, and XNU always panics when init exits. The four virtio
drivers and the `sm8350` version string confirm the kernel was built from our own config.
Only the LLDB debug-macro packaging fails in `make kernel`; the kernel itself links clean, so
copy it from `xnu-src/.../BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837` to
`hakios-kernel` when that step stops the build.

**The UEFI does not need writing.** furangelz has a full lemonadep package at
`uefi/files/Platforms/OnePlus/lemonadepPkg` (DSC/FDF/Drivers/Library/Binaries) plus
`Resources/DTBs/lemonadep.dtb` on their `lemonadep` branch — their README is stale and still
says "UEFI for redwood", which is why it is easy to miss. **Their `prebuilt/uefi/` is
redwood's build** (it ships `redwood.dtb`), so the package has to be compiled, not copied.
Mu-Silicium upstream has `Platforms/OnePlus/lemonadePkg`, which is the OnePlus **9**
(`sm8350-oneplus-lemonade.dtb`) — a sibling on the same SoC, and a cleaner reference than the
`lisa`/`KodiakPkg` one furangelz had to follow. A search for "lemonadep" matches "lemonade**P**kg"
by accident; do not take that as upstream support for the Pro.

**The lemonadep UEFI is built** (2026-10-29 ~02:10). In WSL at `~/hakios/fz`:
`SILICIUM_UEFI.fd`, **2,097,152 bytes**, sha256
`199a1c48c4d94454ae0822be499e70ba33e1c3e538af4c31c5eacb2d749d0bd2`, at
`Mu-Silicium/Build/lemonadepPkg/RELEASE_CLANGPDB/FV/`. Built in 61 s, 48 images verified.
That size is exactly the `size = 0x200000` in `Resources/Configs/lemonadep.toml` (whose
`[uefi_fd] base = 0x9FD00000`, gzip kernel, `append_dtb = true`), and
`Resources/DTBs/lemonadep.dtb` (1.5 MB, from the phone's vendor_boot) is beside it.

How, exactly:
- sparse-fetch furangelz's `uefi/` from their `lemonadep` branch (11 MB), then `uefi/apply.sh`
  — it clones Project-Silicium/Mu-Silicium at `BASE` = `e51cd877e31c8fbb322032dc4cb6715e83d25ad3`
  with submodules (~3 GB), applies `mu-silicium.patch` and copies `files/.` over it;
- Ubuntu packages: `build-essential lld nasm clang llvm gcc-aarch64-linux-gnu python3-venv
  acpica-tools device-tree-compiler`;
- `python3 -m venv venv && venv/bin/pip install -r Mu-Silicium/pip-requirements.txt`;
- the documented **mu_nasm/NuGet/Mono** workaround: symlink the system `nasm`/`ndisasm` into
  `Mu_Basecore/BaseTools/Bin/mu_nasm_extdep/Linux-x86-64/` with an `extdep_state.yaml`;
- `python3 build_uefi.py -d lemonadep -r RELEASE` (their README says `-d redwood`).
Despite their note that a from-scratch build "has not been re-run since", it worked first try.

Two things to know about that package: the DTB line in `lemonadep.fdf` is **commented out**
and still names `sm8350-oneplus-lemonade.dtb` (the OnePlus 9's), because the package was
derived from upstream's `lemonadePkg` — the real tree is delivered at runtime through
`Resources/`, as redwood does it. And `lemonadepPkg` now sits next to upstream's
`lemonadePkg` in the same tree, so the sibling is there to diff against.

**The IPSW, the loader and a bootable image are done** (2026-10-29 ~02:55).

- There is **no `sysimage/fetch.sh` on the `hakios` branch, by design**: `unpack.sh` says "The
  project ships no file of Apple's and downloads none. The IPSW is yours." The file it wants is
  `iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw`, **6,165,698,198 bytes**, sha1
  `6ee6e5b447902dbb36a3991ca20d032d6b4cc759`, from Apple's own CDN (`updates.cdn-apple.com`,
  URL via api.ipsw.me). Downloaded to `~/hakios/ipsw/` in WSL, both size and sha1 verified.
  `make unpack IPSW=...` gave 2.9 G rootfs, 26 M cryptex, 11 M App cryptex, 2.6 G dyld cache
  and `sysimage/DeviceTree.d22ap.im4p` (33,694 bytes).
- **`identity.h` is silently a placeholder until the IPSW exists.** `make loader` will happily
  build against `mkidentity.py --placeholder` ("no Apple device-tree data") and say nothing.
  Delete `xnuloader/identity.h` and run `make loader` again once `sysimage/` is unpacked: it
  then says "made from the device tree (186040 bytes)" and `loader.efi` grows 65,536 -> 70,656.
- **LZFSE**: `mkidentity.sh` wants `lzfse`; its own fallback that clones and builds
  github.com/lzfse/lzfse fails (`No rule to make target 'lzfse'`). Ubuntu packages it:
  `apt-get install lzfse` (noble/universe), which is the first decoder it looks for anyway.
- **`mkxnuimg.py` was redwood-hardcoded** (`Build/redwoodPkg/...`, `Resources/DTBs/redwood.dtb`,
  `xnu-redwood.img`, and `MU` fixed to a `Mu-Silicium/` inside the tree). Patched here to read
  `CONFIG_DEVICE` from `.config` (overridable with `HAKIOS_DEVICE`) and to honour `MU_DIR`, since
  `uefi/apply.sh` puts Mu-Silicium beside the `uefi/` it was run from, not in the kernel tree.
  Worth sending upstream — any second device hits this. With it:
  `MU_DIR=~/hakios/fz/Mu-Silicium python3 mkxnuimg.py` -> **`xnu-lemonadep.img`, 2,615,296 bytes**
  (shim 176, uncompressed 2,097,328).

**On-device result so far: the image does not bring the loader up.** `fastboot boot
xnu-lemonadep.img` reports OKAY, and the phone comes back in **stock** fastboot: `product:
lahaina`, serial unchanged. `xl`'s own test is `is_loader() { fb getvar product | grep -q
'product: xnuloader'; }`, so `lahaina` means stock, not the loader. (A `????????????` line in
`fastboot devices` right after the boot is just USB re-enumeration — not a second fastboot
implementation. Don't read it as success.) Nothing was harmed; the phone returns by itself.
Untested guesses for why, in order: the image was built with **no payload** (`mkxnuimg.py`
without `-p`), while `Resources/Configs/lemonadep.toml` has `[boot_shim] requires_kernel_header
= true`; and whether the UEFI starts at all needs the screen or a log to tell.

**The supported workflow needs USB from WSL, which it does not have.** `xl` drives the loader
over the fastboot protocol and runs inside the kernel tree, but `fastboot`/`adb` are not
installed in LemonadepLab and the phone is on the Windows USB bus — `./xl state` says `absent`.
Either install `usbipd-win` on Windows and attach the device to WSL (`usbipd bind` / `usbipd
attach --wsl`), which is the real fix, or keep copying images out to Windows for `fastboot
boot`, which cannot drive `xl run`/`xl log` at all.

**Built artifacts** (copied to `D:\ios port\hakios-out\`): `xnu-lemonadep.img` 2,615,296 bytes,
`hakios-kernel` 14,365,016 bytes.

**usbipd is set up and works** (2026-10-29 ~03:05). `winget install --id dorssel.usbipd-win`
(5.3.0; the MSI elevates, so a UAC prompt appears), then — the phone in fastboot is BUSID
**1-20**, `18d1:d00d` "Android Bootloader Interface":

```
usbipd bind --busid 1-20            # needs admin: Start-Process -Verb RunAs
usbipd attach --wsl LemonadepLab --busid 1-20   # --wsl takes the distro as its argument,
                                                # there is no --distribution flag
apt-get install android-sdk-platform-tools usbutils   # in the distro
```

After that WSL sees it (`lsusb`: `18d1:d00d ... (fastboot)`), `fastboot devices` lists the
phone, and **`./xl state` returns `fastboot` instead of `absent`**. Windows loses the
device while it is attached; `usbipd detach` gives it back. Consider `--auto-attach` for
re-enumeration across runs.

**The loader still does not come up, now confirmed by xl itself**: `MU_DIR=... XL_DEVICE=lemonadep
./xl enter` -> `xl: loader did not come up on USB (state: absent)`. Afterwards the phone is
**gone from the USB bus altogether** — not in `lsusb`, and absent from `usbipd list`'s
Connected section (only the binding persists). So this is not a usbipd re-attach problem: the
phone presents no USB at all after booting `xnu-lemonadep.img`. It needs Power+VolDown to come
back. Nothing is flashed, so Android is untouched.

What that narrows it to: the UEFI either hangs before its USB comes up, or starts and the
XNU loader never does. **Next thing to do is look at the phone's screen during the attempt** —
Mu-Silicium should put up its own framebuffer/banner, and whether anything appears at all
separates "UEFI never ran" from "UEFI ran, loader did not". Other threads: build the image
with a payload (`mkxnuimg.py -p ...`; `lemonadep.toml` has `[boot_shim] requires_kernel_header
= true` and we passed none), and check `lemonadepPkg`'s memory map against the phone — the
package came from furangelz's tree and has never been built or booted by us before tonight.

**Then** step 2's "done when": a run reaches `launchd` and ends by itself. Then drivers in the
documented order (CPUFREQ/THERMAL/SOCINFO → buttons/touch → UFS → ADRENO → ADSP/battery/audio
→ Wi-Fi → rest).

## Upstream restructured into HakiOS — evaluation notes (2026-10-29 ~01:20)

`github.com/oosexgang` is now **HakiOS**, with docs at hackios-web.vercel.app. What exists
publicly: `hakios-xnu-kernel` (the common repo, 186 MB), `hakios-sm7325` (POCO X5 Pro
device repo, 3 MB), `hakios-uefi`, `hakios-metal`, `hakios-mt6768` (a MediaTek device).
The `hakios` common repo the docs name does not exist yet — the docs run ahead of the code.

**On redwood (SC7280 / Adreno 642L), from its own `device.conf`:** display, touch, buttons,
8 cores with `bootcpu=7 cpufreq=1` (big core 2.4 GHz), **GPU via Mesa OpenGL ES** (`gpu=1`),
**UFS with `/private/var` kept across runs** (`var_store=1 ufs_write=1`), audio in and out
through the ADSP, haptics, camera, **Wi-Fi (WCN6750, ath11k)**. Partial: battery level only,
torch, thermal, cellular (modem up, no registration). Missing: Bluetooth. Metal is
**partial and draws nothing** — buffers and libraries only, checked on a Mac.

**What porting lemonadep there would actually involve:**
- it is a documented, scaffolded process (`tools/hakios-device.py new ... --soc ... --fdt`),
  with a prescribed driver order (CPUFREQ/THERMAL → buttons/touch → UFS → ADRENO → ADSP/
  battery/audio → Wi-Fi → camera/modem/BT) and a "done when" for each step;
- but **SM8350 is not a SoC HakiOS knows** (only SC7280), so it needs a new SoC layer as well
  as a new device repo;
- `xnu/files/bsd/dev/hakios.adreno.c` is written for redwood's Adreno 642L, with its clock
  and GDSC addresses from `gcc-sc7280.c`/`gpucc-sc7280.c` and `qcom,yupik-gpucc` — the a6xx
  GMU/firmware logic generalises (it already loads `a660_gmu.bin`, `a660_zap.mdt`,
  `a660_sqe.fw`), the SoC plumbing does not;
- it also has a **zap shader step through TrustZone**, which this port has never done and
  which the 3D core is likely to need;
- it runs in **QEMU**, so driver work can be iterated without the phone.

**The real asset is furangelz's existing SM8350 work**: their `redwood_adreno.c` on the
`lemonadep` branch is 189 KB — the Adreno 660 adaptation of exactly this driver, with GL
working on this phone. But their branch is at `2a9e6691` (2026-10-25), **before** the
restructure, so it is not a drop-in to current HakiOS either and would need rebasing.

**What this port has that HakiOS does not:** a full persistent iOS root on UFS. HakiOS keeps
`/private/var` and runs the system from RAM; installation is still a design document.

**What it costs to switch:** the whole sweet/lemonadep patch series (0001-0082, 1001-1011)
does not transfer — different kernel tree, different boot chain (our shim + `fastboot boot`
vs their Mu-Silicium UEFI + `xnuloader`).

## GPU, Phase 12: the GMU's firmware does not execute — FW_INIT_RESULT stays 0 (2026-10-29 ~00:20)

Kernel `94e7ed70` (pipeline 10, commit 5b796b1) adds the StartReady diagnostics. Stage 9:

```
SWEET GPU: GMU cold start status=3 phase=8
SWEET GPU: GMU start-ready probe=0x173daf1f -> mask=0xffffffff want=0xbabeface, FW_INIT_RESULT=0x0
```

**The pattern was never the problem.** `0xbabeface` is the real a6xx "firmware
initialised" magic, and the DTCM probe (`0x173daf1f`, well above the threshold) selected it
correctly. `FW_INIT_RESULT=0x0` means the GMU wrote nothing: it is out of reset and not
executing.

**Ruled out this round**, all verified against the log and the code:
- the icache *is* copied — `lemonadep_gmu_icache_load()` is called (lemonadep_gpu.c), returns
  the 54476 bytes the log reports, and `lemonadep_clean_to_poc()` flushes it;
- its address is right — `LEMONADEP_GMU_ICACHE_IOVA` is `0x4000`, the GMU VA the firmware
  block targets;
- the SMMU context is good — `GMU SMMU setup status=0`, `sgfsr=0x0`, stream 5 on entries 1/2.

**Still open, in rough order of suspicion:**
- the CM3 boot configuration before SYSRESET is released (does anything set CM3_CFG /
  BOOT_CONFIG for a non-secure boot out of the icache? compare `a6xx_gmu_fw_start()`);
- the DTCM write offset in `load_firmware()`: `A660_GMU_CM3_DTCM_START + (b->addr -
  0x10004000u)`, a magic constant that is worth re-deriving against the real firmware's
  block addresses;
- the context bank: we use bank 0, furangelz give the GMU context 1 and keep 0 for the 3D
  core;
- `fsr=0x400` on that context, still unexplained.

**Scope reality check, recorded so it is not rediscovered.** A running GMU is not GPU
acceleration and does not by itself change anything the user can see. The GMU is the
Adreno's power-management microcontroller; after it comes the 3D core (CP ring, ME_INIT,
microcode, and the zap shader through TrustZone — furangelz's steps 11/12/19). And even a
fully working kernel-side GPU would not make iOS draw with it: CoreAnimation reaches the
GPU through a userland driver, and furangelz's working acceleration is **Mesa/OpenGL ES
injected into their userland** (`USERLAND_VARS="GL=1"`), which their architecture allows
because they build a RAM userland image. This port runs stock iOS from a real UFS root and
has no injection path, so acceleration here needs that built too. Do not promise GPU
acceleration as "a few stages away".

## GPU, Phase 11: the GMU leaves reset without faulting the SoC; firmware never reports ready (2026-10-28 ~23:45)

**The SoC-level fault is gone.** Stage 9 (new: full run, `stop_after = None`) on kernel
`d938f84d` (CircleCI pipeline 8, commit 7780169), image `e3703d34`, `sweetgpu=9 cpus=8`:

```
SWEET GPU: stage=9 stop_after=0
SWEET GPU: GMU SMMU setup status=0 step=7 sids=2 tables=774 leaves=4111 program_phase=5 fsr=0x400 attach_phase=4
SWEET GPU: GMU cold start status=3 phase=8
```

`status=3` is `LemonadepGmuTimeout`; phase 8 is `LemonadepGmuPhaseStartReady`, which writes
`CM3_SYSRESET=1`, `PWR_COL_CP_RESP=0`, `CM3_SYSRESET=0` — **releasing the GMU from reset** —
then polls `CM3_FW_INIT_RESULT` for the pattern the probe chose. The phone stayed up for
minutes afterwards with the kernel in charge (no XBL ramdump, no reboot, no fall back to
Android), and the rest of the boot was normal. The earlier attempt at this same step is what
produced the ramdump, so the Phase 10 diagnosis (unmapped icache block + no GMU SMMU
context) is confirmed as its cause.

**What is unknown now** is why the firmware does not signal init. Nothing is printed to tell
them apart, and that is the next thing to fix before changing any behaviour:
- the probe value read at `CM3_DTCM_START + LEMONADEP_A660_GMU_START_PROBE_OFFSET`, which
  selects between the LOW and HIGH `ready_mask`/`ready_val` pairs. If that read returns a
  stale or zero value the wrong pattern is selected and the poll can never match even if the
  GMU initialised correctly.
- the last value `poll_core()` actually saw in `CM3_FW_INIT_RESULT` (it is captured in
  `value` but discarded when `TRY` returns).
Add both to the log line first; it costs one boot and no risk.

Other candidates after that: whether the icache block's *content* lands where the GMU's
stub expects (the mapping exists, the placement is unverified), and whether blocks other
than the 54476-byte icache one still need delivering.

**Also fixed here:** `stage_stop[]`'s bounds check used
`stage < count ? stage_stop[stage] : LemonadepGmuPhaseNone`, and `None` means "never stop",
so *any* out-of-range `sweetgpu=` — a typo like 99 included — already ran the full
GMU-from-reset path unannounced. Stage 9 now has its own entry and an unknown stage leaves
the GPU alone.

## GPU, Phase 10: the GMU has an SMMU context — stage 8 fully clean on-device (2026-10-28 ~23:20)

**Stage 8 passes end to end for the first time.** Kernel `569e1f1e` (CircleCI job 6,
commit 74c1eb3), boot image `9696d03a`, `sweetgpu=8 cpus=8`:

```
SWEET GPU: smmu streams valid=0 trans=0 bypass=0 fault=6 banks_on=0 -> bank=0 slots=0,1 (2 free)
SWEET GPU: GMU SMMU setup status=0 step=7 sids=2 tables=774 leaves=4111 program_phase=5 fsr=0x400 attach_phase=4
SWEET GPU: GMU cold start status=0 phase=7
SWEET GPU: stopped cleanly as requested (sweetgpu=8)
```

Before the fix the same hardware gave `(0 free)` and `status=6 step=0`. The rest of the
boot is unaffected: 8/8 CPUs, gauge ok, touch `TCM bring-up ok`, 603 `exec`s, `sgfsr=0x0`,
no panic. **Stages 1-8 are now all verified on-device.**

**The bug was ours, not the hardware's.** Two places demanded a claimable stream-table
entry's S2CR equal FAULT *exactly* (`(s2cr & ~exids_valid) == (2u << 16)` in the probe,
`(s2cr & ~valid_s2cr(id0)) != FAULT` in ArmSmmu4K's `scan()`). S2CR keeps the context index
in [7:0] and the boot loader leaves a stale one there, so no entry ever matched and all six
idle entries were reported as "0 free". Both now compare the type field [17:16] alone.

**FAULT is this SMMU's idle state, not a sign of ownership.** Confirmed from
`furangelz/hackios-xnu-kernel` (`xnu/files/bsd/dev/redwood_adreno.c`, branch `lemonadep`),
which runs an Adreno 660 on this same SoC: its `space_off()` *writes* `S2CR_FAULT` to
release a stream, and it never searches for a free slot — it claims fixed entries, with
`CORE_MATCH 0`/`CORE_STREAM 0` for the 3D core and `GMU_MATCH 1`/`GMU_STREAM 5` for the GMU
(`SMMU_STREAM_MASK 0x400`, `SECOND_MATCH 2`). We now take entries 1 and 2 likewise. Our SID
constants already matched theirs; only the entry choice was wrong.

**Other facts worth keeping from that driver** (same SMMU, `kgsl-smmu@3da0000`, which is the
address we already use): its streams are 0 the GPU, 1 its low-priority twin, 2 secure
content, 5 the GMU; the node is `qcom,skip-init`, so the boot loader's table is left as
found; the global registers (smr, s2cr, cbar, cba2r) are **the hypervisor's imitation**, so
every write must be read back (ArmSmmu4KAttachStreams already does this and quarantines on
mismatch); the SMMU needs its own clock vote (`GPUCC_SMMU_VOTE_CBCR 0x5000`); and the 3D
core additionally needs TrustZone + the zap shader (their steps 11/12/19) while the GMU
alone does not.

**Open on this:** `fsr=0x400` on the GMU's context after programming (bit 10, not one of
arm-smmu's named fault bits) — benign so far in that cold start still returned `status=0`,
but unexplained. The context bank is still the probe's choice (bank 0, since none are
enabled); furangelz reserve context 0 for the 3D core and give the GMU context 1, so this
needs revisiting before the core is brought up.

**Next:** stage 8 stops at `StartProbe` because `stage_stop[8]` says so; the GMU has not
been released from reset. Going further means a full run (`stop_after = None`, through
`StartReady`/`Done`), which is **the exact step that produced the XBL ramdump / SoC-level
fault before**. The diagnosed cause of that fault — the 54476-byte icache block unmapped
and no GMU SMMU context — is what this phase fixed, so the attempt is now justified, but it
is still the riskiest step in this series and should be its own stage, not a silent default.

## KERNEL BUILDS AGAIN — moved CI to CircleCI, off GitHub macOS runners (2026-10-28 ~20:30)

A kernel with patch 1011 in it now exists and is verified:
`kernel.development.sweet`, 16,795,472 bytes, Mach-O 64 (`0xfeedfacf`),
sha256 `4a0bf9e614dd503f34a38fa4e0debf43af062b91aff1ba2c1f07582986684b73`.
CircleCI job 4 (pipeline 4, commit 23c7ba9):
https://circleci.com/gh/popovluka/ios-lemonadep/4
Overlay confirmed compiled in by scanning the binary: `BQ28Z610` (the
gauge chip from 1011), `redwood_dpu` x10, `lemonadep` x35, `sweet_fbcon`,
`sweet_early`. NOT yet booted on the device.

**Why CI moved.** GitHub-hosted macOS runners stopped assigning jobs
entirely — `runner_name: ""` and "The job was not acquired by Runner of
type hosted even after multiple attempts" on every dispatch, for the full
15-minute timeout. `macos-14` is in its retirement brownout (fully retired
2026-11-02), but `macos-15` arm64 failed identically, so switching labels
does not help. CircleCI is a separate pool: free plan gives 30,000
macOS credits/month on `m4pro.medium`, and it picked up a runner in
~6 seconds every time. Config: `.circleci/config.yml` (kernel only; the
ubuntu `shim` job still runs fine on GitHub Actions).

**Xcode 16 compatibility (patch 0082).** CircleCI's m4pro class only has
Xcode 16+/26 — Xcode 15 is gone industry-wide — and Xcode 16's `iig`
emits dispatchers that read `rpc.kernelContent`, a field xnu-8796's
`IORPC` lacks (added in xnu-10063+). Patch 0082 declares it, unused.
Two traps here, both of which cost a run each:
- xnu-8796 has **two byte-identical copies** of the header,
  `iokit/DriverKit/IORPC.h` and `iokit/IOKit/IORPC.h`. Both must be
  patched; the DriverKit-side `iig` output includes the latter.
- Do **not** guard the field with `#ifdef KERNEL`. xnu installs these
  headers through `unifdef` in both a `-DKERNEL` and a `-UKERNEL` flavor,
  so a guarded field is stripped from the exported copy that the
  generated code actually includes.
The sibling fork `nmtm82g6zw-commits/ios-sweet-grus` has this same patch
(as their 0135) but guarded and on one file only — their workflow is
still pinned to `macos-14` despite the commit message, so their version
was never actually compiled. Don't trust it as prior art.

**darwin-xnu-build silently skips non-applying patches** (warning only).
That turned two of my own off-by-one hunk counts into confusing
downstream compile errors instead of patch errors:
- 0082 declared `old=6` with 5 old lines -> "corrupt patch at line 19" ->
  skipped -> `no member named 'kernelContent' in 'IORPC'`.
- 1011's `bsd/sys/redwood_input.h` hunk declared 25 lines but had 26, so
  `git apply` dropped the closing `#endif` ->
  "redwood_input.h:6:2: error: unterminated conditional directive".
`.circleci/config.yml` now fails the build if any `0xxx-`/`1xxx-` patch
is skipped, and greps the finished kernel for overlay symbols. Before
spending a run, validate hunk arithmetic AND dry-run against real xnu
source (`git apply --check` on the files fetched from
`apple-oss-distributions/xnu` at tag `xnu-8796.141.3`) — hunk math alone
would not have caught the wrong-file/guard problem.

**CircleCI log gotcha.** `MAKEFLAGS=-k` plus parallel jobs produced >50MB
of byte-interleaved output, which hit CircleCI's per-step cap and made
the real error unreadable. The config now drops `-k` and redirects the
build to a log file stored as an artifact.

Next concrete action: build a boot image from this kernel
(`lemonadep/m5-battery-dpu-build.sh`, boot args
`board=lemonadep battery=1 backlight=1`) and temporary-`fastboot boot` it
to see whether the battery gauge and DPU backlight come up. Needs
`ufsrw=1` for test boots; pull logs with TWRP + `sweet-log-pull.sh`.

## Patch 1011 (battery + DPU) CI-green; kernel build hit another macOS-runner drought (2026-10-28)

Overlay patch 1011 (`redwood_battery`/`redwood_gauge`/`redwood_dpu` + RedwoodBattery.cpp,
porting furangelz's hackios drivers onto our ios-sweet tree) finally passed
`check-overlay.sh` after three rounds of hunk fixes, all `git apply` context/line-count
bugs, not logic bugs:

- `bsd/conf/files` / `iokit/conf/files.arm64`: wrong `@@` line numbers (commit bab8dc6).
- `bsd/dev/sweet_ufs.c` hunk 2: call-site context assumed `reboot_arm()` was adjacent to
  `gpu_init()`/`wlan_init()`; it's ~140 lines away (fixed before this session).
- `bsd/dev/sweet_ufs.c` hunk 1: **`git apply` requires at least one trailing context
  line to anchor a hunk whose insertion isn't at EOF** -- `@@ -56,4 +56,6 @@` with 0
  trailing context failed to locate text that was verbatim present; adding 3 trailing
  context lines (`@@ -56,7 +56,9 @@`) fixed it (commit bab7d70). Worth remembering for
  any future hand-written hunk.

CI green on run 40674361007. Triggered the kernel build (`gh workflow run lemonadep.yml
-f build_kernel=true`) twice -- runs 40674761758 and 40677147114 -- and **both kernel
jobs were cancelled after ~15 min with `runner_name: ""` (never assigned)**, same
signature as the drought on 2026-10-22 above. The `ubuntu-24.04` shim job in the same
runs got a real runner (`GitHub Actions 1000000191`) in under a second both times, so
this is macos-14-pool-specific, not account/billing. Per the 2026-10-22 precedent, this
can last hours; retrying every 15 min just burns queue attempts. **Don't loop
retry-and-sleep on this** -- check `gh api repos/popovluka/ios-lemonadep/actions/jobs/<id>
--jq .runner_name` on the most recent kernel job before re-dispatching, and only
re-dispatch once it's non-empty somewhere, or after a long (hour+) gap.

NEXT: re-run `gh workflow run lemonadep.yml -f build_kernel=true` once a macOS runner is
confirmed available again, then `gh run download <id> -n xnu-kernel-lemonadep`, then
boot via `lemonadep/m5-battery-dpu-build.sh` with `board=lemonadep battery=1
backlight=1` (temporary `fastboot boot`, confirm with user first).
