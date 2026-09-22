[CmdletBinding()]
param(
    [string]$ImagePath = "",
    [string]$DolphinToolPath = "C:\tools\Dolphin-x64\DolphinTool.exe"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedGameId = "GYWE41"
$ExpectedDolSha1 = "1CCFD9DFB5C250C2F45C70C74CC45E5D88D22374"

function Get-NormalizedPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
}

function Test-PathWithin {
    param(
        [Parameter(Mandatory = $true)][string]$Child,
        [Parameter(Mandatory = $true)][string]$Parent
    )
    $NormalizedChild = Get-NormalizedPath -Path $Child
    $NormalizedParent = Get-NormalizedPath -Path $Parent
    $ParentPrefix = $NormalizedParent + [System.IO.Path]::DirectorySeparatorChar
    return $NormalizedChild.StartsWith(
        $ParentPrefix, [System.StringComparison]::OrdinalIgnoreCase)
}

$ProjectRoot = Get-NormalizedPath -Path (Join-Path $PSScriptRoot "..")
$CmakePath = Join-Path $ProjectRoot "CMakeLists.txt"
$ToolsPath = Join-Path $ProjectRoot "tools"
if (-not (Test-Path -LiteralPath $CmakePath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $ToolsPath -PathType Container)) {
    throw "Unable to verify the project root."
}

$DiscDir = Get-NormalizedPath -Path (Join-Path $ProjectRoot "disc")
$BuildDir = Get-NormalizedPath -Path (Join-Path $ProjectRoot "build")
if (-not (Test-PathWithin -Child $DiscDir -Parent $ProjectRoot) -or
    -not (Test-PathWithin -Child $BuildDir -Parent $ProjectRoot)) {
    throw "Extraction paths escaped the verified project root."
}

if ([string]::IsNullOrWhiteSpace($ImagePath)) {
    $RomDir = Join-Path $ProjectRoot "rom"
    $Candidates = @(
        Get-ChildItem -LiteralPath $RomDir -File -ErrorAction Stop |
            Where-Object { $_.Extension -match "^\.(iso|gcm|rvz|ciso|wbfs)$" }
    )
    if ($Candidates.Count -ne 1) {
        throw "Expected exactly one supported image in rom/. Found $($Candidates.Count); pass -ImagePath explicitly."
    }
    $ImagePath = $Candidates[0].FullName
}

$ResolvedImagePath = (Resolve-Path -LiteralPath $ImagePath).Path
if (-not (Test-Path -LiteralPath $ResolvedImagePath -PathType Leaf)) {
    throw "Image file not found: $ResolvedImagePath"
}
if (-not (Test-Path -LiteralPath $DolphinToolPath -PathType Leaf)) {
    throw "DolphinTool not found: $DolphinToolPath"
}
$ResolvedDolphinTool = (Resolve-Path -LiteralPath $DolphinToolPath).Path

if (Test-Path -LiteralPath $BuildDir) {
    $BuildItem = Get-Item -LiteralPath $BuildDir -Force
    if (($BuildItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing to use a reparse-point build directory for staging."
    }
} else {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

$StagingDir = Get-NormalizedPath -Path (
    Join-Path $BuildDir ("disc-staging-" + [Guid]::NewGuid().ToString("N")))
if (-not (Test-PathWithin -Child $StagingDir -Parent $BuildDir)) {
    throw "The generated staging directory escaped build/."
}
New-Item -ItemType Directory -Path $StagingDir | Out-Null

$Promoted = $false
$BackupDir = $null
try {
    Write-Host "Extracting to isolated staging directory: $StagingDir"
    $ExtractionOutput = @(
        & $ResolvedDolphinTool extract -i $ResolvedImagePath -o $StagingDir -q 2>&1
    )
    $ExtractionExitCode = $LASTEXITCODE
    if ($ExtractionExitCode -ne 0) {
        $OutputText = $ExtractionOutput -join [Environment]::NewLine
        throw (
            "DolphinTool extraction failed with exit code $ExtractionExitCode." +
            [Environment]::NewLine + $OutputText)
    }

    $BootPath = Join-Path $StagingDir "sys\boot.bin"
    $DolPath = Join-Path $StagingDir "sys\main.dol"
    $FilesDir = Join-Path $StagingDir "files"
    if (-not (Test-Path -LiteralPath $BootPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $DolPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $FilesDir -PathType Container)) {
        throw "Staged extraction is incomplete: expected sys/boot.bin, sys/main.dol, and files/."
    }
    if (-not (Get-ChildItem -LiteralPath $FilesDir -Recurse -File |
              Select-Object -First 1)) {
        throw "Staged extraction contains no game files."
    }

    $BootStream = [System.IO.File]::OpenRead($BootPath)
    try {
        $GameIdBytes = New-Object byte[] 6
        if ($BootStream.Read($GameIdBytes, 0, $GameIdBytes.Length) -ne
            $GameIdBytes.Length) {
            throw "Staged boot.bin is too short to contain a GameCube game ID."
        }
    } finally {
        $BootStream.Dispose()
    }
    $GameId = [System.Text.Encoding]::ASCII.GetString($GameIdBytes)
    if ($GameId -ne $ExpectedGameId) {
        throw "Staged game ID '$GameId' does not match required target '$ExpectedGameId'."
    }

    $DolSha1 = (Get-FileHash -LiteralPath $DolPath -Algorithm SHA1).Hash
    if ($DolSha1 -ne $ExpectedDolSha1) {
        throw "Staged main.dol SHA1 '$DolSha1' does not match required target '$ExpectedDolSha1'."
    }
    Write-Host "Staged identity verified: $GameId, main.dol SHA1 $DolSha1"

    if (Test-Path -LiteralPath $DiscDir) {
        $DiscItem = Get-Item -LiteralPath $DiscDir -Force
        if (($DiscItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to replace a reparse-point disc directory."
        }
        $Timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
        $BackupDir = Get-NormalizedPath -Path (
            Join-Path $ProjectRoot ("disc_backup_" + $Timestamp))
        if (Test-Path -LiteralPath $BackupDir) {
            $BackupDir = Get-NormalizedPath -Path (
                Join-Path $ProjectRoot (
                    "disc_backup_" + $Timestamp + "_" +
                    [Guid]::NewGuid().ToString("N").Substring(0, 8)))
        }
        if (-not (Test-PathWithin -Child $BackupDir -Parent $ProjectRoot)) {
            throw "Backup path escaped the verified project root."
        }
        Write-Host "Preserving existing extraction at: $BackupDir"
        Move-Item -LiteralPath $DiscDir -Destination $BackupDir
    }

    try {
        Move-Item -LiteralPath $StagingDir -Destination $DiscDir
        $Promoted = $true
    } catch {
        if ($BackupDir -and
            (Test-Path -LiteralPath $BackupDir) -and
            -not (Test-Path -LiteralPath $DiscDir)) {
            Move-Item -LiteralPath $BackupDir -Destination $DiscDir
        }
        throw
    }

    Write-Host "Verified extraction promoted to: $DiscDir"
    if ($BackupDir) {
        Write-Host "Previous extraction remains recoverable at: $BackupDir"
    }
} finally {
    if (-not $Promoted -and (Test-Path -LiteralPath $StagingDir)) {
        if (-not (Test-PathWithin -Child $StagingDir -Parent $BuildDir)) {
            throw "Refusing to clean an unverified staging path."
        }
        Remove-Item -LiteralPath $StagingDir -Recurse -Force
    }
}
