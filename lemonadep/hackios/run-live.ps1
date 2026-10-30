# A run the way xl does it, from Windows: start it (run.ps1, or straight from the loader with the
# system image already resident: -Resident), ask for the result every 3 s until it is not "running",
# then fetch the whole log (loader + kernel message buffer) into $Dir\$Log. The phone stays in the
# loader. Needs the loader's USB alive during the run (see HANDOFF: which drivers break it).
param([string]$Dir = "$PSScriptRoot\..\out\hackios", [string]$Log = "run.log", [int]$Minutes = 5, [switch]$Resident)
$env:PATH = "C:\platform-tools;$env:PATH"
function fb([string]$a, [int]$t = 8) {
    $j = Start-Job -ArgumentList $a { param($a) cmd /c "fastboot $a 2>&1" }
    if (Wait-Job $j -Timeout $t) { $r = (Receive-Job $j) -join ' ' } else {
        Stop-Job $j; Get-Process fastboot -ErrorAction SilentlyContinue | Stop-Process -Force; $r = 'no answer' }
    Remove-Job $j -Force; return $r
}
Get-Process fastboot -ErrorAction SilentlyContinue | Stop-Process -Force
if ($Resident -and (fb "getvar product") -match 'xnuloader') {
    fb "stage `"$Dir\run-payload.bin`"" 60 | Out-Null
    fb "oem run" 10 | Out-Null
} else {
    & "$PSScriptRoot\run.ps1" -Dir $Dir | Out-Null
}
$t0 = Get-Date; $dead = 0
while (((Get-Date) - $t0).TotalMinutes -lt $Minutes) {
    Start-Sleep 3
    $r = fb "getvar result"
    if ($r -match 'result:' -and $r -notmatch 'running') { "$([int]((Get-Date) - $t0).TotalSeconds)s: $($r -replace ' Finished.*','')"; break }
    if ($r -notmatch 'result:') { if (++$dead -ge 10) { "loader stopped answering"; exit 1 } } else { $dead = 0 }
}
Remove-Item "$Dir\$Log.raw" -ErrorAction SilentlyContinue
fb "oem log" 10 | Out-Null
fb "get_staged `"$Dir\$Log.raw`"" 60 | Out-Null
$b = [IO.File]::ReadAllBytes("$Dir\$Log.raw") | Where-Object { $_ -ne 0 }
[IO.File]::WriteAllText("$Dir\$Log", [Text.Encoding]::UTF8.GetString([byte[]]$b))
"log: $Dir\$Log ($((Get-Item "$Dir\$Log").Length) bytes)"
