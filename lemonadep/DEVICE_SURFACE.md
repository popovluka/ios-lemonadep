# ios-sweet device-specific surface (STEP 1 of the lemonadep port)

Base: `dipa-studio/ios-sweet` @ `425b251` (2026-10-18 state: home screen on sweet).
Goal: list every place that encodes **sweet (Redmi Note 10 Pro, SM7150)** hardware
facts, and what the OnePlus 9 Pro (**lemonadep, SM8350**) port has to change.

No lemonadep values appear in this file on purpose. They come in STEP 2
(`HWMAP_lemonadep.md`), each with a source file + DT node.

## How hardware facts flow through the project

```
 build time (host)                         run time (phone)
 ───────────────────                       ───────────────────────────────
 tools/mkadt.py   ── ADT (MMIO, RAM, CPUs, fb, UFS, identity) ─┐
 tools/mkpayload.py ── payload = kernel + ADT + boot-args ─────┤
 shim/build.sh    ── boot.img v2 (shim Image + payload ramdisk + stock DTB)
                                                               │
 ABL ──fastboot boot──► shim (start.S, loader.c, console.c) ───┘
        x0 = live FDT        └─► XNU (xnu/patches) reads the ADT
                                 … except the patches that hard-code MMIO (tier C)
```

Three tiers of device coupling:

| Tier | Meaning | Port action |
| --- | --- | --- |
| **A** generic | No hardware facts (toolchain, crypto, HFS, AMFI, IOKit glue) | Reuse unchanged |
| **B** ADT-driven | Kernel code is generic; values come from the ADT / boot_args | Reuse unchanged **if** the lemonadep ADT is right |
| **C** hard-coded | Sweet MMIO, PMIC registers, panel geometry, I²C addresses compiled in | Needs a lemonadep variant (or ADT-ification) |

---

## 1. `tools/mkadt.py` — the Apple Device Tree (tier B source of truth)

Every value below is a sweet constant at the top of the file or inline in `build()`.

| Item | sweet value / where | Lemonadep must change |
| --- | --- | --- |
| `PHYS_BASE`/`MEM_SIZE` (low-bank fallback) | `0xb0000000` / `0x4d000000` (above ramdump_fb, below rmtfs) | Recompute from lemonadep reserved-memory; kernel RAM must lie **above every XPU carve-out** because XNU `start.s` maps `[kernel..physBase+memSize)` cacheable |
| `HIGH_PHYS_BASE`/`HIGH_MEM_SIZE` (`--highmem`, used on phone) | `0x1_0000_0000` + `0xfcc00000` (sweet upper DDR bank) | Lemonadep DRAM banks differ (8/12 GB SKUs); pick a bank free of carve-outs. **SKU-dependent** → needs per-SKU value or runtime check |
| `PANIC_LOG_SIZE` / `/pram` | 1 MiB at top of dram range | Generic; follows the bank |
| `SOC_BASE` | `0x01000000` (arm-io ranges parent, must be ≠ 0) | Can stay; just must be below all MMIO used |
| `GICD`, `GICR` (+ sizes) | `0x17a00000`, `0x17a60000` / `0x100000` | SM8350 GIC bases + GICR span for **8 PEs** |
| `FB`, `FB_SIZE`, `FB_W/H/STRIDE` | `0x9c000000`, `0x1700000`, 1080×2400, 4320 | cont_splash region + panel geometry (OP9P is a much larger panel) |
| `UFS`, `UFS_SIZE` | `0x01d84000` / `0x3000` | SM8350 `ufshc` base |
| `TIMEBASE` | 19.2 MHz | Verify SM8350 `cntfrq` (DT `arch_timer`) |
| `VTIMER_INTID` | 19 (Qualcomm PPI 3) | Verify from SM8350 `timer` node interrupts |
| CPU nodes | 8 cores, `reg = core<<8` (MPIDR Aff1), `compatible 'qcom,kryo470'`, A55 1.8 GHz / A76 2.3 GHz, per-core L2 ids | SM8350 1×X1 + 3×A78 + 4×A55: MPIDRs from DT `cpus`, clocks, cache ids. **Keep `qcom,kryo470` in the compatible list** or patch 0018's `IONameMatch` must change (see §6) |
| Root `compatible` | `D22AP, iPhone10,3, xiaomi,sweet, qcom,sm7150, AppleARM` | `xiaomi,sweet` is the **IONameMatch for SweetPlatformExpert** (patch 0016). Keep that token (cheap) or change the patch |
| `/chosen/display-scale` | 3 (1080 px @3x) | Recheck for the OP9P panel width |
| `/arm-io/disp0` canvas | 1125×2436 (iPhone X) | iOS-side identity; unchanged |
| `SWEET_ECID`, chip-id 0x8015, security-status props, `/asmb`, `/defaults`, `/buttons`, `/product` (copied from real D22AP ADT) | iPhone X identity | Unchanged (device-agnostic) |
| `--qemu` mode | QEMU virt GIC / CNTFRQ | Unchanged |

