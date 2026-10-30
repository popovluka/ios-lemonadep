# Fetch the running kernel's log from the XNU loader every $Every seconds (like xl's XL_FOLLOW),
# into $Dir\live.txt (the latest) and live-N.txt (each one kept).
param([string]$Dir = "$PSScriptRoot\..\out\hackios", [int]$Every = 20, [int]$Minutes = 15)
$env:PATH = "C:\platform-tools;$env:PATH"    # a current fastboot (see run.ps1)

$end = (Get-Date).AddMinutes($Minutes); $n = 0
while ((Get-Date) -lt $end) {
    Start-Sleep $Every; $n++
    $j = Start-Job -ArgumentList "$Dir\live.raw" {
        param($raw)
        cmd /c "fastboot oem log 2>&1" | Out-Null
        cmd /c "fastboot get_staged `"$raw`" 2>&1" | Out-Null
    }
    if (-not (Wait-Job $j -Timeout 40)) { Stop-Job $j; "t+$($n*$Every)s: loader did not answer"; continue }
    if (Test-Path "$Dir\live.raw") {
        $b = [IO.File]::ReadAllBytes("$Dir\live.raw") | Where-Object { $_ -ne 0 }
        $s = [Text.Encoding]::UTF8.GetString([byte[]]$b)
        [IO.File]::WriteAllText("$Dir\live.txt", $s); [IO.File]::WriteAllText("$Dir\live-$n.txt", $s)
        "t+$($n*$Every)s: $($s.Length) bytes"
        Remove-Item "$Dir\live.raw"
    }
}
