#define STATUS_MONITOR_NO_DESCRIPTOR     ((NTSTATUS)0xC01D0001L)

//
// MessageId: STATUS_MONITOR_UNKNOWN_DESCRIPTOR_FORMAT
//
// MessageText:
//
// Format of the obtained monitor descriptor is not supported by this release.
//
#define STATUS_MONITOR_UNKNOWN_DESCRIPTOR_FORMAT ((NTSTATUS)0xC01D0002L)

//
// MessageId: STATUS_MONITOR_INVALID_DESCRIPTOR_CHECKSUM
//
// MessageText:
//
// Checksum of the obtained monitor descriptor is invalid.
//
#define STATUS_MONITOR_INVALID_DESCRIPTOR_CHECKSUM ((NTSTATUS)0xC01D0003L)

//
// MessageId: STATUS_MONITOR_INVALID_STANDARD_TIMING_BLOCK
//
// MessageText:
//
// Monitor descriptor contains an invalid standard timing block.
//
#define STATUS_MONITOR_INVALID_STANDARD_TIMING_BLOCK ((NTSTATUS)0xC01D0004L)

//
// MessageId: STATUS_MONITOR_WMI_DATABLOCK_REGISTRATION_FAILED
//
// MessageText:
//
// WMI data block registration failed for one of the MSMonitorClass WMI subclasses.
//
#define STATUS_MONITOR_WMI_DATABLOCK_REGISTRATION_FAILED ((NTSTATUS)0xC01D0005L)

//
// MessageId: STATUS_MONITOR_INVALID_SERIAL_NUMBER_MONDSC_BLOCK
//
// MessageText:
//
// Provided monitor descriptor block is either corrupted or does not contain monitor's detailed serial number.
//
#define STATUS_MONITOR_INVALID_SERIAL_NUMBER_MONDSC_BLOCK ((NTSTATUS)0xC01D0006L)

//
// MessageId: STATUS_MONITOR_INVALID_USER_FRIENDLY_MONDSC_BLOCK
//
// MessageText:
//
// Provided monitor descriptor block is either corrupted or does not contain monitor's user friendly name.
//
#define STATUS_MONITOR_INVALID_USER_FRIENDLY_MONDSC_BLOCK ((NTSTATUS)0xC01D0007L)

//
// MessageId: STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA
//
// MessageText:
//
// There is no monitor descriptor data at the specified (offset, size) region.
//
#define STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA ((NTSTATUS)0xC01D0008L)

//
// MessageId: STATUS_MONITOR_INVALID_DETAILED_TIMING_BLOCK
//
// MessageText:
//
// Monitor descriptor contains an invalid detailed timing block.
//
#define STATUS_MONITOR_INVALID_DETAILED_TIMING_BLOCK ((NTSTATUS)0xC01D0009L)

//
// MessageId: STATUS_MONITOR_INVALID_MANUFACTURE_DATE
//
// MessageText:
//
// Monitor descriptor contains invalid manufacture date.
//
#define STATUS_MONITOR_INVALID_MANUFACTURE_DATE ((NTSTATUS)0xC01D000AL)


//
// Graphics Facility Error codes (dxg.sys, dxgkrnl.sys)
//

//
//   Common Windows Graphics Kernel Subsystem status codes {0x0000..0x00ff}
//
//
// MessageId: STATUS_GRAPHICS_NOT_EXCLUSIVE_MODE_OWNER
//
// MessageText:
//
// Exclusive mode ownership is needed to create unmanaged primary allocation.
//
#define STATUS_GRAPHICS_NOT_EXCLUSIVE_MODE_OWNER ((NTSTATUS)0xC01E0000L)

//
// MessageId: STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER
//
// MessageText:
//
// The driver needs more DMA buffer space in order to complete the requested operation.
//
#define STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER ((NTSTATUS)0xC01E0001L)

//
// MessageId: STATUS_GRAPHICS_INVALID_DISPLAY_ADAPTER
//
// MessageText:
//
// Specified display adapter handle is invalid.
//
#define STATUS_GRAPHICS_INVALID_DISPLAY_ADAPTER ((NTSTATUS)0xC01E0002L)

//
// MessageId: STATUS_GRAPHICS_ADAPTER_WAS_RESET
//
// MessageText:
//
// Specified display adapter and all of its state has been reset.
//
#define STATUS_GRAPHICS_ADAPTER_WAS_RESET ((NTSTATUS)0xC01E0003L)

//
// MessageId: STATUS_GRAPHICS_INVALID_DRIVER_MODEL
//
// MessageText:
//
// The driver stack doesn't match the expected driver model.
//
#define STATUS_GRAPHICS_INVALID_DRIVER_MODEL ((NTSTATUS)0xC01E0004L)

//
// MessageId: STATUS_GRAPHICS_PRESENT_MODE_CHANGED
//
// MessageText:
//
// Present happened but ended up into the changed desktop mode
//
#define STATUS_GRAPHICS_PRESENT_MODE_CHANGED ((NTSTATUS)0xC01E0005L)

//
// MessageId: STATUS_GRAPHICS_PRESENT_OCCLUDED
//
// MessageText:
//
// Nothing to present due to desktop occlusion
//
#define STATUS_GRAPHICS_PRESENT_OCCLUDED ((NTSTATUS)0xC01E0006L)

//
// MessageId: STATUS_GRAPHICS_PRESENT_DENIED
//
// MessageText:
//
// Not able to present due to denial of desktop access
//
#define STATUS_GRAPHICS_PRESENT_DENIED   ((NTSTATUS)0xC01E0007L)

//
// MessageId: STATUS_GRAPHICS_CANNOTCOLORCONVERT
//
// MessageText:
//
// Not able to present with color conversion
//
#define STATUS_GRAPHICS_CANNOTCOLORCONVERT ((NTSTATUS)0xC01E0008L)

//
// MessageId: STATUS_GRAPHICS_DRIVER_MISMATCH
//
// MessageText:
//
// The kernel driver detected a version mismatch between it and the user mode driver.
//
#define STATUS_GRAPHICS_DRIVER_MISMATCH  ((NTSTATUS)0xC01E0009L)

//
// MessageId: STATUS_GRAPHICS_PARTIAL_DATA_POPULATED
//
// MessageText:
//
// Specified buffer is not big enough to contain entire requested dataset. Partial data populated up to the size of the buffer. Caller needs to provide buffer of size as specified in the partially populated buffer's content (interface specific).
//
#define STATUS_GRAPHICS_PARTIAL_DATA_POPULATED ((NTSTATUS)0x401E000AL)

//
// MessageId: STATUS_GRAPHICS_PRESENT_REDIRECTION_DISABLED
//
// MessageText:
//
// Present redirection is disabled (desktop windowing management subsystem is off).
//
#define STATUS_GRAPHICS_PRESENT_REDIRECTION_DISABLED ((NTSTATUS)0xC01E000BL)

//
// MessageId: STATUS_GRAPHICS_PRESENT_UNOCCLUDED
//
// MessageText:
//
// Previous exclusive VidPn source owner has released its ownership
//
#define STATUS_GRAPHICS_PRESENT_UNOCCLUDED ((NTSTATUS)0xC01E000CL)

//
// MessageId: STATUS_GRAPHICS_WINDOWDC_NOT_AVAILABLE
//
// MessageText:
//
// Window DC is not available for presentation
//
#define STATUS_GRAPHICS_WINDOWDC_NOT_AVAILABLE ((NTSTATUS)0xC01E000DL)

//
// MessageId: STATUS_GRAPHICS_WINDOWLESS_PRESENT_DISABLED
//
// MessageText:
//
// Windowless present is disabled (desktop windowing management subsystem is off).
//
#define STATUS_GRAPHICS_WINDOWLESS_PRESENT_DISABLED ((NTSTATUS)0xC01E000EL)

