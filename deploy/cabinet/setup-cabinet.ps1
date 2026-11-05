<#
  setup-cabinet.ps1 - een Game-INN TC3-kast-pc eenmalig klaarzetten voor PCSX2x6 met TC3-link (ILinkNet).

  Als administrator in PowerShell, op elke kast-pc:
    iwr -UseBasicParsing https://github.com/<eigenaar>/pcsx2x6/releases/latest/download/setup-cabinet.ps1 -OutFile $env:TEMP\setup-cabinet.ps1
    powershell -ExecutionPolicy Bypass -File $env:TEMP\setup-cabinet.ps1 -Repo <eigenaar>/pcsx2x6 -Side Links -PeerIP <ip van de rechter pc>
    ... en op de andere kast -Side Rechts -PeerIP <ip van de linker pc>

  Wat het doet:
    1. installatiemap (standaard C:\PCSX2X6\pcsx2x6-link) aanmaken, portable, en bios/dongles/instellingen
       overnemen uit de bestaande installatie (-From, standaard C:\PCSX2X6) als die map nieuw is
    2. laatste build van GitHub installeren (update-pcsx2x6.ps1)
    3. PCSX2.ini: [ILinkNet] PeerIP/Port, [JVS] LightgunLinkAs2P (Rechts = 2P), testmenutoetsen
    4. Windows-firewall: UDP 37800 inkomend toelaten voor pcsx2-qt.exe
    5. start-pcsx2x6.cmd: werkt eerst bij en start dan de emulator (laat de snelkoppeling/autostart hierheen wijzen)
#>
param(
    [Parameter(Mandatory = $true)][string]$Repo,
    [Parameter(Mandatory = $true)][ValidateSet('Links', 'Rechts')][string]$Side,
    [Parameter(Mandatory = $true)][string]$PeerIP,
    [string]$InstallDir = 'C:\PCSX2X6\pcsx2x6-link',
    [string]$From = 'C:\PCSX2X6',
    [int]$Port = 37800
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw "Start PowerShell als administrator (nodig voor de firewallregel)." }

$exe = Join-Path $InstallDir 'pcsx2-qt.exe'
if (Get-Process -Name 'pcsx2-qt' -ErrorAction SilentlyContinue) { throw "Sluit eerst PCSX2x6 (pcsx2-qt.exe draait)." }

function Set-IniValue([string]$Path, [string]$Section, [string]$Key, [string]$Value, [switch]$OnlyIfMissing) {
    $lines = [System.Collections.Generic.List[string]]::new()
    if (Test-Path $Path) { [IO.File]::ReadAllLines($Path) | ForEach-Object { $lines.Add($_) } }
    $sec = -1; $end = $lines.Count
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -eq "[$Section]") { $sec = $i; $end = $lines.Count; continue }
        if ($sec -ge 0 -and $t.StartsWith('[') -and $i -gt $sec) { $end = $i; break }
    }
    if ($sec -lt 0) {
        if ($lines.Count -gt 0 -and $lines[$lines.Count - 1].Trim() -ne '') { $lines.Add('') }
        $lines.Add("[$Section]"); $lines.Add("$Key = $Value"); $lines.Add('')
    } else {
        $found = $false
        for ($i = $sec + 1; $i -lt $end; $i++) {
            if ($lines[$i] -match "^\s*$([regex]::Escape($Key))\s*=") {
                if (-not $OnlyIfMissing) { $lines[$i] = "$Key = $Value" }
                $found = $true; break
            }
        }
        if (-not $found) {
            $ins = $end
            while ($ins -gt $sec + 1 -and $lines[$ins - 1].Trim() -eq '') { $ins-- }
            $lines.Insert($ins, "$Key = $Value")
        }
    }
    [IO.File]::WriteAllLines($Path, $lines, [Text.UTF8Encoding]::new($false))
}

