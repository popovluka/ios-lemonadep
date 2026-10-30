# One test run with nobody at the phone (kernel boot-arg logkeep=1, see kernel/lemonadep_logkeep.c):
# boot the UEFI + loader, upload, run; the run ends by resetting the phone into fastboot; boot the
# loader again and fetch the log the kernel left in RAM ("oem logkeep") into $Dir\$Log.
param([string]$Dir = "$PSScriptRoot\..\out\hackios", [string]$Log = "run.log", [int]$WaitMinutes = 6)
$env:PATH = "C:\platform-tools;$env:PATH"

function fb([string]$a, [int]$t = 20) {
    $j = Start-Job -ArgumentList $a { param($a) cmd /c "fastboot $a 2>&1" }
    if (Wait-Job $j -Timeout $t) { $r = (Receive-Job $j) -join "`n" } else {
        Stop-Job $j; Get-Process fastboot -ErrorAction SilentlyContinue | Stop-Process -Force; $r = $null }
    Remove-Job $j -Force; return $r
}
function wait_product([string]$want, [int]$secs) {
    $end = (Get-Date).AddSeconds($secs)
    while ((Get-Date) -lt $end) {
        $r = fb "getvar product" 8
        if ($r -match "product: $want") { return $true }
        Start-Sleep 3
    }
    return $false
}

Get-Process fastboot -ErrorAction SilentlyContinue | Stop-Process -Force
"$(Get-Date -Format T) run"
& "$PSScriptRoot\run.ps1" -Dir $Dir | Out-Null
"$(Get-Date -Format T) running; waiting for the phone to come back in fastboot"
if (-not (wait_product "lahaina" ($WaitMinutes * 60))) { "$(Get-Date -Format T) NOT back in fastboot (hung, or crash dump)"; exit 1 }
"$(Get-Date -Format T) back in fastboot; booting the loader for the kept log"
fb "boot `"$Dir\lemonadep-boot.img`"" 30 | Out-Null
if (-not (wait_product "xnuloader" 60)) { "loader did not come up"; exit 1 }
$r = fb "oem logkeep" 15
if ($r -notmatch 'OKAY') { "no kept log: $r"; exit 1 }
Remove-Item "$Dir\$Log" -ErrorAction SilentlyContinue
fb "get_staged `"$Dir\$Log`"" 60 | Out-Null
"$(Get-Date -Format T) log: $Dir\$Log ($((Get-Item "$Dir\$Log").Length) bytes); the phone waits in the loader"
