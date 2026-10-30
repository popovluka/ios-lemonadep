# Does the loader answer USB while a kernel runs? Start the run (run.ps1), poll "getvar result" every
# 2 s for $Seconds, then wait for the phone to come back to fastboot by itself (logkeep=1).
# Prints how many polls answered. No one at the phone.
param([string]$Dir = "$PSScriptRoot\..\out\hackios", [int]$Seconds = 90, [int]$LogAt = 45, [string]$Log = "live.txt")
$got = $false
$env:PATH = "C:\platform-tools;$env:PATH"
function fb([string]$a, [int]$t = 8) {
    $j = Start-Job -ArgumentList $a { param($a) cmd /c "fastboot $a 2>&1" }
    if (Wait-Job $j -Timeout $t) { $r = (Receive-Job $j) -join ' ' } else {
        Stop-Job $j; Get-Process fastboot -ErrorAction SilentlyContinue | Stop-Process -Force; $r = 'no answer' }
    Remove-Job $j -Force; return $r
}
Get-Process fastboot -ErrorAction SilentlyContinue | Stop-Process -Force
& "$PSScriptRoot\run.ps1" -Dir $Dir | Out-Null
$t0 = Get-Date; $ok = 0; $n = 0; $first = $null
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    $r = fb "getvar result"; $n++
    if ($r -match 'result:') { $ok++; if (-not $first) { $first = $r } }
    # the log so far, once a while into the run (and kept: live-<t>.txt)
    $s = [int]((Get-Date) - $t0).TotalSeconds
    if ($r -match 'running' -and ($s -ge $LogAt) -and -not $got) {
        Remove-Item "$Dir\live.raw" -ErrorAction SilentlyContinue
        fb "oem log" 10 | Out-Null
        fb "get_staged `"$Dir\live.raw`"" 30 | Out-Null
        if ((Test-Path "$Dir\live.raw") -and (Get-Item "$Dir\live.raw").Length -gt 0) {
            $b = [IO.File]::ReadAllBytes("$Dir\live.raw") | Where-Object { $_ -ne 0 }
            [IO.File]::WriteAllText("$Dir\$Log", [Text.Encoding]::UTF8.GetString([byte[]]$b))
            "t+${s}s live log $((Get-Item "$Dir\$Log").Length) bytes -> $Log"; $got = $true
        }
    }
    Start-Sleep 2
}
"polls $n, answered $ok; first answer: $first"
$end = (Get-Date).AddMinutes(5)
while ((Get-Date) -lt $end) { if ((fb "getvar product") -match 'product: lahaina') { "back in fastboot"; exit 0 }; Start-Sleep 3 }
"not back in fastboot"
