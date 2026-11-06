// Minimal stand-ins for the kernel APIs used by clearnaipipe.cpp, so its logic can be unit-tested as a normal program.
// Test only: none of this is part of the driver.
#pragma once
#include <windows.h>
#include <winioctl.h>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#ifndef _In_
#define _In_
#define _Out_
#define _Inout_
#define _Out_writes_bytes_(x)
#define _Use_decl_annotations_
#endif
#define PAGED_CODE()

typedef LONG NTSTATUS;
typedef UCHAR KIRQL;
typedef ULONG_PTR KSPIN_LOCK;
#ifndef NT_SUCCESS
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#endif
#undef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000)
#undef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#undef STATUS_DEVICE_NOT_READY
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3)
#undef STATUS_INVALID_DEVICE_REQUEST
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010)
#undef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023)
#undef STATUS_INSUFFICIENT_RESOURCES
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009A)

#define IRP_MJ_CREATE 0x00
#define IRP_MJ_CLOSE 0x02
#define IRP_MJ_READ 0x03
#define IRP_MJ_WRITE 0x04
#define IRP_MJ_DEVICE_CONTROL 0x0e
#define IRP_MJ_POWER 0x16
#define IRP_MJ_SYSTEM_CONTROL 0x17
#define IRP_MJ_CLEANUP 0x12
#define IRP_MJ_PNP 0x1b
#define IRP_MJ_MAXIMUM_FUNCTION 0x1b
#define DO_BUFFERED_IO 0x4
#define DO_DEVICE_INITIALIZING 0x80
#define IO_NO_INCREMENT 0
#define FILE_DEVICE_SECURE_OPEN 0x100
#define POOL_FLAG_NON_PAGED 0x40

struct UNICODE_STRING_K { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };
#define UNICODE_STRING UNICODE_STRING_K
#define PUNICODE_STRING UNICODE_STRING_K*
#define RTL_CONSTANT_STRING(s) { (USHORT)(sizeof(s) - sizeof(WCHAR)), (USHORT)sizeof(s), (PWSTR)(s) }

struct DEVICE_OBJECT { ULONG Flags; };
typedef DEVICE_OBJECT* PDEVICE_OBJECT;
struct IO_STACK_LOCATION {
    UCHAR MajorFunction;
    union { struct { ULONG InputBufferLength; ULONG OutputBufferLength; ULONG IoControlCode; } DeviceIoControl; } Parameters;
};
typedef IO_STACK_LOCATION* PIO_STACK_LOCATION;
struct IRP {
    struct { NTSTATUS Status; ULONG_PTR Information; } IoStatus;
    struct { void* SystemBuffer; } AssociatedIrp;
    IO_STACK_LOCATION Stack;
};
typedef IRP* PIRP;
typedef NTSTATUS DRIVER_DISPATCH(PDEVICE_OBJECT, PIRP);
typedef DRIVER_DISPATCH* PDRIVER_DISPATCH;
struct DRIVER_OBJECT { PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1]; };
typedef DRIVER_OBJECT* PDRIVER_OBJECT;

// Controllable fake clock (100 ns units) and fault injection, used by the tests.
extern LONGLONG g_FakeInterruptTime;
extern bool g_FailAlloc;
extern int g_DeviceDeleted, g_LinkDeleted;
static inline ULONGLONG KeQueryInterruptTime() { return (ULONGLONG)g_FakeInterruptTime; }

static inline void KeInitializeSpinLock(KSPIN_LOCK* l) { *l = 0; }
static inline void KeAcquireSpinLock(KSPIN_LOCK*, KIRQL* irql) { *irql = 0; }
static inline void KeReleaseSpinLock(KSPIN_LOCK*, KIRQL) {}
static inline void* ExAllocatePool2(ULONG, SIZE_T size, ULONG) { return g_FailAlloc ? nullptr : calloc(1, size); }
static inline void ExFreePoolWithTag(void* p, ULONG) { free(p); }
static inline PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP irp) { return &irp->Stack; }
static inline void IoCompleteRequest(PIRP, int) {}
static inline NTSTATUS IoCreateDeviceSecure(PDRIVER_OBJECT, ULONG, UNICODE_STRING*, ULONG, ULONG, BOOLEAN,
                                            UNICODE_STRING*, const GUID*, PDEVICE_OBJECT* out) {
    *out = (PDEVICE_OBJECT)calloc(1, sizeof(DEVICE_OBJECT));
    (*out)->Flags = DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
static inline NTSTATUS IoCreateSymbolicLink(UNICODE_STRING*, UNICODE_STRING*) { return STATUS_SUCCESS; }
static inline void IoDeleteSymbolicLink(UNICODE_STRING*) { g_LinkDeleted++; }
static inline void IoDeleteDevice(PDEVICE_OBJECT d) { g_DeviceDeleted++; free(d); }