//
// MessageId: STATUS_GRAPHICS_PRESENT_INVALID_WINDOW
//
// MessageText:
//
// Window handle is invalid
//
#define STATUS_GRAPHICS_PRESENT_INVALID_WINDOW ((NTSTATUS)0xC01E000FL)

//
// MessageId: STATUS_GRAPHICS_PRESENT_BUFFER_NOT_BOUND
//
// MessageText:
//
// No buffer is bound to composition surface
//
#define STATUS_GRAPHICS_PRESENT_BUFFER_NOT_BOUND ((NTSTATUS)0xC01E0010L)

//
// MessageId: STATUS_GRAPHICS_VAIL_STATE_CHANGED
//
// MessageText:
//
// Vail state has been changed
//
#define STATUS_GRAPHICS_VAIL_STATE_CHANGED ((NTSTATUS)0xC01E0011L)

//
// MessageId: STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN
//
// MessageText:
//
// Notifying indirect display UMDF class driver to abandon current swapchain.
//
#define STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN ((NTSTATUS)0xC01E0012L)

//
// MessageId: STATUS_GRAPHICS_INDIRECT_DISPLAY_DEVICE_STOPPED
//
// MessageText:
//
// Notifying indirect display UMDF class driver that indirect display device has been stopped.
//
#define STATUS_GRAPHICS_INDIRECT_DISPLAY_DEVICE_STOPPED ((NTSTATUS)0xC01E0013L)

//
// MessageId: STATUS_GRAPHICS_MPO_ALLOCATION_UNPINNED
//
// MessageText:
//
// The allocation for the MPO has been unpinned.
//
#define STATUS_GRAPHICS_MPO_ALLOCATION_UNPINNED ((NTSTATUS)0xC01E0018L)

//
//   Video Memory Manager (VidMM) specific status codes {0x0100..0x01ff}
//
//
// MessageId: STATUS_GRAPHICS_NO_VIDEO_MEMORY
//
// MessageText:
//
// Not enough video memory available to complete the operation.
//
#define STATUS_GRAPHICS_NO_VIDEO_MEMORY  ((NTSTATUS)0xC01E0100L)

//
// MessageId: STATUS_GRAPHICS_CANT_LOCK_MEMORY
//
// MessageText:
//
// Couldn't probe and lock the underlying memory of an allocation.
//
#define STATUS_GRAPHICS_CANT_LOCK_MEMORY ((NTSTATUS)0xC01E0101L)

//
// MessageId: STATUS_GRAPHICS_ALLOCATION_BUSY
//
// MessageText:
//
// The allocation is currently busy.
//
#define STATUS_GRAPHICS_ALLOCATION_BUSY  ((NTSTATUS)0xC01E0102L)

//
// MessageId: STATUS_GRAPHICS_TOO_MANY_REFERENCES
//
// MessageText:
//
// An object being referenced has already reached the maximum reference count and can't be referenced any further.
//
#define STATUS_GRAPHICS_TOO_MANY_REFERENCES ((NTSTATUS)0xC01E0103L)

//
// MessageId: STATUS_GRAPHICS_TRY_AGAIN_LATER
//
// MessageText:
//
// A problem couldn't be solved due to some currently existing condition. The problem should be tried again later.
//
#define STATUS_GRAPHICS_TRY_AGAIN_LATER  ((NTSTATUS)0xC01E0104L)

//
// MessageId: STATUS_GRAPHICS_TRY_AGAIN_NOW
//
// MessageText:
//
// A problem couldn't be solved due to some currently existing condition. The problem should be tried again immediately.
//
#define STATUS_GRAPHICS_TRY_AGAIN_NOW    ((NTSTATUS)0xC01E0105L)

//
// MessageId: STATUS_GRAPHICS_ALLOCATION_INVALID
//
// MessageText:
//
// The allocation is invalid.
//
#define STATUS_GRAPHICS_ALLOCATION_INVALID ((NTSTATUS)0xC01E0106L)

//
// MessageId: STATUS_GRAPHICS_UNSWIZZLING_APERTURE_UNAVAILABLE
//
// MessageText:
//
// No more unswizzling aperture are currently available.
//
#define STATUS_GRAPHICS_UNSWIZZLING_APERTURE_UNAVAILABLE ((NTSTATUS)0xC01E0107L)

//
// MessageId: STATUS_GRAPHICS_UNSWIZZLING_APERTURE_UNSUPPORTED
//
// MessageText:
//
// The current allocation can't be unswizzled by an aperture.
//
#define STATUS_GRAPHICS_UNSWIZZLING_APERTURE_UNSUPPORTED ((NTSTATUS)0xC01E0108L)

//
// MessageId: STATUS_GRAPHICS_CANT_EVICT_PINNED_ALLOCATION
//
// MessageText:
//
// The request failed because a pinned allocation can't be evicted.
//
#define STATUS_GRAPHICS_CANT_EVICT_PINNED_ALLOCATION ((NTSTATUS)0xC01E0109L)

//
// MessageId: STATUS_GRAPHICS_INVALID_ALLOCATION_USAGE
//
// MessageText:
//
// The allocation can't be used from its current segment location for the specified operation.
//
#define STATUS_GRAPHICS_INVALID_ALLOCATION_USAGE ((NTSTATUS)0xC01E0110L)

//
// MessageId: STATUS_GRAPHICS_CANT_RENDER_LOCKED_ALLOCATION
//
// MessageText:
//
// A locked allocation can't be used in the current command buffer.
//
#define STATUS_GRAPHICS_CANT_RENDER_LOCKED_ALLOCATION ((NTSTATUS)0xC01E0111L)

//
// MessageId: STATUS_GRAPHICS_ALLOCATION_CLOSED
//
// MessageText:
//
// The allocation being referenced has been closed permanently.
//
#define STATUS_GRAPHICS_ALLOCATION_CLOSED ((NTSTATUS)0xC01E0112L)

//
// MessageId: STATUS_GRAPHICS_INVALID_ALLOCATION_INSTANCE
//
// MessageText:
//
// An invalid allocation instance is being referenced.
//
#define STATUS_GRAPHICS_INVALID_ALLOCATION_INSTANCE ((NTSTATUS)0xC01E0113L)

//
// MessageId: STATUS_GRAPHICS_INVALID_ALLOCATION_HANDLE
//
// MessageText:
//
// An invalid allocation handle is being referenced.
//
#define STATUS_GRAPHICS_INVALID_ALLOCATION_HANDLE ((NTSTATUS)0xC01E0114L)

//
// MessageId: STATUS_GRAPHICS_WRONG_ALLOCATION_DEVICE
//
// MessageText:
//
// The allocation being referenced doesn't belong to the current device.
//
#define STATUS_GRAPHICS_WRONG_ALLOCATION_DEVICE ((NTSTATUS)0xC01E0115L)

//
// MessageId: STATUS_GRAPHICS_ALLOCATION_CONTENT_LOST
//
// MessageText:
//
// The specified allocation lost its content.
//
#define STATUS_GRAPHICS_ALLOCATION_CONTENT_LOST ((NTSTATUS)0xC01E0116L)

//
//   Video GPU Scheduler (VidSch) specific status codes {0x0200..0x02ff}
//
//
// MessageId: STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE
//
// MessageText:
//
// GPU exception is detected on the given device. The device is not able to be scheduled.
//
#define STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE ((NTSTATUS)0xC01E0200L)

//
// MessageId: STATUS_GRAPHICS_SKIP_ALLOCATION_PREPARATION
//
// MessageText:
//
// Skip preparation of allocations referenced by the DMA buffer.
//
#define STATUS_GRAPHICS_SKIP_ALLOCATION_PREPARATION ((NTSTATUS)0x401E0201L)

//
//   Video Present Network Management (VidPNMgr) specific status codes {0x0300..0x03ff}
//
//
// MessageId: STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY
//
// MessageText:
//
// Specified VidPN topology is invalid.
//
#define STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY ((NTSTATUS)0xC01E0300L)

