# lemonadep port: honesty report (STEP 4)

State as of 2026-10-22 (branch `lemonadep`, pushed to `popovluka/ios-sweet`) (local branch `lemonadep`, not pushed).
**Nothing has run on a OnePlus 9 Pro yet.** Everything below "verified" was
verified on a host (WSL `LemonadepLab`, Ubuntu 24.04), not on the phone.

## 1. What exists

### Implemented (host-verified)

| Piece | Where | Verified how |
| --- | --- | --- |
| lemonadep hardware map with sources | `HWMAP_lemonadep.md`, `BOOT_lemonadep.md` | line citations re-checked against the vendor DT |
| ADT board profile | `tools/mkadt.py --board lemonadep` | sweet output byte-identical (4 variants); lemonadep ADT dumps and parses |
| Shim board config | `shim/board.h` | both boards compile, 0 warnings, 0 undefined symbols, no ABS64 relocations |
| Payload search in a concatenated initrd, misaligned copy | `shim/payload_scan.c`, `shim/loader.c` | `shim/host_tests.sh` (ASan/UBSan): magic at 24 alignments, near-misses, straddling end; copy at 8 misalignments |
| Generic FDT walk (`/memory`, `/reserved-memory`, `bootargs`) | `shim/fdt.c` | host test against a `dtc`-compiled tree |
| ADT in-place patch (fb geometry) | `shim/adt_patch.c` | host test + re-parse with `tools/adt.py` |
| DRAM safety check (live `/memory`, no-map overlap) | `shim/loader.c` `check_dram` | compiled only; exercised on device |
| Boot image header v3 | `shim/build.sh` | `unpack_bootimg.py` reads it back as v3 |
| CI | `.github/workflows/lemonadep.yml` | YAML parses; shim steps run locally. **Never run on GitHub** |

### Implemented but unverifiable off-device

| Piece | Assumption that may be wrong |
| --- | --- |
| `shim/display.c` splash discovery | ABL leaves a CTL with DSI0 (INTF_1) active and an SSPP whose `SRC0_ADDR` lies in the splash region; MDSS registers are readable (clocks on) |
| Autorefresh enable | Writing `INTF_TEAR_AUTOREFRESH_CONFIG = BIT31|1` + `CTL_START` makes MDP push a frame per TE without Linux's surrounding setup |
| `CTL_START` kick fallback | One START re-sends the frame with ABL's configuration |
| Diagnostic pages | Text is drawn for 2/3/4-byte pixels; colours other than black/white may be swapped |

### Stubbed / deliberately absent

| Area | State |
| --- | --- |
| Kernel overlay patches | 1001 written, host-checked, and compiled in the macOS kernel job (run 39698732689); not booted. 1002-1006 plans (`OVERLAY_PATCHES.md`) |
| Early kernel framebuffer (0010/0012) | fixed by 1001: geometry from `boot_args->Video` (builds; boot untested) |
| Touch (Synaptics S3908) | no driver; `sweettouch=0` |
| Buttons (PMK8350 PON / PM8350 GPIO6) | no driver; `sweetkeys=0` |
| USB networking | off; `sweetusb=0` (sweet default is ON) |
| Battery | static 100 % (sweet default) |
| UFS driver | no change needed *if* ABL leaves the host running (same base `0x1d84000`) |
| `tools/qemu-sweet.py` | sweet only; QEMU has no MDSS, so the lemonadep shim cannot run there |
| PS_HOLD reset | not found in any source; PSCI SYSTEM_RESET only |

### TODO(lemonadep): facts only the device can give

| ID | Fact | Read from (checklist step) |
| --- | --- | --- |
| D1 | ~~`fastboot boot` of a v3 image~~ **accepted** (2026-10-22) | done |
| D2 | ~~Stock `os_version` / `os_patch_level`~~ **16.0.0 / 2026-08** (Evolution X boot_b, header v3) | done |
| D3 | ~~Entry EL, load address, DTB~~ **EL1, 0xA0080000, DTB 0xAE755000** | done |
| D4 | ~~Splash plane~~ **0xE4D00000, 1080×2412, stride 4320, 32 bpp (VIG0+VIG1 split)** | done |
| D5 | ~~Autorefresh~~ **already on from ABL; text visible** | done |
| D6 | ~~DRAM banks~~ **8 GB: `0x80000000+0x39700000`, `0xc0000000+0x1c0000000`** (live FDT) | done; shim `MEM` re-checks |
| D7 | ~~Retail panel~~ **amb670yf01 dsc cmd** (`/proc/cmdline`) | done |
| D8 | ~~vtimer, SGIs, CPU_ON~~ **INTID 27 OK, SGI OK, all 7 secondaries start** | done |
| D9 | ~~UFS state~~ **running after ABL, UFSHCI 3.0, 64-bit DMA, reads OK, userdata LUN 0** | done |
| D10 | Recovery with adb root for the log pull. Android has Magisk root, so backups and reads work now; **once userdata holds iOS, Android won't boot**, so the M2 log pull still needs a recovery | 0.4 |
| D11 | PON gen-3 RT status register, TLMM GPIO offsets, S3908 protocol | source work (OP-K), not device |

