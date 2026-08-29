#ifndef _NETIOAPI_H_
#define _NETIOAPI_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _IPHLPAPI_H
    #define NETIO_STATUS DWORD
    #define NETIOAPI_API_ WINAPI
#else
    #define NETIO_STATUS NTSTATUS
    #define NETIOAPI_API_ NTAPI
#endif

#define NETIOAPI_API NETIO_STATUS NETIOAPI_API_

#ifdef _WS2IPDEF_
#include <ntddndis.h>
#include <ifdef.h>
#include <nldef.h>

NETIOAPI_API
ConvertInterfaceIndexToLuid(
  _In_ NET_IFINDEX InterfaceIndex,
  _Out_ PNET_LUID InterfaceLuid);

NETIOAPI_API
ConvertInterfaceLuidToIndex(
  _In_ const NET_LUID *InterfaceLuid,
  _Out_ PNET_IFINDEX InterfaceIndex);

NETIOAPI_API
ConvertInterfaceLuidToNameA(
  _In_ const NET_LUID *InterfaceLuid,
  _Out_writes_(Length) PSTR InterfaceName,
  _In_ SIZE_T Length);

NETIOAPI_API
ConvertInterfaceLuidToNameW(
  _In_ const NET_LUID *InterfaceLuid,
  _Out_writes_(Length) PWSTR InterfaceName,
  _In_ SIZE_T Length);

NETIOAPI_API
ConvertInterfaceNameToLuidA(
  _In_ const CHAR *InterfaceName,
  _Out_ PNET_LUID InterfaceLuid);

NETIOAPI_API
ConvertInterfaceNameToLuidW(
  _In_ const WCHAR *InterfaceName,
  _Out_ PNET_LUID InterfaceLuid);

typedef struct _MIB_IF_ROW2 {
    NET_LUID InterfaceLuid;
    NET_IFINDEX InterfaceIndex;
    GUID InterfaceGuid;
    WCHAR Alias[IF_MAX_STRING_SIZE + 1];
    WCHAR Description[IF_MAX_STRING_SIZE + 1];
    ULONG PhysicalAddressLength;
    UCHAR PhysicalAddress[IF_MAX_PHYS_ADDRESS_LENGTH];
    UCHAR PermanentPhysicalAddress[IF_MAX_PHYS_ADDRESS_LENGTH];
    ULONG Mtu;
    IFTYPE Type;
    TUNNEL_TYPE TunnelType;
    NDIS_MEDIUM MediaType;
    NDIS_PHYSICAL_MEDIUM PhysicalMediumType;
    NET_IF_ACCESS_TYPE AccessType;
    NET_IF_DIRECTION_TYPE DirectionType;
    struct {
        BOOLEAN HardwareInterface : 1;
        BOOLEAN FilterInterface : 1;
        BOOLEAN ConnectorPresent : 1;
        BOOLEAN NotAuthenticated : 1;
        BOOLEAN NotMediaConnected : 1;
        BOOLEAN Paused : 1;
        BOOLEAN LowPower : 1;
        BOOLEAN EndPointInterface : 1;
    } InterfaceAndOperStatusFlags;
    IF_OPER_STATUS OperStatus;
    NET_IF_ADMIN_STATUS AdminStatus;
    NET_IF_MEDIA_CONNECT_STATE MediaConnectState;
    NET_IF_NETWORK_GUID NetworkGuid;
    NET_IF_CONNECTION_TYPE ConnectionType;
    ULONG64 TransmitLinkSpeed;
    ULONG64 ReceiveLinkSpeed;
    ULONG64 InOctets;
    ULONG64 InUcastPkts;
    ULONG64 InNUcastPkts;
    ULONG64 InDiscards;
    ULONG64 InErrors;
    ULONG64 InUnknownProtos;
    ULONG64 InUcastOctets;
    ULONG64 InMulticastOctets;
    ULONG64 InBroadcastOctets;
    ULONG64 OutOctets;
    ULONG64 OutUcastPkts;
    ULONG64 OutNUcastPkts;
    ULONG64 OutDiscards;
    ULONG64 OutErrors;
    ULONG64 OutUcastOctets;
    ULONG64 OutMulticastOctets;
    ULONG64 OutBroadcastOctets;
    ULONG64 OutQLen;
} MIB_IF_ROW2, *PMIB_IF_ROW2;

