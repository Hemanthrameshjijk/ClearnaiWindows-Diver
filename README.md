# ClearNAI (driver edition)

Real-time background voice/noise cancellation delivered as a Windows virtual audio device.
See `docs/ARCHITECTURE.md`. Layout: `driver/` (KMDF WaveRT, SYSVAD fork), `engine/` (Rust),
`installer/` (WiX), `tools/` (benchmark, handle_inspector), `docs/`.

Milestone 1 = passthrough.
- Engine: `cargo run -p clearnai-engine`
- Latency: `cargo run -p clearnai-benchmark -- raw.wav clearnai.wav`

Requires the sibling `../Clearnaiwindows-main` checkout for shared crates.
