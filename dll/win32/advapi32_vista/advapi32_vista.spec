
@ stdcall RegCopyTreeW(ptr wstr ptr)
@ stdcall RegDeleteTreeA(long str)
@ stdcall RegDeleteTreeW(long wstr)
@ stdcall RegSetKeyValueW(long wstr wstr long ptr long)
@ stdcall RegLoadMUIStringW(ptr wstr wstr long ptr long wstr)
@ stdcall RegLoadMUIStringA(ptr str str long ptr long str)

@ stdcall EventActivityIdControl(long ptr) ntdll_vista.EtwEventActivityIdControl
@ stdcall EventEnabled(int64 ptr) ntdll_vista.EtwEventEnabled
@ stdcall EventProviderEnabled(int64 long int64) ntdll_vista.EtwEventProviderEnabled
@ stdcall EventRegister(ptr ptr ptr ptr) ntdll_vista.EtwEventRegister
@ stdcall EventSetInformation(int64 long ptr long) ntdll_vista.EtwEventSetInformation
@ stdcall EventUnregister(int64) ntdll_vista.EtwEventUnregister
@ stdcall EventWrite(int64 ptr long ptr) ntdll_vista.EtwEventWrite
@ stdcall EventWriteEx(int64 ptr int64 long ptr ptr long ptr) ntdll_vista.EtwEventWriteEx
@ stdcall EventWriteString(int64 long int64 wstr) ntdll_vista.EtwEventWriteString
@ stdcall EventWriteTransfer(int64 ptr ptr ptr long ptr) ntdll_vista.EtwEventWriteTransfer
