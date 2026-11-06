@ stdcall ProcessInit()
@ stdcall ProcessTerm(ptr long long)
@ stdcall ThreadInit()
@ stdcall ThreadTerm(ptr long)
@ stdcall FlushInstructionCacheHeavy(ptr long)
@ stdcall BTCpu64FlushInstructionCache(ptr long)
@ stdcall BTCpu64NotifyMemoryDirty(ptr long)
@ stdcall BTCpu64IsProcessorFeaturePresent(long)
@ extern BeginSimulation
@ extern DispatchJump
@ extern ExitToX64
@ extern RetToEntryThunk
