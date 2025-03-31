#include <acpi.h>
#include <kernel_api.h>
#include <uacpi/uacpi.h>
#include <uacpi/event.h>


#include <initguid.h>
#include <ntddk.h>
#include <ntifs.h>
#include <mountdev.h>
#include <mountmgr.h>
#include <ketypes.h>
#include <iotypes.h>
#include <rtlfuncs.h>
#include <arc/arc.h>
//#define NDEBUG
#include <debug.h>

typedef struct _UACPI_ALLOCATION
{
   PHYSICAL_ADDRESS PhyAddress;
   PVOID            VirtAddress;
   SIZE_T           Size;
} UACPI_ALLOCATION, *PUACPI_ALLOCATION;

/*
 * (semaphore-like) event object.
 */
typedef struct _ACPI_SEM {
    KEVENT Event;
    KSPIN_LOCK Lock;
} ACPI_SEM, *PACPI_SEM;

UINT32
ACPIInitUACPI()
{
    DPRINT1("Starting up uACPI...\n");
    uacpi_status status = uacpi_initialize(0);
    if (uacpi_unlikely_error(status)) {
        DPRINT1("uacpi_initialize error: %s\n", uacpi_status_to_string(status));
    }
    DPRINT1("UACPI Initial Initialization Successful\n");
    // load the acpi namespace
    status = uacpi_namespace_load();
    if (uacpi_unlikely_error(status)) {
        DPRINT1("uacpi_namespace_load error: %s\n", uacpi_status_to_string(status));
    }

    // initialize the namespace
    status = uacpi_namespace_initialize();
    if (uacpi_unlikely_error(status)) {
        DPRINT1("uacpi_namespace_initialize error: %s\n", uacpi_status_to_string(status));
    }

    // initialize the namespace
    status = uacpi_finalize_gpe_initialization();
    if (uacpi_unlikely_error(status)) {
        DPRINT1("uACPI GPE initialization error: %s\n", uacpi_status_to_string(status));
    }

    DPRINT1("Finish GPE init\n");
    return 0;
}


//TODO:
#ifndef UACPI_FORMATTED_LOGGING
void uacpi_kernel_log(uacpi_log_level Level, const uacpi_char* Char)
{
    DPRINT1("uACPI: %s", Char);
}
#else
UACPI_PRINTF_DECL(2, 3)
void uacpi_kernel_log(uacpi_log_level Level, const uacpi_char* Char, ...)
{
    DPRINT1("uACPI: %s", Char);
}
void uacpi_kernel_vlog(uacpi_log_level Level, const uacpi_char* Char, uacpi_va_list list)
{
    DPRINT1("uACPI: %s", Char);
}
#endif

uacpi_u64
uacpi_kernel_get_nanoseconds_since_boot(void)
{
    /*  The number of milliseconds that have elapsed since the system was started. */
    LARGE_INTEGER PerfFrequency, PerformanceCounter;
    PerformanceCounter = KeQueryPerformanceCounter(&PerfFrequency);
    uacpi_u64 MiliSecSinceBoot = (uacpi_u64)(PerformanceCounter.QuadPart * 1000) / PerfFrequency.QuadPart;

    /* Now let's return nanoseconds. */
    return (MiliSecSinceBoot * 1000000);
}

void
uacpi_kernel_stall(uacpi_u8 usec)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -10 * usec; // Convert microseconds to 100-nanosecond intervals
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

void uacpi_kernel_sleep(uacpi_u64 msec)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -10 * 1000 * msec; // Convert milliseconds to 100-nanosecond intervals
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

uacpi_handle
uacpi_kernel_create_event(void)
{
    PACPI_SEM Sem;
    DPRINT("uacpi_kernel_create_event: enter\n");
    Sem = ExAllocatePoolWithTag(NonPagedPool, sizeof(ACPI_SEM), 'LpcA');
    ASSERT(Sem);

    KeInitializeEvent(&Sem->Event, SynchronizationEvent, FALSE);
    KeInitializeSpinLock(&Sem->Lock);

    return (uacpi_handle)Sem;
}

