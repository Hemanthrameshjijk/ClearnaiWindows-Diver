// Unit tests for the driver's pipe logic (clearnaipipe.cpp compiled as a normal program against tools/pipe_unittest/shim).
// Covers what the VM tone tests can't: every sample rate / bit depth / channel count, underrun, engine timeout,
// latency clamp, ring wrap-around, IOCTL validation, dispatch isolation and cleanup.
#include "../../driver/ClearNAI/sysvad/EndpointsCommon/clearnaipipe.cpp"
#include <cstdio>
#include <vector>
#include <algorithm>

LONGLONG g_FakeInterruptTime = 0;
bool g_FailAlloc = false;
int g_DeviceDeleted = 0, g_LinkDeleted = 0;

static int g_checks = 0, g_failed = 0;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failed++; printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static NTSTATUS g_forwardedMajor = -1;
static NTSTATUS Forwarded(PDEVICE_OBJECT, PIRP irp) { g_forwardedMajor = irp->Stack.MajorFunction; return 0x1234; }

static DRIVER_OBJECT g_drv;
static PDEVICE_OBJECT g_foreign;

static NTSTATUS Ioctl(PDEVICE_OBJECT dev, ULONG code, const void* data, ULONG bytes) {
    IRP irp = {};
    irp.Stack.MajorFunction = IRP_MJ_DEVICE_CONTROL;
    irp.Stack.Parameters.DeviceIoControl.IoControlCode = code;
    irp.Stack.Parameters.DeviceIoControl.InputBufferLength = bytes;
    irp.AssociatedIrp.SystemBuffer = const_cast<void*>(data);
    return ClearNaiDispatch(dev, &irp);
}
static NTSTATUS IoctlOut(PDEVICE_OBJECT dev, ULONG code, void* out, ULONG outBytes, ULONG_PTR* info = nullptr) {
    IRP irp = {};
    irp.Stack.MajorFunction = IRP_MJ_DEVICE_CONTROL;
    irp.Stack.Parameters.DeviceIoControl.IoControlCode = code;
    irp.Stack.Parameters.DeviceIoControl.OutputBufferLength = outBytes;
    irp.AssociatedIrp.SystemBuffer = out;
    NTSTATUS st = ClearNaiDispatch(dev, &irp);
    if (info) *info = irp.IoStatus.Information;
    return st;
}
static void WriteRamp(ULONGLONG from, ULONG n) {   // sample value == absolute index, so any read is checkable
    std::vector<SHORT> v(n);
    for (ULONG i = 0; i < n; i++) v[i] = (SHORT)((from + i) & 0x7FFF);
    while (n) {
        ULONG chunk = n > CLEARNAI_PIPE_MAX_WRITE / 2 ? CLEARNAI_PIPE_MAX_WRITE / 2 : n;
        NTSTATUS st = Ioctl(g_ControlDevice, IOCTL_CLEARNAI_WRITE, v.data() + (v.size() - n), chunk * 2);
        CHECK(st == STATUS_SUCCESS, "write failed 0x%lx", (unsigned long)st);
        n -= chunk;
    }
}
static void Tick(LONGLONG ms) { g_FakeInterruptTime += ms * 10000; }

static SHORT Sample16(const UCHAR* p) { return *(const SHORT*)p; }

