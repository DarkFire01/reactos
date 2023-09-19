@ stdcall D3DKMTCreateDCFromMemory(ptr)
@ stdcall D3DKMTDestroyDCFromMemory(ptr)

;New
@ stdcall D3DKMTCloseAdapter(ptr)
@ stdcall D3DKMTCreateDevice(ptr)
@ stdcall D3DKMTDestroyDevice(ptr)
@ stdcall D3DKMTOpenAdapterFromGdiDisplayName(ptr)
@ stdcall D3DKMTOpenAdapterFromLuid(ptr)
@ stdcall D3DKMTQueryVideoMemoryInfo(ptr)
@ stdcall D3DKMTSetVidPnSourceOwner(ptr)