void
uacpi_kernel_free_event(uacpi_handle Handle)
{
    DPRINT1("uacpi_kernel_free_event: enter\n");
    if (Handle)
        ExFreePoolWithTag(Handle, 'LpcA');
}

uacpi_bool
uacpi_kernel_wait_for_event(uacpi_handle Handle, uacpi_u16 Timeout)
{
    PACPI_SEM Sem = Handle;
    KIRQL OldIrql;
    LARGE_INTEGER TimeoutNT;

    TimeoutNT.QuadPart = Timeout * 10000;
    KeWaitForSingleObject(&Sem->Event,
                              Executive,
                              KernelMode,
                              FALSE,
                              &TimeoutNT);
    KeAcquireSpinLock(&Sem->Lock, &OldIrql);
    KeSetEvent(&Sem->Event, IO_NO_INCREMENT, FALSE);

    KeReleaseSpinLock(&Sem->Lock, OldIrql);
    return TRUE;
}


void uacpi_kernel_signal_event(uacpi_handle Handle)
{
    PACPI_SEM Sem = Handle;
    KIRQL OldIrql;


    KeAcquireSpinLock(&Sem->Lock, &OldIrql);
    KeSetEvent(&Sem->Event, IO_NO_INCREMENT, FALSE);

    KeReleaseSpinLock(&Sem->Lock, OldIrql);
}


void uacpi_kernel_reset_event(uacpi_handle Handle)
{
    PACPI_SEM Sem = Handle;
    KIRQL OldIrql;


    KeAcquireSpinLock(&Sem->Lock, &OldIrql);
    KeResetEvent((PKEVENT)&Sem->Event);
    KeReleaseSpinLock(&Sem->Lock, OldIrql);
}

uacpi_handle
uacpi_kernel_create_spinlock(void)
{
    PKSPIN_LOCK spinlock = ExAllocatePoolWithTag(NonPagedPool, sizeof(KSPIN_LOCK), 'Spnl');
    if (!spinlock)
        return NULL;

    KeInitializeSpinLock(spinlock);
    return (uacpi_handle)spinlock;
}

void
uacpi_kernel_free_spinlock(uacpi_handle Handle)
{
    if (Handle)
        ExFreePoolWithTag(Handle, 'Spnl');
}

uacpi_cpu_flags
uacpi_kernel_lock_spinlock(uacpi_handle Handle)
{
    KIRQL oldIrql;
    KeAcquireSpinLock((PKSPIN_LOCK)Handle, &oldIrql);
    return (uacpi_cpu_flags)oldIrql;
}

void
uacpi_kernel_unlock_spinlock(uacpi_handle Handle, uacpi_cpu_flags Flags)
{
    KeReleaseSpinLock((PKSPIN_LOCK)Handle, (KIRQL)Flags);
}

void*
uacpi_kernel_alloc(uacpi_size size)
{
    return ExAllocatePoolWithTag(NonPagedPool, size, 'Aloc');
}

void *uacpi_kernel_calloc(uacpi_size count, uacpi_size size)
{
    void* memory = ExAllocatePoolWithTag(NonPagedPool, count * size, 'Aloc');
    if (memory)
        RtlZeroMemory(memory, count * size);
    return memory;
}

#ifndef UACPI_SIZED_FREES
void uacpi_kernel_free(void *mem)
{
    if (mem)
        ExFreePoolWithTag(mem, 'Aloc');
}
#else
void uacpi_kernel_free(void *mem, uacpi_size size_hint)
{
    if (mem)
        ExFreePoolWithTag(mem, 'ipcA');
}
#endif

uacpi_handle
uacpi_kernel_create_mutex(void)
{
    PKMUTEX mutex = ExAllocatePoolWithTag(NonPagedPool, sizeof(KMUTEX), 'Mtx ');
    if (!mutex)
        return NULL;

    KeInitializeMutex(mutex, 0);
    return (uacpi_handle)mutex;
}

