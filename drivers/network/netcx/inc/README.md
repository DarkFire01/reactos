# netcx/inc

Headers that belong to `netadaptercx` itself, or that stand in for something
Microsoft links from a source tree nobody else has. Anything a client driver
also needs lives in `sdk/include/ddk` instead, not here.

Microsoft's Network-Adapter-Class-Extension drop ships implementation files
only, so neither `netadaptercx` nor `drivers/network/dd/rtethsample` compiles
until every header it includes exists somewhere. 52 includes across the drop
still resolve to nothing.

`NdisBringupDocs/NetCxFrontier.md` in the workspace tracks that number and what
is behind it.

## Already in the sdk

Reconstructed and moved out of here, because a client driver needs them too:

    net/ (27, plus the 20 private halves)  netcx/ (17)
    ndis/offload.h  ndis/powermanagement.h  ndis/receivescale.h
    ndis/oid.h  ndis/oidrequest.h  ndis/statusindication.h
    ndis/statusconvert.h  ndis/types.h
    ndiswdf.h  ndis_p.h  ntddndis_p.h  rsclib.h  seglib.h  batchinglib.h

`ndiswdf.h` is the NDIS side of the binding, so it is a specification for work
in `drivers/network/ndis` as much as a header: `NdisWdfRegisterCx` and
`NdisWdfRegisterMiniportDriver` still do not exist there.

The libraries behind `rsclib.h`, `seglib.h` and `batchinglib.h` are stubbed
under `sdk/lib/drivers/rsclib` and `sdk/lib/drivers/seglib`.

## Here

Stand-ins written against how the drop calls them:

    NxTrace.hpp  NxTraceLogging.hpp  NetClientBuffer.h
    NetClientDriverConfigurationConstants.h  NdisStatisticalIoctls.h
    KString.h  KStackStorage.h  KStopwatch.h  KMemoryAccess.h  EcEvents.h
    ntassert.h  pooltypes.h  umwdm.h  wdfcxbase.h  new.h  cstddef
    nt/rtl/integermacro.h

`wil/` is the kernel-mode subset of github.com/microsoft/wil, MIT:

    wil/common.h  wil/resource.h  wil/wistd_memory.h  wil/wistd_type_traits.h

## Still missing

No public source exists for these, so each one has to be recovered from how the
drop calls it:

    NxApi.hpp  NxCollection.hpp  NxXlat.hpp  NxXlatTraceLogging.hpp
    NxApp.hpp  NxNbl.hpp  NxNblDatapath.hpp  NxPerfTuner.hpp  Driver.hpp
    netadaptercx_triage.h  NetClientApi.h  NetClientAdapter.h
    NetClientQueue.h  NetClientTypes.h  NetClientBufferImpl.h
    NetClientDriverConfigurationImpl.hpp  NetAdapterCxPc.h
    Net20.h  Net21.h  Net22.h  smfx.h  FxObjectBase.hpp
    pktmonclnt.h  pktmonloc.h  pcwdata.h  KLoaderApi.h  affinity.h
    80211hdr.h  wmi/NetDevice.h  nt/rtl/failfast.h

`NxApi.hpp` is generated from `netfuncenum.h` and carries the exported function
table `adapter/version.cpp` hands to a binding client, so its entry order is the
client ABI. `NetClient*.h` are the internal interface between the adapter half
and the translator half. `smfx.h` is the state machine framework runtime: the
generated tables in `adapter/statemachines` are in the drop, the engine they
call is not.

The user-mode only ones are reached with `_KERNEL_MODE` undefined and can stay
missing: `mockdma.h`, `NdisUm.h`, `UmPool.h`.

## Not headers

75 source files include a generated `.tmh`. The CMakeLists writes an empty stub
for each at configure time; real WPP needs `tracewpp` in the build.

`adapter/version.cpp` and `ec/driver/driver.cpp` both call TraceLogging
(`TRACELOGGING_DEFINE_PROVIDER`), which lands on ETW. Stub or route to
`DbgPrintEx`.

## Out of scope for now

`ec/driver/driver.cpp` builds a second binary upstream, the execution context
module. It binds through `KLoaderApi.h` and is left out of the CMakeLists;
`ec/lib` is compiled into `netadaptercx` instead.
