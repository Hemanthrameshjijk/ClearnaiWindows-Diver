//! Client for the driver's control device `\\.\ClearNAIPipe` (see
//! `driver/ClearNAI/sysvad/EndpointsCommon/clearnaipipe.h` for the contract).

use std::time::{Duration, Instant};

use windows::core::w;
use windows::Win32::Foundation::{CloseHandle, GENERIC_READ, GENERIC_WRITE, HANDLE};
use windows::Win32::Storage::FileSystem::{
    CreateFileW, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING,
};
use windows::Win32::System::IO::DeviceIoControl;

/// CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA); must match clearnaipipe.h.
const IOCTL_CLEARNAI_WRITE: u32 = (0x22 << 16) | (2 << 14) | (0x800 << 2);

/// CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS); must match clearnaipipe.h.
const IOCTL_CLEARNAI_STATS: u32 = (0x22 << 16) | (0x801 << 2);

/// Mirror of CLEARNAI_PIPE_STATS in clearnaipipe.h (ten u64 counters).
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct PipeStats {
    pub write_calls: u64,
    pub samples_written: u64,
    pub read_calls: u64,
    pub syncs: u64,
    pub underruns: u64,
    pub jumps: u64,
    pub timeouts: u64,
    pub max_write_gap_ms: u64,
    pub min_backlog: u64,
    pub max_backlog: u64,
}

/// Handle to the driver's control device. Reopens itself if the driver is not
/// there yet (or was reinstalled), so the engine can start before the driver.
pub struct DriverPipe {
    handle: Option<HANDLE>,
    last_try: Instant,
}

impl DriverPipe {
    pub fn new() -> Self {
        Self { handle: None, last_try: Instant::now() - Duration::from_secs(10) }
    }

    pub fn connected(&self) -> bool {
        self.handle.is_some()
    }

    fn try_open(&mut self) {
        if self.last_try.elapsed() < Duration::from_secs(1) {
            return;
        }
        self.last_try = Instant::now();
        let h = unsafe {
            CreateFileW(
                w!("\\\\.\\ClearNAIPipe"),
                GENERIC_READ.0 | GENERIC_WRITE.0,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                None,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                None,
            )
        };
        self.handle = h.ok();
    }

    /// Reads the driver's diagnostic counters (None if the driver is unavailable).
    pub fn stats(&mut self) -> Option<PipeStats> {
        if self.handle.is_none() {
            self.last_try = Instant::now() - Duration::from_secs(10);
            self.try_open();
        }
        let h = self.handle?;
        let mut st = PipeStats::default();
        let mut returned = 0u32;
        let ok = unsafe {
            DeviceIoControl(
                h,
                IOCTL_CLEARNAI_STATS,
                None,
                0,
                Some(&mut st as *mut PipeStats as *mut _),
                std::mem::size_of::<PipeStats>() as u32,
                Some(&mut returned),
                None,
            )
        };
        if ok.is_err() || returned as usize != std::mem::size_of::<PipeStats>() { None } else { Some(st) }
    }

    /// Sends mono 48 kHz samples (f32, -1..1) as PCM16. Returns false if the driver is unavailable.
    pub fn write(&mut self, frame: &[f32]) -> bool {
        if self.handle.is_none() {
            self.try_open();
        }
        let Some(h) = self.handle else { return false };
        let pcm: Vec<i16> = frame.iter().map(|s| (s.clamp(-1.0, 1.0) * 32767.0) as i16).collect();
        let mut returned = 0u32;
        let ok = unsafe {
            DeviceIoControl(
                h,
                IOCTL_CLEARNAI_WRITE,
                Some(pcm.as_ptr() as *const _),
                (pcm.len() * 2) as u32,
                None,
                0,
                Some(&mut returned),
                None,
            )
        };
        if ok.is_err() {
            unsafe {
                let _ = CloseHandle(h);
            }
            self.handle = None;
            return false;
        }
        true
    }
}
