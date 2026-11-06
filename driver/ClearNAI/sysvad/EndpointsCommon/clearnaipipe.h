/*++

ClearNAI engine -> driver audio pipe.

The user-mode engine opens \\.\ClearNAIPipe and writes PCM16 / mono / 48 kHz
with IOCTL_CLEARNAI_WRITE. The samples land in a non-paged ring; each capture
stream of "ClearNAI Microphone" reads from it (own cursor, so several apps can
record at once). When the engine is absent the capture stream gets silence.

--*/

#ifndef _CLEARNAI_PIPE_H_
#define _CLEARNAI_PIPE_H_

#include <ntddk.h>

#define CLEARNAI_PIPE_RATE          48000

// Max bytes accepted per IOCTL (100 ms of PCM16 mono).
#define CLEARNAI_PIPE_MAX_WRITE     (CLEARNAI_PIPE_RATE / 10 * 2)

#define IOCTL_CLEARNAI_WRITE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA)

// Diagnostics, readable with IOCTL_CLEARNAI_STATS (counters run from driver load; take a before/after delta).
#define IOCTL_CLEARNAI_STATS     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _CLEARNAI_PIPE_STATS
{
    ULONGLONG   WriteCalls;         // accepted IOCTL_CLEARNAI_WRITE calls
    ULONGLONG   SamplesWritten;     // source samples accepted
    ULONGLONG   ReadCalls;          // capture-stream reads (one per DMA position update)
    ULONGLONG   Syncs;              // a reader (re)started at the jitter margin
    ULONGLONG   Underruns;          // a read ran out of data
    ULONGLONG   Jumps;              // a playing reader was too far behind and skipped ahead
    ULONGLONG   Timeouts;           // a playing reader found the engine silent for 250 ms
    ULONGLONG   MaxWriteGapMs;      // longest pause between two writes
    ULONGLONG   MinBacklog;         // smallest queued margin seen by a playing reader, in samples (all-ones = none)
    ULONGLONG   MaxBacklog;         // largest queued margin seen by a playing reader, in samples
} CLEARNAI_PIPE_STATS;

// Per capture-stream read position, in 48 kHz source samples.
typedef struct _CLEARNAI_PIPE_CURSOR
{
    ULONGLONG   Pos;        // next source sample to consume
    ULONG       Frac;       // 16.16 fractional position between source samples
    BOOLEAN     Started;
    BOOLEAN     Rebuffer;   // ran dry: output silence until PREFILL samples are queued again
} CLEARNAI_PIPE_CURSOR;

#ifdef __cplusplus
extern "C" {
#endif

// Creates the ring and the control device, and hooks the driver's IRP table.
// Must be called after PcInitializeAdapterDriver.
NTSTATUS ClearNaiPipe_Init(_In_ PDRIVER_OBJECT DriverObject);

VOID ClearNaiPipe_Cleanup(VOID);

VOID ClearNaiPipe_ResetCursor(_Out_ CLEARNAI_PIPE_CURSOR *Cursor);

// Fills Dst with FrameCount frames in the stream's format (PCM, 16/24/32 bit
// container, any channel count, any rate; nearest-sample resampling).
// Writes silence when the engine is not feeding the pipe. IRQL <= DISPATCH.
VOID ClearNaiPipe_ReadFrames(
    _Inout_ CLEARNAI_PIPE_CURSOR *Cursor,
    _Out_writes_bytes_(FrameCount * Channels * (BitsPerSample / 8)) PUCHAR Dst,
    _In_ ULONG FrameCount,
    _In_ ULONG Channels,
    _In_ ULONG BitsPerSample,
    _In_ ULONG SampleRate);

#ifdef __cplusplus
}
#endif

#endif // _CLEARNAI_PIPE_H_
