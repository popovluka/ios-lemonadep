# Boot the UEFI, upload, start the run, and from that moment keep talking to the loader (every
# $Every seconds, as xl does): the result, and the log so far into $Dir\$Log. Ends when the result
# is no longer "running", or after $Minutes.
param([string]$Dir = "$PSScriptRoot\..\out\hackios", [string]$Log = "run.log", [int]$Every = 2, [int]$Minutes = 5)
$env:PATH = "C:\platform-tools;$env:PATH"
Get-Process fastboot -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

& "$PSScriptRoot\run.ps1" -Dir $Dir | Out-Null
$t0 = Get-Date; $n = 0; $got = 0
while (((Get-Date) - $t0).TotalMinutes -lt $Minutes) {
    $j = Start-Job { cmd /c "fastboot -s XNULOADER getvar result 2>&1" }
    if (Wait-Job $j -Timeout 10) { $r = (Receive-Job $j) -join ' ' } else {
        Stop-Job $j; Get-Process fastboot -ErrorAction SilentlyContinue | Stop-Process -Force; $r = 'no answer' }
    Remove-Job $j -Force
    $s = [int]((Get-Date) - $t0).TotalSeconds
    if ($n++ % 5 -eq 0 -or $r -notmatch 'running') { "t+${s}s $r" }
    if ($r -match 'result:') {
        cmd /c "fastboot -s XNULOADER oem log 2>&1" | Out-Null
        cmd /c "fastboot -s XNULOADER get_staged `"$Dir\$Log.raw`" 2>&1" | Out-Null
        if (Test-Path "$Dir\$Log.raw") {
            $b = [IO.File]::ReadAllBytes("$Dir\$Log.raw") | Where-Object { $_ -ne 0 }
            [IO.File]::WriteAllText("$Dir\$Log", [Text.Encoding]::UTF8.GetString([byte[]]$b)); $got++
        }
        if ($r -notmatch 'running') { "t+${s}s run over; log $((Get-Item "$Dir\$Log").Length) bytes"; break }
    }
    Start-Sleep $Every
}
