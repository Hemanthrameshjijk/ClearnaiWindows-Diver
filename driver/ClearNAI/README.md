# ClearNAI driver

`sysvad/` is Microsoft's SYSVAD sample (MIT, github.com/microsoft/Windows-driver-samples, audio/sysvad),
trimmed to one speaker and one microphone endpoint, with these ClearNAI changes:

- `TabletAudioSample/minipairs.h`: only `SpeakerMiniports` and `MicInMiniports` are registered.
- `TabletAudioSample/ComponentizedAudioSample.inx`: hardware ID `Root\ClearNAI`, provider/names "ClearNAI Speaker", "ClearNAI Microphone".
- `EndpointsCommon/clearnaipipe.{h,cpp}` (new): control device `\\.\ClearNAIPipe` plus a non-paged ring.
- `EndpointsCommon/minwavertstream.cpp`: capture `WriteBytes` reads the pipe instead of generating a sine.
- `adapter.cpp`: creates/removes the pipe in `DriverEntry`/`DriverUnload`.

Output is now named `ClearNAI.sys` (renamed in source, not yet rebuilt or re-tested). `dist/driver` and `tools/vm` still use the old `TabletAudioSample.sys` names until the next build. See `docs/SIGNING.md`.

## Build (VS 2026 + WDK 28000.2526 + SDK 10.0.28000)

Copy `sysvad/` to a short path (e.g. `C:\b\sysvad`; the tracking files break past MAX_PATH), then with the 64-bit MSBuild
(`C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe`):

```
msbuild EndpointsCommon\EndpointsCommon.vcxproj     /p:Configuration=Debug /p:Platform=x64
msbuild TabletAudioSample\TabletAudioSample.vcxproj /p:Configuration=Debug /p:Platform=x64
```

## Install (VM only, test-signing on)

`bcdedit /set testsigning on`, reboot, then in an admin shell from the build output folder:
`pnputil /add-driver ComponentizedAudioSample.inf /install` and create the device with
`devcon install ComponentizedAudioSample.inf Root\ClearNAI`. Then run `clearnai-engine`.

Status: builds, installs and loads in a Hyper-V VM (`tools/vm`), both endpoints appear, and a 440 Hz tone sent through
`\\.\ClearNAIPipe` is recorded back from "ClearNAI Microphone" at the right level with ~100% of its energy at 440 Hz
(`engine/src/bin/pipetest.rs`). Also verified in the VM: two apps recording at once both get a clean tone, and when the engine stops
writing for 1.5 s the microphone goes silent and the tone resumes afterwards. Scenarios: the local test-VM scripts (not in this repo).
All three scenarios, plus a device-remove / package-delete / reinstall cycle, also pass with Driver Verifier (standard flags +
special pool) enabled on the driver, with no bugchecks (the local test-VM scripts (not in this repo), `reinstall-in-vm.ps1`, `unload-in-vm.ps1`).
Soak (2 min, 1198 windows of 100 ms): every window at the right level with a clean tone, 0 phase jumps, clock drift -0.4 ppm
(`run-pipetest.ps1 -Scenario soak|soakrt`). Earlier runs showed glitches in the first seconds; the driver's own counters
(`IOCTL_CLEARNAI_STATS`, printed by pipetest) showed an 84 ms stall of the *test writer*, which the driver handled as designed
(underrun -> rebuffer). The driver now keeps a 30 ms jitter margin and, after running dry, outputs silence until it is rebuilt.

Logic tests that need no VM: `tools/pipe_unittest` compiles `clearnaipipe.cpp` against kernel stand-ins and checks 2074 cases
(8 sample rates x 4 bit depths x 3 channel counts, underrun/rebuffer, engine timeout, latency clamp, ring wrap, IOCTL validation,
dispatch isolation, cleanup, counters): `g++ -std=c++17 -Wno-unknown-pragmas -Wno-multichar -Ishim test.cpp && ./a.exe`.

Final VM run on the rebuffering driver with counters (Driver Verifier on): 2-minute soak 1199/1199 clean windows, 0 jumps,
whole-recording tone share 100%; two apps both 100%; engine-restart gap: 1 engine-timeout counted, silence 1.4 s, tone resumed.

Not yet tested: a real softphone, recording below 48 kHz in a real app (only the unit tests cover
resampling), the real microphone through `clearnai-engine`, and the driver's actual unload path (`DriverUnload` /
`ClearNaiPipe_Cleanup`): the service stays RUNNING after the device is removed and `sc stop` did not stop it, so unload only
happens at reboot/shutdown and has not been observed. The `MaxWriteGapMs` counter is a lifetime maximum, so it includes idle time
between test launches (pipetest shows n/a above 60 s).

Note: the INF needs a signed catalog, which the Debug build doesn't produce. Generate it with
`Inf2Cat /driver:<pkg> /os:10_X64` and sign the .cat with the test certificate (see the local test-VM scripts (not in this repo) for the VM flow).
Run Inf2Cat on a folder holding only the INF, .sys and DLL: extra files (test programs) make it fail with an empty .cat.