typedef struct _MIB_IF_TABLE2
{
    ULONG NumEntries;
    MIB_IF_ROW2 Table[1];
} MIB_IF_TABLE2, *PMIB_IF_TABLE2;

typedef enum _MIB_IF_TABLE_LEVEL {
    MibIfTableNormal,
    MibIfTableRaw,
    MibIfTableNormalWithoutStatistics
} MIB_IF_TABLE_LEVEL, *PMIB_IF_TABLE_LEVEL;

NETIOAPI_API GetIfEntry2(IN OUT PMIB_IF_ROW2 Row);
NETIOAPI_API GetIfTable2(OUT PMIB_IF_TABLE2 *Table);
NETIOAPI_API GetIfTable2Ex(IN MIB_IF_TABLE_LEVEL Level, OUT PMIB_IF_TABLE2 *Table);


#define IF_NAMESIZE (IF_MAX_STRING_SIZE + 1)

typedef enum _MIB_NOTIFICATION_TYPE {
    MibParameterNotification,
    MibAddInstance,
    MibDeleteInstance,
    MibInitialNotification
} MIB_NOTIFICATION_TYPE, *PMIB_NOTIFICATION_TYPE;

typedef struct _MIB_IPINTERFACE_ROW {
    ADDRESS_FAMILY Family;
    NET_LUID InterfaceLuid;
    NET_IFINDEX InterfaceIndex;
    ULONG MaxReassemblySize;
    ULONG64 InterfaceIdentifier;
    ULONG MinRouterAdvertisementInterval;
    ULONG MaxRouterAdvertisementInterval;
    BOOLEAN AdvertisingEnabled;
    BOOLEAN ForwardingEnabled;
    BOOLEAN WeakHostSend;
    BOOLEAN WeakHostReceive;
    BOOLEAN UseAutomaticMetric;
    BOOLEAN UseNeighborUnreachabilityDetection;
    BOOLEAN ManagedAddressConfigurationSupported;
    BOOLEAN OtherStatefulConfigurationSupported;
    BOOLEAN AdvertiseDefaultRoute;
    NL_ROUTER_DISCOVERY_BEHAVIOR RouterDiscoveryBehavior;
    ULONG DadTransmits;
    ULONG BaseReachableTime;
    ULONG RetransmitTime;
    ULONG PathMtuDiscoveryTimeout;
    NL_LINK_LOCAL_ADDRESS_BEHAVIOR LinkLocalAddressBehavior;
    ULONG LinkLocalAddressTimeout;
    ULONG ZoneIndices[ScopeLevelCount];
    ULONG SitePrefixLength;
    ULONG Metric;
    ULONG NlMtu;
    BOOLEAN Connected;
    BOOLEAN SupportsWakeUpPatterns;
    BOOLEAN SupportsNeighborDiscovery;
    BOOLEAN SupportsRouterDiscovery;
    ULONG ReachableTime;
    NL_INTERFACE_OFFLOAD_ROD TransmitOffload;
    NL_INTERFACE_OFFLOAD_ROD ReceiveOffload;
    BOOLEAN DisableDefaultRoutes;
} MIB_IPINTERFACE_ROW, *PMIB_IPINTERFACE_ROW;

typedef struct _MIB_IPINTERFACE_TABLE {
    ULONG NumEntries;
    MIB_IPINTERFACE_ROW Table[1];
} MIB_IPINTERFACE_TABLE, *PMIB_IPINTERFACE_TABLE;

typedef struct _MIB_UNICASTIPADDRESS_ROW {
    SOCKADDR_INET Address;
    NET_LUID InterfaceLuid;
    NET_IFINDEX InterfaceIndex;
    NL_PREFIX_ORIGIN PrefixOrigin;
    NL_SUFFIX_ORIGIN SuffixOrigin;
    ULONG ValidLifetime;
    ULONG PreferredLifetime;
    UINT8 OnLinkPrefixLength;
    BOOLEAN SkipAsSource;
    NL_DAD_STATE DadState;
    SCOPE_ID ScopeId;
    LARGE_INTEGER CreationTimeStamp;
} MIB_UNICASTIPADDRESS_ROW, *PMIB_UNICASTIPADDRESS_ROW;

typedef struct _MIB_UNICASTIPADDRESS_TABLE {
    ULONG NumEntries;
    MIB_UNICASTIPADDRESS_ROW Table[1];
} MIB_UNICASTIPADDRESS_TABLE, *PMIB_UNICASTIPADDRESS_TABLE;

