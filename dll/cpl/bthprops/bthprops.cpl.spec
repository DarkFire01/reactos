@ stub BluetoothAddressToString
@ stdcall BluetoothAuthenticateDevice(ptr ptr ptr wstr long)
@ stdcall BluetoothAuthenticateDeviceEx(ptr ptr ptr ptr long)
@ stdcall BluetoothAuthenticateMultipleDevices(ptr ptr long ptr)
@ stub BluetoothAuthenticationAgent
@ stub BluetoothDisconnectDevice
@ stdcall BluetoothDisplayDeviceProperties(ptr ptr)
@ stdcall BluetoothEnableDiscovery(ptr long) bluetoothapis.BluetoothEnableDiscovery
@ stdcall BluetoothEnableIncomingConnections(ptr long) bluetoothapis.BluetoothEnableIncomingConnections
@ stdcall BluetoothEnumerateInstalledServices(ptr ptr ptr ptr) bluetoothapis.BluetoothEnumerateInstalledServices
@ stub BluetoothEnumerateInstalledServicesEx
@ stub BluetoothFindBrowseGroupClose
@ stub BluetoothFindClassIdClose
@ stdcall BluetoothFindDeviceClose(ptr) bluetoothapis.BluetoothFindDeviceClose
@ stub BluetoothFindFirstBrowseGroup
@ stub BluetoothFindFirstClassId
@ stdcall BluetoothFindFirstDevice(ptr ptr) bluetoothapis.BluetoothFindFirstDevice
@ stub BluetoothFindFirstProfileDescriptor
@ stub BluetoothFindFirstProtocolDescriptorStack
@ stub BluetoothFindFirstProtocolEntry
@ stdcall BluetoothFindFirstRadio(ptr ptr) bluetoothapis.BluetoothFindFirstRadio
@ stub BluetoothFindFirstService
@ stub BluetoothFindFirstServiceEx
@ stub BluetoothFindNextBrowseGroup
@ stub BluetoothFindNextClassId
@ stdcall BluetoothFindNextDevice(ptr ptr) bluetoothapis.BluetoothFindNextDevice
@ stub BluetoothFindNextProfileDescriptor
@ stub BluetoothFindNextProtocolDescriptorStack
@ stub BluetoothFindNextProtocolEntry
@ stdcall BluetoothFindNextRadio(ptr ptr) bluetoothapis.BluetoothFindNextRadio
@ stub BluetoothFindNextService
@ stub BluetoothFindProfileDescriptorClose
@ stub BluetoothFindProtocolDescriptorStackClose
@ stub BluetoothFindProtocolEntryClose
@ stdcall BluetoothFindRadioClose(ptr) bluetoothapis.BluetoothFindRadioClose
@ stub BluetoothFindServiceClose
@ stdcall BluetoothGetDeviceInfo(ptr ptr) bluetoothapis.BluetoothGetDeviceInfo
@ stdcall BluetoothGetRadioInfo(ptr ptr) bluetoothapis.BluetoothGetRadioInfo
@ stdcall BluetoothIsConnectable(ptr) bluetoothapis.BluetoothIsConnectable
@ stdcall BluetoothIsDiscoverable(ptr) bluetoothapis.BluetoothIsDiscoverable
@ stdcall BluetoothIsVersionAvailable(long long) bluetoothapis.BluetoothIsVersionAvailable
@ stub BluetoothMapClassOfDeviceToImageIndex
@ stub BluetoothMapClassOfDeviceToString
@ stdcall BluetoothRegisterForAuthentication(ptr ptr ptr ptr) bluetoothapis.BluetoothRegisterForAuthentication
@ stdcall BluetoothRegisterForAuthenticationEx(ptr ptr ptr ptr) bluetoothapis.BluetoothRegisterForAuthenticationEx
@ stdcall BluetoothRemoveDevice(ptr) bluetoothapis.BluetoothRemoveDevice
@ stdcall BluetoothSdpEnumAttributes(ptr long ptr ptr) bluetoothapis.BluetoothSdpEnumAttributes
@ stdcall BluetoothSdpGetAttributeValue(ptr long long ptr) bluetoothapis.BluetoothSdpGetAttributeValue
@ stdcall BluetoothSdpGetContainerElementData(ptr long ptr ptr) bluetoothapis.BluetoothSdpGetContainerElementData
@ stdcall BluetoothSdpGetElementData(ptr long ptr) bluetoothapis.BluetoothSdpGetElementData
@ stub BluetoothSdpGetString
@ stdcall BluetoothSelectDevices(ptr)
@ stdcall BluetoothSelectDevicesFree(ptr)
@ stdcall BluetoothSendAuthenticationResponse(ptr ptr wstr) bluetoothapis.BluetoothSendAuthenticationResponse
@ stdcall BluetoothSendAuthenticationResponseEx(ptr ptr) bluetoothapis.BluetoothSendAuthenticationResponseEx
@ stdcall BluetoothSetLocalServiceInfo(ptr ptr long ptr) bluetoothapis.BluetoothSetLocalServiceInfo
@ stdcall BluetoothSetServiceState(ptr ptr ptr long) bluetoothapis.BluetoothSetServiceState
@ stdcall BluetoothUnregisterAuthentication(ptr) bluetoothapis.BluetoothUnregisterAuthentication
@ stdcall BluetoothUpdateDeviceRecord(ptr) bluetoothapis.BluetoothUpdateDeviceRecord
@ stub BthpEnableAllServices
@ stub BthpFindPnpInfo
@ stub BthpMapStatusToErr
@ stdcall CPlApplet(ptr long ptr ptr)
@ stdcall -private DllCanUnloadNow()
@ stub DllGetClassObject
