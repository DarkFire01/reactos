#
# NetAdapterCx, the WDF class extension that NDIS 6.x miniports bind to.
# Imported from Microsoft's Network-Adapter-Class-Extension drop (MIT, see
# LICENSE). The drop ships no build files and is missing the WDK and internal
# headers it includes, so this target does not compile yet. inc/README.md has
# the inventory.
#
# Like every KMDF client this links wdfdriverentry, which raises the
# _WIN32_WINNT floor above the tree default. The default has to come off first.
#
remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE_ADAPTER
    adapter/adapterpnppower.cpp
    adapter/devicepnppower.cpp
    adapter/nxadapter.cpp
    adapter/nxadapterapi.cpp
    adapter/nxadaptercollection.cpp
    adapter/nxadapterdriverapi.cpp
    adapter/nxadapterextension.cpp
    adapter/nxadapterextensionapi.cpp
    adapter/nxbufferqueue.cpp
    adapter/nxbufferqueueapi.cpp
    adapter/nxconfiguration.cpp
    adapter/nxconfigurationapi.cpp
    adapter/nxdefaultdatapath.cpp
    adapter/nxdevice.cpp
    adapter/nxdeviceapi.cpp
    adapter/nxdriver.cpp
    adapter/nxexecutioncontext.cpp
    adapter/nxexecutioncontextapi.cpp
    adapter/nxexecutioncontexttask.cpp
    adapter/nxextension.cpp
    adapter/nxoffload.cpp
    adapter/nxpowerpolicyapi.cpp
    adapter/nxqueue.cpp
    adapter/nxqueueapi.cpp
    adapter/nxqueueverifier.cpp
    adapter/nxutility.cpp
    adapter/verifier.cpp
    adapter/version.cpp
    adapter/config/netclientdriverconfigurationimpl.cpp
    adapter/extension/nxextensionlayout.cpp
    adapter/idle/idlestatemachine.cpp
    adapter/idle/powerreferenceholder.cpp
    adapter/monitor/nxpacketmonitor.cpp
    adapter/powerpolicy/nxidlerestrictions.cpp
    adapter/powerpolicy/nxpowerentry.cpp
    adapter/powerpolicy/nxpowerlist.cpp
    adapter/powerpolicy/nxpoweroffload.cpp
    adapter/powerpolicy/nxpowerpolicy.cpp
    adapter/powerpolicy/nxwakesource.cpp
    adapter/queue/queue.cpp
    adapter/rxscaling/nxrxscaling.cpp
    adapter/types/nxadapterextensiontypes.cpp
    adapter/types/nxadaptertypes.cpp)

list(APPEND SOURCE_BM
    bm/bufferheader.cpp
    bm/buffermanager.cpp
    bm/bufferpool.cpp
    bm/commonbuffervector.cpp
    bm/dmaallocator.cpp
    bm/ibuffervector.cpp
    bm/mdl.cpp
    bm/netclientbufferimpl.cpp
    bm/nonpagedallocator.cpp
    bm/nonpagedbuffervector.cpp)

list(APPEND SOURCE_TRANSLATOR
    translator/nxbouncebufferpool.cpp
    translator/nxdma.cpp
    translator/nxnbldatapath.cpp
    translator/nxnblqueue.cpp
    translator/nxnbltranslation.cpp
    translator/nxoffload.cpp
    translator/nxoffloadv1.cpp
    translator/nxpacketlayout.cpp
    translator/nxringbuffer.cpp
    translator/nxringcontext.cpp
    translator/nxrxxlat.cpp
    translator/nxstatistics.cpp
    translator/nxtranslationapp.cpp
    translator/nxtxdemux.cpp
    translator/nxtxxlat.cpp
    translator/queue.cpp
    translator/queuecontrol.cpp
    translator/rxscaling.cpp
    translator/txpeeraddressdemux.cpp
    translator/txscaling.cpp
    translator/txuserprioritydemux.cpp
    translator/txwmminfodemux.cpp
    translator/checksum/checksum.cpp
    translator/checksum/km/checksumkm.cpp
    translator/coalescing/km/coalescingkm.cpp
    translator/framelayout/layer2layout.cpp
    translator/framelayout/layer3layout.cpp
    translator/framelayout/layer4layout.cpp
    translator/framelayout/layoutparser.cpp
    translator/metadata/metadatatranslator.cpp
    translator/segmentation/segmentation.cpp
    translator/segmentation/km/segmentationkm.cpp)

