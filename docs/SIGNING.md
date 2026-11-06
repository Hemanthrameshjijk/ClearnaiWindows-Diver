# Driver signing and release checklist

The test build is signed with a self-made certificate and only loads in test mode. A release needs Microsoft signing.

## One-time setup
1. Company or legal entity (required by Partner Center).
2. EV code-signing certificate (about $250-500 a year; check the CA's refund terms before buying).
3. Partner Center account, then enrol in the Windows Hardware Program.
4. Normal Authenticode certificate (the EV one works) for `clearnai.exe`, `clearnai-engine.exe` and the installer.

## Before each submission
- [ ] Build Release x64 (and ARM64 if supported). Output is `ClearNAI.sys`, `clearnai.inf`/`ComponentizedAudioSample.inf`, `clearnai.cat` name in the INF.
- [ ] InfVerif and Code Analysis report no warnings.
- [ ] Test in test mode on a real PC: install, tone round trip, call app, restart, sleep/resume, uninstall.
- [ ] Driver Verifier (standard + low resources) soak, no breaks.
- [ ] Test with Memory Integrity ON (normal PCs have it on).
- [ ] Remove unused sample code and endpoints.
- [ ] Set `DriverVer` (date, version) in the INX for every release. Currently 11/05/2026, 1.0.0.0.

## Attestation submission
1. Folder with only the INF, `ClearNAI.sys` and any DLL. Run `Inf2Cat` for the target OS (e.g. `/os:10_x64`). A stray exe in the folder gave an empty `.cat` earlier.
2. Make a `.cab` of that folder (`makecab`), and sign the CAB with the EV certificate.
3. Partner Center > Hardware > Submit new hardware, upload the CAB, select the OS versions.
4. Download the signed package; it contains the Microsoft-signed `.cat`.
5. Install it on a PC with Secure Boot on and test mode off, and run the real-PC checks again.

## Installer
- Ship the Microsoft-signed package; install it with `pnputil /add-driver clearnai.inf /install` plus device creation (devcon is not redistributable: use `SetupAPI` / `pnputil /add-device` or a small installer step).
- Do not ship the test certificate or `Install-ClearNAI-Driver.ps1`.
- Upgrade and uninstall must remove the old package (`pnputil /delete-driver`).
- Fall back to the VB-CABLE route if the driver is missing or fails to load.
- Include `driver/ClearNAI/sysvad/LICENSE` and the third-party notices.

## Still not done
- Rebuild with the new names and re-test (not built yet after the rename).
- Real INF/hardware ID decision (`Root\ClearNAI` is a root-enumerated test device).
- Speaker path (ClearNAI Speaker is not routed to real speakers).
- Licence audit of the app's dependencies and models.
