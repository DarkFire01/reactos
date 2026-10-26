@ stub BluetoothAddressToString
@ stub BluetoothDisconnectDevice
@ stdcall BluetoothEnableDiscovery(ptr long)
@ stdcall BluetoothEnableIncomingConnections(ptr long)
@ stdcall BluetoothEnumerateInstalledServices(ptr ptr ptr ptr)
@ stub BluetoothEnumerateInstalledServicesEx
@ stub BluetoothEnumerateLocalServices
@ stub BluetoothFindBrowseGroupClose
@ stub BluetoothFindClassIdClose
@ stdcall BluetoothFindDeviceClose(ptr)
@ stub BluetoothFindFirstBrowseGroup
@ stub BluetoothFindFirstClassId
@ stdcall BluetoothFindFirstDevice(ptr ptr)
@ stub BluetoothFindFirstProfileDescriptor
@ stub BluetoothFindFirstProtocolDescriptorStack
@ stub BluetoothFindFirstProtocolEntry
@ stdcall BluetoothFindFirstRadio(ptr ptr)
@ stub BluetoothFindFirstService
@ stub BluetoothFindFirstServiceEx
@ stub BluetoothFindNextBrowseGroup
@ stub BluetoothFindNextClassId
@ stdcall BluetoothFindNextDevice(ptr ptr)
@ stub BluetoothFindNextProfileDescriptor
@ stub BluetoothFindNextProtocolDescriptorStack
@ stub BluetoothFindNextProtocolEntry
@ stdcall BluetoothFindNextRadio(ptr ptr)
@ stub BluetoothFindNextService
@ stub BluetoothFindProfileDescriptorClose
@ stub BluetoothFindProtocolDescriptorStackClose
@ stub BluetoothFindProtocolEntryClose
@ stdcall BluetoothFindRadioClose(ptr)
@ stub BluetoothFindServiceClose
@ stub BluetoothGATTAbortReliableWrite
@ stub BluetoothGATTBeginReliableWrite
@ stub BluetoothGATTEndReliableWrite
@ stub BluetoothGATTGetCharacteristicValue
@ stub BluetoothGATTGetCharacteristics
@ stub BluetoothGATTGetDescriptorValue
@ stub BluetoothGATTGetDescriptors
@ stub BluetoothGATTGetIncludedServices
@ stub BluetoothGATTGetServices
@ stub BluetoothGATTRegisterEvent
@ stub BluetoothGATTSetCharacteristicValue
@ stub BluetoothGATTSetDescriptorValue
@ stub BluetoothGATTUnregisterEvent
@ stdcall BluetoothGetDeviceInfo(ptr ptr)
@ stub BluetoothGetLocalServiceInfo
@ stdcall BluetoothGetRadioInfo(ptr ptr)
@ stub BluetoothGetServicePnpInstance
@ stdcall BluetoothIsConnectable(ptr)
@ stdcall BluetoothIsDiscoverable(ptr)
@ stdcall BluetoothIsVersionAvailable(long long)
@ stdcall BluetoothRegisterForAuthentication(ptr ptr ptr ptr)
@ stdcall BluetoothRegisterForAuthenticationEx(ptr ptr ptr ptr)
@ stdcall BluetoothRemoveDevice(ptr)
@ stdcall BluetoothSdpEnumAttributes(ptr long ptr ptr)
@ stdcall BluetoothSdpGetAttributeValue(ptr long long ptr)
@ stdcall BluetoothSdpGetContainerElementData(ptr long ptr ptr)
@ stdcall BluetoothSdpGetElementData(ptr long ptr)
@ stub BluetoothSdpGetString
@ stdcall BluetoothSendAuthenticationResponse(ptr ptr wstr)
@ stdcall BluetoothSendAuthenticationResponseEx(ptr ptr)
@ stdcall BluetoothSetLocalServiceInfo(ptr ptr long ptr)
@ stdcall BluetoothSetServiceState(ptr ptr ptr long)
@ stub BluetoothSetServiceStateEx
@ stdcall BluetoothUnregisterAuthentication(ptr)
@ stdcall BluetoothUpdateDeviceRecord(ptr)
@ stub BthpCheckForUnsupportedGuid
@ stub BthpCleanupBRDeviceNode
@ stub BthpCleanupDeviceLocalServices
@ stub BthpCleanupDeviceRemoteServices
@ stub BthpCleanupLEDeviceNodes
@ stub BthpEnableAllServices
@ stub BthpEnableConnectableAndDiscoverable
@ stub BthpEnableRadioSoftware
@ stub BthpFindPnpInfo
@ stub BthpGATTCloseSession
@ stub BthpInnerRecord
@ stub BthpIsBluetoothServiceRunning
@ stub BthpIsConnectableByDefault
@ stub BthpIsDiscoverable
@ stub BthpIsDiscoverableByDefault
@ stub BthpIsRadioSoftwareEnabled
@ stub BthpIsTopOfServiceGroup
@ stub BthpMapStatusToErr
@ stub BthpNextRecord
@ stub BthpRegisterForAuthentication
@ stub BthpSetServiceState
@ stub BthpSetServiceStateEx
@ stub BthpTranspose16Bits
@ stub BthpTranspose32Bits
@ stub BthpTransposeAndExtendBytes
@ stdcall -private DllCanUnloadNow()
@ stub FindNextOpenVCOMPort
@ stub InstallIncomingComPort
@ stub ShouldForceAuthentication