//
// MessageId: STATUS_GRAPHICS_VIDPN_TOPOLOGY_NOT_SUPPORTED
//
// MessageText:
//
// Specified VidPN topology is valid but is not supported by this model of the display adapter.
//
#define STATUS_GRAPHICS_VIDPN_TOPOLOGY_NOT_SUPPORTED ((NTSTATUS)0xC01E0301L)

//
// MessageId: STATUS_GRAPHICS_VIDPN_TOPOLOGY_CURRENTLY_NOT_SUPPORTED
//
// MessageText:
//
// Specified VidPN topology is valid but is not supported by the display adapter at this time, due to current allocation of its resources.
//
#define STATUS_GRAPHICS_VIDPN_TOPOLOGY_CURRENTLY_NOT_SUPPORTED ((NTSTATUS)0xC01E0302L)

//
// MessageId: STATUS_GRAPHICS_INVALID_VIDPN
//
// MessageText:
//
// Specified VidPN handle is invalid.
//
#define STATUS_GRAPHICS_INVALID_VIDPN    ((NTSTATUS)0xC01E0303L)

//
// MessageId: STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE
//
// MessageText:
//
// Specified video present source is invalid.
//
#define STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE ((NTSTATUS)0xC01E0304L)

//
// MessageId: STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET
//
// MessageText:
//
// Specified video present target is invalid.
//
#define STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET ((NTSTATUS)0xC01E0305L)

//
// MessageId: STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED
//
// MessageText:
//
// Specified VidPN modality is not supported (e.g. at least two of the pinned modes are not cofunctional).
//
#define STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED ((NTSTATUS)0xC01E0306L)

//
// MessageId: STATUS_GRAPHICS_MODE_NOT_PINNED
//
// MessageText:
//
// No mode is pinned on the specified VidPN source/target.
//
#define STATUS_GRAPHICS_MODE_NOT_PINNED  ((NTSTATUS)0x401E0307L)

//
// MessageId: STATUS_GRAPHICS_INVALID_VIDPN_SOURCEMODESET
//
// MessageText:
//
// Specified VidPN source mode set is invalid.
//
#define STATUS_GRAPHICS_INVALID_VIDPN_SOURCEMODESET ((NTSTATUS)0xC01E0308L)

//
// MessageId: STATUS_GRAPHICS_INVALID_VIDPN_TARGETMODESET
//
// MessageText:
//
// Specified VidPN target mode set is invalid.
//
#define STATUS_GRAPHICS_INVALID_VIDPN_TARGETMODESET ((NTSTATUS)0xC01E0309L)

//
// MessageId: STATUS_GRAPHICS_INVALID_FREQUENCY
//
// MessageText:
//
// Specified video signal frequency is invalid.
//
#define STATUS_GRAPHICS_INVALID_FREQUENCY ((NTSTATUS)0xC01E030AL)

//
// MessageId: STATUS_GRAPHICS_INVALID_ACTIVE_REGION
//
// MessageText:
//
// Specified video signal active region is invalid.
//
#define STATUS_GRAPHICS_INVALID_ACTIVE_REGION ((NTSTATUS)0xC01E030BL)

//
// MessageId: STATUS_GRAPHICS_INVALID_TOTAL_REGION
//
// MessageText:
//
// Specified video signal total region is invalid.
//
#define STATUS_GRAPHICS_INVALID_TOTAL_REGION ((NTSTATUS)0xC01E030CL)

//
// MessageId: STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE
//
// MessageText:
//
// Specified video present source mode is invalid.
//
#define STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE ((NTSTATUS)0xC01E0310L)

//
// MessageId: STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET_MODE
//
// MessageText:
//
// Specified video present target mode is invalid.
//
#define STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET_MODE ((NTSTATUS)0xC01E0311L)

//
// MessageId: STATUS_GRAPHICS_PINNED_MODE_MUST_REMAIN_IN_SET
//
// MessageText:
//
// Pinned mode must remain in the set on VidPN's cofunctional modality enumeration.
//
#define STATUS_GRAPHICS_PINNED_MODE_MUST_REMAIN_IN_SET ((NTSTATUS)0xC01E0312L)

//
// MessageId: STATUS_GRAPHICS_PATH_ALREADY_IN_TOPOLOGY
//
// MessageText:
//
// Specified video present path is already in VidPN's topology.
//
#define STATUS_GRAPHICS_PATH_ALREADY_IN_TOPOLOGY ((NTSTATUS)0xC01E0313L)

//
// MessageId: STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET
//
// MessageText:
//
// Specified mode is already in the mode set.
//
#define STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET ((NTSTATUS)0xC01E0314L)

//
// MessageId: STATUS_GRAPHICS_INVALID_VIDEOPRESENTSOURCESET
//
// MessageText:
//
// Specified video present source set is invalid.
//
#define STATUS_GRAPHICS_INVALID_VIDEOPRESENTSOURCESET ((NTSTATUS)0xC01E0315L)

//
// MessageId: STATUS_GRAPHICS_INVALID_VIDEOPRESENTTARGETSET
//
// MessageText:
//
// Specified video present target set is invalid.
//
#define STATUS_GRAPHICS_INVALID_VIDEOPRESENTTARGETSET ((NTSTATUS)0xC01E0316L)

//
// MessageId: STATUS_GRAPHICS_SOURCE_ALREADY_IN_SET
//
// MessageText:
//
// Specified video present source is already in the video present source set.
//
#define STATUS_GRAPHICS_SOURCE_ALREADY_IN_SET ((NTSTATUS)0xC01E0317L)

//
// MessageId: STATUS_GRAPHICS_TARGET_ALREADY_IN_SET
//
// MessageText:
//
// Specified video present target is already in the video present target set.
//
#define STATUS_GRAPHICS_TARGET_ALREADY_IN_SET ((NTSTATUS)0xC01E0318L)

//
// MessageId: STATUS_GRAPHICS_INVALID_VIDPN_PRESENT_PATH
//
// MessageText:
//
// Specified VidPN present path is invalid.
//
#define STATUS_GRAPHICS_INVALID_VIDPN_PRESENT_PATH ((NTSTATUS)0xC01E0319L)

//
// MessageId: STATUS_GRAPHICS_NO_RECOMMENDED_VIDPN_TOPOLOGY
//
// MessageText:
//
// Miniport has no recommendation for augmentation of the specified VidPN's topology.
//
#define STATUS_GRAPHICS_NO_RECOMMENDED_VIDPN_TOPOLOGY ((NTSTATUS)0xC01E031AL)

//
// MessageId: STATUS_GRAPHICS_INVALID_MONITOR_FREQUENCYRANGESET
//
// MessageText:
//
// Specified monitor frequency range set is invalid.
//
#define STATUS_GRAPHICS_INVALID_MONITOR_FREQUENCYRANGESET ((NTSTATUS)0xC01E031BL)

//
// MessageId: STATUS_GRAPHICS_INVALID_MONITOR_FREQUENCYRANGE
//
// MessageText:
//
// Specified monitor frequency range is invalid.
//
#define STATUS_GRAPHICS_INVALID_MONITOR_FREQUENCYRANGE ((NTSTATUS)0xC01E031CL)

//
// MessageId: STATUS_GRAPHICS_FREQUENCYRANGE_NOT_IN_SET
//
// MessageText:
//
// Specified frequency range is not in the specified monitor frequency range set.
//
#define STATUS_GRAPHICS_FREQUENCYRANGE_NOT_IN_SET ((NTSTATUS)0xC01E031DL)

//
// MessageId: STATUS_GRAPHICS_NO_PREFERRED_MODE
//
// MessageText:
//
// Specified mode set does not specify preference for one of its modes.
//
#define STATUS_GRAPHICS_NO_PREFERRED_MODE ((NTSTATUS)0x401E031EL)

