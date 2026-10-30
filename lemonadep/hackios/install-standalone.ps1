# Install the standalone HackiOS boot (build-standalone.sh) on a OnePlus 9 Pro: afterwards the phone
# starts iOS by itself on power-on, no computer. Two steps, both destructive -- read HANDOFF first.
#
#   -SystemImage  needs the phone in TWRP (fastboot boot twrp.img): writes system-rd.bin raw into
#                 userdata at 16 GiB (only past the 11.3 GB ios-sweet root; Android's data there is gone
#                 anyway once userdata holds iOS) and checks it back by SHA-256.
#   -Boot         needs the phone in its bootloader (fastboot): flashes lemonadep-standalone.img to the
#                 active slot's boot partition. Back the partition up first (backup/partitions/).
#
# Undo: fastboot flash boot_<slot> backup/partitions/boot_<slot>.img
param([string]$Dir = "$PSScriptRoot\..\out\hackios", [switch]$SystemImage, [switch]$Boot)
$env:PATH = "C:\platform-tools;$env:PATH"
if ($SystemImage) {
    adb push "$Dir\system-rd.bin" /tmp/system-rd.bin
    $n = (Get-Item "$Dir\system-rd.bin").Length
    adb shell "dd if=/tmp/system-rd.bin of=/dev/block/by-name/userdata bs=1048576 seek=16384 conv=fsync && sync && dd if=/dev/block/by-name/userdata bs=1048576 skip=16384 count=$([math]::Ceiling($n / 1MB)) 2>/dev/null | head -c $n | sha256sum && sha256sum /tmp/system-rd.bin; rm /tmp/system-rd.bin"
}
if ($Boot) {
    $slot = ((fastboot getvar current-slot 2>&1) -join ' ') -replace '.*current-slot: (\w).*', '$1'
    "active slot: $slot"
    fastboot flash "boot_$slot" "$Dir\lemonadep-standalone.img"
    fastboot reboot
}