## 2. Top risks (most likely to stop progress first)

1. ~~**Command-mode LTPO DSC panel (AMB670YF01).**~~ **Retired for the boot console (M1a, 2026-10-22):** ABL leaves INTF autorefresh on, and the shim's text was visible. Still open for later: whether XNU's present path and display power-off (0079) keep autorefresh running. sweet's panel scanned RAM by itself; this one only shows a frame when MDP sends it.
   - If autorefresh does not start and START kicks do nothing, **nothing we draw is visible**: not the shim, not XNU, not iOS.
   - Fallback: the shim still proves it ran by rebooting after about 4.5 min. Beyond that, real DSI command-mode transfers mean porting part of the SDE encoder sequence.
   - LTPO/ADFR (variable refresh, "fake frames") may also need panel commands that ABL never sent.
2. **Boot path.** OnePlus ABL may refuse `fastboot boot`; LineageOS never uses it.
   - The fallback is flashing the **inactive** slot's `boot`, which is destructive to that slot.
   - Wrong `os_version`/patch level can trip rollback/AVB checks.
   - The vendor ramdisk in front of ours is handled by the scan, but untested on hardware.
3. ~~**UFS after ABL.**~~ **Retired (M1a run 4):** ABL leaves the host running and reads work. Writes are still untested. The kernel driver has no PHY/clock/regulator code. It only works if ABL leaves the UFS host enabled with the link up (true on sweet).
   - If ABL powers UFS down after loading from USB, we need a real SM8350 QMP UFS PHY bring-up: large, sourceable from mainline `phy-qcom-qmp-ufs.c` and the vendor `ufs-qcom.c`.
   - Inline crypto (wrapped keys) is configured by Android, not by us. Plaintext pass-through is expected but unproven.
4. **Memory.** No source gives the DRAM layout; Qualcomm's hypervisor may protect regions no DT lists. The kernel maps its whole RAM window cacheable, and a speculative access to a protected range is a hard fault. Mitigation: start with a 1 GiB window above 4 GiB, after D6.
5. **Kernel early writes (0010/0012) to `0x9c000000`.** On lemonadep that is ordinary DRAM. Overlay 1001 removes them; it builds in CI but has not booted.
6. **Heterogeneous SMP (A55 + A78 + X1) as one cluster.** It works on sweet's A55 + A76, but cache geometry differs per core type. Bring up `cpus=1`, then 4, then 8.
7. **Getting logs back.** sweet used OrangeFox. lemonadep keeps recovery in `vendor_boot`, and which recovery gives `adb` root with raw `dd` of userdata is unknown. Without it, debugging is photos of the screen only.
8. **Touch** is a different vendor/IC (Synaptics TCM, not Goodix) on a different SE: a new driver, not a port.
9. **Pin controller base disagreement** (vendor `0x0f000000` vs mainline `0x0f100000`). It must be settled before any GPIO write (touch reset/power).

### Found on the device (M1a, 2026-10-22)

10. **The SM8350 hypervisor restricts EL1.** EL1 gets a virtual GIC that faults a redistributor write arriving while the previous one is pending, and the physical timer is off-limits. XNU's GIC init and `_enable_virtual_timer()` hit both. Overlay **1002** adds the RWP waits and an ADT-gated CNTP skip. Shim run 5 tests whether polling RWP is enough. Other EL1 registers XNU touches may be trapped too, and only the M1b boot will tell.

## 3. Milestones

| # | Milestone | Needs (code) | Needs (device, destructive?) |
| --- | --- | --- | --- |
| M1 | **Boot + framebuffer**: shim text visible, then XNU kernel console visible | shim (done); overlay 1001 for the kernel half | `fastboot boot` only, no |
| M2 | **UFS mounts the iOS root** | sweet UFS/HFS patches (as-is) | flash iOS root to `userdata`: **yes, erases Android data** |
| M3 | **Lock screen** | overlay 1003 (panel geometry); 1002 if autorefresh failed | no |
| M4 | **Touch** | overlay 1005 (S3908), TLMM offsets, RPMh `ldoc3` | no |
| M5 | **Home screen** (unlock, swipe, keys) | overlay 1004 (keys) | no |

