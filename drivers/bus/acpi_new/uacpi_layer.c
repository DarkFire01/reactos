#include <acpi.h>
#include <kernel_api.h>
#include <uacpi/uacpi.h>
#include <uacpi/event.h>

#include <ntifs.h>

//#define NDEBUG
#include <debug.h>

UINT32 LaunchUACPI()
{
    uacpi_status ret = uacpi_initialize(0);
    if (uacpi_unlikely_error(ret)) {
        DPRINT1("uacpi_initialize error: %s", uacpi_status_to_string(ret));
        return -ENODEV;
    }

    __debugbreak();
    return 0;
}
uacpi_thread_id uacpi_kernel_get_thread_id(void)
{
    __debugbreak();
    return 0;
}

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rdsp_address)
{
    DPRINT1("uacpi_kernel_get_rsdp: call\n");
    __debugbreak();
    return 0;
}

uacpi_status uacpi_kernel_raw_memory_read(
    uacpi_phys_addr address, uacpi_u8 byte_width, uacpi_u64 *out_value)
{
    __debugbreak();
    return 0;
}

uacpi_status uacpi_kernel_raw_memory_write(
    uacpi_phys_addr address, uacpi_u8 byte_width, uacpi_u64 in_value)
{
    __debugbreak();
    return 0;
}

uacpi_status uacpi_kernel_raw_io_read(
    uacpi_io_addr address, uacpi_u8 byte_width, uacpi_u64 *out_value
)
{
    __debugbreak();
    return 0;
}

uacpi_status uacpi_kernel_raw_io_write(
    uacpi_io_addr address, uacpi_u8 byte_width, uacpi_u64 in_value
)
{
    __debugbreak();
    return 0;
}

uacpi_status uacpi_kernel_pci_read(
    uacpi_pci_address *address, uacpi_size offset,
    uacpi_u8 byte_width, uacpi_u64 *value
)
{
    PCI_SLOT_NUMBER slot;

    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = address->device;
    slot.u.bits.FunctionNumber = address->function;

    DPRINT("uacpi_kernel_pci_read, slot=0x%X, func=0x%X\n", slot.u.AsULONG, offset);

    /* Width is in BITS */
    HalGetBusDataByOffset(PCIConfiguration,
        address->bus,
        slot.u.AsULONG,
        value,
        offset,
        (byte_width >> 3));

    return 0;
}

uacpi_status uacpi_kernel_pci_write(
    uacpi_pci_address *address, uacpi_size offset,
    uacpi_u8 byte_width, uacpi_u64 value
)
{
    ULONG buf = value;
    PCI_SLOT_NUMBER slot;

    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = address->device;
    slot.u.bits.FunctionNumber = address->function;

    DPRINT("uacpi_kernel_pci_write, slot=0x%x\n", slot.u.AsULONG);

    /* Width is in BITS */
    HalSetBusDataByOffset(PCIConfiguration,
        address->bus,
        slot.u.AsULONG,
        &buf,
        offset,
        (byte_width >> 3));
    return 0;
}

uacpi_status uacpi_kernel_io_map(
    uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle
)
{
    __debugbreak();
    return 0;
}

void uacpi_kernel_io_unmap(uacpi_handle handle)
{
    __debugbreak();
}

uacpi_status uacpi_kernel_io_read(
    uacpi_handle handle, uacpi_size offset,
    uacpi_u8 byte_width, uacpi_u64 *value)
{
    __debugbreak();
    return 0;
}

uacpi_status uacpi_kernel_io_write(
    uacpi_handle handle, uacpi_size offset,
    uacpi_u8 byte_width, uacpi_u64 value
)
{
    __debugbreak();
    return 0;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
    PHYSICAL_ADDRESS Address;
    PVOID Ptr;

    DPRINT("AcpiOsMapMemory(phys 0x%p  size 0x%X)\n", addr, len);

    Address.QuadPart = (ULONG)addr;
    Ptr = MmMapIoSpace(Address, len, MmNonCached);
    if (!Ptr)
    {
        DPRINT1("Mapping failed\n");
    }

    return Ptr;
}

