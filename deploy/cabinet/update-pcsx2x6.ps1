<#
  update-pcsx2x6.ps1 - PCSX2x6 (TC3-link) op een Game-INN-kast bijwerken vanaf GitHub-releases.

  Haalt releases/latest/download/version.json van de fork. Is die commit nieuwer dan wat hier staat
  (pcsx2x6.version) en draait de emulator niet, dan wordt pcsx2x6.zip gedownload, de checksum gecontroleerd
  en de programmabestanden overschreven. Instellingen, BIOS, dongles, savestates enz. blijven staan.

  Gebruik (normaal via de geplande taak van setup-cabinet.ps1):
    powershell -NoProfile -ExecutionPolicy Bypass -File update-pcsx2x6.ps1 [-Force]
#>
param(
    [string]$InstallDir = $PSScriptRoot,
    [string]$Repo = "",
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$cfgFile = Join-Path $InstallDir 'pcsx2x6-cabinet.json'
if (-not $Repo -and (Test-Path $cfgFile)) { $Repo = (Get-Content $cfgFile -Raw | ConvertFrom-Json).repo }
if (-not $Repo) { throw "Repo onbekend: geef -Repo eigenaar/pcsx2x6 of draai eerst setup-cabinet.ps1" }

$logDir = Join-Path $InstallDir 'logs'
New-Item -ItemType Directory -Force $logDir | Out-Null
$log = Join-Path $logDir 'pcsx2x6-update.log'
function Log([string]$m) {
    $line = "{0}  {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m
    Add-Content -Path $log -Value $line
    Write-Host $line
}

# Mappen/bestanden met gegevens van deze kast: nooit overschrijven
$keep = @('bios', 'cache', 'cheats', 'covers', 'gamesettings', 'inis', 'inputprofiles', 'logs', 'memcards',
          'snaps', 'sstates', 'textures', 'videos', 'Roms', 'portable.txt', 'pcsx2x6.version', 'pcsx2x6-cabinet.json')

$base = "https://github.com/$Repo/releases/latest/download"
try {
    # GitHub levert release-bestanden als octet-stream: zelf als JSON lezen (PowerShell 5.1)
    $resp = Invoke-WebRequest -Uri "$base/version.json" -UseBasicParsing -TimeoutSec 20 -Headers @{ 'Cache-Control' = 'no-cache' }
    $text = if ($resp.Content -is [byte[]]) { [Text.Encoding]::UTF8.GetString($resp.Content) } else { [string]$resp.Content }
    $rel = $text | ConvertFrom-Json
    if (-not $rel.commit -or -not $rel.sha256) { throw "version.json onvolledig" }
} catch {
    Log "geen verbinding met GitHub ($($_.Exception.Message)) - later opnieuw"
    exit 0
}

$verFile = Join-Path $InstallDir 'pcsx2x6.version'
$have = if (Test-Path $verFile) { (Get-Content $verFile -Raw).Trim() } else { '' }
if (-not $Force -and $have -eq $rel.commit) { exit 0 }

$exePath = Join-Path $InstallDir 'pcsx2-qt.exe'
$running = Get-Process -Name 'pcsx2-qt' -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and ($_.Path -ieq $exePath) }
if ($running) {
    Log "nieuwe versie $($rel.version) klaar, maar de emulator draait - later opnieuw"
    exit 0
}

Log "bijwerken: $(if ($have) { $have.Substring(0, [Math]::Min(9, $have.Length)) } else { '(niets)' }) -> $($rel.version)  $($rel.subject)"
$tmp = Join-Path $env:TEMP ("pcsx2x6-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
    $zip = Join-Path $tmp 'pcsx2x6.zip'
    Invoke-WebRequest -Uri "$base/pcsx2x6.zip" -OutFile $zip -UseBasicParsing -TimeoutSec 900
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $rel.sha256.ToLower()) { throw "checksum klopt niet ($hash)" }

    $x = Join-Path $tmp 'x'
    Expand-Archive -Path $zip -DestinationPath $x -Force
    if (-not (Test-Path (Join-Path $x 'pcsx2-qt.exe'))) { throw "pcsx2-qt.exe ontbreekt in de zip" }

    New-Item -ItemType Directory -Force $InstallDir | Out-Null
    $n = 0
    Get-ChildItem $x | Where-Object { $keep -notcontains $_.Name } | ForEach-Object {
        Copy-Item -Path $_.FullName -Destination $InstallDir -Recurse -Force
        $n++
    }
    Set-Content -Path $verFile -Value $rel.commit -NoNewline
    Log "klaar: $($rel.version) geinstalleerd ($n items)"
} catch {
    Log "MISLUKT: $($_.Exception.Message)"
    exit 1
} finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
