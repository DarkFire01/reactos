; The NT 6.2 USB stack imports this driver by ordinal, so the numbering below
; has to match the usbd.sys it was built against. Ordinals 1 to 3 are the
; decorated aliases kept for old drivers, the rest follow in name order. The
; two DllXxx entries land on 4 and 5, which is where Windows has them.

1 stdcall -arch=i386 _USBD_CreateConfigurationRequestEx@8(ptr ptr) USBD_CreateConfigurationRequestEx
2 stdcall -arch=i386 _USBD_ParseConfigurationDescriptorEx@28(ptr ptr long long long long long) USBD_ParseConfigurationDescriptorEx
3 stdcall -arch=i386 _USBD_ParseDescriptors@16(ptr long ptr long) USBD_ParseDescriptors
@ stdcall -private DllInitialize(ptr)
@ stdcall -private DllUnload()
6 stdcall USBD_AddDeviceToGlobalList(long ptr long ptr long long long)
7 stdcall USBD_AllocateHubNumber()
8 stdcall USBD_CalculateUsbBandwidth(long long long)
9 stdcall USBD_CreateConfigurationRequest(ptr ptr)
10 stdcall USBD_CreateConfigurationRequestEx(ptr ptr)
11 stdcall USBD_GetInterfaceLength(ptr ptr)
12 stdcall USBD_GetPdoRegistryParameter(ptr ptr long ptr long)
13 stdcall USBD_GetRegistryKeyValue(ptr wstr long ptr long)
14 stdcall USBD_GetUSBDIVersion(ptr)
15 stdcall USBD_MarkDeviceAsDisconnected(ptr)
16 stdcall USBD_ParseConfigurationDescriptor(ptr long long)
17 stdcall USBD_ParseConfigurationDescriptorEx(ptr ptr long long long long long)
18 stdcall USBD_ParseDescriptors(ptr long ptr long)
19 stdcall USBD_QueryBusTime(ptr ptr)
20 stdcall USBD_RegisterHcFilter(ptr ptr)
21 stdcall USBD_ReleaseHubNumber(long)
22 stdcall USBD_RemoveDeviceFromGlobalList(ptr)
23 stdcall USBD_ValidateConfigurationDescriptor(ptr long long ptr long)

; Windows does not export these, the ReactOS USB stack does use them
@ stdcall USBD_Debug_GetHeap(long long long long)
@ stdcall USBD_Debug_RetHeap(ptr long long)
@ stdcall USBD_Debug_LogEntry(ptr ptr ptr ptr)
