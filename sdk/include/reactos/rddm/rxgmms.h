/*
 * PROJECT:     ReactOS Display Driver Model (DxgKrnl_ms)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared dxgkrnl <-> dxgmms (VidMm / VidSch) interface contract
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/*
 * Boundary between dxgkrnl.sys and the memory-manager / scheduler driver (dxgmms2.sys for
 * the Win10 model, dxgmms1.sys for Vista/Win7).
 *
 * dxgmms{1,2}.sys EXPORT VidMmInitializeAdapter / VidSchInitializeAdapter (names + signatures
 * match the Reference/win10 decompilation). dxgkrnl IMPORTS them and, once per adapter from
 * DpiFdoStartAdapter, passes its ADAPTER_RENDER (the per-adapter render-core that lives on the
 * DXGADAPTER) and receives back an opaque VIDMM_GLOBAL* / VIDSCH_GLOBAL* stored on the adapter.
 *
 * ADAPTER_RENDER is the genuine cross-module object name from the reference (a large C++
 * render-core, Reference/win10/dxgkrnl.h:17256). We carry it as a named C struct grown to the
 * fields VidMm/VidSch actually consume (roadmap §5.1); since dxgkrnl owns the producer and
 * dxgmms the consumer, the field set - not the original ABI offsets - is the contract.
 *
 * VIDMM_GLOBAL / VIDSCH_GLOBAL contents stay private to dxgmms; dxgkrnl only holds the pointer.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque per-adapter manager state, owned by dxgmms, held by reference in dxgkrnl. */
typedef struct _VIDMM_GLOBAL  VIDMM_GLOBAL,  *PVIDMM_GLOBAL;
typedef struct _VIDSCH_GLOBAL VIDSCH_GLOBAL, *PVIDSCH_GLOBAL;

/* Bump on any incompatible change to ADAPTER_RENDER below. */
#define RXGMMS_INTERFACE_VERSION 3

/**
 * @brief Per-adapter render core passed from dxgkrnl into VidMm/VidSch at init.
 *        Reference/win10/dxgkrnl.h:17256 (ADAPTER_RENDER). Grown as the port consumes fields.
 */
typedef struct _ADAPTER_RENDER
{
    ULONG  Size;                   /* sizeof(ADAPTER_RENDER) */
    ULONG  Version;                /* RXGMMS_INTERFACE_VERSION */
    PVOID  DxgAdapter;             /* DXGADAPTER* this render core belongs to */
    PVOID  MiniportDeviceContext;  /* hAdapter handed to the WDDM miniport DDIs */
    PVOID  DxgkInterface;          /* DXGKRNL_INTERFACE* (DxgkCb* callback table) */
    PVOID  MiniportDriverInitData; /* PDRIVER_INITIALIZATION_DATA - all miniport DxgkDdi* entries;
                                    * VidSch/VidMm call DxgkDdiSubmitCommand/BuildPagingBuffer/...
                                    * through this. Cast where dispmprt.h is in scope. */
    /* Memory segments, discovered by dxgkrnl via DXGKQAITYPE_QUERYSEGMENT before init. */
    ULONG  NbSegments;
    ULONG  PagingBufferSegmentId;
    ULONG  PagingBufferSize;
    ULONG  PagingBufferPrivateDataSize;
} ADAPTER_RENDER, *PADAPTER_RENDER;

/* --- Exported by dxgmms{1,2}.sys, imported by dxgkrnl.sys ------------------------------- */

/**
 * @brief Bring up the video memory manager for an adapter (Reference dxgmms2.c:25194 ->
 *        VidMmiInit). Returns the VidMm instance, or NULL on failure.
 */
PVIDMM_GLOBAL
NTAPI
VidMmInitializeAdapter(
    _In_ PADAPTER_RENDER RenderCore);

/**
 * @brief Bring up the GPU scheduler for an adapter (Reference dxgmms2.c:85379 -> VidSchiInit).
 *        Returns the VidSch instance, or NULL on failure.
 *
 * @param RenderCore       The adapter render core (also yields the VidMm it cooperates with).
 * @param hDriver          Miniport device context / driver handle.
 * @param GpuRunningTime   Out: scheduler GPU running-time counter the adapter polls.
 */
PVIDSCH_GLOBAL
NTAPI
VidSchInitializeAdapter(
    _In_  PADAPTER_RENDER RenderCore,
    _In_  PVOID           hDriver,
    _Out_ PULONGLONG      GpuRunningTime);

