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
