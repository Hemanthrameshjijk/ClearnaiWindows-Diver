# Self-signs files with a local code-signing certificate (created on first run).
# Usage:  .\sign-selfsigned.ps1 -Files path\to\ClearNAI-Setup.exe, path\to\clearnairt.exe
# Add -TrustOnThisPc (run as Administrator) to make this PC trust the certificate.
# Other PCs: copy ClearNAI-Test.cer and import it into Trusted Root + Trusted Publishers.
param(
    [Parameter(Mandatory)][string[]]$Files,
    [string]$Subject = 'CN=ClearNAI Test Signing',
    [switch]$TrustOnThisPc
)
$ErrorActionPreference = 'Stop'

$cer = Join-Path $PSScriptRoot 'ClearNAI-Test.cer'
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Where-Object { $_.Subject -eq $Subject -and $_.NotAfter -gt (Get-Date) } |
    Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject `
        -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
        -NotAfter (Get-Date).AddYears(3) -CertStoreLocation Cert:\CurrentUser\My
    Write-Host "Created certificate $($cert.Thumbprint)"
}
Export-Certificate -Cert $cert -FilePath $cer -Force | Out-Null

if ($TrustOnThisPc) {
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Write-Host 'Certificate trusted on this PC.'
}

$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { throw 'signtool.exe not found (install the Windows SDK).' }

foreach ($f in $Files) {
    & $signtool.FullName sign /fd SHA256 /sha1 $cert.Thumbprint /tr http://timestamp.digicert.com /td SHA256 $f
    if ($LASTEXITCODE -ne 0) { throw "Signing failed: $f" }
}
Write-Host "Signed $($Files.Count) file(s). Public cert: $cer"