int main() {
    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) g_drv.MajorFunction[i] = Forwarded;
    g_foreign = (PDEVICE_OBJECT)calloc(1, sizeof(DEVICE_OBJECT));
    g_FakeInterruptTime = 1000000000;

    // ---- init failure paths
    g_FailAlloc = true;
    CHECK(ClearNaiPipe_Init(&g_drv) == STATUS_INSUFFICIENT_RESOURCES, "alloc failure not reported");
    g_FailAlloc = false;
    CHECK(g_drv.MajorFunction[IRP_MJ_CREATE] == Forwarded, "dispatch table touched after failed init");

    // ---- init
    CHECK(ClearNaiPipe_Init(&g_drv) == STATUS_SUCCESS, "init");
    CHECK(g_ControlDevice && !(g_ControlDevice->Flags & DO_DEVICE_INITIALIZING), "device still initializing");
    CHECK(g_ControlDevice->Flags & DO_BUFFERED_IO, "buffered io flag");
    CHECK(g_drv.MajorFunction[IRP_MJ_PNP] == Forwarded && g_drv.MajorFunction[IRP_MJ_POWER] == Forwarded &&
          g_drv.MajorFunction[IRP_MJ_SYSTEM_CONTROL] == Forwarded, "PnP/Power/WMI must be left to PortCls");
    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        if (i == IRP_MJ_PNP || i == IRP_MJ_POWER || i == IRP_MJ_SYSTEM_CONTROL) continue;
        CHECK(g_drv.MajorFunction[i] == ClearNaiDispatch, "major %d not hooked", i);
    }

    // ---- dispatch isolation
    {
        IRP irp = {}; irp.Stack.MajorFunction = IRP_MJ_READ;
        CHECK(ClearNaiDispatch(g_foreign, &irp) == 0x1234 && g_forwardedMajor == IRP_MJ_READ, "foreign device IRP not forwarded");
        irp.Stack.MajorFunction = IRP_MJ_CREATE;
        CHECK(ClearNaiDispatch(g_ControlDevice, &irp) == STATUS_SUCCESS, "create on control device");
        irp.Stack.MajorFunction = IRP_MJ_CLEANUP;
        CHECK(ClearNaiDispatch(g_ControlDevice, &irp) == STATUS_SUCCESS, "cleanup on control device");
        irp.Stack.MajorFunction = IRP_MJ_CLOSE;
        CHECK(ClearNaiDispatch(g_ControlDevice, &irp) == STATUS_SUCCESS, "close on control device");
        g_forwardedMajor = -1;
        irp.Stack.MajorFunction = IRP_MJ_READ;
        CHECK(ClearNaiDispatch(g_ControlDevice, &irp) == STATUS_INVALID_DEVICE_REQUEST && g_forwardedMajor == -1,
              "stray READ on control device must not reach PortCls");
        irp.Stack.MajorFunction = IRP_MJ_WRITE;
        CHECK(ClearNaiDispatch(g_ControlDevice, &irp) == STATUS_INVALID_DEVICE_REQUEST && g_forwardedMajor == -1, "stray WRITE");
    }

    // ---- IOCTL validation
    {
        SHORT buf[CLEARNAI_PIPE_MAX_WRITE / 2 + 8] = {};
        ULONGLONG before = g_WritePos;
        CHECK(Ioctl(g_ControlDevice, IOCTL_CLEARNAI_WRITE, buf, 0) == STATUS_INVALID_PARAMETER, "zero length");
        CHECK(Ioctl(g_ControlDevice, IOCTL_CLEARNAI_WRITE, buf, 3) == STATUS_INVALID_PARAMETER, "odd length");
        CHECK(Ioctl(g_ControlDevice, IOCTL_CLEARNAI_WRITE, buf, CLEARNAI_PIPE_MAX_WRITE + 2) == STATUS_INVALID_PARAMETER, "oversize");
        CHECK(Ioctl(g_ControlDevice, IOCTL_CLEARNAI_WRITE, nullptr, 8) == STATUS_INVALID_PARAMETER, "null buffer");
        CHECK(Ioctl(g_ControlDevice, 0x12345678, buf, 8) == STATUS_INVALID_DEVICE_REQUEST, "unknown ioctl");
        CHECK(Ioctl(g_ControlDevice, IOCTL_CLEARNAI_WRITE, buf, CLEARNAI_PIPE_MAX_WRITE) == STATUS_SUCCESS, "max-size write accepted");
        CHECK(g_WritePos == before + CLEARNAI_PIPE_MAX_WRITE / 2, "bad writes must not advance the ring");
    }

    // ---- silence when no engine has ever written (fresh cursor, stale clock)
    {
        g_WritePos = 0; g_LastWriteTime = 0;
        CLEARNAI_PIPE_CURSOR c; ClearNaiPipe_ResetCursor(&c);
        UCHAR out[480 * 2] ; memset(out, 0xAB, sizeof(out));
        ClearNaiPipe_ReadFrames(&c, out, 480, 1, 16, 48000);
        bool allZero = true; for (UCHAR b : out) allZero &= (b == 0);
        CHECK(allZero && !c.Started, "no engine => silence");
        UCHAR o8[64]; memset(o8, 0, sizeof(o8));
        ClearNaiPipe_ReadFrames(&c, o8, 64, 1, 8, 48000);
        bool all80 = true; for (UCHAR b : o8) all80 &= (b == 0x80);
        CHECK(all80, "8-bit silence must be 0x80");
    }

    // ---- format / rate matrix
    const int rates[] = { 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000 };
    const int bitsList[] = { 8, 16, 24, 32 };
    const int chans[] = { 1, 2, 6 };
    int combos = 0;
    for (int rate : rates) for (int bits : bitsList) for (int ch : chans) {
        combos++;
        g_WritePos = 0; g_LastWriteTime = 0;
        WriteRamp(0, 12000);                      // ramp 0..11999, 250 ms at 48 kHz
        CLEARNAI_PIPE_CURSOR c; ClearNaiPipe_ResetCursor(&c);
        // A reader starts PREFILL_SAMPLES behind the writer, so only that much source is available:
        // ask for the frames it covers at this rate (underrun is tested separately).
        ULONG frames = (ULONG)((ULONGLONG)PREFILL_SAMPLES * rate / 48000) - 2;
        ULONG bps = bits / 8;
        std::vector<UCHAR> out((size_t)frames * ch * bps);
        ClearNaiPipe_ReadFrames(&c, out.data(), frames, ch, bits, rate);
        CHECK(c.Started, "rate %d bits %d ch %d: cursor not started", rate, bits, ch);
        ULONGLONG start = (ULONGLONG)12000 - PREFILL_SAMPLES;
        // Source positions follow floor(k * 48000 / rate) from the starting sample; verify exactly via the same 16.16 step.
        ULONG step = (ULONG)(((ULONGLONG)48000 << 16) / rate);
        ULONGLONG pos = start; ULONG frac = 0;
        bool ok = true; ULONG badFrame = 0;
        for (ULONG f = 0; f < frames && ok; f++) {
            SHORT expect = (SHORT)(pos & 0x7FFF);
            for (int cc = 0; cc < ch && ok; cc++) {
                const UCHAR* p = &out[((size_t)f * ch + cc) * bps];
                long got;
                switch (bits) {
                case 8:  got = ((long)p[0] - 128); ok = (got == (expect >> 8)); break;
                case 16: got = Sample16(p); ok = (got == expect); break;
                case 24: got = (SHORT)(p[1] | (p[2] << 8)); ok = (p[0] == 0 && got == expect); break;
                default: { LONG v = *(const LONG*)p; ok = (v == ((LONG)expect << 16)); break; }
                }
                if (!ok) badFrame = f;
            }
            frac += step; pos += frac >> 16; frac &= 0xFFFF;
        }
        CHECK(ok, "rate %d bits %d ch %d: wrong sample at frame %lu", rate, bits, ch, (unsigned long)badFrame);
        CHECK(c.Pos == pos && c.Frac == frac, "rate %d: cursor %llu.%lu expected %llu.%lu", rate,
              (unsigned long long)c.Pos, (unsigned long)c.Frac, (unsigned long long)pos, (unsigned long)frac);
    }
    printf("format/rate matrix: %d combinations\n", combos);

    // ---- long-run rate accuracy: 10 s at 44.1 kHz must consume 480000 +/- 1 source samples
    {
        g_WritePos = 0; g_LastWriteTime = 0;
        CLEARNAI_PIPE_CURSOR c; ClearNaiPipe_ResetCursor(&c);
        ULONGLONG consumed0 = 0; bool first = true; ULONGLONG total = 0;
        std::vector<UCHAR> out(441 * 2);
        for (int tick = 0; tick < 1000; tick++) {          // 1000 x 10 ms
            WriteRamp(g_WritePos, 480);
            Tick(10);
            g_LastWriteTime = g_FakeInterruptTime;
            ULONGLONG before = c.Pos;
            ClearNaiPipe_ReadFrames(&c, out.data(), 441, 1, 16, 44100);
            if (first) { consumed0 = c.Pos; first = false; } else total += c.Pos - before;
        }
        (void)consumed0;
        CHECK(total >= 479000 && total <= 481000, "10 s @44.1k consumed %llu source samples", (unsigned long long)total);
        CHECK(g_WritePos - c.Pos <= PREFILL_SAMPLES + 480, "steady state backlog %llu", (unsigned long long)(g_WritePos - c.Pos));
    }

    // ---- underrun: ask for more than is available
    {
        g_WritePos = 0; g_LastWriteTime = g_FakeInterruptTime;
        WriteRamp(0, 2000);                                  // reader starts 960 behind => only 960 available
        CLEARNAI_PIPE_CURSOR c; ClearNaiPipe_ResetCursor(&c);
        std::vector<UCHAR> out(2000 * 2, 0xCD);
        ClearNaiPipe_ReadFrames(&c, out.data(), 2000, 1, 16, 48000);
        CHECK(c.Pos == 2000, "underrun: cursor must stop at the write position, got %llu", (unsigned long long)c.Pos);
        bool tailZero = true; for (size_t i = PREFILL_SAMPLES * 2; i < out.size(); i++) tailZero &= (out[i] == 0);
        bool headReal = Sample16(&out[0]) == (SHORT)(2000 - PREFILL_SAMPLES);
        CHECK(tailZero && headReal, "underrun: head real data, tail silence (tailZero=%d headReal=%d)", tailZero, headReal);
        // Rebuffer: a trickle smaller than the jitter margin must NOT be released (silence), and nothing may be skipped.
        WriteRamp(2000, 480);
        UCHAR o2[480 * 2]; memset(o2, 0x99, sizeof(o2));
        ClearNaiPipe_ReadFrames(&c, o2, 480, 1, 16, 48000);
        bool silent = true; for (UCHAR b : o2) silent &= (b == 0);
        CHECK(silent && c.Pos == 2000, "rebuffering: backlog %llu < margin must stay silent (pos %llu)", (unsigned long long)(g_WritePos - c.Pos), (unsigned long long)c.Pos);
        WriteRamp(2480, PREFILL_SAMPLES);                      // margin restored
        ClearNaiPipe_ReadFrames(&c, o2, 480, 1, 16, 48000);
        CHECK(Sample16(&o2[0]) == (SHORT)2000 && Sample16(&o2[479 * 2]) == (SHORT)2479, "resume exactly where it stopped, no samples skipped");
    }

    // ---- jitter simulation: the sender is late by 0-8 ms on every burst (like a Windows timer thread) and stalls 60 ms once
    // (the margin is 30 ms), then resumes on its own schedule. The reader pulls 48 frames every ms.
    // Without rebuffering the reader sits at zero margin after the stall and stutters on every late burst; with it
    // there is one clean gap and the 30 ms margin absorbs the 8 ms jitter afterwards.
    {
        g_WritePos = 0; g_LastWriteTime = g_FakeInterruptTime;
        CLEARNAI_PIPE_CURSOR c; ClearNaiPipe_ResetCursor(&c);
        std::vector<SHORT> outAll; outAll.reserve(48 * 10000);
        ULONGLONG next = 0, dueFrames = 0;
        uint32_t rng = 12345;
        int nextWriteMs = 0;
        for (int ms = 0; ms < 10000; ms++) {
            Tick(1);
            bool stalled = (ms >= 3000 && ms < 3060);
            if (ms % 10 == 0) dueFrames++;                       // one 10 ms frame becomes due every 10 ms
            if (ms >= nextWriteMs && !stalled && dueFrames) {
                std::vector<SHORT> v(480 * dueFrames);
                for (size_t i = 0; i < v.size(); i++) v[i] = (SHORT)(1 + (next++ % 30000));
                for (size_t off = 0; off < v.size(); off += 4800)   // IOCTL limit is 100 ms
                    Ioctl(g_ControlDevice, IOCTL_CLEARNAI_WRITE, v.data() + off, (ULONG)(std::min<size_t>(4800, v.size() - off) * 2));
                g_LastWriteTime = g_FakeInterruptTime;
                dueFrames = 0;
                rng = rng * 1664525u + 1013904223u;
                nextWriteMs = ms + 8 + (int)((rng >> 16) % 9);       // next wake-up: 8..16 ms later
            }
            UCHAR o[48 * 2];
            ClearNaiPipe_ReadFrames(&c, o, 48, 1, 16, 48000);
            for (int i = 0; i < 48; i++) outAll.push_back(Sample16(&o[i * 2]));
        }
        int runs = 0; size_t silentTotal = 0, run = 0;
        for (size_t i = 24000; i < outAll.size(); i++) {         // after the first 500 ms
            if (outAll[i] == 0) { silentTotal++; run++; } else { if (run) runs++; run = 0; }
        }
        if (run) runs++;
        printf("jitter sim: %d silent run(s), %zu silent samples\n", runs, silentTotal);
        CHECK(runs <= 2, "jitter sim: %d separate silent runs (expected one clean gap, got stutter)", runs);
        CHECK(silentTotal < 48 * 120, "jitter sim: %zu silent samples (stall was 60 ms = 2880)", silentTotal);
        CHECK(c.Started && !c.Rebuffer, "jitter sim: reader must end the run playing normally");
    }

    // ---- engine timeout (250 ms without writes)
    {
        g_WritePos = 0; g_LastWriteTime = g_FakeInterruptTime;
        WriteRamp(0, 4800);
        CLEARNAI_PIPE_CURSOR c; ClearNaiPipe_ResetCursor(&c);
        UCHAR o[480 * 2];
        ClearNaiPipe_ReadFrames(&c, o, 480, 1, 16, 48000);
        CHECK(c.Started && Sample16(&o[0]) != 0, "live before timeout");
        Tick(249);
        ClearNaiPipe_ReadFrames(&c, o, 480, 1, 16, 48000);
        CHECK(Sample16(&o[0]) != 0 || Sample16(&o[2]) != 0, "still live at 249 ms");
        Tick(2);
        memset(o, 0xEE, sizeof(o));
        ClearNaiPipe_ReadFrames(&c, o, 480, 1, 16, 48000);
        bool z = true; for (UCHAR b : o) z &= (b == 0);
        CHECK(z && !c.Started, "silence after 250 ms without writes");
        // engine returns: reader re-syncs close to the writer, not 250 ms of stale audio
        WriteRamp(g_WritePos, 4800);
        g_LastWriteTime = g_FakeInterruptTime;
        ClearNaiPipe_ReadFrames(&c, o, 480, 1, 16, 48000);
        CHECK(c.Started && c.Pos == g_WritePos - PREFILL_SAMPLES + 480, "re-sync after engine returns (pos %llu, write %llu)",
              (unsigned long long)c.Pos, (unsigned long long)g_WritePos);
    }

    // ---- latency clamp: reader far behind jumps forward instead of playing stale audio
    {
        g_WritePos = 0; g_LastWriteTime = g_FakeInterruptTime;
        WriteRamp(0, 4800);
        CLEARNAI_PIPE_CURSOR c; ClearNaiPipe_ResetCursor(&c);
        UCHAR o[480 * 2];
        ClearNaiPipe_ReadFrames(&c, o, 480, 1, 16, 48000);
        WriteRamp(g_WritePos, 20000);                          // reader now > 100 ms behind
        g_LastWriteTime = g_FakeInterruptTime;
        ULONGLONG w = g_WritePos;
        ClearNaiPipe_ReadFrames(&c, o, 480, 1, 16, 48000);
        CHECK(Sample16(&o[0]) == (SHORT)((w - PREFILL_SAMPLES) & 0x7FFF), "latency clamp jump target");
    }

    // ---- ring wrap-around (write > 48000 samples in total, reader straddles the boundary)
    {
        g_WritePos = 0; g_LastWriteTime = g_FakeInterruptTime;
        CLEARNAI_PIPE_CURSOR c; ClearNaiPipe_ResetCursor(&c);
        bool ok = true; ULONGLONG lastFirst = 0;
        std::vector<UCHAR> o(480 * 2);
        for (int tick = 0; tick < 400; tick++) {               // 4 s of audio, ring is 1 s
            WriteRamp(g_WritePos, 480);
            g_LastWriteTime = g_FakeInterruptTime;
            ClearNaiPipe_ReadFrames(&c, o.data(), 480, 1, 16, 48000);
            if (tick > 5) {
                for (int i = 1; i < 480; i++) {
                    SHORT prev = Sample16(&o[(i - 1) * 2]), cur = Sample16(&o[i * 2]);
                    if (cur != (SHORT)((prev + 1) & 0x7FFF)) ok = false;
                }
            }
            lastFirst = c.Pos;
        }
        CHECK(ok, "ramp must stay continuous across ring wrap");
        CHECK(lastFirst > RING_SAMPLES * 3, "test actually wrapped the ring several times");
    }

    // ---- unsupported formats stay silent and do not touch the cursor or ring
    {
        g_WritePos = 0; g_LastWriteTime = g_FakeInterruptTime;
        WriteRamp(0, 4800);
        CLEARNAI_PIPE_CURSOR c; ClearNaiPipe_ResetCursor(&c);
        UCHAR o[64]; memset(o, 0x55, sizeof(o));
        ClearNaiPipe_ReadFrames(&c, o, 16, 1, 12, 48000);      // 12-bit: unsupported
        bool z = true; for (UCHAR b : o) z &= (b == 0x55 || b == 0);
        CHECK(!c.Started, "unsupported bit depth must not start the cursor");
        ClearNaiPipe_ReadFrames(&c, o, 16, 0, 16, 48000);      // zero channels
        ClearNaiPipe_ReadFrames(&c, o, 16, 1, 16, 0);          // zero rate
        CHECK(!c.Started, "zero channel/rate must not start the cursor");
        ClearNaiPipe_ReadFrames(&c, o, 0, 1, 16, 48000);       // zero frames
        (void)z;
    }

    // ---- diagnostics counters
    {
        CLEARNAI_PIPE_STATS before = {}, after = {}; ULONG_PTR info = 0;
        CHECK(IoctlOut(g_ControlDevice, IOCTL_CLEARNAI_STATS, &before, sizeof(before) - 1) == STATUS_BUFFER_TOO_SMALL, "stats: short buffer");
        CHECK(IoctlOut(g_ControlDevice, IOCTL_CLEARNAI_STATS, nullptr, sizeof(before)) == STATUS_BUFFER_TOO_SMALL, "stats: null buffer");
        CHECK(IoctlOut(g_ControlDevice, IOCTL_CLEARNAI_STATS, &before, sizeof(before), &info) == STATUS_SUCCESS && info == sizeof(before), "stats: ok");
        // scripted scenario: 2 writes, 1 sync, 1 underrun, 1 jump, 1 timeout
        g_WritePos = 0; g_LastWriteTime = g_FakeInterruptTime;
        WriteRamp(0, 2400);
        Tick(40);
        WriteRamp(g_WritePos, 480);                       // 40 ms gap before this write
        CLEARNAI_PIPE_CURSOR c; ClearNaiPipe_ResetCursor(&c);
        std::vector<UCHAR> out(8000 * 2);
        ClearNaiPipe_ReadFrames(&c, out.data(), 8000, 1, 16, 48000);   // drains the ring => underrun
        WriteRamp(g_WritePos, 20000);                     // reader far behind (or rebuffering) => re-sync
        ClearNaiPipe_ReadFrames(&c, out.data(), 480, 1, 16, 48000);
        ClearNaiPipe_ReadFrames(&c, out.data(), 480, 1, 16, 48000);
        Tick(300);
        ClearNaiPipe_ReadFrames(&c, out.data(), 480, 1, 16, 48000);    // engine silent => timeout
        CHECK(IoctlOut(g_ControlDevice, IOCTL_CLEARNAI_STATS, &after, sizeof(after)) == STATUS_SUCCESS, "stats: read after");
        CHECK(after.Underruns == before.Underruns + 1, "stats: underruns %llu", (unsigned long long)(after.Underruns - before.Underruns));
        CHECK(after.Timeouts == before.Timeouts + 1, "stats: timeouts %llu", (unsigned long long)(after.Timeouts - before.Timeouts));
        CHECK(after.Syncs >= before.Syncs + 1, "stats: syncs");
        CHECK(after.ReadCalls == before.ReadCalls + 4, "stats: reads %llu", (unsigned long long)(after.ReadCalls - before.ReadCalls));
        CHECK(after.WriteCalls >= before.WriteCalls + 3, "stats: write calls");
        CHECK(after.MaxWriteGapMs >= 40, "stats: max write gap %llu ms", (unsigned long long)after.MaxWriteGapMs);
        CHECK(after.MaxBacklog >= 20000 || after.MaxBacklog >= PREFILL_SAMPLES, "stats: max backlog %llu", (unsigned long long)after.MaxBacklog);
        CHECK(after.MinBacklog != (ULONGLONG)-1, "stats: min backlog recorded");
    }

    // ---- cleanup
    {
        int d = g_DeviceDeleted, l = g_LinkDeleted;
        g_LastWriteTime = g_FakeInterruptTime;
        ClearNaiPipe_Cleanup();
        CHECK(g_DeviceDeleted == d + 1 && g_LinkDeleted == l + 1, "cleanup must delete device and link");
        CHECK(g_Ring == nullptr && g_ControlDevice == nullptr, "cleanup must free ring and clear device");
        CLEARNAI_PIPE_CURSOR c; ClearNaiPipe_ResetCursor(&c);
        UCHAR o[64]; memset(o, 0x77, sizeof(o));
        ClearNaiPipe_ReadFrames(&c, o, 32, 1, 16, 48000);      // late reader after unload: silence, no crash
        bool z = true; for (UCHAR b : o) z &= (b == 0);
        CHECK(z, "reader after cleanup gets silence");
        ClearNaiPipe_Cleanup();                                // idempotent
    }

    printf("%d checks, %d failed\n", g_checks, g_failed);
    printf(g_failed ? "UNIT TESTS FAILED\n" : "UNIT TESTS PASSED\n");
    return g_failed ? 1 : 0;
}