## 4. On-device checklist

Rules for every step:
- Photograph the screen (whole screen, readable).
- Send me the photos and the exact host output.
- Stop at the first unexpected result.
- The bootloader key combo is **Volume Up + Volume Down + Power** with the phone off (LineageOS wiki, lemonadep).
- Keep device serials, `getvar all` output and backups out of git (`.gitignore` already covers `backup/`, `dtb/`, `recovery/`).

### 0. Preparation (no risk)

1. **Identify the phone.** Model must be LE2120, LE2121, LE2123 or LE2125 (the lemonadep variants on the LineageOS wiki). Bootloader unlocked. In fastboot:
   - `fastboot getvar current-slot`
   - `fastboot getvar all 2> backup/getvar-all.txt`

   Send me: the current slot, and the lines for `product`, `version-bootloader` and `secure`.
2. **Note the Android build.** Settings → About: Android version, security patch level, OxygenOS build number.
3. **Get the stock boot.img** for exactly that build: from the full OTA zip, extract `payload.bin` → `boot.img`. Then:

   ```
   python3 /opt/aosp-mkbootimg/unpack_bootimg.py --boot_img boot.img --out stock_boot
   ```

   Send me `boot image header version`, `os version`, `os patch level` (→ D2). If the header version is not 3, stop: `BOOT_lemonadep.md` needs revising.
4. **Find a recovery with adb root** (→ D10). Candidate: Lineage Recovery `boot.img` for lemonadep via `fastboot boot`; it may need Lineage's `vendor_boot`, which would be a flash: **ask me first**. Test with `adb shell id`. Expect `uid=0(root)`.
5. **Back up** (from that recovery):
   - `adb shell ls -l /dev/block/by-name/ > backup/by-name.txt`
   - `dd` every calibration partition that exists: at least `persist`, `modemst1`, `modemst2`, `fsg`, `fsc`, plus both slots of `boot`, `vendor_boot`, `dtbo`, `vbmeta*`.
   - Keep the copies and record SHA-256 sums.

   If no adb-root recovery exists: stop and tell me. Milestone 1 can still run (it writes nothing), but M2 must not.

### M1a: shim on screen (no flashing, nothing written to storage)

1. **Build.** Host:

   ```
   BOARD=lemonadep OS_VERSION=<D2> OS_PATCH_LEVEL=<D2> shim/build.sh
   ```

   Expect `shim/out/lemonadep/shim-boot-lemonadep.img` (~200 KB).
2. **Boot it.**

   ```
   fastboot boot shim/out/lemonadep/shim-boot-lemonadep.img
   ```

   - **If fastboot refuses:** send me the exact message (D1). Do **not** try `flash boot_b` on your own: that fallback is destructive and needs a decision.
3. **Within ~2 s, expect:** a black screen with yellow/white text starting `SHIM lemonadep milestone 1`. What other outcomes mean:

   | Screen | Meaning | Do |
   | --- | --- | --- |
   | Text visible | display path works (D5 ok) | go to step 4 |
   | Whole screen white or other solid colour, no text | shim ran, no splash plane found, but writes reach the panel | photo, then wait for the reboot (~4.5 min) |
   | Boot logo stays unchanged | either the shim did not run, or the panel never got a frame | wait 6 min |
   | Phone reboots by itself after ~4.5 min (30 s + 30 s + 180 s + 20 s) | shim ran; display path failed (**risk 1**) | tell me |
   | Never reboots | shim hung or never ran | force-reboot; tell me |

4. **Page 1 (shown 30 s): photograph it.** Values to read:

   | Line | Expect | Feeds |
   | --- | --- | --- |
   | `EL cur=` | 1 (2 = EL2: tell me) | D3 |
   | `img=` | any (load address) | D3 |
   | `DTB magic=` | `D00DFEED` | D3 |
   | `SPLASH dt=` | `E4D00000`, `size=02300000`. `def=` instead means the live tree has no `splash_region`: tell me | D4 |
   | `DISP ctl=` / `pipe=` | small numbers, not `FFFFFFFF` | D4 |
   | `GEOM w= h=` | e.g. `438`/`96C` (1080×2412) if ABL uses the FHD default, `5A0`/`C90` if WQHD | D4 |
   | `GEOM str= bpp=` | stride, bytes per pixel | D4 |
   | `DISP AR was= now=` | `now=` with top bit set (`8000000x`) means autorefresh is on | D5 |
   | `MEM base/size` | one to four banks | D6 |
   | `PANEL` | the `msm_drm.dsi_display0=` value | D7 |
   | `PSCI ver=` | e.g. `00010001` | |

