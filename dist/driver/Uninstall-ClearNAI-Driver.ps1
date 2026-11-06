#Requires -RunAsAdministrator
<#
 Removes the ClearNAI test driver and the ClearNAI TEST certificate from this PC's trust stores. Run elevated.
   .\Uninstall-ClearNAI-Driver.ps1            removes it
   .\Uninstall-ClearNAI-Driver.ps1 -DryRun    only shows what it would do
 Afterwards you can turn test mode off:  bcdedit /set testsigning off  (and re-enable Secure Boot / Memory Integrity), then restart.
#>
param([switch] $DryRun, [string] $DevconPath)
$ErrorActionPreference = 'Continue'
$thumb = 'FFF1785B7ABC3112EE4B6415D7C516DA90E6FC6D'   # the ClearNAI test certificate (ClearNAI.cer)
function Run($text, [scriptblock] $action) { if ($DryRun) { Write-Host "  [dry run] $text" } else { Write-Host "  -> $text"; & $action } }

if (-not $DevconPath) {
    $DevconPath = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Tools" -Recurse -Filter devcon.exe -ErrorAction SilentlyContinue |
        Where-Object FullName -match '\\x64\\' | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if ($DevconPath -and (Test-Path $DevconPath)) {
    Run 'remove the ClearNAI device' { & $DevconPath remove 'Root\ClearNAI' }
} else {
    Write-Host 'devcon.exe not found: remove the device "ClearNAI Virtual Audio Device" in Device Manager (Sound, video and game controllers) instead.'
}
$oem = (pnputil /enum-drivers | Out-String) -split "(?m)^\s*$" | Where-Object { $_ -match 'componentizedaudiosample' } |
    ForEach-Object { if ($_ -match 'Published Name:\s+(oem\d+\.inf)') { $Matches[1] } }
foreach ($o in $oem) { Run "delete driver package $o" { pnputil /delete-driver $o /uninstall } }
foreach ($store in 'Root', 'TrustedPublisher') {
    Run "remove the ClearNAI test certificate from $store" {
        $c = Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Thumbprint -eq $thumb
        if ($c) { $c | Remove-Item } else { Write-Host '     (not present)' }
    }
}
Write-Host 'Done. Restart Windows to unload the driver completely.'

