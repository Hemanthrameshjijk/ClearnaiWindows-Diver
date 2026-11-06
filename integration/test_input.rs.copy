//! Hidden test microphone, for machines with no microphone (the Hyper-V test VM).
//!
//! Only active when the environment variable `CLEARNAI_TEST_MIC` is set; normal use never sees it.
//!   CLEARNAI_TEST_MIC=tone          a 440 Hz tone
//!   CLEARNAI_TEST_MIC=C:\path.wav   a WAV file (PCM16 or float32, any rate/channels), looped
//! The engine then feeds this instead of opening a physical microphone, so the whole chain
//! (cleanup stages -> ClearNAI driver -> "ClearNAI Microphone") can be exercised without any audio hardware.
//!
//! Additive and removable together with `driver_pipe.rs` (see `ClearnaiWindows-Diver/integration/README.md`).

#![cfg(windows)]

use std::thread;
use std::time::{Duration, Instant};

use dsp_core::{Producer, FRAME_SAMPLES, SAMPLE_RATE_HZ};

pub enum Source {
    Tone,
    Wav(Vec<f32>),
}

/// The configured test source, if `CLEARNAI_TEST_MIC` is set. A bad path or file is reported and ignored.
pub fn from_env() -> Option<Source> {
    let v = std::env::var("CLEARNAI_TEST_MIC").ok()?;
    if v.eq_ignore_ascii_case("tone") {
        return Some(Source::Tone);
    }
    match std::fs::read(&v).map_err(|e| e.to_string()).and_then(|b| wav_to_mono_48k(&b)) {
        Ok(samples) if !samples.is_empty() => Some(Source::Wav(samples)),
        Ok(_) => {
            eprintln!("[clearnai] CLEARNAI_TEST_MIC '{v}' has no audio; using the real microphone.");
            None
        }
        Err(e) => {
            eprintln!("[clearnai] CLEARNAI_TEST_MIC '{v}' unusable ({e}); using the real microphone.");
            None
        }
    }
}

/// Feeds the source into `producer` at real-time speed (one 10 ms frame every 10 ms), forever.
pub fn start(source: Source, mut producer: Producer<f32>) {
    let _ = thread::Builder::new().name("clearnai-test-mic".into()).spawn(move || {
        let step = 2.0 * std::f32::consts::PI * 440.0 / SAMPLE_RATE_HZ as f32;
        let (mut phase, mut pos) = (0.0f32, 0usize);
        let start = Instant::now();
        let mut n: u64 = 0;
        loop {
            if producer.slots() >= FRAME_SAMPLES {
                for _ in 0..FRAME_SAMPLES {
                    let s = match &source {
                        Source::Tone => {
                            let v = 0.3 * phase.sin();
                            phase = (phase + step) % (2.0 * std::f32::consts::PI);
                            v
                        }
                        Source::Wav(w) => {
                            let v = w[pos];
                            pos = (pos + 1) % w.len();
                            v
                        }
                    };
                    let _ = producer.push(s);
                }
            }
            n += 1;
            if let Some(wait) = (start + Duration::from_millis(10 * n)).checked_duration_since(Instant::now()) {
                thread::sleep(wait);
            }
        }
    });
    eprintln!("[clearnai] TEST MICROPHONE active (CLEARNAI_TEST_MIC): no physical microphone is opened.");
}

fn le16(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}
fn le32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