5. **Page 2 (30 s): GIC and SMP** (→ D8).
   - `VTIMER irqs=` about `1E` (30 in 300 ms) with `other=0`. **0 means INTID 27 is wrong: stop.**
   - `SGI1 irqs=0A`.
   - `CPU1..CPU7 ret=00000000` with a non-zero `mpidr=`.
6. **Page 3 (180 s): UFS probe, read-only** (→ D9). Record:
   - `ver=`, `cap=` (bit 24 = 64-bit DMA), `hce=` (expect 1), `hcs=`
   - `UFS TUR rc=0`, `UFS LUN0 lastlba`
   - the GPT list: does `userdata` appear on LUN 0, with `fs f2fs`?

   **`host not enabled/ready: stop` = risk 3.**
7. The phone resets itself. Return to fastboot.

**After M1a I update** `board.h`/`mkadt.py` defaults and HWMAP with D3–D9, write overlay 1001, and start the CI kernel build.

### M1b: XNU kernel console (no flashing; root mount will fail)

1. **Get the kernel.** You (or I, with your OK to run `gh` against your fork):

   ```
   gh workflow run lemonadep.yml -f build_kernel=true -f os_version=<D2> -f os_patch_level=<D2>
   ```

   Then download `xnu-kernel-lemonadep`.
2. **Build the payload.** Use a 1 GiB `--dram` window inside a page-1 `MEM` bank above 4 GiB (I'll give the exact numbers). Use the `README.md` commands with read-only boot-args: **no `ufsrw=1`**, because the log ring would overwrite the end of Android's userdata.
3. **Boot it.**

   ```
   fastboot boot shim/out/lemonadep/shim-boot-lemonadep.img
   ```

   - Expect: shim page 1, then `LOADER` lines, `JUMP`, `XNU jumping`, then white kernel text.
   - The run is expected to **end in a panic about the root device**, because userdata is still Android f2fs. That is a pass for M1.
   - Photograph the last screen.
   - If the screen freezes at `XNU jumping`: photo of the bottom edge (1001 stage bands).
   - **Any `LOADER` line other than the jump** (e.g. `kernel RAM overlaps no-map carve-out`) means the shim refused to start the kernel: photo, stop.
4. Then retry with `cpus=4` (A55s), then `cpus=8`. Photograph any SMP panic.

### M2: UFS mounts the iOS root (destructive)

1. **Build the iOS root image.** Exactly as for sweet: README §2, iPhone X 16.7.16 (20H392). It is a 16 GiB image, smaller than userdata.
2. **⚠ Destructive: erases all Android user data.** Only after step 0.5 backups and my explicit confirmation in chat:

   ```
   fastboot -S 512M flash userdata ios16-root.simg
   ```

   - Android on this slot will no longer boot; recovery and the bootloaders are untouched.
3. **Boot** with `rd=ufsroot0 ufsrw=1 rootrw=1` added. The log ring now goes to the end of userdata, past the 16 GiB image.
   - Expect: `SWEET UFS: "userdata" on LUN 0: ... read-write`, then launchd and daemon lines.
   - Then the Apple logo, which can take 4–5 min. That is normal on sweet.
4. **Pull the log** from the adb-root recovery (0.4):

   ```
   tools/sweet-log-pull.sh logs/lemonadep-m2.txt
   ```

   Send me the file. User-space crashes (`CRASH ...` lines):

   ```
   tools/sweet-crash.py <dyld_shared_cache_arm64> <mounted root> logs/lemonadep-m2.txt
   ```

   Kernel panics are symbolicated against `kernel.development.sweet` (`keepsyms=1` already prints names).

### M3: lock screen

1. Boot the kernel with overlay 1003 (panel geometry), plus 1002 if D5 failed. Add `sweetfb_noconsole=1 sweetfb_hz=30`.
2. Expect: the lock screen, centred, 4–5 min after power-on.
   - Photo.
   - Pull the log and send the `SWEET FB` lines.

### M4: touch

1. Boot with overlay 1005 and its enable boot-arg (named when written).
2. Expect: `TOUCH`/S3908 log lines (chip ID, report rate).
3. Swipe up on the lock screen. Expect: unlock.
4. Pull the log either way.

### M5: home screen

1. Boot with overlay 1004 (keys).
2. Check each:
   - Power key: lock/wake (`SWEET FB: display off/on`).
   - Volume up/down: HUD.
   - Swipe up: home screen.
3. Pull the log.