void uacpi_kernel_unmap(void *addr, uacpi_size len)
{
    __debugbreak();
}

void *uacpi_kernel_alloc(uacpi_size size)
{
    return ExAllocatePoolWithTag(NonPagedPool, size, 'ipcA');
}

void *uacpi_kernel_calloc(uacpi_size count, uacpi_size size)
{
    return ExAllocatePoolWithTag(NonPagedPool, size * count, 'ipcA');
}

/*
 * Free a previously allocated memory block.
 *
 * 'mem' might be a NULL pointer. In this case, the call is assumed to be a
 * no-op.
 *
 * An optionally enabled 'size_hint' parameter contains the size of the original
 * allocation. Note that in some scenarios this incurs additional cost to
 * calculate the object size.
 */
#ifndef UACPI_SIZED_FREES
void uacpi_kernel_free(void *mem)
{
    if (!mem)
        DPRINT1("Attempt to free null pointer!!!\n");
    ExFreePoolWithTag(mem, 'ipcA');
}
#else
void uacpi_kernel_free(void *mem, uacpi_size size_hint)
{
    if (!mem)
        DPRINT1("Attempt to free null pointer!!!\n");
    ExFreePoolWithTag(mem, 'ipcA');
}
#endif

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
    __debugbreak();
}
void uacpi_kernel_vlog(uacpi_log_level Level, const uacpi_char* Char, uacpi_va_list list)
{
    DPRINT1("uACPI: %s", Char);
    __debugbreak();
}
#endif

uacpi_u64 uacpi_kernel_get_ticks(void)
{
    __debugbreak();
    return 0;
}

void uacpi_kernel_stall(uacpi_u8 usec)
{
    KeStallExecutionProcessor(1000 * usec);
}

void uacpi_kernel_sleep(uacpi_u64 msec)
{
    KeStallExecutionProcessor(msec);
}

/*
 * Create/free an opaque non-recursive kernel mutex object.
 */
uacpi_handle uacpi_kernel_create_mutex(void)
{
    __debugbreak();
    return 0;
}

void uacpi_kernel_free_mutex(uacpi_handle handle)
{
    __debugbreak();
}

/*
 * Create/free an opaque kernel (semaphore-like) event object.
 */
uacpi_handle uacpi_kernel_create_event(void)
{
    __debugbreak();
    return 0;
}

void uacpi_kernel_free_event(uacpi_handle Handle)
{
    __debugbreak();
}

uacpi_bool uacpi_kernel_acquire_mutex(uacpi_handle Hadnle, uacpi_u16 Mutex)
{
    __debugbreak();
    return 0;
}

void uacpi_kernel_release_mutex(uacpi_handle Handle)
{
    __debugbreak();
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle Handle, uacpi_u16 Event)
{
    __debugbreak();
    return 0;
}

void uacpi_kernel_signal_event(uacpi_handle Handle)
{
    __debugbreak();
}

void uacpi_kernel_reset_event(uacpi_handle Handle)
{
    __debugbreak();
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request* Req)
{
    __debugbreak();
    return 0;
}

uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
    uacpi_handle *out_irq_handle)
{
    __debugbreak();
    return 0;
}
uacpi_status uacpi_kernel_uninstall_interrupt_handler(
    uacpi_interrupt_handler handler, uacpi_handle irq_handle
)
{
    __debugbreak();
    return 0;
}

uacpi_handle uacpi_kernel_create_spinlock(void)
{
    UNIMPLEMENTED;;
    return NULL;
}

void uacpi_kernel_free_spinlock(uacpi_handle Handle)
{
    UNIMPLEMENTED;
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle Handle)
{
    UNIMPLEMENTED;
    return 0;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle Handle, uacpi_cpu_flags falgs)
{
    UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_schedule_work(
    uacpi_work_type type, uacpi_work_handler Handler, uacpi_handle ctx)
{
    __debugbreak();
    return 0;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void)
{
    __debugbreak();
    return 0;
}