//
// MessageId: STATUS_GRAPHICS_FREQUENCYRANGE_ALREADY_IN_SET
//
// MessageText:
//
// Specified frequency range is already in the specified monitor frequency range set.
//
#define STATUS_GRAPHICS_FREQUENCYRANGE_ALREADY_IN_SET ((NTSTATUS)0xC01E031FL)

//
// MessageId: STATUS_GRAPHICS_STALE_MODESET
//
// MessageText:
//
// Specified mode set is stale. Please reacquire the new mode set.
//
#define STATUS_GRAPHICS_STALE_MODESET    ((NTSTATUS)0xC01E0320L)

//
// MessageId: STATUS_GRAPHICS_INVALID_MONITOR_SOURCEMODESET
//
// MessageText:
//
// Specified monitor source mode set is invalid.
//
#define STATUS_GRAPHICS_INVALID_MONITOR_SOURCEMODESET ((NTSTATUS)0xC01E0321L)

//
// MessageId: STATUS_GRAPHICS_INVALID_MONITOR_SOURCE_MODE
//
// MessageText:
//
// Specified monitor source mode is invalid.
//
#define STATUS_GRAPHICS_INVALID_MONITOR_SOURCE_MODE ((NTSTATUS)0xC01E0322L)

//
// MessageId: STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN
//
// MessageText:
//
// Miniport does not have any recommendation regarding the request to provide a functional VidPN given the current display adapter configuration.
//
#define STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN ((NTSTATUS)0xC01E0323L)

//
// MessageId: STATUS_GRAPHICS_MODE_ID_MUST_BE_UNIQUE
//
// MessageText:
//
// ID of the specified mode is already used by another mode in the set.
//
#define STATUS_GRAPHICS_MODE_ID_MUST_BE_UNIQUE ((NTSTATUS)0xC01E0324L)

//
// MessageId: STATUS_GRAPHICS_EMPTY_ADAPTER_MONITOR_MODE_SUPPORT_INTERSECTION
//
// MessageText:
//
// System failed to determine a mode that is supported by both the display adapter and the monitor connected to it.
//
#define STATUS_GRAPHICS_EMPTY_ADAPTER_MONITOR_MODE_SUPPORT_INTERSECTION ((NTSTATUS)0xC01E0325L)

//
// MessageId: STATUS_GRAPHICS_VIDEO_PRESENT_TARGETS_LESS_THAN_SOURCES
//
// MessageText:
//
// Number of video present targets must be greater than or equal to the number of video present sources.
//
#define STATUS_GRAPHICS_VIDEO_PRESENT_TARGETS_LESS_THAN_SOURCES ((NTSTATUS)0xC01E0326L)

//
// MessageId: STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY
//
// MessageText:
//
// Specified present path is not in VidPN's topology.
//
#define STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY ((NTSTATUS)0xC01E0327L)

//
// MessageId: STATUS_GRAPHICS_ADAPTER_MUST_HAVE_AT_LEAST_ONE_SOURCE
//
// MessageText:
//
// Display adapter must have at least one video present source.
//
#define STATUS_GRAPHICS_ADAPTER_MUST_HAVE_AT_LEAST_ONE_SOURCE ((NTSTATUS)0xC01E0328L)

//
// MessageId: STATUS_GRAPHICS_ADAPTER_MUST_HAVE_AT_LEAST_ONE_TARGET
//
// MessageText:
//
// Display adapter must have at least one video present target.
//
#define STATUS_GRAPHICS_ADAPTER_MUST_HAVE_AT_LEAST_ONE_TARGET ((NTSTATUS)0xC01E0329L)

//
// MessageId: STATUS_GRAPHICS_INVALID_MONITORDESCRIPTORSET
//
// MessageText:
//
// Specified monitor descriptor set is invalid.
//
#define STATUS_GRAPHICS_INVALID_MONITORDESCRIPTORSET ((NTSTATUS)0xC01E032AL)

//
// MessageId: STATUS_GRAPHICS_INVALID_MONITORDESCRIPTOR
//
// MessageText:
//
// Specified monitor descriptor is invalid.
//
#define STATUS_GRAPHICS_INVALID_MONITORDESCRIPTOR ((NTSTATUS)0xC01E032BL)

//
// MessageId: STATUS_GRAPHICS_MONITORDESCRIPTOR_NOT_IN_SET
//
// MessageText:
//
// Specified descriptor is not in the specified monitor descriptor set.
//
#define STATUS_GRAPHICS_MONITORDESCRIPTOR_NOT_IN_SET ((NTSTATUS)0xC01E032CL)

//
// MessageId: STATUS_GRAPHICS_MONITORDESCRIPTOR_ALREADY_IN_SET
//
// MessageText:
//
// Specified descriptor is already in the specified monitor descriptor set.
//
#define STATUS_GRAPHICS_MONITORDESCRIPTOR_ALREADY_IN_SET ((NTSTATUS)0xC01E032DL)

//
// MessageId: STATUS_GRAPHICS_MONITORDESCRIPTOR_ID_MUST_BE_UNIQUE
//
// MessageText:
//
// ID of the specified monitor descriptor is already used by another descriptor in the set.
//
#define STATUS_GRAPHICS_MONITORDESCRIPTOR_ID_MUST_BE_UNIQUE ((NTSTATUS)0xC01E032EL)

//
// MessageId: STATUS_GRAPHICS_INVALID_VIDPN_TARGET_SUBSET_TYPE
//
// MessageText:
//
// Specified video present target subset type is invalid.
//
#define STATUS_GRAPHICS_INVALID_VIDPN_TARGET_SUBSET_TYPE ((NTSTATUS)0xC01E032FL)

//
// MessageId: STATUS_GRAPHICS_RESOURCES_NOT_RELATED
//
// MessageText:
//
// Two or more of the specified resources are not related to each other, as defined by the interface semantics.
//
#define STATUS_GRAPHICS_RESOURCES_NOT_RELATED ((NTSTATUS)0xC01E0330L)

//
// MessageId: STATUS_GRAPHICS_SOURCE_ID_MUST_BE_UNIQUE
//
// MessageText:
//
// ID of the specified video present source is already used by another source in the set.
//
#define STATUS_GRAPHICS_SOURCE_ID_MUST_BE_UNIQUE ((NTSTATUS)0xC01E0331L)

//
// MessageId: STATUS_GRAPHICS_TARGET_ID_MUST_BE_UNIQUE
//
// MessageText:
//
// ID of the specified video present target is already used by another target in the set.
//
#define STATUS_GRAPHICS_TARGET_ID_MUST_BE_UNIQUE ((NTSTATUS)0xC01E0332L)

//
// MessageId: STATUS_GRAPHICS_NO_AVAILABLE_VIDPN_TARGET
//
// MessageText:
//
// Specified VidPN source cannot be used because there is no available VidPN target to connect it to.
//
#define STATUS_GRAPHICS_NO_AVAILABLE_VIDPN_TARGET ((NTSTATUS)0xC01E0333L)

//
// MessageId: STATUS_GRAPHICS_MONITOR_COULD_NOT_BE_ASSOCIATED_WITH_ADAPTER
//
// MessageText:
//
// Newly arrived monitor could not be associated with a display adapter.
//
#define STATUS_GRAPHICS_MONITOR_COULD_NOT_BE_ASSOCIATED_WITH_ADAPTER ((NTSTATUS)0xC01E0334L)

//
// MessageId: STATUS_GRAPHICS_NO_VIDPNMGR
//
// MessageText:
//
// Display adapter in question does not have an associated VidPN manager.
//
#define STATUS_GRAPHICS_NO_VIDPNMGR      ((NTSTATUS)0xC01E0335L)