#
# The execution context and the histogram statistics are libraries upstream,
# shared between this driver and ec/driver. Only this driver is built here, so
# they are compiled straight in.
#
list(APPEND SOURCE_EC
    ec/lib/activitytracker.cpp
    ec/lib/dispatch.cpp
    ec/lib/executioncontext.cpp
    ec/lib/executioncontextstatistics.cpp
    ec/lib/executioncontexttask.cpp
    ec/lib/irqlcontroller.cpp
    ec/lib/notificationreserved.cpp
    ec/lib/pollreserved.cpp
    ec/lib/thread.cpp
    ec/lib/timebudget.cpp
    ec/lib/timebudgetwarninglogger.cpp
    statistics/histogram.cpp
    statistics/histogramapi.cpp
    statistics/histogramhub.cpp)

#
# WPP generates one .tmh per source file to hold the message descriptors it
# builds. The pass is not run here and the trace macros are defined away in
# inc/NxTrace.hpp, so the headers only have to exist. Scan the sources for the
# ones they ask for rather than guessing, because the names do not always match
# the file that includes them, and headers ask for them too.
#
set(NETCX_TMH_DIR ${CMAKE_CURRENT_BINARY_DIR}/tmh)
file(MAKE_DIRECTORY ${NETCX_TMH_DIR})
file(GLOB_RECURSE NETCX_TMH_SCAN
     ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
foreach(_source ${NETCX_TMH_SCAN})
    file(STRINGS ${_source} _tmh_includes
         REGEX "^#include \"[A-Za-z0-9_]+\.tmh\"")
    foreach(_line ${_tmh_includes})
        string(REGEX REPLACE "^#include \"(.*)\"" "\\1" _tmh "${_line}")
        if(NOT EXISTS ${NETCX_TMH_DIR}/${_tmh})
            file(WRITE ${NETCX_TMH_DIR}/${_tmh}
                 "/* WPP is not run in this tree, see inc/NxTrace.hpp */\n")
        endif()
    endforeach()
endforeach()

add_library(netadaptercx MODULE
    ${SOURCE_ADAPTER}
    ${SOURCE_BM}
    ${SOURCE_TRANSLATOR}
    ${SOURCE_EC}
    netadaptercx.rc)

set_module_type(netadaptercx kmdfdriver)
add_importlibs(netadaptercx ndis ntoskrnl hal)

target_include_directories(netadaptercx BEFORE PRIVATE
    ${NETCX_TMH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/adapter
    ${CMAKE_CURRENT_SOURCE_DIR}/ec/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/ec/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/statistics
    ${CMAKE_CURRENT_SOURCE_DIR}/translator
    ${REACTOS_SOURCE_DIR}/drivers/usb/usb3-reactos/inc
    ${REACTOS_SOURCE_DIR}/drivers/usb/usb3-reactos/inc/pci
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/shared/inc/private/common)

target_compile_definitions(netadaptercx PRIVATE
    _KERNEL_MODE=1
    NDIS_SUPPORT_NDIS680=1
    NDIS_WRAPPER=1)

add_cd_file(TARGET netadaptercx DESTINATION reactos/system32/drivers NO_CAB FOR all)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(netadaptercx
        PRIVATE -Wno-unknown-pragmas -Wno-unused-parameter -Wno-switch
        PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-reorder -Wno-invalid-offsetof -Wno-delete-non-virtual-dtor>)
endif()
