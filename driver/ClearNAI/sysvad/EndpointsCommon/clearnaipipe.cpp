/*++

ClearNAI engine -> driver audio pipe. See clearnaipipe.h.

Locking: one spin lock guards the ring and its write position. Writers
(the engine's IOCTL) run at PASSIVE_LEVEL, readers (capture stream position
update) at DISPATCH_LEVEL, so everything that touches the ring is non-paged
and takes the lock with KeAcquireSpinLock.

--*/

#include <sysvad.h>
#include <wdmsec.h>
#include "clearnaipipe.h"

#define CLEARNAI_POOLTAG        'ianC'

#define RING_SAMPLES            CLEARNAI_PIPE_RATE          // 1 s of mono PCM16
#define MAX_LATENCY_SAMPLES     (CLEARNAI_PIPE_RATE / 10)   // jump forward past 100 ms of backlog
#define PREFILL_SAMPLES         (CLEARNAI_PIPE_RATE * 3 / 100)  // jitter margin: start readers 30 ms behind the writer
#define ENGINE_TIMEOUT_100NS    (2500000LL)                 // 250 ms without writes = engine gone

static const GUID ClearNaiControlClassGuid =
    { 0x6b1b2f6e, 0x4f13, 0x4b8c, { 0x9d, 0x5a, 0x7c, 0x1e, 0x2a, 0x9f, 0x3d, 0x41 } };

static KSPIN_LOCK       g_PipeLock;
static SHORT           *g_Ring = NULL;
static ULONGLONG        g_WritePos = 0;         // absolute count of source samples written
static LONGLONG         g_LastWriteTime = 0;    // KeQueryInterruptTime of the last write
static PDEVICE_OBJECT   g_ControlDevice = NULL;
static CLEARNAI_PIPE_STATS g_Stats;             // guarded by g_PipeLock
static PDRIVER_DISPATCH g_SavedDispatch[IRP_MJ_MAXIMUM_FUNCTION + 1];

DRIVER_DISPATCH ClearNaiDispatch;

#pragma code_seg()

static NTSTATUS CompleteIrp(_In_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Info)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS HandleWrite(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack)
{
    ULONG bytes = Stack->Parameters.DeviceIoControl.InputBufferLength;
    const SHORT *src = (const SHORT *)Irp->AssociatedIrp.SystemBuffer;

    if (src == NULL || bytes == 0 || (bytes & 1) != 0 || bytes > CLEARNAI_PIPE_MAX_WRITE)
    {
        return STATUS_INVALID_PARAMETER;
    }

    ULONG samples = bytes / sizeof(SHORT);
    KIRQL irql;
    KeAcquireSpinLock(&g_PipeLock, &irql);
    if (g_Ring == NULL)
    {
        KeReleaseSpinLock(&g_PipeLock, irql);
        return STATUS_DEVICE_NOT_READY;
    }
    for (ULONG i = 0; i < samples; i++)
    {
        g_Ring[(g_WritePos + i) % RING_SAMPLES] = src[i];
    }
    g_WritePos += samples;
    {
        LONGLONG now = (LONGLONG)KeQueryInterruptTime();
        if (g_LastWriteTime != 0)
        {
            ULONGLONG gapMs = (ULONGLONG)((now - g_LastWriteTime) / 10000);
            if (gapMs > g_Stats.MaxWriteGapMs)
            {
                g_Stats.MaxWriteGapMs = gapMs;
            }
        }
        g_LastWriteTime = now;
    }
    g_Stats.WriteCalls++;
    g_Stats.SamplesWritten += samples;
    KeReleaseSpinLock(&g_PipeLock, irql);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ClearNaiDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    UCHAR major = stack->MajorFunction;

    if (DeviceObject == g_ControlDevice)
    {
        switch (major)
        {
        case IRP_MJ_CREATE:
        case IRP_MJ_CLEANUP:
        case IRP_MJ_CLOSE:
            return CompleteIrp(Irp, STATUS_SUCCESS, 0);
        case IRP_MJ_DEVICE_CONTROL:
            if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_CLEARNAI_WRITE)
            {
                NTSTATUS st = HandleWrite(Irp, stack);
                return CompleteIrp(Irp, st, NT_SUCCESS(st) ? stack->Parameters.DeviceIoControl.InputBufferLength : 0);
            }
            if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_CLEARNAI_STATS)
            {
                ULONG outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
                if (Irp->AssociatedIrp.SystemBuffer == NULL || outLen < sizeof(CLEARNAI_PIPE_STATS))
                {
                    return CompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
                }
                KIRQL irql;
                KeAcquireSpinLock(&g_PipeLock, &irql);
                *(CLEARNAI_PIPE_STATS *)Irp->AssociatedIrp.SystemBuffer = g_Stats;
                KeReleaseSpinLock(&g_PipeLock, irql);
                return CompleteIrp(Irp, STATUS_SUCCESS, sizeof(CLEARNAI_PIPE_STATS));
            }
            return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        default:
            return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        }
    }

    // Anything else belongs to the PortCls function device: pass it on untouched.
    if (major <= IRP_MJ_MAXIMUM_FUNCTION && g_SavedDispatch[major] != NULL)
    {
        return g_SavedDispatch[major](DeviceObject, Irp);
    }
    return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