//
// MessageId: STATUS_GRAPHICS_NO_ACTIVE_VIDPN
//
// MessageText:
//
// VidPN manager of the display adapter in question does not have an active VidPN.
//
#define STATUS_GRAPHICS_NO_ACTIVE_VIDPN  ((NTSTATUS)0xC01E0336L)

//
// MessageId: STATUS_GRAPHICS_STALE_VIDPN_TOPOLOGY
//
// MessageText:
//
// Specified VidPN topology is stale. Please reacquire the new topology.
//
#define STATUS_GRAPHICS_STALE_VIDPN_TOPOLOGY ((NTSTATUS)0xC01E0337L)

//
// MessageId: STATUS_GRAPHICS_MONITOR_NOT_CONNECTED
//
// MessageText:
//
// There is no monitor connected on the specified video present target.
//
#define STATUS_GRAPHICS_MONITOR_NOT_CONNECTED ((NTSTATUS)0xC01E0338L)

//
// MessageId: STATUS_GRAPHICS_SOURCE_NOT_IN_TOPOLOGY
//
// MessageText:
//
// Specified source is not part of the specified VidPN's topology.
//
#define STATUS_GRAPHICS_SOURCE_NOT_IN_TOPOLOGY ((NTSTATUS)0xC01E0339L)

//
// MessageId: STATUS_GRAPHICS_INVALID_PRIMARYSURFACE_SIZE
//
// MessageText:
//
// Specified primary surface size is invalid.
//
#define STATUS_GRAPHICS_INVALID_PRIMARYSURFACE_SIZE ((NTSTATUS)0xC01E033AL)

//
// MessageId: STATUS_GRAPHICS_INVALID_VISIBLEREGION_SIZE
//
// MessageText:
//
// Specified visible region size is invalid.
//
#define STATUS_GRAPHICS_INVALID_VISIBLEREGION_SIZE ((NTSTATUS)0xC01E033BL)

//
// MessageId: STATUS_GRAPHICS_INVALID_STRIDE
//
// MessageText:
//
// Specified stride is invalid.
//
#define STATUS_GRAPHICS_INVALID_STRIDE   ((NTSTATUS)0xC01E033CL)

//
// MessageId: STATUS_GRAPHICS_INVALID_PIXELFORMAT
//
// MessageText:
//
// Specified pixel format is invalid.
//
#define STATUS_GRAPHICS_INVALID_PIXELFORMAT ((NTSTATUS)0xC01E033DL)

//
// MessageId: STATUS_GRAPHICS_INVALID_COLORBASIS
//
// MessageText:
//
// Specified color basis is invalid.
//
#define STATUS_GRAPHICS_INVALID_COLORBASIS ((NTSTATUS)0xC01E033EL)

//
// MessageId: STATUS_GRAPHICS_INVALID_PIXELVALUEACCESSMODE
//
// MessageText:
//
// Specified pixel value access mode is invalid.
//
#define STATUS_GRAPHICS_INVALID_PIXELVALUEACCESSMODE ((NTSTATUS)0xC01E033FL)

//
// MessageId: STATUS_GRAPHICS_TARGET_NOT_IN_TOPOLOGY
//
// MessageText:
//
// Specified target is not part of the specified VidPN's topology.
//
#define STATUS_GRAPHICS_TARGET_NOT_IN_TOPOLOGY ((NTSTATUS)0xC01E0340L)

//
// MessageId: STATUS_GRAPHICS_NO_DISPLAY_MODE_MANAGEMENT_SUPPORT
//
// MessageText:
//
// Failed to acquire display mode management interface.
//
#define STATUS_GRAPHICS_NO_DISPLAY_MODE_MANAGEMENT_SUPPORT ((NTSTATUS)0xC01E0341L)

//
// MessageId: STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE
//
// MessageText:
//
// Specified VidPN source is already owned by a DMM client and cannot be used until that client releases it.
//
#define STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE ((NTSTATUS)0xC01E0342L)

//
// MessageId: STATUS_GRAPHICS_CANT_ACCESS_ACTIVE_VIDPN
//
// MessageText:
//
// Specified VidPN is active and cannot be accessed.
//
#define STATUS_GRAPHICS_CANT_ACCESS_ACTIVE_VIDPN ((NTSTATUS)0xC01E0343L)

//
// MessageId: STATUS_GRAPHICS_INVALID_PATH_IMPORTANCE_ORDINAL
//
// MessageText:
//
// Specified VidPN present path importance ordinal is invalid.
//
#define STATUS_GRAPHICS_INVALID_PATH_IMPORTANCE_ORDINAL ((NTSTATUS)0xC01E0344L)

//
// MessageId: STATUS_GRAPHICS_INVALID_PATH_CONTENT_GEOMETRY_TRANSFORMATION
//
// MessageText:
//
// Specified VidPN present path content geometry transformation is invalid.
//
#define STATUS_GRAPHICS_INVALID_PATH_CONTENT_GEOMETRY_TRANSFORMATION ((NTSTATUS)0xC01E0345L)

//
// MessageId: STATUS_GRAPHICS_PATH_CONTENT_GEOMETRY_TRANSFORMATION_NOT_SUPPORTED
//
// MessageText:
//
// Specified content geometry transformation is not supported on the respective VidPN present path.
//
#define STATUS_GRAPHICS_PATH_CONTENT_GEOMETRY_TRANSFORMATION_NOT_SUPPORTED ((NTSTATUS)0xC01E0346L)

//
// MessageId: STATUS_GRAPHICS_INVALID_GAMMA_RAMP
//
// MessageText:
//
// Specified gamma ramp is invalid.
//
#define STATUS_GRAPHICS_INVALID_GAMMA_RAMP ((NTSTATUS)0xC01E0347L)

//
// MessageId: STATUS_GRAPHICS_GAMMA_RAMP_NOT_SUPPORTED
//
// MessageText:
//
// Specified gamma ramp is not supported on the respective VidPN present path.
//
#define STATUS_GRAPHICS_GAMMA_RAMP_NOT_SUPPORTED ((NTSTATUS)0xC01E0348L)

//
// MessageId: STATUS_GRAPHICS_MULTISAMPLING_NOT_SUPPORTED
//
// MessageText:
//
// Multi-sampling is not supported on the respective VidPN present path.
//
#define STATUS_GRAPHICS_MULTISAMPLING_NOT_SUPPORTED ((NTSTATUS)0xC01E0349L)

//
// MessageId: STATUS_GRAPHICS_MODE_NOT_IN_MODESET
//
// MessageText:
//
// Specified mode is not in the specified mode set.
//
#define STATUS_GRAPHICS_MODE_NOT_IN_MODESET ((NTSTATUS)0xC01E034AL)

//
// MessageId: STATUS_GRAPHICS_DATASET_IS_EMPTY
//
// MessageText:
//
// Specified data set (e.g. mode set, frequency range set, descriptor set, topology, etc.) is empty.
//
#define STATUS_GRAPHICS_DATASET_IS_EMPTY ((NTSTATUS)0x401E034BL)

//
// MessageId: STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET
//
// MessageText:
//
// Specified data set (e.g. mode set, frequency range set, descriptor set, topology, etc.) does not contain any more elements.
//
#define STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET ((NTSTATUS)0x401E034CL)

//
// MessageId: STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY_RECOMMENDATION_REASON
//
// MessageText:
//
// Specified VidPN topology recommendation reason is invalid.
//
#define STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY_RECOMMENDATION_REASON ((NTSTATUS)0xC01E034DL)

//
// MessageId: STATUS_GRAPHICS_INVALID_PATH_CONTENT_TYPE
//
// MessageText:
//
// Specified VidPN present path content type is invalid.
//
#define STATUS_GRAPHICS_INVALID_PATH_CONTENT_TYPE ((NTSTATUS)0xC01E034EL)

