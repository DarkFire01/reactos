@ stdcall BCryptCloseAlgorithmProvider(ptr long) ksecdd.BCryptCloseAlgorithmProvider
@ stdcall BCryptCreateHash(ptr ptr ptr long ptr long long) ksecdd.BCryptCreateHash
@ stdcall BCryptDecrypt(ptr ptr long ptr ptr long ptr long ptr long) ksecdd.BCryptDecrypt
@ stdcall BCryptDestroyHash(ptr) ksecdd.BCryptDestroyHash
@ stdcall BCryptDestroyKey(ptr) ksecdd.BCryptDestroyKey
@ stdcall BCryptEncrypt(ptr ptr long ptr ptr long ptr long ptr long) ksecdd.BCryptEncrypt
@ stdcall BCryptExportKey(ptr ptr wstr ptr long ptr long) ksecdd.BCryptExportKey
@ stdcall BCryptFinalizeKeyPair(ptr long) ksecdd.BCryptFinalizeKeyPair
@ stdcall BCryptFinishHash(ptr ptr long long) ksecdd.BCryptFinishHash
@ stdcall BCryptGenerateKeyPair(ptr ptr long long) ksecdd.BCryptGenerateKeyPair
@ stdcall BCryptGenerateSymmetricKey(ptr ptr ptr long ptr long long) ksecdd.BCryptGenerateSymmetricKey
@ stdcall BCryptGenRandom(ptr ptr long long) ksecdd.BCryptGenRandom
@ stdcall BCryptGetProperty(ptr wstr ptr long ptr long) ksecdd.BCryptGetProperty
@ stdcall BCryptHashData(ptr ptr long long) ksecdd.BCryptHashData
@ stdcall BCryptImportKeyPair(ptr ptr wstr ptr ptr long long) ksecdd.BCryptImportKeyPair
@ stdcall BCryptOpenAlgorithmProvider(ptr wstr wstr long) ksecdd.BCryptOpenAlgorithmProvider
@ stdcall BCryptSetProperty(ptr wstr ptr long long) ksecdd.BCryptSetProperty
@ stdcall BCryptVerifySignature(ptr ptr ptr long ptr long long) ksecdd.BCryptVerifySignature
@ stdcall EntropyProvideData(ptr ptr long long) ksecdd.EntropyProvideData
@ stdcall EntropyRegisterCallback(ptr ptr ptr) ksecdd.EntropyRegisterCallback
@ stdcall EntropyRegisterSource(ptr long ptr) ksecdd.EntropyRegisterSource
@ stdcall EntropyUnregisterSource(ptr) ksecdd.EntropyUnregisterSource
