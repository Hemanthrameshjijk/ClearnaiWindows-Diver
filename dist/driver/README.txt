ClearNAI virtual microphone driver - TEST build (not installed by the ClearNAI installer)

What it is: a virtual audio device with two endpoints, "ClearNAI Microphone" and a speaker. ClearNAI sends its cleaned microphone
audio to it, and other apps pick "ClearNAI Microphone" as their microphone. If it is not installed, ClearNAI works as before
(it uses the VB-CABLE route).

Why it is not installed automatically: it is signed with a self-made test certificate. Windows only loads such a driver in "test
mode" (Secure Boot off, usually Memory Integrity off), and installing it means trusting that certificate on the PC. Use a test PC.

To install:   open PowerShell as Administrator in this folder, then   .\Install-ClearNAI-Driver.ps1     (add -DryRun to preview)
To remove:    .\Uninstall-ClearNAI-Driver.ps1
Needs devcon.exe from the Windows Driver Kit (the script finds it, or pass -DevconPath).

For a driver that loads on normal PCs it must be signed through Microsoft (attestation signing): not done.
