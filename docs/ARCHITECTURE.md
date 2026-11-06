# ClearNAI architecture

```
real mic -> [engine: WASAPI capture -> (DSP/ONNX, later) -> DeviceIoControl]
         -> \\.\ClearNAIPipe (driver control device, IOCTL_CLEARNAI_WRITE)
         -> non-paged ring in the driver -> capture miniport -> "ClearNAI Microphone"
app picks "ClearNAI Microphone" as its input.
```

- **Driver**: one KMDF/PortCls device, SYSVAD-derived, two endpoints (Speaker, Microphone). A thin pipe only.
- **Engine**: user mode, so a crash never takes the OS down. Rust: the WASAPI, ring-buffer and
  model code already exists in Rust (`../Clearnaiwindows-main/crates`), Rust gives memory safety in
  the real-time and shared-memory paths, and `ort` provides ONNX Runtime (broader operator coverage
  than tract). The driver stays C: kernel-mode Rust is not production-ready.
- **Wire contract**: PCM16, mono, 48 kHz, up to 100 ms per `IOCTL_CLEARNAI_WRITE` (`driver/ClearNAI/sysvad/EndpointsCommon/clearnaipipe.h`).
  A named shared-memory section was rejected: it is pageable (unsafe to read at DISPATCH_LEVEL), and a Local\ section is
  invisible to the kernel. Each capture stream keeps its own read cursor, converts to the stream format and emits
  silence when the engine stops writing for 250 ms.
- **Milestone 1**: passthrough only. No USB bus layer, no model training, test-signing only.
- **Benchmark**: `tools/benchmark` cross-correlates raw-mic and ClearNAI-mic recordings of a clap.
