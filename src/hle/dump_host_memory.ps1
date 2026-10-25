# Writes the platform's own memory images for awl-capture-compare:
#   build\host-dumps\at-entry-mem1.raw      memory as the game program starts (first run)
#   build\host-dumps\after-osinit-mem1.raw  memory when OSInit has returned (second run, stopped at that instruction)
#   build\boot-trace.log                    the first run's trace, with the call lines the comparison needs
# Run from the repository root (the executable mounts disc\ relative to it). Nothing here opens a window.
param(
    [string]$Host_ = "build\Release\awl-host.exe",
    [string]$Dol = "rom\main.dol",
    [string]$Disc = "disc",
    [string]$Out = "build\host-dumps",
    [string]$Trace = "build\boot-trace.log"
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $Out | Out-Null
$trace = $Trace

& $Host_ --dol $Dol --disc $Disc --trace $trace --limit 3000000000 --dump-boot "$Out\at-entry-mem1.raw"
if ($LASTEXITCODE -ne 0) { throw "first run failed with exit code $LASTEXITCODE" }

$line = Select-String -Path $trace -Pattern '^boot\.after-osinit icount=(\d+)' | Select-Object -First 1
if (-not $line) { throw "no boot.after-osinit line in $trace (is --calls enabled and OSInit recorded?)" }
$icount = $line.Matches[0].Groups[1].Value

# The second run stops at the instruction count where OSInit returned (exit code 3 = instruction limit is expected).
& $Host_ --dol $Dol --disc $Disc --trace "$Out\after-osinit-trace.log" --limit $icount --dump-mem "$Out\after-osinit-mem1.raw"
if (-not (Test-Path "$Out\after-osinit-mem1.raw")) { throw "second run wrote no memory image" }
Write-Host "wrote $Out\at-entry-mem1.raw and $Out\after-osinit-mem1.raw (OSInit returned at instruction $icount)"
exit 0