/// Minimal RIFF/WAVE reader: PCM16 or float32 (also inside WAVE_FORMAT_EXTENSIBLE), mixed down to mono, resampled to 48 kHz.
fn wav_to_mono_48k(b: &[u8]) -> Result<Vec<f32>, String> {
    if b.len() < 12 || &b[0..4] != b"RIFF" || &b[8..12] != b"WAVE" {
        return Err("not a RIFF/WAVE file".into());
    }
    let (mut fmt, mut data): (Option<(u16, u16, u32, u16)>, Option<&[u8]>) = (None, None);
    let mut o = 12;
    while o + 8 <= b.len() {
        let id = &b[o..o + 4];
        let size = le32(b, o + 4) as usize;
        let body = &b[(o + 8).min(b.len())..(o + 8 + size).min(b.len())];
        if id == b"fmt " && body.len() >= 16 {
            let mut tag = le16(body, 0);
            if tag == 0xFFFE && body.len() >= 26 {
                tag = le16(body, 24); // sub-format's leading format code
            }
            fmt = Some((tag, le16(body, 2), le32(body, 4), le16(body, 14)));
        } else if id == b"data" {
            data = Some(body);
        }
        o += 8 + size + (size & 1);
    }
    let ((tag, ch, rate, bits), data) = (fmt.ok_or("no fmt chunk")?, data.ok_or("no data chunk")?);
    let ch = ch.max(1) as usize;
    let sample = |i: usize| -> f32 {
        match (tag, bits) {
            (1, 16) => i16::from_le_bytes([data[i * 2], data[i * 2 + 1]]) as f32 / 32768.0,
            (3, 32) => f32::from_le_bytes([data[i * 4], data[i * 4 + 1], data[i * 4 + 2], data[i * 4 + 3]]),
            _ => 0.0,
        }
    };
    if !matches!((tag, bits), (1, 16) | (3, 32)) {
        return Err(format!("unsupported WAV format (tag {tag}, {bits} bit): use PCM16 or float32"));
    }
    let bytes_per = (bits / 8) as usize;
    let frames = data.len() / (bytes_per * ch);
    let mono: Vec<f32> = (0..frames).map(|f| (0..ch).map(|c| sample(f * ch + c)).sum::<f32>() / ch as f32).collect();
    if rate == SAMPLE_RATE_HZ || rate == 0 {
        return Ok(mono);
    }
    // Linear resample to 48 kHz.
    let ratio = rate as f64 / SAMPLE_RATE_HZ as f64;
    let out_len = (mono.len() as f64 / ratio) as usize;
    Ok((0..out_len)
        .map(|i| {
            let p = i as f64 * ratio;
            let (i0, frac) = (p as usize, (p - p.floor()) as f32);
            let a = mono[i0.min(mono.len() - 1)];
            let b = mono[(i0 + 1).min(mono.len() - 1)];
            a + (b - a) * frac
        })
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn wav16(rate: u32, ch: u16, samples: &[i16]) -> Vec<u8> {
        let data: Vec<u8> = samples.iter().flat_map(|s| s.to_le_bytes()).collect();
        let mut v = b"RIFF".to_vec();
        v.extend((36 + data.len() as u32).to_le_bytes());
        v.extend(b"WAVEfmt ");
        v.extend(16u32.to_le_bytes());
        v.extend(1u16.to_le_bytes());
        v.extend(ch.to_le_bytes());
        v.extend(rate.to_le_bytes());
        v.extend((rate * ch as u32 * 2).to_le_bytes());
        v.extend((ch * 2).to_le_bytes());
        v.extend(16u16.to_le_bytes());
        v.extend(b"data");
        v.extend((data.len() as u32).to_le_bytes());
        v.extend(data);
        v
    }

    #[test]
    fn mono_48k_is_passed_through() {
        let out = wav_to_mono_48k(&wav16(48_000, 1, &[0, 16384, -16384, 32767])).unwrap();
        assert_eq!(out.len(), 4);
        assert!((out[1] - 0.5).abs() < 1e-4 && (out[2] + 0.5).abs() < 1e-4);
    }

    #[test]
    fn stereo_is_mixed_down() {
        let out = wav16(48_000, 2, &[16384, -16384, 32767, 32767]);
        let m = wav_to_mono_48k(&out).unwrap();
        assert_eq!(m.len(), 2);
        assert!(m[0].abs() < 1e-4 && m[1] > 0.99);
    }

    #[test]
    fn sixteen_khz_is_resampled_to_three_times_the_length() {
        let src: Vec<i16> = (0..1600).map(|i| ((i as f32 * 0.1).sin() * 10000.0) as i16).collect();
        let out = wav_to_mono_48k(&wav16(16_000, 1, &src)).unwrap();
        assert!((out.len() as i64 - 4800).abs() <= 3, "len {}", out.len());
    }

    #[test]
    fn garbage_is_rejected() {
        assert!(wav_to_mono_48k(b"not a wav file at all").is_err());
    }
}
