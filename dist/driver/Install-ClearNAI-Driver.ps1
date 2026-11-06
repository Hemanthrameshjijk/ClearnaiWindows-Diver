#Requires -RunAsAdministrator
<#
 Installs the ClearNAI virtual microphone driver (TEST-SIGNED build) on this PC.

 READ THIS FIRST
  * The driver is signed with a self-made TEST certificate, not a Microsoft one. Windows only loads such a driver when
    "test mode" is on, which needs Secure Boot OFF (BIOS/UEFI) and, usually, Memory Integrity OFF.
  * This script adds that test certificate to this PC's Trusted Root and Trusted Publishers stores (so Windows accepts the
    driver package). That lowers this PC's security until you run Uninstall-ClearNAI-Driver.ps1.
  * A faulty kernel driver can blue-screen the PC. Use a test machine.
  * Nothing here is run by the ClearNAI installer: you start this script yourself, on purpose.

 Usage (elevated PowerShell):   .\Install-ClearNAI-Driver.ps1            (asks you to type YES)
                                .\Install-ClearNAI-Driver.ps1 -DryRun    (only shows what it would do)
#>
param(
    [switch] $DryRun,
    [string] $DevconPath
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$inf = Join-Path $here 'ComponentizedAudioSample.inf'
$cer = Join-Path $here 'ClearNAI.cer'
foreach ($f in $inf, $cer, (Join-Path $here 'ClearNAI.sys'), (Join-Path $here 'clearnai.cat')) {
    if (-not (Test-Path $f)) { throw "missing driver file: $f" }
}

function Step($text) { Write-Host "  -> $text" }
function Run($text, [scriptblock] $action) { if ($DryRun) { Write-Host "  [dry run] $text" } else { Step $text; & $action } }

# --- state of this PC
$secureBoot = try { [bool](Confirm-SecureBootUEFI) } catch { $false }
$testSigning = [bool]((bcdedit /enum '{current}' 2>$null) -match 'testsigning\s+Yes')
$hvci = $false
try { $hvci = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity' -ErrorAction Stop).Enabled -eq 1 } catch {}
Write-Host ''
Write-Host 'This PC:'
Write-Host ("  Secure Boot ........ {0}" -f $(if ($secureBoot) { 'ON  (blocks test mode)' } else { 'off' }))
Write-Host ("  Test mode .......... {0}" -f $(if ($testSigning) { 'ON' } else { 'OFF (the driver will not load)' }))
Write-Host ("  Memory Integrity ... {0}" -f $(if ($hvci) { 'ON  (may block this driver)' } else { 'off' }))
Write-Host ''
if (-not $testSigning -and -not $DryRun) {
    Write-Host 'Test mode is off, so Windows would refuse to load this driver.'
    Write-Host 'To use it: turn Secure Boot off in the BIOS/UEFI, turn Memory Integrity off (Windows Security > Device security >'
    Write-Host 'Core isolation), run  bcdedit /set testsigning on  and restart. Then run this script again.'
    exit 1
}

# --- devcon (creates the root device the driver attaches to)
if (-not $DevconPath) {
    $DevconPath = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Tools" -Recurse -Filter devcon.exe -ErrorAction SilentlyContinue |
        Where-Object FullName -match '\\x64\\' | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $DevconPath -or -not (Test-Path $DevconPath)) {
    throw 'devcon.exe not found. Install the Windows Driver Kit (it contains devcon) or pass -DevconPath <path to devcon.exe>.'
}
Write-Host "devcon: $DevconPath"

if (-not $DryRun) {
    $answer = Read-Host 'Type YES to trust the ClearNAI test certificate and install the driver'
    if ($answer -cne 'YES') { Write-Host 'Cancelled. Nothing was changed.'; exit 2 }
}

Run 'trust the ClearNAI test certificate (Trusted Root + Trusted Publishers)' {
    certutil -addstore -f Root $cer | Out-Null
    certutil -addstore -f TrustedPublisher $cer | Out-Null
}
Run 'add the driver package to the driver store' { pnputil /add-driver $inf /install }
Run 'create the ClearNAI device (Root\ClearNAI) and bind the driver' { & $DevconPath install $inf 'Root\ClearNAI' }
if (-not $DryRun) {
    Start-Sleep -Seconds 8
    Write-Host ''
    Write-Host 'Audio endpoints from the driver:'
    Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object FriendlyName -match 'ClearNAI' |
        Format-Table FriendlyName, Status -AutoSize
    Write-Host 'Pick "ClearNAI Microphone" as the microphone in your call app; run ClearNAI so it feeds that microphone.'
}