//
// MessageId: STATUS_GRAPHICS_INVALID_COPYPROTECTION_TYPE
//
// MessageText:
//
// Specified VidPN present path copy protection type is invalid.
//
#define STATUS_GRAPHICS_INVALID_COPYPROTECTION_TYPE ((NTSTATUS)0xC01E034FL)

//
// MessageId: STATUS_GRAPHICS_UNASSIGNED_MODESET_ALREADY_EXISTS
//
// MessageText:
//
// No more than one unassigned mode set can exist at any given time for a given VidPN source/target.
//
#define STATUS_GRAPHICS_UNASSIGNED_MODESET_ALREADY_EXISTS ((NTSTATUS)0xC01E0350L)

//
// MessageId: STATUS_GRAPHICS_PATH_CONTENT_GEOMETRY_TRANSFORMATION_NOT_PINNED
//
// MessageText:
//
// Specified content transformation is not pinned on the specified VidPN present path.
//
#define STATUS_GRAPHICS_PATH_CONTENT_GEOMETRY_TRANSFORMATION_NOT_PINNED ((NTSTATUS)0x401E0351L)

//
// MessageId: STATUS_GRAPHICS_INVALID_SCANLINE_ORDERING
//
// MessageText:
//
// Specified scanline ordering type is invalid.
//
#define STATUS_GRAPHICS_INVALID_SCANLINE_ORDERING ((NTSTATUS)0xC01E0352L)

//
// MessageId: STATUS_GRAPHICS_TOPOLOGY_CHANGES_NOT_ALLOWED
//
// MessageText:
//
// Topology changes are not allowed for the specified VidPN.
//
#define STATUS_GRAPHICS_TOPOLOGY_CHANGES_NOT_ALLOWED ((NTSTATUS)0xC01E0353L)

//
// MessageId: STATUS_GRAPHICS_NO_AVAILABLE_IMPORTANCE_ORDINALS
//
// MessageText:
//
// All available importance ordinals are already used in specified topology.
//
#define STATUS_GRAPHICS_NO_AVAILABLE_IMPORTANCE_ORDINALS ((NTSTATUS)0xC01E0354L)

//
// MessageId: STATUS_GRAPHICS_INCOMPATIBLE_PRIVATE_FORMAT
//
// MessageText:
//
// Specified primary surface has a different private format attribute than the current primary surface
//
#define STATUS_GRAPHICS_INCOMPATIBLE_PRIVATE_FORMAT ((NTSTATUS)0xC01E0355L)

//
// MessageId: STATUS_GRAPHICS_INVALID_MODE_PRUNING_ALGORITHM
//
// MessageText:
//
// Specified mode pruning algorithm is invalid
//
#define STATUS_GRAPHICS_INVALID_MODE_PRUNING_ALGORITHM ((NTSTATUS)0xC01E0356L)

//
// MessageId: STATUS_GRAPHICS_INVALID_MONITOR_CAPABILITY_ORIGIN
//
// MessageText:
//
// Specified monitor capability origin is invalid.
//
#define STATUS_GRAPHICS_INVALID_MONITOR_CAPABILITY_ORIGIN ((NTSTATUS)0xC01E0357L)

//
// MessageId: STATUS_GRAPHICS_INVALID_MONITOR_FREQUENCYRANGE_CONSTRAINT
//
// MessageText:
//
// Specified monitor frequency range constraint is invalid.
//
#define STATUS_GRAPHICS_INVALID_MONITOR_FREQUENCYRANGE_CONSTRAINT ((NTSTATUS)0xC01E0358L)

//
// MessageId: STATUS_GRAPHICS_MAX_NUM_PATHS_REACHED
//
// MessageText:
//
// Maximum supported number of present paths has been reached.
//
#define STATUS_GRAPHICS_MAX_NUM_PATHS_REACHED ((NTSTATUS)0xC01E0359L)

//
// MessageId: STATUS_GRAPHICS_CANCEL_VIDPN_TOPOLOGY_AUGMENTATION
//
// MessageText:
//
// Miniport requested that augmentation be cancelled for the specified source of the specified VidPN's topology.
//
#define STATUS_GRAPHICS_CANCEL_VIDPN_TOPOLOGY_AUGMENTATION ((NTSTATUS)0xC01E035AL)

//
// MessageId: STATUS_GRAPHICS_INVALID_CLIENT_TYPE
//
// MessageText:
//
// Specified client type was not recognized.
//
#define STATUS_GRAPHICS_INVALID_CLIENT_TYPE ((NTSTATUS)0xC01E035BL)

//
// MessageId: STATUS_GRAPHICS_CLIENTVIDPN_NOT_SET
//
// MessageText:
//
// Client VidPN is not set on this adapter (e.g. no user mode initiated mode changes took place on this adapter yet).
//
#define STATUS_GRAPHICS_CLIENTVIDPN_NOT_SET ((NTSTATUS)0xC01E035CL)

//
//   Port specific status codes {0x0400..0x04ff}
//
//
// MessageId: STATUS_GRAPHICS_SPECIFIED_CHILD_ALREADY_CONNECTED
//
// MessageText:
//
// Specified display adapter child device already has an external device connected to it.
//
#define STATUS_GRAPHICS_SPECIFIED_CHILD_ALREADY_CONNECTED ((NTSTATUS)0xC01E0400L)    

//
// MessageId: STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED
//
// MessageText:
//
// Specified display adapter child device does not support descriptor exposure.
//
#define STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED ((NTSTATUS)0xC01E0401L)    

//
// MessageId: STATUS_GRAPHICS_UNKNOWN_CHILD_STATUS
//
// MessageText:
//
// Child device presence was not reliably detected.
//
#define STATUS_GRAPHICS_UNKNOWN_CHILD_STATUS ((NTSTATUS)0x401E042FL)

//
// MessageId: STATUS_GRAPHICS_NOT_A_LINKED_ADAPTER
//
// MessageText:
//
// The display adapter is not linked to any other adapters.
//
#define STATUS_GRAPHICS_NOT_A_LINKED_ADAPTER ((NTSTATUS)0xC01E0430L)

//
// MessageId: STATUS_GRAPHICS_LEADLINK_NOT_ENUMERATED
//
// MessageText:
//
// Lead adapter in a linked configuration was not enumerated yet.
//
#define STATUS_GRAPHICS_LEADLINK_NOT_ENUMERATED ((NTSTATUS)0xC01E0431L)

//
// MessageId: STATUS_GRAPHICS_CHAINLINKS_NOT_ENUMERATED
//
// MessageText:
//
// Some chain adapters in a linked configuration were not enumerated yet.
//
#define STATUS_GRAPHICS_CHAINLINKS_NOT_ENUMERATED ((NTSTATUS)0xC01E0432L)

//
// MessageId: STATUS_GRAPHICS_ADAPTER_CHAIN_NOT_READY
//
// MessageText:
//
// The chain of linked adapters is not ready to start because of an unknown failure.
//
#define STATUS_GRAPHICS_ADAPTER_CHAIN_NOT_READY ((NTSTATUS)0xC01E0433L)

//
// MessageId: STATUS_GRAPHICS_CHAINLINKS_NOT_STARTED
//
// MessageText:
//
// An attempt was made to start a lead link display adapter when the chain links were not started yet.
//
#define STATUS_GRAPHICS_CHAINLINKS_NOT_STARTED ((NTSTATUS)0xC01E0434L)

//
// MessageId: STATUS_GRAPHICS_CHAINLINKS_NOT_POWERED_ON
//
// MessageText:
//
// An attempt was made to power up a lead link display adapter when the chain links were powered down.
//
#define STATUS_GRAPHICS_CHAINLINKS_NOT_POWERED_ON ((NTSTATUS)0xC01E0435L)

//
// MessageId: STATUS_GRAPHICS_INCONSISTENT_DEVICE_LINK_STATE
//
// MessageText:
//
// The adapter link was found to be in an inconsistent state. Not all adapters are in an expected PNP/Power state.
//
#define STATUS_GRAPHICS_INCONSISTENT_DEVICE_LINK_STATE ((NTSTATUS)0xC01E0436L)

