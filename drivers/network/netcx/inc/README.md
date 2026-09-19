# netcx/inc

Shared headers for `netadaptercx` and its clients (`drivers/network/dd/rtethsample`).

Microsoft's Network-Adapter-Class-Extension drop ships implementation files only.
Neither driver compiles until the headers below land here. 173 angle-bracket
includes were counted across both trees; 121 of them resolve to nothing in this
tree today.

## Reachable already

These live in `drivers/usb/usb3-reactos/inc` and `inc/pci`, which both
CMakeLists already put on the include path:

    nt.h  ntosp.h  ntrtl.h  zwapi.h  wdmsec.h  WppRecorder.h

## WDK public headers

Part of the WDK NetAdapterCx SDK. They describe the packet ring ABI and the
NBL helpers, so they set the binary contract between the class extension, the
miniport and ndis.sys. Get these right first, the rest follows.

`net/` (41):

    checksum.h  checksum_p.h  checksumtypes.h  checksumtypes_p.h
    databuffer_p.h  extension.h  fragment.h  gso.h  gso_p.h  gsotypes.h
    gsotypes_p.h  ieee8021q.h  ieee8021q_p.h  ieee8021qtypes.h
    ieee8021qtypes_p.h  logicaladdress.h  logicaladdress_p.h
    logicaladdresstypes_p.h  lso.h  mdl_p.h  mdltypes_p.h  packet.h
    packethash.h  packethash_p.h  packethashtypes.h  packethashtypes_p.h
    returncontext_p.h  returncontexttypes_p.h  ring.h  ringcollection.h
    rsc.h  rsc_p.h  rsctypes.h  rsctypes_p.h  virtualaddress.h
    virtualaddress_p.h  virtualaddresstypes_p.h
    wifi/exemptionaction.h  wifi/exemptionaction_p.h
    wifi/exemptionactiontypes.h  wifi/exemptionactiontypes_p.h

`ndis/` (13):

    encapsulationconfig.h  nbl.h  nbl8021q.h  nblaccessors.h  nblapi.h
    nblchain.h  nblqueue.h  nblreceive.h  nblrsc.h  nblsend.h
    oidrequest.h  statusconvert.h  types.h

Class extension surface:

    netadaptercx.h  netadapter_p.h  netadapterextension_p.h
    netadapteroffload.h  netbufferqueue_p.h  netdevice_p.h  netfuncenum.h
    preview/netadapter.h  preview/netadaptercx.h  preview/netadapteroffload.h

Rest:

    ndiswdf.h  ndis_p.h  ntddndis_p.h  wdfcxbase.h  smfx.h
    TraceLoggingProvider.h  pcwdata.h  pooltypes.h  ntassert.h  ntdef.h
    nturtl.h  umwdm.h  FxObjectBase.hpp

`ndiswdf.h` is the NDIS side of the binding. `NdisWdfRegisterCx` and
`NdisWdfRegisterMiniportDriver` do not exist in ndis.sys here, so that header
is a specification for work in `drivers/network/ndis`, not just a copy job.

`smfx.h` is the state machine framework runtime. The generated state tables in
`adapter/statemachines` are in the drop; the engine they call is not.

## Never published

No public source exists for these. They have to be reconstructed from how the
drop calls them.

    NxApi.hpp  NxCollection.hpp  NxTrace.hpp  NxTraceLogging.hpp
    NxXlat.hpp  NxXlatTraceLogging.hpp  NxApp.hpp  NxNbl.hpp
    NxNblDatapath.hpp  NxPerfTuner.hpp  Driver.hpp  netadaptercx_triage.h
    NetClientApi.h  NetClientAdapter.h  NetClientQueue.h  NetClientBuffer.h
    NetClientBufferImpl.h  NetClientTypes.h
    NetClientDriverConfigurationConstants.h
    NetClientDriverConfigurationImpl.hpp  NetAdapterCxPc.h
    KString.h  KStackStorage.h  KStopwatch.h  KLoaderApi.h  KMemoryAccess.h
    BatchingLib.h  SegLib.h  rsclib.h  EcEvents.h
    pktmonclnt.h  pktmonloc.h  Net20.h  Net21.h  Net22.h
    80211hdr.h  NdisStatisticalIoctls.h  wmi/NetDevice.h  affinity.h
    nt/rtl/failfast.h  nt/rtl/integermacro.h

`NxApi.hpp` is generated from `netfuncenum.h` and carries the exported function
table that `adapter/version.cpp` hands to a binding client, so its entry order
is the client ABI. `NetClient*.h` are the internal interface between the
adapter half and the translator half.

The user-mode ones are only reached when `_KERNEL_MODE` is undefined and can be
skipped: `mockdma.h`, `NdisUm.h`, `UmPool.h`.

## wil

Open source at github.com/microsoft/wil, MIT. Only the kernel-mode subset is
used:

    wil/common.h  wil/resource.h  wil/wistd_type_traits.h

## Not headers

75 source files include a generated `.tmh`. WPP needs `tracewpp` in the build,
or the `WPP_INIT_TRACING` / `LogError` macros need stubbing out.

`adapter/version.cpp` and `ec/driver/driver.cpp` both call TraceLogging
(`TRACELOGGING_DEFINE_PROVIDER`), which lands on ETW. Stub or route to
`DbgPrintEx`.

## Out of scope for now

`ec/driver/driver.cpp` builds a second binary upstream, the execution context
module. It binds through `KLoaderApi.h` and is left out of the CMakeLists;
`ec/lib` is compiled into `netadaptercx` instead.
