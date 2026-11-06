//! Latency harness (clap test).
//!
//! Record the raw mic and the "ClearNAI Microphone" at the same time, clap a few
//! times, then: `clearnai-benchmark raw.wav clearnai.wav`
//!
//! Prints the lag, in ms, that maximises the cross-correlation of the two
//! recordings (positive = ClearNAI mic is later than the raw mic).
//! Both files must share a sample rate; channels are down-mixed to mono.

use anyhow::{bail, Result};

fn load_mono(path: &str) -> Result<(Vec<f32>, u32)> {
    let mut r = hound::WavReader::open(path)?;
    let spec = r.spec();
    let ch = spec.channels as usize;
    let samples: Vec<f32> = match spec.sample_format {
        hound::SampleFormat::Float => r.samples::<f32>().collect::<Result<_, _>>()?,
        hound::SampleFormat::Int => {
            let max = (1i64 << (spec.bits_per_sample - 1)) as f32;
            r.samples::<i32>().map(|s| s.map(|v| v as f32 / max)).collect::<Result<_, _>>()?
        }
    };
    let mono = samples.chunks(ch).map(|c| c.iter().sum::<f32>() / ch as f32).collect();
    Ok((mono, spec.sample_rate))
}

/// Lag (in samples) of `b` relative to `a` within +/- `max_lag`, by direct correlation.
fn best_lag(a: &[f32], b: &[f32], max_lag: i64) -> i64 {
    let mut best = (0i64, f64::MIN);
    for lag in -max_lag..=max_lag {
        let mut acc = 0f64;
        for (i, &x) in a.iter().enumerate() {
            let j = i as i64 + lag;
            if j >= 0 && (j as usize) < b.len() {
                acc += x as f64 * b[j as usize] as f64;
            }
        }
        if acc > best.1 {
            best = (lag, acc);
        }
    }
    best.0
}

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        bail!("usage: clearnai-benchmark <raw.wav> <clearnai.wav>");
    }
    let (a, ra) = load_mono(&args[1])?;
    let (b, rb) = load_mono(&args[2])?;
    if ra != rb {
        bail!("sample rates differ ({ra} vs {rb}); resample first");
    }
    let lag = best_lag(&a, &b, (ra as i64) / 2); // search +/-500 ms
    println!("latency: {:.1} ms ({} samples @ {} Hz)", lag as f64 * 1000.0 / ra as f64, lag, ra);
    Ok(())
}
