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