Port shape: a profile table (`sweet`, `lemonadep`) feeding the same `build()`,
so sweet output is byte-identical.

## 2. The shim (`shim/`) — memory map, MMU, framebuffer console, loader

| File | Sweet facts | Lemonadep change |
| --- | --- | --- |
| `console.h` | `FB_BASE 0x9c000000`, `FB_W 1080`, `FB_H 2400`, `FB_STRIDE`, a8r8g8b8 | Lemonadep splash base/geometry/format. Font `SCALE 3` probably fine; may want 4 on a 1440-wide panel |
| `mmu.h` | `FB_PHYS 0x9c000000` | Same value as console.h (dedupe into one board header) |
| `mmu.c` | 16K granule, T0SZ=28 (64 GB VA, L2 start), **identity-maps first 8 GB as Device**, fb 32 MB block Normal-NC, only image pages WB | Generic design. Check: (a) lemonadep DRAM/MMIO within the 8 GB Device window; 12 GB SKU RAM above 8 GB is unmapped (fine for the shim, it's only the test path); (b) splash region may span >1 32 MB block → map every block it touches; (c) `TCR` IPS=36-bit vs SM8350 PARange |
| `shim.c` | `IMEM_RESTART_REASON 0x146aa65c`, `PSHOLD 0x0c264000`, `GICD_BASE`, `GICR_BASE/SIZE`, vtimer/ptimer INTIDs 19/18, `NR_CPUS 8`, `MPIDR = i<<8`, `to_us()` assumes 19.2 MHz | All are diagnostics/reset only (loader runs first). Port the constants; `test_smp` MPIDR scheme must match SM8350 cpu `reg`s |
| `start.S` | Linux arm64 Image header, `flags 0xa` (4K, place anywhere), entry at EL1 MMU off, `x0`=DTB | Generic. **Verify lemonadep ABL entry EL** (sweet: EL1; patch 0065 already handles EL2 for secondaries) |
| `loader.c` | `boot_args.Video` = `FB_BASE`/`FB_STRIDE`/`FB_W`/`FB_H`; payload is found at `/chosen linux,initrd-start` | Geometry via the board header. **Big risk:** with boot header v3/v4, ABL **concatenates vendor_boot's ramdisk in front of ours**, so `initrd-start` will not point at the `SWEETXNU` magic → loader must scan the initrd range for the magic (16-byte aligned). Ramdisk/kernel overlap check stays |
| `fdt.c` | Reads `/chosen` only | Generic |
| `ufs.c` (test probe) | `UFS_BASE 0x01d84000`; JEDEC UFSHCI; assumes ABL left host enabled, link up, 4K LBAs, userdata on LUN 0 | Base address. Behaviour must be re-validated: does ABL leave UFS HCE=1 after `fastboot boot`? Which LUN holds `userdata` on lemonadep? |
| `shim.ld` | Linked at 0, 8 secondary stacks | Generic |
| `build.sh` | `mkbootimg --header_version 2 ... --dtb ../dtb/boot_unpacked/dtb --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 --tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 --os_version 17.0.0 --os_patch_level 2035-06` | See §4 |

## 3. UFS host driver

Two copies of the same polled single-slot UFSHCI driver:

- `shim/ufs.c` — read-only probe (base hard-coded, tier C).
- `bsd/dev/sweet_ufs.c` (patches **0028, 0030, 0034, 0047, 0054, 0069**, + log/usb hooks in 0060/0072/0077) — the root disk `ufsroot0`.

The kernel driver is **tier B**: base comes from ADT `/sweet-ufs reg`, partition
found by GPT name (`ufsroot=`, default `userdata`). It does **no** link
start-up, PHY, clock, regulator or ICE programming: it **adopts the host ABL
left running** (checks `HCE`, `HCS.DP`, `UTRLRDY`, empty doorbell) and swaps
in its own transfer request list. 64-bit DMA guarded by `CAP.64AS` (0034).

So "porting the UFS driver" to SM8350 is mostly: correct base in the ADT +
confirming ABL leaves the host alive. A real PHY/clock bring-up is only needed if
ABL tears it down. Things to verify for lemonadep:

- Host enabled after `fastboot boot` (first on-device test; the shim probe reports it).
- `userdata` LUN index (driver scans LUN `su.lun`; sweet = 0).
- Inline crypto (ICE): driver never sets crypto fields in the UTRD → plaintext pass-through. Must hold on SM8350 too.
- Persistent log (0047) writes the **last 4 MiB + 4 KiB of the root partition**; `tools/sweet-log-pull.sh` reads it via a recovery with adb root (`/dev/block/by-name/userdata`). Lemonadep needs a recovery that offers adb root (OrangeFox is sweet-specific in the docs).

## 4. Boot image packaging

| Piece | Sweet assumption | Lemonadep |
| --- | --- | --- |
| `shim/build.sh` | boot.img **header v2**, stock DTB embedded (`dtb/boot_unpacked/dtb`), offsets mirror PixelOS | Determine OP9P header version (STEP 2, `BOOT_lemonadep.md`). If v3/v4: no `--dtb` in boot.img (DTB lives in `vendor_boot`/`dtbo`), no `--base/--*_offset`, page size 4096, and the ramdisk concatenation issue in §2 |
| `tools/unpack_bootv2.py` | Only parses v0–v2 | Needs v3/v4 + vendor_boot parsing (to extract the stock DTB for reference) |
| `tools/mkpayload.py` | `SWEETXNU` magic, payload format v1, reads RAM from ADT | Generic — reuse |
| Delivery | `fastboot boot` (no flashing), Xiaomi A-only | OP9P is A/B; verify `fastboot boot` is accepted by the OnePlus ABL and which `vendor_boot`/`dtbo` slot it pairs with |
| Restart reason | IMEM `0x146aa65c` = `0x77665500` → fastboot | Not even working on sweet; low priority |

## 5. CI (`.github/workflows/xnu.yml`)

Builds `MACHINE_CONFIG=SWEET` (default) on macos-14 via blacktop/darwin-xnu-build
at a pinned SHA, copies **all** `xnu/patches/*` in. No device-specific values
except the machine config name. A lemonadep job = same job with a different patch
set (sweet series + a lemonadep overlay), artifact name `xnu-kernel-lemonadep`.
The shim and ADT are not built in CI today (host-built locally).

## 6. XNU patch series — classification

Rule from the brief: reuse unchanged, flag the secretly sweet-specific ones.

### Tier A — generic, reuse unchanged
0001, 0002, 0005, 0006, 0007, 0008, 0009, 0011, 0013, 0014, 0015, 0019–0027,
0029, 0031–0033, 0035–0045, 0048–0055, 0057, 0059, 0064, 0066–0068, 0070, 0080.
(Names say "sweet" but contain no hardware facts: crypto, HFS, AMFI, IOKit
services, crash/log diagnostics, sandbox, identity.)

### Tier B — generic code, values from ADT/boot_args (reuse if ADT is right)
| Patch | Reads | Hidden coupling |
| --- | --- | --- |
| 0003 fb serial console | ADT `sweet-fbcon` reg/width/height/stride | a8r8g8b8 32-bpp assumed |
| 0004 SWEET machine config | GIC bases from ADT; Group-1 IRQ path | Assumes **GICv3 with SecurityExtn=1**, PSCI conduit; `SWEET.h` comment says Kryo 470 but values are generic (16K, v8.2, no PAC). SM8350 X1/A78/A55 are v8.2 → OK |
| 0016 platform expert | matches **`IONameMatch 'xiaomi,sweet'`** | ⚠ secretly sweet-specific string → keep token in ADT |
| 0017 vtimer IRQ | ADT `virtual-timer-intid` | — |
| 0018 boot CPU driver | matches **`IONameMatch 'qcom,kryo470'`** | ⚠ keep token in each cpu node's compatible |
| 0028/0030/0034/0069 UFS | ADT `/sweet-ufs` | Adopts ABL-initialised host (see §3) |
| 0046 IOSurface/IOMFB | `PE_current_console` (boot_args Video) | `SWEET_FB_DOT_PITCH 395` ppi (sweet panel) — cosmetic |
| 0047 persistent log | partition tail | — |
| 0056 throttled present | `sweetfb_hz` | — |

### Tier C — hard-coded sweet hardware (⚠ NOT reusable unchanged)
| Patch | Hard-coded facts | Lemonadep impact |
| --- | --- | --- |
| **0010** early fb debug | `start.s`: `0x9c000000 + 2368*4320`, stride 4320; `sweet_early.c`: `EARLY_FB 0x9c000000`, 1080×2400 | Needed for milestone 1 (pre-console markers). Must take lemonadep fb base/geometry (build-time `#ifdef` or a board header) |
| **0012** debug console after TTBR0 | block entry for `0x9c000000` in the V=P table | Same; also block alignment of the new fb base |
| **0058** HID event services | touch normalisation `/1080`, `/2400` | Panel resolution |
| **0060** keys + touch | SPMI PMIC arb core/obs/cnfg `0x0c440000/0x0e600000/0x0c40a000`, PON `0x0810`, PM6150L GPIO2 `0xc110` (SID 4); GENI SE7 `0xa84000`, GCC `0x100000`, TLMM north `0x3900000`, RCG/branch offsets, GPIO numbers, Goodix @ `0x5d` regs, 1080×2400 | Entirely board-specific: lemonadep PMIC (PM8350/PMK8350 family) button map, QUP SE + touch IC (not Goodix GT9896) |
| **0061** iPhone X display size | centres 1080×2400 inside 1125×2436 | OP9P panel is larger than the iPhone X canvas → needs scaling or a different presented size |
| **0062** touch power + battery | RPMh RSC `0x18220000`, TCS `0x18220d00`, DRV 2, TCS counts/stride, cmd-db `0x0c3f000c`; regulator name for PM6150L LDO11; PM6150 FG regs `0x48xx`, charger `0x10xx/0x13xx` | Touch rail + fuel gauge differ completely; battery can stay static first |
| **0063** GENI packing / Goodix protocol | SE packing words, GT9896 protocol | Touch IC protocol differs |
| **0065** SMP PSCI | `smc` CPU_ON, A55-only (`phys_id ≤ 0x500`) | MPIDR/topology check |
| **0078** 8-core SMP | `phys_id ≤ 0x700`, "0-5 A55, 6-7 A76" comment | Topology check must match SM8350 MPIDRs (and X1/A78/A55 order) |
| **0071** touch edge | edge tuning for sweet panel | Re-tune |
| **0072–0077, 0081** USB ECM | GCC `0x100000`, USB30_PRIM GDSC/CBCR offsets, DWC3 `0x0a600000`, **QUSB2 PHY** `0x088e2000` + tune offsets, QSCRATCH `0x0a6f8800` | SM8350 uses a different USB PHY family; off by default (`sweetusb`), defer |
| **0079** display power | fade loop on panel fb | Generic-ish; display on/off goes through ABL panel → re-check |

Plan for tier C: keep the sweet series untouched and add a **lemonadep overlay
series** (`lemonadep/xnu/patches/`) that either ADT-ifies the constants (better:
sweet ADT gets the same props, behaviour unchanged) or `#if`s per board. Until
then lemonadep boots with `sweettouch=0 sweetkeys=0` and USB off.

## 7. Other tools / docs

| File | Sweet coupling |
| --- | --- |
| `tools/qemu-sweet.py` | fb `0x9c000000` 1080×2400, kernel `0xb0000000`; QEMU path is sweet-geometry only |
| `tools/sweet-log-pull.sh`, `sweet-log.py` | Generic (needs adb-root recovery) |
| `tools/mkiosroot.sh`, `sweet-root-tweaks.py`, `mkinit.py`, `remotezip.py`, `sweet-crash.py`, `sparsediff.py`, `xnu-cc.py`, `adt.py` | Device-agnostic (iOS-side) |
| `README.md`, `NOTES.md`, `site/` | Sweet docs; NOTES "Hardware facts" is the measured reference for what we need to measure on lemonadep |
| `.gitignore` | `backup/`, `dtb/`, `recovery/` — reuse for lemonadep dumps |

## 8. What STEP 2 must therefore extract

1. Reserved-memory map + DRAM banks (per SKU) → kernel `phys_base/mem_size`.
2. cont_splash: base, size, w/h, stride, format.
3. GICD/GICR bases, GICR span, timer PPIs, CNTFRQ.
4. CPU `reg`s (MPIDR), cluster layout, PSCI method.
5. UFS host base (+ PHY, version/clocks for the record), userdata LUN.
6. PMIC/SPMI arb bases, PON + volume key mapping.
7. Panel (model, resolution, mode) and touch (bus, SE base, address, IC).
8. Boot header version, vendor_boot/dtbo, `fastboot boot` behaviour on A/B.

Unknowns that only the device can answer (become on-device checklist items):
entry EL, ABL's UFS state after `fastboot boot`, splash actually scanned out
in video mode (LTPO panel may be command mode → needs a refresh kick), whether
the vendor ramdisk is prepended to ours.