/**
 * @brief Queue a DMA buffer for GPU submission. @p Context is an opaque VidSch context handle
 *        (NULL = adapter-global). @p DmaBufferGpuVa is the command buffer's GPU virtual address
 *        (D3DGPU_VIRTUAL_ADDRESS, 64-bit) - passed whole so 64-bit VAs are not truncated on i386.
 *        Returns once queued; completion is fence-tracked.
 */
NTSTATUS
NTAPI
VidSchSubmitCommand(
    _In_     PVIDSCH_GLOBAL VidSch,
    _In_opt_ PVOID          Context,
    _In_     ULONGLONG      DmaBufferGpuVa,
    _In_     UINT           DmaBufferSize,
    _In_opt_ PVOID          PrivateData,
    _In_     UINT           PrivateDataSize);

/**
 * @brief Queue a DMA buffer (from VidMmAcquireDmaBuffer) for async submission - used by both the
 *        present and render paths. The buffer is referenced until its GPU fence completes and then
 *        released to its pool; the caller must NOT free or reuse it after this call. @p Context is
 *        an opaque VidSch context handle.
 */
NTSTATUS
NTAPI
VidSchSubmitDmaBuffer(
    _In_     PVIDSCH_GLOBAL VidSch,
    _In_opt_ PVOID          Context,
    _In_     PVOID          DmaBuffer);

/** @brief Block until the GPU has finished all submitted work (CDD flush before GDI draws). */
NTSTATUS
NTAPI
VidSchFlush(_In_ PVIDSCH_GLOBAL VidSch);

/** @brief Advance the completed-fence value from the miniport ISR/DPC. */
VOID
NTAPI
VidSchNotifyInterrupt(
    _In_ PVIDSCH_GLOBAL VidSch,
    _In_ ULONGLONG      CompletedFenceId);

/* --- VidMm allocations ------------------------------------------------------------------ */

/** @brief The allocation must land in a segment whose descriptor has Flags.CpuVisible set. */
#define VIDMM_ALLOC_F_CPU_VISIBLE  0x00000001
/**
 * @brief A scan-out primary. Implies CPU_VISIBLE (GDI/the CDD draws into it through the CPU
 *        aperture while the display engine scans it out) and biases placement toward the low end
 *        of the segment, since a display engine derives its start x/y from the byte offset.
 */
#define VIDMM_ALLOC_F_PRIMARY      0x00000002

/**
 * @brief Placement request for VidMmCreateAllocation. Mirrors the subset of the reference
 *        _VIDMM_GLOBAL_ALLOC (Reference/win10/dxgmms2.h:18401) that drives segment selection:
 *        RealSize, Alignment and SupportedSegmentSet.
 */
typedef struct _VIDMM_ALLOCATION_INFO
{
    SIZE_T Size;                /* RealSize; 0 is treated as one page */
    ULONG  Alignment;           /* 0 = PAGE_SIZE */
    ULONG  SupportedSegmentSet; /* bit (SegmentId - 1) per permitted segment; 0 = any */
    ULONG  Flags;               /* VIDMM_ALLOC_F_* */
} VIDMM_ALLOCATION_INFO, *PVIDMM_ALLOCATION_INFO;

/** @brief Allocate and track a video-memory allocation; returns an opaque handle (NULL on fail). */
PVOID
NTAPI
VidMmCreateAllocation(
    _In_ PVIDMM_GLOBAL                VidMm,
    _In_ const VIDMM_ALLOCATION_INFO *Info);

/** @brief Destroy a VidMm allocation obtained from VidMmCreateAllocation. */
VOID
NTAPI
VidMmDestroyAllocation(
    _In_ PVIDMM_GLOBAL VidMm,
    _In_ PVOID         Allocation);

/**
 * @brief Return an allocation's GPU physical address (segment base + offset) and segment id, for
 *        programming scanout via the miniport DxgkDdiSetVidPnSourceAddress. The address is zero
 *        (and *SegmentId 0) if the allocation was never placed in a segment. Returns a ULONGLONG
 *        (not PHYSICAL_ADDRESS by value) to keep a stable i386 stdcall return ABI across the export.
 */
ULONGLONG
NTAPI
VidMmGetAllocationPhysicalAddress(
    _In_      PVIDMM_GLOBAL VidMm,
    _In_      PVOID         Allocation,
    _Out_opt_ PULONG        SegmentId);