//
// MessageId: STATUS_GRAPHICS_LEADLINK_START_DEFERRED
//
// MessageText:
//
// Starting the leadlink adapter has been deferred temporarily.
//
#define STATUS_GRAPHICS_LEADLINK_START_DEFERRED ((NTSTATUS)0x401E0437L)

//
// MessageId: STATUS_GRAPHICS_NOT_POST_DEVICE_DRIVER
//
// MessageText:
//
// The driver trying to start is not the same as the driver for the POSTed display adapter.
//
#define STATUS_GRAPHICS_NOT_POST_DEVICE_DRIVER ((NTSTATUS)0xC01E0438L)

//
// MessageId: STATUS_GRAPHICS_POLLING_TOO_FREQUENTLY
//
// MessageText:
//
// The display adapter is being polled for children too frequently at the same polling level.
//
#define STATUS_GRAPHICS_POLLING_TOO_FREQUENTLY ((NTSTATUS)0x401E0439L)

//
// MessageId: STATUS_GRAPHICS_START_DEFERRED
//
// MessageText:
//
// Starting the adapter has been deferred temporarily.
//
#define STATUS_GRAPHICS_START_DEFERRED   ((NTSTATUS)0x401E043AL)

//
// MessageId: STATUS_GRAPHICS_ADAPTER_ACCESS_NOT_EXCLUDED
//
// MessageText:
//
// An operation is being attempted that requires the display adapter to be in a quiescent state.
//
#define STATUS_GRAPHICS_ADAPTER_ACCESS_NOT_EXCLUDED ((NTSTATUS)0xC01E043BL)

//
// MessageId: STATUS_GRAPHICS_DEPENDABLE_CHILD_STATUS
//
// MessageText:
//
// We can depend on the child device presence returned by the driver.
//
#define STATUS_GRAPHICS_DEPENDABLE_CHILD_STATUS ((NTSTATUS)0x401E043CL)

//
//   OPM, PVP and UAB status codes {0x0500..0x057F}
//
//
// MessageId: STATUS_GRAPHICS_OPM_NOT_SUPPORTED
//
// MessageText:
//
// The driver does not support OPM.
//
#define STATUS_GRAPHICS_OPM_NOT_SUPPORTED ((NTSTATUS)0xC01E0500L)    

//
// MessageId: STATUS_GRAPHICS_COPP_NOT_SUPPORTED
//
// MessageText:
//
// The driver does not support COPP.
//
#define STATUS_GRAPHICS_COPP_NOT_SUPPORTED ((NTSTATUS)0xC01E0501L)    

//
// MessageId: STATUS_GRAPHICS_UAB_NOT_SUPPORTED
//
// MessageText:
//
// The driver does not support UAB.
//
#define STATUS_GRAPHICS_UAB_NOT_SUPPORTED ((NTSTATUS)0xC01E0502L)    

//
// MessageId: STATUS_GRAPHICS_OPM_INVALID_ENCRYPTED_PARAMETERS
//
// MessageText:
//
// The specified encrypted parameters are invalid.
//
#define STATUS_GRAPHICS_OPM_INVALID_ENCRYPTED_PARAMETERS ((NTSTATUS)0xC01E0503L)    

//
// MessageId: STATUS_GRAPHICS_OPM_NO_PROTECTED_OUTPUTS_EXIST
//
// MessageText:
//
// The GDI display device passed to this function does not have any active protected outputs.
//
#define STATUS_GRAPHICS_OPM_NO_PROTECTED_OUTPUTS_EXIST ((NTSTATUS)0xC01E0505L)

//
// MessageId: STATUS_GRAPHICS_OPM_INTERNAL_ERROR
//
// MessageText:
//
// An internal error caused an operation to fail.
//
#define STATUS_GRAPHICS_OPM_INTERNAL_ERROR ((NTSTATUS)0xC01E050BL)

//
// MessageId: STATUS_GRAPHICS_OPM_INVALID_HANDLE
//
// MessageText:
//
// The function failed because the caller passed in an invalid OPM user mode handle.
//
#define STATUS_GRAPHICS_OPM_INVALID_HANDLE ((NTSTATUS)0xC01E050CL)

//
// MessageId: STATUS_GRAPHICS_PVP_INVALID_CERTIFICATE_LENGTH
//
// MessageText:
//
// A certificate could not be returned because the certificate buffer passed to the function was too small.
//
#define STATUS_GRAPHICS_PVP_INVALID_CERTIFICATE_LENGTH ((NTSTATUS)0xC01E050EL)

//
// MessageId: STATUS_GRAPHICS_OPM_SPANNING_MODE_ENABLED
//
// MessageText:
//
// The DxgkDdiOpmCreateProtectedOutput function could not create a protected output because the Video Present Target is in spanning mode.
//
#define STATUS_GRAPHICS_OPM_SPANNING_MODE_ENABLED ((NTSTATUS)0xC01E050FL)

//
// MessageId: STATUS_GRAPHICS_OPM_THEATER_MODE_ENABLED
//
// MessageText:
//
// The DxgkDdiOpmCreateProtectedOutput function could not create a protected output because the Video Present Target is in theater mode.
//
#define STATUS_GRAPHICS_OPM_THEATER_MODE_ENABLED ((NTSTATUS)0xC01E0510L)

//
// MessageId: STATUS_GRAPHICS_PVP_HFS_FAILED
//
// MessageText:
//
// The function failed because the display adapter's Hardware Functionality Scan failed to validate the graphics hardware.
//
#define STATUS_GRAPHICS_PVP_HFS_FAILED   ((NTSTATUS)0xC01E0511L)

//
// MessageId: STATUS_GRAPHICS_OPM_INVALID_SRM
//
// MessageText:
//
// The HDCP System Renewability Message passed to this function did not comply with section 5 of the HDCP 1.1 specification.
//
#define STATUS_GRAPHICS_OPM_INVALID_SRM  ((NTSTATUS)0xC01E0512L)

//
// MessageId: STATUS_GRAPHICS_OPM_OUTPUT_DOES_NOT_SUPPORT_HDCP
//
// MessageText:
//
// The protected output cannot enable the High-bandwidth Digital Content Protection (HDCP) System because it does not support HDCP.
//
#define STATUS_GRAPHICS_OPM_OUTPUT_DOES_NOT_SUPPORT_HDCP ((NTSTATUS)0xC01E0513L)

//
// MessageId: STATUS_GRAPHICS_OPM_OUTPUT_DOES_NOT_SUPPORT_ACP
//
// MessageText:
//
// The protected output cannot enable Analogue Copy Protection (ACP) because it does not support ACP.
//
#define STATUS_GRAPHICS_OPM_OUTPUT_DOES_NOT_SUPPORT_ACP ((NTSTATUS)0xC01E0514L)

//
// MessageId: STATUS_GRAPHICS_OPM_OUTPUT_DOES_NOT_SUPPORT_CGMSA
//
// MessageText:
//
// The protected output cannot enable the Content Generation Management System Analogue (CGMS-A) protection technology because it does not support CGMS-A.
//
#define STATUS_GRAPHICS_OPM_OUTPUT_DOES_NOT_SUPPORT_CGMSA ((NTSTATUS)0xC01E0515L)

//
// MessageId: STATUS_GRAPHICS_OPM_HDCP_SRM_NEVER_SET
//
// MessageText:
//
// The DxgkDdiOPMGetInformation function cannot return the version of the SRM being used because the application never successfully passed an SRM to the protected output.
//
#define STATUS_GRAPHICS_OPM_HDCP_SRM_NEVER_SET ((NTSTATUS)0xC01E0516L)

