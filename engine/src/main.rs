//! ClearNAI engine, milestone 1: raw passthrough.
//!
//! Real mic (WASAPI) -> driver pipe (`\\.\ClearNAIPipe`) -> "ClearNAI Microphone".
//! No DSP/ML yet: the point is to validate the whole pipe end to end. Noise
//! suppression slots in between the ring pop and `DriverPipe::write`.

#[cfg(windows)]
mod pipe;

#[cfg(windows)]
fn main() -> anyhow::Result<()> {
    use std::sync::Arc;
    use std::time::Duration;

    use audio_io::capture::MicCapture;
    use dsp_core::{make_ring_buffer, FrameCounters, FRAME_SAMPLES};

    let (producer, mut consumer) = make_ring_buffer(1.0);
    let counters = Arc::new(FrameCounters::default());
    let _capture = MicCapture::start_default(producer, counters)?;
    let mut driver = pipe::DriverPipe::new();
    eprintln!("clearnai-engine: passthrough running (Ctrl+C to stop)");

    let mut frame = vec![0.0f32; FRAME_SAMPLES];
    let mut was_connected = false;
    loop {
        if consumer.slots() >= FRAME_SAMPLES {
            for s in frame.iter_mut() {
                *s = consumer.pop().unwrap_or(0.0);
            }
            driver.write(&frame); // passthrough: no processing
            if driver.connected() != was_connected {
                was_connected = driver.connected();
                eprintln!("driver pipe {}", if was_connected { "connected" } else { "lost (is the ClearNAI driver installed?)" });
            }
        } else {
            std::thread::sleep(Duration::from_millis(2));
        }
    }
}

#[cfg(not(windows))]
fn main() {
    eprintln!("clearnai-engine only runs on Windows");
}