void
uacpi_kernel_free_mutex(uacpi_handle handle)
{
    if (handle)
        ExFreePoolWithTag(handle, 'Mtx ');
}

uacpi_status
uacpi_kernel_acquire_mutex(uacpi_handle Handle, uacpi_u16 Timeout)
{
    NTSTATUS status;
    LARGE_INTEGER interval;

    if (Timeout == 0xFFFF)
    {
        status = KeWaitForSingleObject((PKMUTEX)Handle, Executive, KernelMode, FALSE, NULL);
    }
    else
    {
        interval.QuadPart = -10 * 1000 * Timeout; // Convert milliseconds to 100-nanosecond intervals
        status = KeWaitForSingleObject((PKMUTEX)Handle, Executive, KernelMode, FALSE, &interval);
    }

    return (status == STATUS_SUCCESS) ? UACPI_STATUS_OK : UACPI_STATUS_TIMEOUT;
}

void
uacpi_kernel_release_mutex(uacpi_handle Handle)
{
    KeReleaseMutex((PKMUTEX)Handle, FALSE);
}

uacpi_status uacpi_kernel_io_map(
    uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle
)
{
    PUACPI_ALLOCATION Allocation = ExAllocatePoolWithTag(NonPagedPool, sizeof(UACPI_ALLOCATION), 'LpcA');;
    PHYSICAL_ADDRESS Address;
    PVOID OutPtr;

    Address.QuadPart = (LONGLONG)base;
    OutPtr = MmMapIoSpace(Address, len, MmNonCached);

    Allocation->PhyAddress = Address;
    Allocation->Size = len;
    Allocation->VirtAddress = OutPtr;

    DPRINT("uacpi_kernel_io_map(phys 0x%p  size 0x%X)\n", Address.QuadPart, len);
    *out_handle = (uacpi_handle)Allocation;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle)
{
    PUACPI_ALLOCATION Allocation = (PUACPI_ALLOCATION)handle;
    DPRINT("Entry: uacpi_kernel_io_unmap\n");
    MmUnmapIoSpace(Allocation->VirtAddress, Allocation->Size);
    ExFreePoolWithTag(handle, 'LpcA');
}


uacpi_status
uacpi_kernel_handle_firmware_request(uacpi_firmware_request* Req)
{
    if (Req == 0)
    {
        DPRINT1("uacpi_kernel_handle_firmware_request: BreakPoint!\n");
        ASSERT(FALSE);
    }
    else
    {
        DPRINT1("uacpi_kernel_handle_firmware_request: Fatal!\n");
        ASSERT(FALSE);
    }
    return UACPI_STATUS_OK;
}

uacpi_thread_id
uacpi_kernel_get_thread_id(void)
{
    /* Thread ID must be non-zero */
    ULONG_PTR ThreadID = (ULONG_PTR)PsGetCurrentThreadId() + 1;
    return (VOID*)ThreadID;
}

void *
uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
    PHYSICAL_ADDRESS physicalAddress;
    void *mappedAddress;

    physicalAddress.QuadPart = addr;
    mappedAddress = MmMapIoSpace(physicalAddress, len, MmNonCached);

    if (!mappedAddress)
    {
        DPRINT1("uacpi_kernel_map: Failed to map physical memory at address 0x%llx, length 0x%llx\n", addr, len);
    }

    return mappedAddress;
}

void
uacpi_kernel_unmap(void *addr, uacpi_size len)
{
    if (addr)
       MmUnmapIoSpace(addr, len);
}

uacpi_status
uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rdsp_address)
{
    *out_rdsp_address = (uacpi_phys_addr)KeLoaderBlock->Extension->AcpiTable;
    return 0;
}


static PKINTERRUPT AcpiInterrupt;
static BOOLEAN AcpiInterruptHandlerRegistered = FALSE;
static uacpi_interrupt_handler AcpiIrqHandler = NULL;
static PVOID AcpiIrqContext = NULL;
static ULONG AcpiIrqNumber = 0;