//
// MessageId: STATUS_GRAPHICS_OPM_RESOLUTION_TOO_HIGH
//
// MessageText:
//
// The DxgkDdiOPMConfigureProtectedOutput function cannot enable the specified output protection technology because the output's screen resolution is too high.
//
#define STATUS_GRAPHICS_OPM_RESOLUTION_TOO_HIGH ((NTSTATUS)0xC01E0517L)

//
// MessageId: STATUS_GRAPHICS_OPM_ALL_HDCP_HARDWARE_ALREADY_IN_USE
//
// MessageText:
//
// The DxgkDdiOPMConfigureProtectedOutput function cannot enable HDCP because the display adapter's HDCP hardware is already being used by other physical outputs.
//
#define STATUS_GRAPHICS_OPM_ALL_HDCP_HARDWARE_ALREADY_IN_USE ((NTSTATUS)0xC01E0518L)

//
// MessageId: STATUS_GRAPHICS_OPM_PROTECTED_OUTPUT_NO_LONGER_EXISTS
//
// MessageText:
//
// The operating system asynchronously destroyed this OPM protected output because the operating system's state changed. This error typically occurs because the monitor PDO associated with this protected output was removed, the monitor PDO associated with this protected output was stopped, or the protected output's session became a non-console session.
//
#define STATUS_GRAPHICS_OPM_PROTECTED_OUTPUT_NO_LONGER_EXISTS ((NTSTATUS)0xC01E051AL)

//
// MessageId: STATUS_GRAPHICS_OPM_PROTECTED_OUTPUT_DOES_NOT_HAVE_COPP_SEMANTICS
//
// MessageText:
//
// Either the DxgkDdiOPMGetCOPPCompatibleInformation, DxgkDdiOPMGetInformation, or DxgkDdiOPMConfigureProtectedOutput function failed. This error is returned when the caller tries to use a COPP specific command while the protected output has OPM semantics only.
//
#define STATUS_GRAPHICS_OPM_PROTECTED_OUTPUT_DOES_NOT_HAVE_COPP_SEMANTICS ((NTSTATUS)0xC01E051CL)

//
// MessageId: STATUS_GRAPHICS_OPM_INVALID_INFORMATION_REQUEST
//
// MessageText:
//
// The DxgkDdiOPMGetInformation and DxgkDdiOPMGetCOPPCompatibleInformation functions return this error code if the passed in sequence number is not the expected sequence number or the passed in OMAC value is invalid.
//
#define STATUS_GRAPHICS_OPM_INVALID_INFORMATION_REQUEST ((NTSTATUS)0xC01E051DL)

//
// MessageId: STATUS_GRAPHICS_OPM_DRIVER_INTERNAL_ERROR
//
// MessageText:
//
// The function failed because an unexpected error occurred inside of a display driver.
//
#define STATUS_GRAPHICS_OPM_DRIVER_INTERNAL_ERROR ((NTSTATUS)0xC01E051EL)

//
// MessageId: STATUS_GRAPHICS_OPM_PROTECTED_OUTPUT_DOES_NOT_HAVE_OPM_SEMANTICS
//
// MessageText:
//
// Either the DxgkDdiOPMGetCOPPCompatibleInformation, DxgkDdiOPMGetInformation, or DxgkDdiOPMConfigureProtectedOutput function failed. This error is returned when the caller tries to use an OPM specific command while the protected output has COPP semantics only.
//
#define STATUS_GRAPHICS_OPM_PROTECTED_OUTPUT_DOES_NOT_HAVE_OPM_SEMANTICS ((NTSTATUS)0xC01E051FL)

//
// MessageId: STATUS_GRAPHICS_OPM_SIGNALING_NOT_SUPPORTED
//
// MessageText:
//
// The DxgkDdiOPMGetCOPPCompatibleInformation and DxgkDdiOPMConfigureProtectedOutput functions return this error if the display driver does not support the DXGKMDT_OPM_GET_ACP_AND_CGMSA_SIGNALING and DXGKMDT_OPM_SET_ACP_AND_CGMSA_SIGNALING GUIDs.
//
#define STATUS_GRAPHICS_OPM_SIGNALING_NOT_SUPPORTED ((NTSTATUS)0xC01E0520L)

//
// MessageId: STATUS_GRAPHICS_OPM_INVALID_CONFIGURATION_REQUEST
//
// MessageText:
//
// The DxgkDdiOPMConfigureProtectedOutput function returns this error code if the passed in sequence number is not the expected sequence number or the passed in OMAC value is invalid.
//
#define STATUS_GRAPHICS_OPM_INVALID_CONFIGURATION_REQUEST ((NTSTATUS)0xC01E0521L)

//
//   Monitor Configuration API status codes {0x0580..0x05DF}
//
//
// MessageId: STATUS_GRAPHICS_I2C_NOT_SUPPORTED
//
// MessageText:
//
// The monitor connected to the specified video output does not have an I2C bus.
//
#define STATUS_GRAPHICS_I2C_NOT_SUPPORTED ((NTSTATUS)0xC01E0580L)    

//
// MessageId: STATUS_GRAPHICS_I2C_DEVICE_DOES_NOT_EXIST
//
// MessageText:
//
// No device on the I2C bus has the specified address.
//
#define STATUS_GRAPHICS_I2C_DEVICE_DOES_NOT_EXIST ((NTSTATUS)0xC01E0581L)    

//
// MessageId: STATUS_GRAPHICS_I2C_ERROR_TRANSMITTING_DATA
//
// MessageText:
//
// An error occurred while transmitting data to the device on the I2C bus.
//
#define STATUS_GRAPHICS_I2C_ERROR_TRANSMITTING_DATA ((NTSTATUS)0xC01E0582L)    

//
// MessageId: STATUS_GRAPHICS_I2C_ERROR_RECEIVING_DATA
//
// MessageText:
//
// An error occurred while receiving data from the device on the I2C bus.
//
#define STATUS_GRAPHICS_I2C_ERROR_RECEIVING_DATA ((NTSTATUS)0xC01E0583L)    

//
// MessageId: STATUS_GRAPHICS_DDCCI_VCP_NOT_SUPPORTED
//
// MessageText:
//
// The monitor does not support the specified VCP code.
//
#define STATUS_GRAPHICS_DDCCI_VCP_NOT_SUPPORTED ((NTSTATUS)0xC01E0584L)    

//
// MessageId: STATUS_GRAPHICS_DDCCI_INVALID_DATA
//
// MessageText:
//
// The data received from the monitor is invalid.
//
#define STATUS_GRAPHICS_DDCCI_INVALID_DATA ((NTSTATUS)0xC01E0585L)    

//
// MessageId: STATUS_GRAPHICS_DDCCI_MONITOR_RETURNED_INVALID_TIMING_STATUS_BYTE
//
// MessageText:
//
// The function failed because a monitor returned an invalid Timing Status byte when the operating system used the DDC/CI Get Timing Report & Timing Message command to get a timing report from a monitor.
//
#define STATUS_GRAPHICS_DDCCI_MONITOR_RETURNED_INVALID_TIMING_STATUS_BYTE ((NTSTATUS)0xC01E0586L)

//
// MessageId: STATUS_GRAPHICS_DDCCI_INVALID_CAPABILITIES_STRING
//
// MessageText:
//
// A monitor returned a DDC/CI capabilities string which did not comply with the ACCESS.bus 3.0, DDC/CI 1.1, or MCCS 2 Revision 1 specification.
//
#define STATUS_GRAPHICS_DDCCI_INVALID_CAPABILITIES_STRING ((NTSTATUS)0xC01E0587L)
#define STATUS_GRAPHICS_MCA_INTERNAL_ERROR ((NTSTATUS)0xC01E0588L)
#define STATUS_GRAPHICS_DDCCI_INVALID_MESSAGE_COMMAND ((NTSTATUS)0xC01E0589L)
#define STATUS_GRAPHICS_DDCCI_INVALID_MESSAGE_LENGTH ((NTSTATUS)0xC01E058AL)