Write-Host "== 1. Installatiemap $InstallDir"
$new = -not (Test-Path $exe)
New-Item -ItemType Directory -Force $InstallDir | Out-Null
if ($new -and (Test-Path $From) -and ((Resolve-Path $From).Path -ne (Resolve-Path $InstallDir).Path)) {
    foreach ($d in 'bios', 'memcards', 'inis', 'gamesettings', 'inputprofiles') {
        $s = Join-Path $From $d
        if ((Test-Path $s) -and -not (Test-Path (Join-Path $InstallDir $d))) {
            Copy-Item $s -Destination $InstallDir -Recurse
            Write-Host "   overgenomen: $d"
        }
    }
}
New-Item -ItemType File -Force (Join-Path $InstallDir 'portable.txt') | Out-Null
@{ repo = $Repo; side = $Side } | ConvertTo-Json | Set-Content (Join-Path $InstallDir 'pcsx2x6-cabinet.json')

$upd = Join-Path $InstallDir 'update-pcsx2x6.ps1'
$local = if ($PSScriptRoot) { Join-Path $PSScriptRoot 'update-pcsx2x6.ps1' } else { '' }
if ($local -and (Test-Path $local) -and ($local -ne $upd)) { Copy-Item $local $upd -Force }
else { Invoke-WebRequest "https://github.com/$Repo/releases/latest/download/update-pcsx2x6.ps1" -OutFile $upd -UseBasicParsing }

Write-Host "== 2. Laatste build installeren"
& powershell -NoProfile -ExecutionPolicy Bypass -File $upd -InstallDir $InstallDir -Repo $Repo -Force
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) { throw "installeren mislukt - zie $InstallDir\logs\pcsx2x6-update.log" }

Write-Host "== 3. Instellingen (inis\PCSX2.ini)"
$ini = Join-Path $InstallDir 'inis\PCSX2.ini'
New-Item -ItemType Directory -Force (Split-Path $ini) | Out-Null
Set-IniValue $ini 'ILinkNet' 'Enabled' 'true'
Set-IniValue $ini 'ILinkNet' 'Port' "$Port"
Set-IniValue $ini 'ILinkNet' 'PeerIP' $PeerIP
Set-IniValue $ini 'JVS' 'LightgunLinkAs2P' $(if ($Side -eq 'Rechts') { 'true' } else { 'false' })
Set-IniValue $ini 'JVS' 'P1_Up' 'Keyboard/Up' -OnlyIfMissing
Set-IniValue $ini 'JVS' 'P1_Down' 'Keyboard/Down' -OnlyIfMissing
Set-IniValue $ini 'JVS' 'P1_Button1' 'Keyboard/Return' -OnlyIfMissing
Write-Host "   kant: $Side$(if ($Side -eq 'Rechts') { ' (2P)' } else { ' (1P)' }), andere kast: ${PeerIP}:$Port"

Write-Host "== 4. Firewall"
$rule = 'PCSX2x6 TC3-link (UDP)'
Get-NetFirewallRule -DisplayName $rule -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName $rule -Direction Inbound -Action Allow -Protocol UDP -LocalPort $Port `
    -Program $exe -Profile Any | Out-Null
Write-Host "   inkomend UDP $Port toegelaten voor $exe"

Write-Host "== 5. Startscript"
$start = Join-Path $InstallDir 'start-pcsx2x6.cmd'
@"
@echo off
rem Eerst bijwerken (enkel als er een nieuwe build is), dan PCSX2x6 starten. Argumenten gaan door naar de emulator.
powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0update-pcsx2x6.ps1"
start "" "%~dp0pcsx2-qt.exe" %*
"@ | Set-Content -Path $start -Encoding ASCII
Write-Host "   $start"
Write-Host ""
Write-Host "Klaar. Laat de snelkoppeling/autostart van de kast naar start-pcsx2x6.cmd wijzen."
Write-Host "In het TC3-testmenu (T) op beide kasten: GAME OPTIONS -> LINK : ON."