BOOLEAN NTAPI
OslIsrStub(
  PKINTERRUPT Interrupt,
  PVOID ServiceContext)
{
  INT32 Status;

  Status = (*AcpiIrqHandler)(AcpiIrqContext);

  if (Status == UACPI_INTERRUPT_HANDLED)
    return TRUE;
  else
    return FALSE;
}


uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
    uacpi_handle *out_irq_handle)
{
    ULONG Vector;
    KIRQL DIrql;
    KAFFINITY Affinity;
    NTSTATUS Status;

    if (AcpiInterruptHandlerRegistered)
    {
        DPRINT1("Reregister interrupt attempt failed\n");
        return 1;
    }

    DPRINT("uacpi_kernel_install_interrupt_handler()\n");
    Vector = HalGetInterruptVector(
        Internal,
        0,
        irq,
        irq,
        &DIrql,
        &Affinity);

    AcpiIrqNumber = irq;
    AcpiIrqHandler = handler;
    AcpiIrqContext = ctx;
    AcpiInterruptHandlerRegistered = TRUE;

    Status = IoConnectInterrupt(
        &AcpiInterrupt,
        OslIsrStub,
        NULL,
        NULL,
        Vector,
        DIrql,
        DIrql,
        LevelSensitive,
        TRUE,
        Affinity,
        FALSE);

    if (!NT_SUCCESS(Status))
    {
        DPRINT("Could not connect to interrupt %d\n", Vector);
        return 1;
    }
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(
    uacpi_interrupt_handler handler, uacpi_handle irq_handle
)
{
    DPRINT("uacpi_kernel_uninstall_interrupt_handler()\n");

    if (AcpiInterruptHandlerRegistered)
    {
        IoDisconnectInterrupt(AcpiInterrupt);
        AcpiInterrupt = NULL;
        AcpiInterruptHandlerRegistered = FALSE;
    }
    else
    {
        DPRINT1("Trying to remove non-existing interrupt handler\n");
        return 1;
    }
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_schedule_work(
    uacpi_work_type type, uacpi_work_handler Handler, uacpi_handle ctx)
{
    HANDLE ThreadHandle;
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;

    DPRINT("uacpi_kernel_schedule_work: Entry\n");

    InitializeObjectAttributes(&ObjectAttributes,
                               NULL,
                               OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    Status = PsCreateSystemThread(&ThreadHandle,
                                  THREAD_ALL_ACCESS,
                                  &ObjectAttributes,
                                  NULL,
                                  NULL,
                                  (PKSTART_ROUTINE)Handler,
                                  ctx);
    if (!NT_SUCCESS(Status))
        return 1;

    ZwClose(ThreadHandle);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void)
{
    DPRINT("uacpi_kernel_wait_for_work_completion: Enter\n");
    return 1;
}

//TODO: this one isn't going to simple.
uacpi_status 
uacpi_kernel_pci_device_open(
    uacpi_pci_address address, uacpi_handle *out_handle
)
{
    uacpi_pci_address *addr = ExAllocatePoolWithTag(NonPagedPool, sizeof(uacpi_pci_address), 'Spnl');
memcpy(addr, &address, sizeof(uacpi_pci_address));
*out_handle = addr;
return UACPI_STATUS_OK;
}

void
uacpi_kernel_pci_device_close(uacpi_handle Handle)
{
    UNIMPLEMENTED;
    __debugbreak();
}

uacpi_status
uacpi_kernel_pci_read8(uacpi_handle device, uacpi_size offset, uacpi_u8 *Value)
{
    uacpi_pci_address *address = (uacpi_pci_address *)device;
    PCI_SLOT_NUMBER slot;

    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = address->device;
    slot.u.bits.FunctionNumber = address->function;

    DPRINT("uacpi_kernel_pci_read8, slot=0x%X, func=0x%X\n", slot.u.AsULONG, offset);

    /* Width is in BITS */
    HalGetBusDataByOffset(PCIConfiguration,
        address->bus,
        slot.u.AsULONG,
        Value,
        offset,
        (0x1 >> 3));
        return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_pci_read16(uacpi_handle device, uacpi_size offset, uacpi_u16 *value)
{
    uacpi_pci_address *address = (uacpi_pci_address *)device;
    PCI_SLOT_NUMBER slot;

    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = address->device;
    slot.u.bits.FunctionNumber = address->function;

    DPRINT("uacpi_kernel_pci_read16, slot=0x%X, func=0x%X\n", slot.u.AsULONG, offset);

    /* Width is in BITS */
    HalGetBusDataByOffset(PCIConfiguration,
        address->bus,
        slot.u.AsULONG,
        value,
        offset,
        (0x2 >> 3));
        return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_pci_read32(uacpi_handle device, uacpi_size offset, uacpi_u32 *value)
{
    uacpi_pci_address *address = (uacpi_pci_address *)device;
    PCI_SLOT_NUMBER slot;

    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = address->device;
    slot.u.bits.FunctionNumber = address->function;

    DPRINT("uacpi_kernel_pci_read32, slot=0x%X, func=0x%X\n", slot.u.AsULONG, offset);

    /* Width is in BITS */
    HalGetBusDataByOffset(PCIConfiguration,
        address->bus,
        slot.u.AsULONG,
        value,
        offset,
        (0x4 >> 3));
        return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_pci_write8(uacpi_handle device, uacpi_size offset, uacpi_u8 value)
{
    UNIMPLEMENTED;
    __debugbreak();
    return 1;
}

uacpi_status
uacpi_kernel_pci_write16(uacpi_handle device, uacpi_size offset, uacpi_u16 value)
{
    UNIMPLEMENTED;
    __debugbreak();
    return 1;
}

uacpi_status
uacpi_kernel_pci_write32(uacpi_handle device, uacpi_size offset, uacpi_u32 va_list)
{
    UNIMPLEMENTED;
    __debugbreak();
    return 1;
}

uacpi_status
uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset, uacpi_u8 *out_value)
{
    PUACPI_ALLOCATION Allocation = (PUACPI_ALLOCATION)handle;
    ULONG_PTR Address = (ULONG_PTR)Allocation->VirtAddress;
    Address += offset;
    *out_value = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)Address);
    return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset, uacpi_u16 *out_value)
{
    PUACPI_ALLOCATION Allocation = (PUACPI_ALLOCATION)handle;
    ULONG_PTR Address = (ULONG_PTR)Allocation->VirtAddress;
    Address += offset;
    *out_value = READ_PORT_USHORT((PUSHORT)(ULONG_PTR)Address);
    return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset, uacpi_u32 *out_value)
{
    PUACPI_ALLOCATION Allocation = (PUACPI_ALLOCATION)handle;
    ULONG_PTR Address = (ULONG_PTR)Allocation->VirtAddress;
    Address += offset;
    *out_value = READ_PORT_ULONG((PULONG)(ULONG_PTR)Address);
    return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset, uacpi_u8 in_value)
{
    PUACPI_ALLOCATION Allocation = (PUACPI_ALLOCATION)handle;
    ULONG_PTR Address = (ULONG_PTR)Allocation->VirtAddress;
    Address += offset;
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)Address, in_value);
    return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset, uacpi_u16 in_value)
{
    PUACPI_ALLOCATION Allocation = (PUACPI_ALLOCATION)handle;
    ULONG_PTR Address = (ULONG_PTR)Allocation->VirtAddress;
    Address += offset;
    WRITE_PORT_USHORT((PUSHORT)(ULONG_PTR)Address, in_value);
    return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset, uacpi_u32 in_value)
{
    PUACPI_ALLOCATION Allocation = (PUACPI_ALLOCATION)handle;
    ULONG_PTR Address = (ULONG_PTR)Allocation->VirtAddress;
    Address += offset;
    WRITE_PORT_ULONG((PULONG)(ULONG_PTR)Address, in_value);
    return UACPI_STATUS_OK;
}