VOID ClearNaiPipe_ResetCursor(_Out_ CLEARNAI_PIPE_CURSOR *Cursor)
{
    Cursor->Pos = 0;
    Cursor->Frac = 0;
    Cursor->Started = FALSE;
    Cursor->Rebuffer = FALSE;
}

VOID ClearNaiPipe_ReadFrames(
    _Inout_ CLEARNAI_PIPE_CURSOR *Cursor,
    _Out_writes_bytes_(FrameCount * Channels * (BitsPerSample / 8)) PUCHAR Dst,
    _In_ ULONG FrameCount,
    _In_ ULONG Channels,
    _In_ ULONG BitsPerSample,
    _In_ ULONG SampleRate)
{
    ULONG bytesPerSample = BitsPerSample / 8;
    ULONG frameBytes = Channels * bytesPerSample;
    BOOLEAN supported = (BitsPerSample == 8 || BitsPerSample == 16 || BitsPerSample == 24 || BitsPerSample == 32) &&
                        Channels > 0 && SampleRate > 0;

    // Default: silence. 8-bit PCM is unsigned, so its silence is 0x80.
    if (frameBytes == 0)
    {
        return;
    }
    RtlFillMemory(Dst, (SIZE_T)FrameCount * frameBytes, BitsPerSample == 8 ? 0x80 : 0x00);
    if (!supported)
    {
        return;
    }

    ULONG step = (ULONG)(((ULONGLONG)CLEARNAI_PIPE_RATE << 16) / SampleRate);

    KIRQL irql;
    KeAcquireSpinLock(&g_PipeLock, &irql);

    BOOLEAN live = (g_Ring != NULL) &&
                   ((LONGLONG)KeQueryInterruptTime() - g_LastWriteTime) < ENGINE_TIMEOUT_100NS;
    g_Stats.ReadCalls++;
    if (!live)
    {
        if (Cursor->Started && g_Ring != NULL)
        {
            g_Stats.Timeouts++;
        }
        Cursor->Started = FALSE;
        KeReleaseSpinLock(&g_PipeLock, irql);
        return;
    }

    ULONGLONG wpos = g_WritePos;
    if (!Cursor->Started || Cursor->Pos > wpos || (wpos - Cursor->Pos) > MAX_LATENCY_SAMPLES)
    {
        if (Cursor->Started)
        {
            g_Stats.Jumps++;
        }
        g_Stats.Syncs++;
        Cursor->Pos = (wpos > PREFILL_SAMPLES) ? (wpos - PREFILL_SAMPLES) : 0;
        Cursor->Frac = 0;
        Cursor->Started = TRUE;
        Cursor->Rebuffer = FALSE;
    }

    if (!Cursor->Rebuffer)
    {
        ULONGLONG backlog = wpos - Cursor->Pos;
        if (backlog < g_Stats.MinBacklog)
        {
            g_Stats.MinBacklog = backlog;
        }
        if (backlog > g_Stats.MaxBacklog)
        {
            g_Stats.MaxBacklog = backlog;
        }
    }

    // After running dry, wait until the jitter margin is back instead of trickling samples out as the writer
    // delivers them (which keeps the reader at zero margin and makes every late burst an audible gap).
    if (Cursor->Rebuffer)
    {
        if ((wpos - Cursor->Pos) < PREFILL_SAMPLES)
        {
            KeReleaseSpinLock(&g_PipeLock, irql);
            return;     // silence
        }
        Cursor->Rebuffer = FALSE;
    }

    for (ULONG f = 0; f < FrameCount; f++)
    {
        if (Cursor->Pos >= wpos)
        {
            Cursor->Rebuffer = TRUE;    // underrun: the rest stays silent until the margin is rebuilt
            g_Stats.Underruns++;
            break;
        }

        SHORT s = g_Ring[Cursor->Pos % RING_SAMPLES];
        PUCHAR out = Dst + (SIZE_T)f * frameBytes;
        for (ULONG c = 0; c < Channels; c++)
        {
            switch (BitsPerSample)
            {
            case 8:
                *out = (UCHAR)((s >> 8) + 128);
                break;
            case 16:
                *(SHORT *)out = s;
                break;
            case 24:
                out[0] = 0;
                out[1] = (UCHAR)(s & 0xFF);
                out[2] = (UCHAR)((s >> 8) & 0xFF);
                break;
            case 32:
                *(LONG *)out = ((LONG)s) << 16;
                break;
            }
            out += bytesPerSample;
        }

        Cursor->Frac += step;
        Cursor->Pos += Cursor->Frac >> 16;
        Cursor->Frac &= 0xFFFF;
    }

    KeReleaseSpinLock(&g_PipeLock, irql);
}

