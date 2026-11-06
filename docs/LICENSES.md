# Third-party licenses

| Component | License | Use |
|---|---|---|
| Windows-driver-samples / SYSVAD | MIT | Driver base. Licence text is in `driver/ClearNAI/sysvad/LICENSE`; ship it with the installer in third-party notices |
| ONNX Runtime (`ort` bindings) | MIT (ort: MIT/Apache-2.0) | Inference, later milestone |
| NSIS (installer build tool) | zlib/libpng | Installer; WiX is not used |
| `wasapi` crate | MIT | Capture |
| `windows` crate | MIT/Apache-2.0 | Win32 bindings |
| `rtrb` | MIT/Apache-2.0 | SPSC ring (via dsp-core) |
| `hound` | Apache-2.0 | WAV I/O |
| `anyhow` | MIT/Apache-2.0 | Errors |
| RNNoise / DTLN (candidate prototype models) | BSD-3 / MIT | Verify before bundling |

Regenerate the Rust rows from `Cargo.lock` with `cargo license` before any release.
