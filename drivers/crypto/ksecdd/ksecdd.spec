@ stdcall AcceptSecurityContext(ptr ptr ptr long long ptr ptr ptr ptr)
@ stdcall AcquireCredentialsHandleW(ptr ptr long ptr ptr long ptr ptr ptr)
@ stdcall AddCredentialsW(ptr ptr ptr long ptr long ptr ptr)
@ stdcall ApplyControlToken(ptr ptr)
@ stdcall BCryptCloseAlgorithmProvider(ptr long) cng.BCryptCloseAlgorithmProvider
@ stdcall BCryptCreateHash(ptr ptr ptr long ptr long long) cng.BCryptCreateHash
@ stdcall BCryptDecapsulate(ptr ptr long ptr long ptr long) cng.BCryptDecapsulate
@ stdcall BCryptDecrypt(ptr ptr long ptr ptr long ptr long ptr long) cng.BCryptDecrypt
@ stdcall BCryptDeriveKey(ptr wstr ptr ptr long ptr long) cng.BCryptDeriveKey
@ stdcall BCryptDeriveKeyCapi(ptr ptr ptr long long) cng.BCryptDeriveKeyCapi
@ stdcall BCryptDeriveKeyPBKDF2(ptr ptr long ptr long int64 ptr long long) cng.BCryptDeriveKeyPBKDF2
@ stdcall BCryptDestroyHash(ptr) cng.BCryptDestroyHash
@ stdcall BCryptDestroyKey(ptr) cng.BCryptDestroyKey
@ stdcall BCryptDestroySecret(ptr) cng.BCryptDestroySecret
@ stdcall BCryptDuplicateHash(ptr ptr ptr long long) cng.BCryptDuplicateHash
@ stdcall BCryptDuplicateKey(ptr ptr ptr long long) cng.BCryptDuplicateKey
@ stdcall BCryptEncapsulate(ptr ptr long ptr ptr long ptr long) cng.BCryptEncapsulate
@ stdcall BCryptEncrypt(ptr ptr long ptr ptr long ptr long ptr long) cng.BCryptEncrypt
@ stdcall BCryptEnumAlgorithms(long ptr ptr long) cng.BCryptEnumAlgorithms
@ stdcall BCryptEnumProviders(wstr ptr ptr long) cng.BCryptEnumProviders
@ stdcall BCryptExportKey(ptr ptr wstr ptr long ptr long) cng.BCryptExportKey
@ stdcall BCryptFinalizeKeyPair(ptr long) cng.BCryptFinalizeKeyPair
@ stdcall BCryptFinishHash(ptr ptr long long) cng.BCryptFinishHash
@ stdcall BCryptFreeBuffer(ptr) cng.BCryptFreeBuffer
@ stdcall BCryptGenerateKeyPair(ptr ptr long long) cng.BCryptGenerateKeyPair
@ stdcall BCryptGenerateSymmetricKey(ptr ptr ptr long ptr long long) cng.BCryptGenerateSymmetricKey
@ stdcall BCryptGenRandom(ptr ptr long long) cng.BCryptGenRandom
@ stdcall BCryptGetFipsAlgorithmMode(ptr) cng.BCryptGetFipsAlgorithmMode
@ stdcall BCryptGetProperty(ptr wstr ptr long ptr long) cng.BCryptGetProperty
@ stdcall BCryptHashData(ptr ptr long long) cng.BCryptHashData
@ stdcall BCryptImportKey(ptr ptr wstr ptr ptr long ptr long long) cng.BCryptImportKey
@ stdcall BCryptImportKeyPair(ptr ptr wstr ptr ptr long long) cng.BCryptImportKeyPair
@ stdcall BCryptKeyDerivation(ptr ptr ptr long ptr long) cng.BCryptKeyDerivation
@ stdcall BCryptOpenAlgorithmProvider(ptr wstr wstr long) cng.BCryptOpenAlgorithmProvider
@ stdcall BCryptRegisterConfigChangeNotify(ptr) cng.BCryptRegisterConfigChangeNotify
@ stdcall BCryptResolveProviders(wstr long wstr wstr long long ptr ptr) cng.BCryptResolveProviders
@ stdcall BCryptSecretAgreement(ptr ptr ptr long) cng.BCryptSecretAgreement
@ stdcall BCryptSetProperty(ptr wstr ptr long long) cng.BCryptSetProperty
@ stdcall BCryptSignHash(ptr ptr ptr long ptr long ptr long) cng.BCryptSignHash
@ stdcall BCryptUnregisterConfigChangeNotify(ptr) cng.BCryptUnregisterConfigChangeNotify
@ stdcall BCryptVerifySignature(ptr ptr ptr long ptr long long) cng.BCryptVerifySignature
@ stdcall CredMarshalTargetInfo()
@ stdcall DeleteSecurityContext(ptr)
@ stdcall EfsDecryptFek()
@ stdcall EfsGenerateKey()
@ stdcall EnumerateSecurityPackagesW(ptr ptr)
@ stdcall ExportSecurityContext(ptr long ptr ptr)
@ stdcall FreeContextBuffer(ptr)
@ stdcall FreeCredentialsHandle(ptr)
@ stdcall GenerateDirEfs()
@ stdcall GenerateSessionKey()
@ stdcall GetSecurityUserInfo(ptr long ptr)
@ stdcall ImpersonateSecurityContext(ptr)
@ stdcall ImportSecurityContextW(ptr ptr ptr ptr)
@ stdcall InitSecurityInterfaceW()
@ stdcall InitializeSecurityContextW(ptr ptr ptr long long long ptr long ptr ptr ptr ptr)
@ stdcall KSecRegisterSecurityProvider()
@ stdcall KSecValidateBuffer()
@ stdcall LsaEnumerateLogonSessions()
@ stdcall LsaGetLogonSessionData()
@ stdcall MakeSignature(ptr long ptr long)
@ stdcall MapSecurityError()
@ stdcall QueryContextAttributesW(ptr long ptr)
@ stdcall QueryCredentialsAttributesW(ptr long ptr)
@ stdcall QuerySecurityContextToken(ptr ptr)
@ stdcall QuerySecurityPackageInfoW(ptr ptr)
@ stdcall RevertSecurityContext(ptr)
@ stdcall SealMessage()
@ stdcall SecLookupAccountName(ptr ptr ptr ptr ptr ptr)
@ stdcall SecLookupAccountSid(ptr ptr ptr ptr ptr ptr)
@ stdcall SecLookupWellKnownSid(long ptr long ptr)
@ stdcall SecMakeSPN(ptr ptr ptr long ptr ptr ptr long)
@ stdcall SecMakeSPNEx(ptr ptr ptr long ptr ptr ptr ptr long)
@ stdcall SecSetPagingMode()
@ stdcall SystemPrng(ptr long) cng.SystemPrng
@ stdcall UnsealMessage()
@ stdcall VerifySignature(ptr ptr long ptr)