typedef struct _IP_ADDRESS_PREFIX {
    SOCKADDR_INET Prefix;
    UINT8 PrefixLength;
} IP_ADDRESS_PREFIX, *PIP_ADDRESS_PREFIX;

typedef struct _MIB_IPFORWARD_ROW2 {
    NET_LUID InterfaceLuid;
    NET_IFINDEX InterfaceIndex;
    IP_ADDRESS_PREFIX DestinationPrefix;
    SOCKADDR_INET NextHop;
    UCHAR SitePrefixLength;
    ULONG ValidLifetime;
    ULONG PreferredLifetime;
    ULONG Metric;
    NL_ROUTE_PROTOCOL Protocol;
    BOOLEAN Loopback;
    BOOLEAN AutoconfigureAddress;
    BOOLEAN Publish;
    BOOLEAN Immortal;
    ULONG Age;
    NL_ROUTE_ORIGIN Origin;
} MIB_IPFORWARD_ROW2, *PMIB_IPFORWARD_ROW2;

typedef struct _MIB_IPFORWARD_TABLE2 {
    ULONG NumEntries;
    MIB_IPFORWARD_ROW2 Table[1];
} MIB_IPFORWARD_TABLE2, *PMIB_IPFORWARD_TABLE2;

typedef VOID (WINAPI *PIPINTERFACE_CHANGE_CALLBACK)(
    PVOID CallerContext,
    PMIB_IPINTERFACE_ROW Row,
    MIB_NOTIFICATION_TYPE NotificationType);

typedef VOID (WINAPI *PUNICAST_IPADDRESS_CHANGE_CALLBACK)(
    PVOID CallerContext,
    PMIB_UNICASTIPADDRESS_ROW Row,
    MIB_NOTIFICATION_TYPE NotificationType);

typedef VOID (WINAPI *PIPFORWARD_CHANGE_CALLBACK)(
    PVOID CallerContext,
    PMIB_IPFORWARD_ROW2 Row,
    MIB_NOTIFICATION_TYPE NotificationType);

VOID NETIOAPI_API_ FreeMibTable(IN PVOID Memory);

NETIOAPI_API GetIpInterfaceTable(
    IN ADDRESS_FAMILY Family,
    OUT PMIB_IPINTERFACE_TABLE *Table);

NETIOAPI_API GetIpForwardTable2(
    IN ADDRESS_FAMILY Family,
    OUT PMIB_IPFORWARD_TABLE2 *Table);

NETIOAPI_API GetBestRoute2(
    IN NET_LUID *InterfaceLuid OPTIONAL,
    IN NET_IFINDEX InterfaceIndex,
    IN CONST SOCKADDR_INET *SourceAddress OPTIONAL,
    IN CONST SOCKADDR_INET *DestinationAddress,
    IN ULONG AddressSortOptions,
    OUT PMIB_IPFORWARD_ROW2 BestRoute,
    OUT SOCKADDR_INET *BestSourceAddress OPTIONAL);

NETIOAPI_API NotifyIpInterfaceChange(
    IN ADDRESS_FAMILY Family,
    IN PIPINTERFACE_CHANGE_CALLBACK Callback,
    IN PVOID CallerContext OPTIONAL,
    IN BOOLEAN InitialNotification,
    IN OUT HANDLE *NotificationHandle);

NETIOAPI_API NotifyUnicastIpAddressChange(
    IN ADDRESS_FAMILY Family,
    IN PUNICAST_IPADDRESS_CHANGE_CALLBACK Callback,
    IN PVOID CallerContext OPTIONAL,
    IN BOOLEAN InitialNotification,
    IN OUT HANDLE *NotificationHandle);

NETIOAPI_API NotifyRouteChange2(
    IN ADDRESS_FAMILY Family,
    IN PIPFORWARD_CHANGE_CALLBACK Callback,
    IN PVOID CallerContext OPTIONAL,
    IN BOOLEAN InitialNotification,
    IN OUT HANDLE *NotificationHandle);

NETIOAPI_API CancelMibChangeNotify2(IN HANDLE NotificationHandle);

NET_IFINDEX NETIOAPI_API_ if_nametoindex(IN PCSTR InterfaceName);
PCHAR NETIOAPI_API_ if_indextoname(IN NET_IFINDEX InterfaceIndex,
                                   OUT PCHAR InterfaceName);

#endif

#ifdef __cplusplus
}
#endif

#endif