#pragma code_seg("PAGE")

NTSTATUS ClearNaiPipe_Init(_In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    NTSTATUS status;
    UNICODE_STRING devName = RTL_CONSTANT_STRING(L"\\Device\\ClearNAIPipe");
    UNICODE_STRING linkName = RTL_CONSTANT_STRING(L"\\DosDevices\\ClearNAIPipe");
    // System and Administrators: all access. Authenticated users: read/write (engine runs unelevated).
    UNICODE_STRING sddl = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)");

    KeInitializeSpinLock(&g_PipeLock);
    g_WritePos = 0;
    g_LastWriteTime = 0;
    RtlZeroMemory(&g_Stats, sizeof(g_Stats));
    g_Stats.MinBacklog = (ULONGLONG)-1;     // "none seen yet"

    g_Ring = (SHORT *)ExAllocatePool2(POOL_FLAG_NON_PAGED, RING_SAMPLES * sizeof(SHORT), CLEARNAI_POOLTAG);
    if (g_Ring == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCreateDeviceSecure(
        DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE,
        &sddl, &ClearNaiControlClassGuid, &g_ControlDevice);
    if (!NT_SUCCESS(status))
    {
        goto Fail;
    }
    g_ControlDevice->Flags |= DO_BUFFERED_IO;

    status = IoCreateSymbolicLink(&linkName, &devName);
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(g_ControlDevice);
        g_ControlDevice = NULL;
        goto Fail;
    }

    // Take over every dispatch entry PortCls installed, except PnP/Power/WMI
    // (which never reach a non-PnP control device), so a stray IRP sent to our
    // device can never reach PortCls with a device object it does not own.
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
    {
        g_SavedDispatch[i] = DriverObject->MajorFunction[i];
        if (i != IRP_MJ_PNP && i != IRP_MJ_POWER && i != IRP_MJ_SYSTEM_CONTROL)
        {
            DriverObject->MajorFunction[i] = ClearNaiDispatch;
        }
    }

    g_ControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;

Fail:
    ExFreePoolWithTag(g_Ring, CLEARNAI_POOLTAG);
    g_Ring = NULL;
    return status;
}

VOID ClearNaiPipe_Cleanup(VOID)
{
    PAGED_CODE();

    UNICODE_STRING linkName = RTL_CONSTANT_STRING(L"\\DosDevices\\ClearNAIPipe");

    if (g_ControlDevice != NULL)
    {
        IoDeleteSymbolicLink(&linkName);
        IoDeleteDevice(g_ControlDevice);
        g_ControlDevice = NULL;
    }

    // Detach the ring under the lock so a late reader sees "engine gone".
    KIRQL irql;
    KeAcquireSpinLock(&g_PipeLock, &irql);
    SHORT *ring = g_Ring;
    g_Ring = NULL;
    KeReleaseSpinLock(&g_PipeLock, irql);
    if (ring != NULL)
    {
        ExFreePoolWithTag(ring, CLEARNAI_POOLTAG);
    }
}
