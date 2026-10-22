# lemonadep handoff (2026-10-22 ~04:00) — read first

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
