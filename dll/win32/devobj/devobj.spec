@ stdcall DevObjChangeState(ptr ptr) setupapi.SetupDiChangeState
@ stdcall DevObjCreateDeviceInfoList(ptr ptr) setupapi.SetupDiCreateDeviceInfoList
@ stdcall DevObjDestroyDeviceInfoList(ptr) setupapi.SetupDiDestroyDeviceInfoList
@ stdcall DevObjEnumDeviceInfo(ptr long ptr) setupapi.SetupDiEnumDeviceInfo
@ stdcall DevObjEnumDeviceInterfaces(ptr ptr ptr long ptr) setupapi.SetupDiEnumDeviceInterfaces
@ stdcall DevObjGetClassDevs(ptr wstr ptr long) setupapi.SetupDiGetClassDevsW
@ stdcall DevObjGetDeviceInterfaceDetail(ptr ptr ptr long ptr ptr) setupapi.SetupDiGetDeviceInterfaceDetailW
@ stdcall DevObjGetDeviceProperty(ptr ptr ptr ptr ptr long ptr long) setupapi.SetupDiGetDevicePropertyW
@ stdcall DevObjOpenDevRegKey(ptr ptr long long long long) setupapi.SetupDiOpenDevRegKey
