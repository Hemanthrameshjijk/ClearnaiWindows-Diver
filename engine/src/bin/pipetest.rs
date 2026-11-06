//! End-to-end driver check, no real microphone needed.
//!
//! Sends a 440 Hz tone through `\\.\ClearNAIPipe` and records the "ClearNAI"
//! capture endpoint through WASAPI at the same time. PASS when the recording
//! is loud enough and its energy is mostly at 440 Hz, i.e. the tone made it
//! engine -> driver -> Windows audio stack -> an app.
//!
//!   pipetest.exe [seconds] [--no-write] [--gap] [--trace] [--rt] [--level]
//!
//!   --no-write  record only (a second recorder next to another instance that writes)
//!   --level     no tone analysis: report how loud / how often active the recording is (speech or anything else)
//!   --rt        writer thread asks for a 1 ms timer and time-critical priority (what a real engine would do)
//!   --trace     also print a 100 ms-window strip chart and count off-level windows (finds dropouts)
//!   --gap       the writer stops for 1.5 s mid-test (engine crash/restart); PASS when the
//!               recording goes silent during the gap and the tone comes back afterwards

#[cfg(windows)]
#[path = "../pipe.rs"]
mod pipe;

#[cfg(windows)]
fn main() -> anyhow::Result<()> {
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::time::{Duration, Instant};

    use audio_io::capture::MicCapture;
    use dsp_core::{make_ring_buffer, FrameCounters, FRAME_SAMPLES, SAMPLE_RATE_HZ};

    // WASAPI device enumeration needs COM on this thread (the capture thread inits its own).
    wasapi::initialize_mta().ok()?;

    const TONE_HZ: f32 = 440.0;
    const GAP_START_MS: u64 = 4000;
    const GAP_END_MS: u64 = 5500;
    let args: Vec<String> = std::env::args().skip(1).collect();
    let no_write = args.iter().any(|a| a == "--no-write");
    let gap = args.iter().any(|a| a == "--gap");
    let rt = args.iter().any(|a| a == "--rt");
    let level = args.iter().any(|a| a == "--level");
    let default_secs = if gap { 10 } else { 5 };
    let seconds: u64 = args.iter().find_map(|a| a.parse().ok()).unwrap_or(default_secs);

    let mut stats_pipe = pipe::DriverPipe::new();
    let before = stats_pipe.stats();

    // Tone writer: one 10 ms frame every 10 ms, phase-continuous.
    let stop = Arc::new(AtomicBool::new(false));
    let writer_stop = stop.clone();
    let writer = std::thread::spawn(move || {
        if no_write {
            return true;
        }
        if rt {
            unsafe {
                windows::Win32::Media::timeBeginPeriod(1);
                let _ = windows::Win32::System::Threading::SetThreadPriority(
                    windows::Win32::System::Threading::GetCurrentThread(),
                    windows::Win32::System::Threading::THREAD_PRIORITY_TIME_CRITICAL,
                );
            }
        }
        let mut p = pipe::DriverPipe::new();
        let mut phase = 0.0f32;
        let step = 2.0 * std::f32::consts::PI * TONE_HZ / SAMPLE_RATE_HZ as f32;
        let mut frame = vec![0.0f32; FRAME_SAMPLES];
        let start = Instant::now();
        let mut n: u64 = 0;
        let mut ever_connected = false;
        while !writer_stop.load(Ordering::Relaxed) {
            for s in frame.iter_mut() {
                *s = 0.3 * phase.sin();
                phase = (phase + step) % (2.0 * std::f32::consts::PI);
            }
            let ms = start.elapsed().as_millis() as u64;
            let in_gap = gap && (GAP_START_MS..GAP_END_MS).contains(&ms);
            if !in_gap && p.write(&frame) {
                ever_connected = true;
            }
            n += 1;
            let due = start + Duration::from_millis(10 * n);
            if let Some(wait) = due.checked_duration_since(Instant::now()) {
                std::thread::sleep(wait);
            }
        }
        ever_connected
    });

    // Recorder: the capture endpoint exposed by the driver.
    let (producer, mut consumer) = make_ring_buffer(2.0);
    let _capture = MicCapture::start_by_name("ClearNAI", producer, Arc::new(FrameCounters::default()))?;
    eprintln!("recording 'ClearNAI' capture endpoint for {seconds}s (no_write={no_write}, gap={gap}, rt={rt})...");

    let mut rec: Vec<f32> = Vec::new();
    let t0 = Instant::now();
    while t0.elapsed() < Duration::from_secs(seconds + 1) {
        while let Ok(s) = consumer.pop() {
            rec.push(s);
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    stop.store(true, Ordering::Relaxed);
    let pipe_ok = writer.join().unwrap_or(false);

    match (before, stats_pipe.stats()) {
        (Some(b), Some(a)) => {
            let min = if a.min_backlog == u64::MAX { "n/a".to_string() } else { format!("{:.1} ms", a.min_backlog as f32 / 48.0) };
            // The driver's gap counter is a lifetime maximum, so a long idle time between two test launches ends up in it.
            let gap = if a.max_write_gap_ms > 60_000 { "n/a (spans sessions)".to_string() } else { format!("{} ms", a.max_write_gap_ms) };
            println!(
                "driver counters (this run): writes {}, reads {}, syncs {}, underruns {}, jumps {}, engine-timeouts {};                  longest gap between writes {}; reader margin min {} / max {:.1} ms (lifetime)",
                a.write_calls - b.write_calls, a.read_calls - b.read_calls, a.syncs - b.syncs, a.underruns - b.underruns,
                a.jumps - b.jumps, a.timeouts - b.timeouts, gap, min, a.max_backlog as f32 / 48.0
            );
        }
        _ => println!("driver counters: unavailable (old driver?)"),
    }
    println!("pipe connected: {pipe_ok}");
    println!("samples recorded: {} ({:.1}s)", rec.len(), rec.len() as f32 / SAMPLE_RATE_HZ as f32);

    if args.iter().any(|a| a == "--trace") {
        print_windows(&rec);
    }
    let pass = if level { analyse_level(&rec) } else if gap { analyse_gap(&rec, pipe_ok) } else { analyse_tone(&rec, pipe_ok, TONE_HZ) };
    println!("{}", if pass { "PASS" } else { "FAIL" });
    std::process::exit(if pass { 0 } else { 1 });
}

/// RMS of the 440 Hz component divided by the total RMS (1.0 = pure tone).
/// Accumulates in f64: the Goertzel recursion in f32 loses precision over millions of samples (a 2-minute recording).
fn tone_share(body: &[f32], tone_hz: f32, rate: f32) -> (f32, f32) {
    let n = body.len().max(1) as f64;
    let rms = (body.iter().map(|&s| (s as f64) * (s as f64)).sum::<f64>() / n).sqrt();
    let (re, im) = goertzel_iq(body, tone_hz, rate);
    // |X| = hypot(re, im); amplitude A = 2|X|/N; rms of that sinusoid = A/sqrt(2).
    let tone_rms = std::f64::consts::SQRT_2 * (re as f64).hypot(im as f64) / n;
    (rms as f32, if rms > 0.0 { (tone_rms / rms) as f32 } else { 0.0 })
}

#[cfg(windows)]
fn analyse_tone(rec: &[f32], pipe_ok: bool, tone_hz: f32) -> bool {
    // Skip the first second (stream start-up / driver pre-fill), analyse the rest.
    let skip = dsp_core::SAMPLE_RATE_HZ as usize;
    let body = if rec.len() > skip { &rec[skip..] } else { rec };
    let rate = dsp_core::SAMPLE_RATE_HZ as f32;
    let (rms, share) = tone_share(body, tone_hz, rate);
    println!("rms: {rms:.4} (sent tone rms ~0.2121)");
    println!("share of energy at {tone_hz} Hz (whole recording): {:.0}%", share * 100.0);
    if body.len() > 20 * dsp_core::SAMPLE_RATE_HZ as usize {
        // A single coherent measurement over minutes is dominated by tiny clock drift, so also judge each 100 ms window.
        let win = dsp_core::SAMPLE_RATE_HZ as usize / 10;
        let (mut good, mut total) = (0usize, 0usize);
        for c in body.chunks_exact(win) {
            let (r, sh) = tone_share(c, tone_hz, rate);
            total += 1;
            if r > 0.15 && r < 0.27 && sh > 0.9 { good += 1; }
        }
        println!("100 ms windows with the right level and >90% tone: {good}/{total}");
        return pipe_ok && rms > 0.05 && good * 100 >= total * 99;
    }
    pipe_ok && rms > 0.05 && share > 0.8
}

/// Strip chart of 100 ms windows after the first second: '#' = level of the sent tone, '.' = silence, '+' = anything else.
#[cfg(windows)]
fn print_windows(rec: &[f32]) {
    let win = dsp_core::SAMPLE_RATE_HZ as usize / 10;
    let rms: Vec<f32> = rec
        .chunks_exact(win)
        .skip(10)
        .map(|c| (c.iter().map(|s| s * s).sum::<f32>() / c.len() as f32).sqrt())
        .collect();
    let line: String = rms.iter().map(|&r| if (r - 0.2121).abs() < 0.02 { '#' } else if r < 0.01 { '.' } else { '+' }).collect();
    let off = line.chars().filter(|&c| c != '#').count();
    println!("trace ({} windows of 100 ms, {} off-level):
{line}", rms.len(), off);
    print_phase(rec, 440.0);
}

/// Complex Goertzel (re, im) of `body` at `tone_hz`, in f64.
fn goertzel_iq(body: &[f32], tone_hz: f32, rate: f32) -> (f32, f32) {
    let w = 2.0 * std::f64::consts::PI * tone_hz as f64 / rate as f64;
    let coeff = 2.0 * w.cos();
    let (mut s1, mut s2) = (0.0f64, 0.0f64);
    for &x in body {
        let s0 = x as f64 + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    ((s1 - s2 * w.cos()) as f32, (s2 * w.sin()) as f32)
}

/// 440 Hz x 0.1 s = 44 whole cycles, so a clean tone has the SAME phase in every 100 ms window. A steady slope in the
/// phase means the sender and the recorder clocks differ (drift); sudden jumps mean audio was skipped or repeated.
#[cfg(windows)]
fn print_phase(rec: &[f32], tone_hz: f32) {
    let rate = dsp_core::SAMPLE_RATE_HZ as f32;
    let win = dsp_core::SAMPLE_RATE_HZ as usize / 10;
    let phases: Vec<f32> = rec
        .chunks_exact(win)
        .skip(10)
        .map(|c| { let (re, im) = goertzel_iq(c, tone_hz, rate); im.atan2(re) })
        .collect();
    let tau = 2.0 * std::f32::consts::PI;
    let deltas: Vec<f32> = phases.windows(2).map(|p| { let mut d = p[1] - p[0]; while d > std::f32::consts::PI { d -= tau; } while d < -std::f32::consts::PI { d += tau; } d }).collect();
    let jumps: Vec<usize> = deltas.iter().enumerate().filter(|(_, d)| d.abs() > 0.5).map(|(i, _)| i).collect();
    let small: Vec<f32> = deltas.iter().copied().filter(|d| d.abs() <= 0.5).collect();
    let mean_small = if small.is_empty() { 0.0 } else { small.iter().sum::<f32>() / small.len() as f32 };
    // dphi per 0.1 s -> frequency offset in Hz, and as parts-per-million of the 48 kHz sample clock
    let hz = mean_small / (tau * 0.1);
    println!("phase: {} windows, {} jumps (>0.5 rad), drift of the rest {:+.5} Hz ({:+.1} ppm of clock)", deltas.len(), jumps.len(), hz, hz / tone_hz * 1e6);
    if !jumps.is_empty() {
        let at: Vec<String> = jumps.iter().take(12).map(|i| format!("{:.1}s", (*i as f32 + 11.0) / 10.0)).collect();
        println!("jumps at (s into recording): {}", at.join(" "));
    }
}

/// For non-tone audio (speech): overall level and how many 100 ms windows are active. PASS if at least a quarter are.
#[cfg(windows)]
fn analyse_level(rec: &[f32]) -> bool {
    let win = dsp_core::SAMPLE_RATE_HZ as usize / 10;
    let skip = 10; // first second
    let rms: Vec<f32> = rec
        .chunks_exact(win)
        .skip(skip)
        .map(|c| (c.iter().map(|s| s * s).sum::<f32>() / c.len() as f32).sqrt())
        .collect();
    let total = rms.len().max(1);
    let active = rms.iter().filter(|&&r| r > 0.01).count();
    let overall = (rms.iter().map(|r| r * r).sum::<f32>() / total as f32).sqrt();
    let peak = rms.iter().cloned().fold(0.0f32, f32::max);
    let line: String = rms.iter().map(|&r| if r > 0.05 { '#' } else if r > 0.01 { '+' } else { '.' }).collect();
    println!("level mode: overall rms {overall:.4}, loudest 100 ms window {peak:.4}, active windows {active}/{total}");
    println!("activity (# loud, + quiet, . silent):
{line}");
    pipe_ok_dummy() && active * 4 >= total
}

#[cfg(windows)]
fn pipe_ok_dummy() -> bool {
    true
}

/// 100 ms RMS windows: expect tone, then a silent stretch (the writer's gap), then tone again.
#[cfg(windows)]
fn analyse_gap(rec: &[f32], pipe_ok: bool) -> bool {
    let win = dsp_core::SAMPLE_RATE_HZ as usize / 10;
    let rms: Vec<f32> = rec
        .chunks_exact(win)
        .map(|c| (c.iter().map(|s| s * s).sum::<f32>() / c.len() as f32).sqrt())
        .collect();
    let line: String = rms.iter().map(|&r| if r > 0.1 { '#' } else if r < 0.01 { '.' } else { '+' }).collect();
    println!("100 ms windows (# tone, . silence, + other):\n{line}");

    // Longest silent run, and whether tone is present both before and after it.
    let (mut best, mut best_start, mut run, mut run_start) = (0usize, 0usize, 0usize, 0usize);
    for (i, &r) in rms.iter().enumerate() {
        if r < 0.01 {
            if run == 0 {
                run_start = i;
            }
            run += 1;
            if run > best {
                best = run;
                best_start = run_start;
            }
        } else {
            run = 0;
        }
    }
    let tone_before = rms[..best_start].iter().any(|&r| r > 0.1);
    let tone_after = rms[(best_start + best).min(rms.len())..].iter().filter(|&&r| r > 0.1).count() >= 10;
    println!("longest silence: {:.1}s; tone before gap: {tone_before}; tone resumed after gap: {tone_after}", best as f32 / 10.0);
    pipe_ok && best >= 8 && tone_before && tone_after
}

#[cfg(not(windows))]
fn main() {
    eprintln!("pipetest only runs on Windows");
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tone(hz: f32, secs: usize, amp: f32) -> Vec<f32> {
        let rate = 48_000.0f64;
        (0..48_000 * secs).map(|i| (amp as f64 * (2.0 * std::f64::consts::PI * hz as f64 * i as f64 / rate).sin()) as f32).collect()
    }

    #[test]
    fn pure_tone_scores_100_percent_even_after_two_minutes() {
        let t = tone(440.0, 120, 0.3);
        let (rms, share) = tone_share(&t, 440.0, 48_000.0);
        assert!((rms - 0.2121).abs() < 0.001, "rms {rms}");
        assert!(share > 0.99, "share {share}");
    }

    #[test]
    fn a_different_tone_scores_near_zero() {
        let t = tone(1000.0, 5, 0.3);
        let (_, share) = tone_share(&t, 440.0, 48_000.0);
        assert!(share < 0.05, "share {share}");
    }

    #[test]
    fn a_phase_jump_hurts_a_long_recording_but_not_each_window() {
        let mut t = tone(440.0, 20, 0.3);
        t.drain(0..7); // drop 7 samples halfway through: a phase jump
        let mut u = tone(440.0, 10, 0.3);
        u.extend(tone(440.0, 10, 0.3).into_iter().skip(7));
        let (_, share_long) = tone_share(&u, 440.0, 48_000.0);
        let (_, share_win) = tone_share(&u[..4800], 440.0, 48_000.0);
        assert!(share_win > 0.99);
        assert!(share_long < share_win && share_long < 0.995, "a phase jump should lower the long measure: {share_long} vs {share_win}");
        let _ = t;
    }
}
