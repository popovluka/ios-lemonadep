# Boot HackiOS on lemonadep from Windows with the stock fastboot (no WSL USB passthrough needed).
# The phone must be in the bootloader's fastboot. Images come from build-boot.sh.
param([string]$Dir = "$PSScriptRoot\..\out\hackios")
# a current fastboot: Touch Portal's 28.0.3 comes first on PATH here and loses the loader once a run starts
$env:PATH = "C:\platform-tools;$env:PATH"

# already in the loader (left there by run-auto.ps1): no UEFI boot needed
if (-not ((cmd /c "fastboot getvar product 2>&1") -match 'product: xnuloader')) {
    fastboot boot "$Dir\lemonadep-boot.img"
}
# the XNU loader enumerates as fastboot with serial XNULOADER, product xnuloader, ~10 s later
$t = 0
while ($t -lt 60) {
    Start-Sleep 3; $t += 3
    if ((cmd /c "fastboot getvar product 2>&1") -match 'product: xnuloader') { break }
}
if ($t -ge 60) { Write-Error "loader did not come up"; exit 1 }

fastboot oem ramdisk               # the 2.4 GB system image stays resident in RAM
fastboot stage "$Dir\system.apfs"  # ~45 s
fastboot stage "$Dir\run-payload.bin"
fastboot oem run
