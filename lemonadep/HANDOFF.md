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
