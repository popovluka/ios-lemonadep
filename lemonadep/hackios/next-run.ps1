# Wait for a run to hand back to the loader, save its log, then start the next run payload
# (the system image stays resident in the loader, so it is not uploaded again).
param([string]$Dir = "$PSScriptRoot\..\out\hackios", [string]$Log = "run.log", [int]$WaitMinutes = 20, [switch]$NoStart)
$env:PATH = "C:\platform-tools;$env:PATH"    # a current fastboot (see run.ps1)

function fb([string]$a, [int]$t = 15) {
    $j = Start-Job -ArgumentList $a { param($a) cmd /c "fastboot -s XNULOADER $a 2>&1" }
    if (Wait-Job $j -Timeout $t) { $r = Receive-Job $j; Remove-Job $j; return ($r -join "`n") }
    Stop-Job $j; Remove-Job $j -Force; return $null
}

$end = (Get-Date).AddMinutes($WaitMinutes)
while ((Get-Date) -lt $end) {
    $r = fb "getvar result" 10
    if ($r -and $r -match 'result:' -and $r -notmatch 'running') { "$(Get-Date -Format T) $($r -split "`n" | Select-String 'result:')"; break }
    Start-Sleep 10
}
fb "oem log" 20 | Out-Null
fb "get_staged `"$Dir\$Log.raw`"" 60 | Out-Null
if (Test-Path "$Dir\$Log.raw") {
    $b = [IO.File]::ReadAllBytes("$Dir\$Log.raw") | Where-Object { $_ -ne 0 }
    [IO.File]::WriteAllText("$Dir\$Log", [Text.Encoding]::UTF8.GetString([byte[]]$b))
    "log: $Dir\$Log ($((Get-Item "$Dir\$Log").Length) bytes)"
} else { "no log fetched" }
if (-not $NoStart) {
    fb "stage `"$Dir\run-payload.bin`"" 60
    fb "oem run" 15
    "next run started $(Get-Date -Format T)"
}