/**
 * @brief Return an allocation's offset WITHIN its segment, plus the segment id.
 *
 * This - not the physical address above - is what DxgkDdiSetVidPnSourceAddress wants: WDDM
 * defines DXGKARG_SETVIDPNSOURCEADDRESS::PrimaryAddress as an offset from the start of
 * PrimarySegment. The two coincide only for a segment whose BaseAddress is 0.
 */
ULONGLONG
NTAPI
VidMmGetAllocationSegmentOffset(
    _In_      PVIDMM_GLOBAL VidMm,
    _In_      PVOID         Allocation,
    _Out_opt_ PULONG        SegmentId);

/**
 * @brief Map an allocation for CPU access and return a kernel VA, or NULL.
 *        Reference VIDMM_PAGE_TABLE_BASE::GetCpuVisibleAddress (dxgmms2.c:88797).
 *
 * For a CpuVisible segment this maps segment.CpuTranslatedAddress + allocation offset as
 * write-combined I/O space - the aperture through which the CPU reaches video memory. @p pContext
 * receives an opaque token that must be handed back to VidMmFreeCpuVisibleAddress.
 */
PVOID
NTAPI
VidMmGetCpuVisibleAddress(
    _In_  PVIDMM_GLOBAL VidMm,
    _In_  PVOID         Allocation,
    _Out_ PVOID        *pContext);

/**
 * @brief Undo VidMmGetCpuVisibleAddress.
 *        Reference VIDMM_PAGE_TABLE_BASE::FreeCpuVisibleAddress (dxgmms2.c:88755).
 */
VOID
NTAPI
VidMmFreeCpuVisibleAddress(
    _In_ PVIDMM_GLOBAL VidMm,
    _In_ PVOID         Allocation,
    _In_ PVOID         Address,
    _In_ PVOID         Context);

/* --- VidMm DMA-buffer pool (per-context pool of pooled, refcounted GPU command buffers) ---- */

/** @brief Create a context's DMA-buffer pool, sized from its DXGK_CONTEXTINFO. Opaque handle. */
PVOID NTAPI VidMmCreateDmaPool(_In_ PVIDMM_GLOBAL VidMm, _In_ UINT DmaBufferSize,
                               _In_ UINT PrivateDataSize, _In_ UINT AllocationListSize,
                               _In_ UINT PatchLocationListSize);
/** @brief Destroy a DMA-buffer pool and all its buffers (at context teardown). */
VOID  NTAPI VidMmDestroyDmaPool(_In_ PVOID DmaPool);
/** @brief Acquire a non-busy DMA buffer (opaque handle), or NULL if the pool is exhausted. */
PVOID NTAPI VidMmAcquireDmaBuffer(_In_ PVOID DmaPool);
/** @brief CPU-writable view of a DMA buffer (the miniport fills it with commands). */
PVOID NTAPI VidMmDmaGetCpuAddress(_In_ PVOID DmaBuffer);
/** @brief A DMA buffer's GPU physical address. */
ULONGLONG NTAPI VidMmDmaGetPhysicalAddress(_In_ PVOID DmaBuffer);
/** @brief A DMA buffer's allocation-list scratch (DXGK_ALLOCATIONLIST[]). */
PVOID NTAPI VidMmDmaGetAllocationList(_In_ PVOID DmaBuffer);
/** @brief A DMA buffer's private-data scratch. */
PVOID NTAPI VidMmDmaGetPrivateData(_In_ PVOID DmaBuffer);
/** @brief A DMA buffer's capacity in bytes. */
UINT  NTAPI VidMmDmaGetSize(_In_ PVOID DmaBuffer);
/** @brief A DMA buffer's output patch-location-list scratch (render). */
PVOID NTAPI VidMmDmaGetPatchLocationList(_In_ PVOID DmaBuffer);
/** @brief Length (entries) of a DMA buffer's output patch-location-list. */
UINT  NTAPI VidMmDmaGetPatchLocationListSize(_In_ PVOID DmaBuffer);

/** @brief Register a scheduler context; returns an opaque context handle (NULL on failure). */
PVOID
NTAPI
VidSchCreateContext(
    _In_     PVIDSCH_GLOBAL VidSch,
    _In_opt_ PVOID          hContext,
    _In_opt_ PVOID          MiniportContext,
    _In_     ULONG          NodeOrdinal);

/** @brief Unregister a scheduler context obtained from VidSchCreateContext. */
VOID
NTAPI
VidSchDestroyContext(
    _In_ PVIDSCH_GLOBAL VidSch,
    _In_ PVOID          Context);

#ifdef __cplusplus
}
#endif
