# lemonadep XNU overlay patch plan

The sweet series `xnu/patches/0001-0081` stays untouched. lemonadep-only changes go
in `lemonadep/xnu/patches/1NNN-lemonadep-*.patch`. `.github/workflows/lemonadep.yml`
copies them after the sweet series, so they apply last.

**Status: plan only. The overlay directory is still empty.** No overlay patch has
been written or compiled yet. Each one needs a CI kernel build (`lemonadep.yml`,
`build_kernel: true`), plus `tools/xnu-cc.py` single-file checks first.

Preferred style: make a sweet constant **data-driven** (ADT or boot_args) rather
than `#if`-ing a lemonadep value in. Then the same code path serves both boards,
and sweet behaviour stays identical because its ADT carries today's values.

## Needed before the first XNU boot on lemonadep (milestone 2)

### 1001-lemonadep-early-fb-from-bootargs — fixes 0010, 0012 (blocker)

**Problem.** 0010 and 0012 hard-code the sweet splash `0x9c000000`, 1080×2400, stride 4320:
- `start.s` writes a 64×32 marker band to `0x9c000000 + 2368*4320` with the MMU off.
- `start.s` maps the 32 MiB block at `0x9c000000` as Normal-NC.
- `sweet_early.c` draws stage bands and panic text at that address.

On lemonadep `0x9c000000` is plain DRAM. It sits between rmtfs (mainline `0x9b800000 + 0x280000`) and OPLUS `kboot_uboot_logmem` (`0x9fe00000`). These writes would land in memory ABL may still use, and show nothing on screen.

**Fix.** Take everything from `boot_args->Video`; the shim fills it with the measured plane:
- `x20` holds the boot_args PA at that point.
- `v_baseAddr` is at offset 40, `v_rowBytes` at 56, `v_width` at 64, `v_height` at 72 (`struct boot_args` in `shim/loader.c`).
- Map every 32 MiB block covering `[v_baseAddr, v_baseAddr + v_height*v_rowBytes)`. lemonadep's splash spans two blocks: `0xe4000000` and `0xe6000000`.
- Draw the band at `v_baseAddr + (v_height-32)*v_rowBytes`.
- `sweet_early.c`: `EARLY_WIDTH/HEIGHT/STRIDE` from boot_args.

**sweet check.** The shim already passes 1080/2400/4320/`0x9c000000`, so the behaviour is identical.

### 1002-lemonadep-fb-kick — fixes 0003, 0046/0056, 0079 (only if needed)

**When.** Only if the first shim boot shows `DISP AR now=` **without** bit 31 set, meaning autorefresh did not stick (see `shim/display.c`). Then the command-mode panel needs a `CTL_START` write after every frame.

**Fix:**
- mkadt: emit `sweet-fbcon/kick-reg = <0 0>` for lemonadep.
- `shim/loader.c`: fill it with `display.c`'s `kick` address.
- fbcon (0003) and `SweetFramebuffer` (0046 present path, 0056 throttle, 0079 fade): after copying a frame, `ml_io_map` the register once and write `1` to it.
  - Rate: at most once per `sweetfb_hz` tick.
  - Never while a transfer is in flight. `CTL_START` re-triggers; Linux waits for the pingpong-done IRQ. A ~16 ms floor is the conservative stand-in.

**sweet check.** No `kick-reg` property means no behaviour change.

### Boot-args (no patch)

Sweet drivers that **default to on** and touch sweet-only MMIO must be switched off:

| Boot-arg | Why |
| --- | --- |
| `sweettouch=0` | 0060 maps GENI SE7 `0xa84000`, TLMM north `0x3900000`, sweet GCC offsets |
| `sweetkeys=0` | 0060 reads PM6150 PON `0x0810` and PM6150L GPIO2 (SID 4). On lemonadep, SID 0 is PMK8350 (different PON layout) |
| `sweetusb=0` | 0072 **defaults to 1**. It votes sweet GCC USB30 GDSC/CBCR offsets and programs a QUSB2 PHY at `0x88e2000`. lemonadep has an SNPS femto PHY at `0x88e3000` |
| (`sweetbattery` unset) | 0062 defaults to a static battery. `sweetbattery=1` would read PM6150 FG registers |

## Needed for milestone 3–5 (lock screen → touch → home screen)

| Planned patch | Fixes | Change | Source facts (HWMAP) |
| --- | --- | --- | --- |
| 1003-lemonadep-panel-geometry | 0058 (`/1080`, `/2400` touch normalisation), 0061 (centres 1080×2400 inside 1125×2436), 0046 (`SWEET_FB_DOT_PITCH 395`) | Read the native size from `PE_current_console`. The FHD default 1080×2412 still fits inside 1125×2436 (offset 22/12). WQHD 1440×3216 does not and would need downscaling: stay on the FHD mode ABL picks | §2 panel timings |
| 1004-lemonadep-keys | 0060 keys | PMK8350 (SID 0) PON_HLOS @ `0x1300`: power = KPDPWR (IRQ bit 7), **vol-up = RESIN** (bit 6). **vol-down = PM8350 (SID 1) GPIO6** @ `0x8d00`. The SPMI arbiter bases are unchanged. TODO(lemonadep): the PON gen-3 RT_STS offset; verify against `drivers/input/misc/qpnp-power-on.c` in OP-K | §5 |
| 1005-lemonadep-touch-s3908 | 0060/0062/0063 touch | New Synaptics TCM driver on GENI I²C **SE4 `0x990000`**, address `0x4B`. IRQ TLMM 23, reset TLMM 22. Power: TLMM 31 (2v8 enable), TLMM 197 (1v8 enable), `pm8350c_l3` via RPMh `ldoc3`. The GENI/RPMh helpers from 0060/0062/0063 can be reused: the RSC/TCS layout is identical. TODO(lemonadep): TLMM per-GPIO register offsets (vendor `0x0f000000` vs mainline `0x0f100000` base) from OP-K `pinctrl-lahaina.c`; S3908 report format from OP-K `oplus_touchscreen` | §6, §7 |
| 1006-lemonadep-board-neutral-matching | 0016 (`IONameMatch 'xiaomi,sweet'`), 0018 (`'qcom,kryo470'`) | Cosmetic. Until it exists, mkadt keeps both tokens in the lemonadep ADT, so it is not needed to boot | DEVICE_SURFACE §6 |

## Unchanged and expected to work (verify on device)

| Patches | Why they should carry over | Device check |
| --- | --- | --- |
| 0004, 0017 | GIC bases are the same. The vtimer INTID comes from the ADT (27 on lemonadep) | shim `VTIMER irqs=` line |
| 0065, 0078 | SMP accepts MPIDR `0x000-0x700`. lemonadep matches (A55 ×4, A78 ×3, X1 ×1) | Start with `cpus=1`, then 4 (A55 only), then 8 |
| 0028-0034, 0047, 0069 | UFS host at the same address; the driver adopts ABL's controller | shim UFS probe page |
| 0062 RPMh core | `apps_rsc` drv-2 `0x18220000`, TCS offset `0xd00`, ACTIVE 2 / SLEEP 3 / WAKE 3 are identical | Only exercised by 1005 |
