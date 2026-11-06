# ClearNAI app <-> ClearNAI driver connection

The driver (this folder's parent project) and the app (`../../Clearnaiwindows-main`) stay separate projects. The only link is
the app sending its final processed mic frames to the driver's pipe `\.\ClearNAIPipe`.

Nothing here is committed. The app change is additive:

| File (in Clearnaiwindows-main) | Change |
|---|---|
| `app/src/driver_pipe.rs` | NEW. `DriverSink`: queue + sender thread that writes frames to the driver pipe. Copy kept here as `driver_pipe.rs.copy`. |
| `app/src/test_input.rs` | NEW. Hidden test microphone (`CLEARNAI_TEST_MIC=tone` or a WAV path) for machines without a mic. Copy: `test_input.rs.copy`. |
| `app/src/lib.rs` | `pub mod driver_pipe;` and `pub mod test_input;` |
| `app/src/engine.rs` | create the sink, pass it to `spawn_mic_pipeline`, push each final frame to it (3 places); use the test microphone instead of opening a physical mic when `CLEARNAI_TEST_MIC` is set |
| `app/Cargo.toml` | extra `windows` features: `Win32_Foundation`, `Win32_Storage_FileSystem`, `Win32_Security`, `Win32_System_IO` |
| `app/installer.nsi` | one new section, "ClearNAI driver package (copied, not installed)": copies `..\..\ClearnaiWindows-Diver\dist\driver\*` to `$INSTDIR\driver` (`/nonfatal`). It never installs the driver or trusts the test certificate. |

The driver package that section copies lives in `../dist/driver` (INF, .sys, .cat, DLL, test .cer, `Install-ClearNAI-Driver.ps1`,
`Uninstall-ClearNAI-Driver.ps1`, `README.txt`). The scripts are run by hand on a test PC.

`driver-connect.patch` is exactly these changes (diff of the files before/after my edit; your other uncommitted edits in those
files are NOT in it).

## Remove the connection (undo)

From `Clearnaiwindows-main`, with Git Bash:

    patch -R -p1 -l < ../ClearnaiWindows-Diver/integration/driver-connect.patch   # undo the 4 edits (dry run with --dry-run first)
    rm app/src/driver_pipe.rs app/src/test_input.rs

(or copy the files from `backup-before-connect/` back over the originals; the backups are the state just before the connection
was added, including your earlier uncommitted work). Do not use `git checkout` on these files: it would also discard your
earlier uncommitted work.

If the driver is not installed the app behaves as before (the sink just drops frames).
