/*
 * PROJECT:     ReactOS Networking Debugging Module
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     kdnet initialization
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdnet.h"
#include <ndk/haltypes.h>
#include <ndk/halfuncs.h>
#include <reactos/kdnetextensibility.h>
#include "kdnet_net.h"
NTSTATUS NTAPI KdDebuggerInitialize0(_In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock);
NTSTATUS NTAPI KdDebuggerInitialize1(_In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock);
extern PDEBUG_NET_DATA      KdNetData;
extern DEBUG_NET_DATA       KdNetDataStorage;
extern DEBUG_NET_PARAMETERS KdNetParameters;

KDNET_SHARED_DATA KdNetSharedData = {0};
BOOLEAN KdNetInitialized = FALSE;
PVOID KdNetHardwareContext = NULL;

/* FrLdrDbgPrint is a very-early boot printf-like routine (COM1). */
ULONG (*FrLdrDbgPrint)(const char *Format, ...);

/* Base36 decode (windbg ENCRYPTION_KEY scheme): each dot-separated part is a
 * base36 number forming 64 bits of the 256-bit key. Stops at the first
 * non-[0-9a-z] char; *end (if given) points there. */
static ULONGLONG KdNetBase36ToU64(const char* s, const char** end)
{
    ULONGLONG v = 0;
    while (*s)
    {
        char c = *s;
        ULONG d;
        if (c >= '0' && c <= '9') d = (ULONG)(c - '0');
        else if (c >= 'a' && c <= 'z') d = (ULONG)(10 + (c - 'a'));
        else if (c >= 'A' && c <= 'Z') d = (ULONG)(10 + (c - 'A'));
        else break;
        v = v * 36 + d;
        ++s;
    }
    if (end) *end = s;
    return v;
}

/* forward decls for option helpers + shared-data global used below (defined later). */
static BOOLEAN KdNetReadTokenCI(const char* opts, const char* key, char* out, SIZE_T outChars);
static BOOLEAN KdNetParseIpv4(const char* s, ULONG* outHostOrder);
static BOOLEAN KdNetParseUlongDec(const char* s, ULONG* out);
static BOOLEAN KdNetHasFlagCI(const char* opts, const char* key);
static KDNET_SHARED_DATA g_KdNetSharedData;

static VOID KdNetWireRuntimeParameters(_In_opt_ PCHAR LoaderOptions)
{
    CHAR tok[160];
    ULONG ip, port;
    NTSTATUS encStatus;

    RtlZeroMemory(&KdNetParameters, sizeof(KdNetParameters));

    if (LoaderOptions && KdNetReadTokenCI(LoaderOptions, "HOST_IP", tok, sizeof(tok)) &&
        KdNetParseIpv4(tok, &ip))
        KdNetParameters.HostIP = ip;
    if (LoaderOptions && KdNetReadTokenCI(LoaderOptions, "TARGET_IP", tok, sizeof(tok)) &&
        KdNetParseIpv4(tok, &ip))
        KdNetParameters.TargetIP = ip;

    port = 50000;
    if (LoaderOptions && KdNetReadTokenCI(LoaderOptions, "HOST_PORT", tok, sizeof(tok)) &&
        KdNetParseUlongDec(tok, &port) && port <= 65535)
        { /* parsed into port */ }
    KdNetParameters.HostPort = (USHORT)port;
    KdNetParameters.TargetPort = (USHORT)port;

    KdNetParameters.Dhcp = (UCHAR)(LoaderOptions && !KdNetHasFlagCI(LoaderOptions, "NO_DHCP"));
    KdNetParameters.VerifyHostMac =
        (UCHAR)(LoaderOptions && KdNetHasFlagCI(LoaderOptions, "VERIFY_HOST_MAC"));

    /* Decode the 4-part base36 ENCRYPTION_KEY into the 32-byte key (windbg scheme). */
    if (LoaderOptions && KdNetReadTokenCI(LoaderOptions, "ENCRYPTION_KEY", tok, sizeof(tok)))
    {
        const char* kp = tok;
        int i;
        for (i = 0; i < 4; i++)
        {
            const char* end;
            ULONGLONG part = KdNetBase36ToU64(kp, &end);
            RtlCopyMemory(&KdNetParameters.Key[i * 8], &part, 8);
            if (*end != '.') break;
            kp = end + 1;
        }
        KdNetParameters.EncryptedLink = 1;
    }

    KdNetParameters.DebuggerActive = 1;

    /* Seed the offer nonce (TargetRandom). Bytes 8..31 must stay stable for the
     * connect handshake; bytes 0..7 are an incrementing counter. */
    {
        ULONGLONG seed = KdNetReadTimeStampCounter();
        ULONG i;
        for (i = 0; i < 32; i++)
            KdNetParameters.TargetRandom[i] = (UCHAR)(seed >> ((i & 7) * 8)) ^ (UCHAR)(i * 7 + 0x5A);
    }

    KdNetData = &KdNetDataStorage;
    RtlZeroMemory(&KdNetDataStorage, sizeof(KdNetDataStorage));
    KdNetDataStorage.KdNet = g_KdNetSharedData;
    KdNetDataStorage.Parameters = &KdNetParameters;
    KdNetDataStorage.TargetIP = KdNetParameters.TargetIP;
    KdNetDataStorage.Vendor = 0xFFFE;   /* extensibility NIC path */

    encStatus = KdNetCryptoInitialize(&KdNetDataStorage.Crypto,
                                      KdNetParameters.Key, KdNetParameters.SessionKey,
                                      FALSE, (BOOLEAN)KdNetParameters.EncryptedLink);

    if (FrLdrDbgPrint)
    {
        FrLdrDbgPrint("kdnet: params HostIP=0x%08lx HostPort=%u EncryptedLink=%u Dhcp=%u\n",
                      KdNetParameters.HostIP, KdNetParameters.HostPort,
                      (ULONG)KdNetParameters.EncryptedLink, (ULONG)KdNetParameters.Dhcp);
        FrLdrDbgPrint("kdnet: Key=%02x%02x%02x%02x..%02x%02x%02x%02x  CryptoInit=0x%08lx\n",
                      KdNetParameters.Key[0], KdNetParameters.Key[1], KdNetParameters.Key[2],
                      KdNetParameters.Key[3], KdNetParameters.Key[28], KdNetParameters.Key[29],
                      KdNetParameters.Key[30], KdNetParameters.Key[31], encStatus);
    }
}

static int KdNetIsSpace(char c)
{
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static char KdNetToUpper(char c)
{
    if (c >= 'a' && c <= 'z') return (char)(c - ('a' - 'A'));
    return c;
}

static const char* KdNetSkipSpaces(const char* s)
{
    while (s && *s && KdNetIsSpace(*s)) ++s;
    return s;
}

static const char* KdNetFindKeyCI(const char* haystack, const char* needle)
{
    const char* p;
    const char* n;

    if (!haystack || !needle || !*needle) return NULL;

    for (p = haystack; *p; ++p)
    {
        /* Require start or whitespace boundary (matches Windows-style LoadOptions). */
        const char* h;

        if (p != haystack && !KdNetIsSpace(p[-1])) continue;

        h = p;
        n = needle;
        while (*n && *h && (KdNetToUpper(*h) == *n))
        {
            ++h;
            ++n;
        }
        if (!*n)
            return h; /* points just after needle */
    }
    return NULL;
}

static const char* KdNetFindValueCI(const char* opts, const char* key)
{
    const char* p = KdNetFindKeyCI(opts, key);
    if (!p) return NULL;

    p = KdNetSkipSpaces(p);
    if (*p == '=')
    {
        ++p;
        p = KdNetSkipSpaces(p);
    }
    return p;
}

static BOOLEAN KdNetReadTokenCI(const char* opts, const char* key, char* out, SIZE_T outChars)
{
    const char* p;
    SIZE_T i = 0;

    if (!out || outChars == 0) return FALSE;
    out[0] = '\0';

    p = KdNetFindValueCI(opts, key);
    if (!p) return FALSE;

    /* Copy until whitespace or end. */
    while (p[i] && !KdNetIsSpace(p[i]) && i + 1 < outChars)
    {
        out[i] = p[i];
        ++i;
    }
    out[i] = '\0';
    return (i != 0);
}

static BOOLEAN KdNetParseUlongDec(const char* s, ULONG* out)
{
    ULONG v = 0;
    BOOLEAN any = FALSE;
    if (!s || !out) return FALSE;

    while (*s >= '0' && *s <= '9')
    {
        any = TRUE;
        v = (v * 10) + (ULONG)(*s - '0');
        ++s;
    }
    if (!any) return FALSE;
    *out = v;
    return TRUE;
}

static BOOLEAN KdNetParseIpv4(const char* s, ULONG* outHostOrder)
{
    ULONG a, b, c, d;
    ULONG t;
    const char* p = s;

    if (!s || !outHostOrder) return FALSE;
    if (!KdNetParseUlongDec(p, &a) || a > 255) return FALSE;
    while (*p >= '0' && *p <= '9') ++p;
    if (*p++ != '.') return FALSE;

    if (!KdNetParseUlongDec(p, &b) || b > 255) return FALSE;
    while (*p >= '0' && *p <= '9') ++p;
    if (*p++ != '.') return FALSE;

    if (!KdNetParseUlongDec(p, &c) || c > 255) return FALSE;
    while (*p >= '0' && *p <= '9') ++p;
    if (*p++ != '.') return FALSE;

    if (!KdNetParseUlongDec(p, &d) || d > 255) return FALSE;

    t = ((a & 0xFF) << 24) | ((b & 0xFF) << 16) | ((c & 0xFF) << 8) | (d & 0xFF);
    *outHostOrder = t;
    return TRUE;
}

static BOOLEAN KdNetHasFlagCI(const char* opts, const char* key)
{
    return (KdNetFindKeyCI(opts, key) != NULL);
}

/**
 * @brief
 * Splits a BUSPARAMS value into the PCI location it names.
 *
 * The debugger writes the location as three decimal fields separated by dots,
 * as in BUSPARAMS=0.3.0. A field that is absent or unparsable is left at zero,
 * which is how a short triplet is treated.
 *
 * @param[in] Value
 * The token's value, with the "BUSPARAMS=" prefix already removed.
 *
 * @param[out] Bus
 * Receives the bus number.
 *
 * @param[out] Device
 * Receives the device number.
 *
 * @param[out] Function
 * Receives the function number.
 */
static VOID
KdNetParseBusParams(
    _In_ PCSTR Value,
    _Out_ PULONG Bus,
    _Out_ PULONG Device,
    _Out_ PULONG Function)
{
    PULONG Fields[3];
    PCSTR Position;
    ULONG Index;

    Fields[0] = Bus;
    Fields[1] = Device;
    Fields[2] = Function;
    Position = Value;

    for (Index = 0; Index < RTL_NUMBER_OF(Fields); ++Index)
    {
        *Fields[Index] = 0;
        KdNetParseUlongDec(Position, Fields[Index]);

        /* Step over the field just read and the dot that closes it */
        while (*Position != '\0' && *Position != '.')
            ++Position;

        if (*Position == '.')
            ++Position;
    }
}
static KDNET_EXTENSIBILITY_EXPORTS g_KdNetExtExports;
static DEBUG_DEVICE_DESCRIPTOR g_KdNetDeviceDescriptor;
static KDNET_SHARED_DATA g_KdNetSharedData;
static UCHAR g_KdNetLinkState;
static UCHAR g_KdNetTargetMac[MAC_ADDRESS_SIZE];
static ULONG g_KdNetLinkSpeed;
static ULONG g_KdNetLinkDuplex;
static PVOID g_KdNetHardwareContext;

/* What KdDebuggerInitialize1 reports: how phase 0 ended, and how many times
   the controller has been brought up (once at boot, again after each D0). */
static NTSTATUS g_KdNetInitStatus = STATUS_SUCCESS;
ULONG KdNetInitializeCount = 0;

/* Defined in kdnet_ext.c; the extension writes its failure reason / chip id here
 * (via the import table) so we can report it when KdInitializeController fails. */
extern PWCHAR KdNetErrorString;
extern ULONG  KdNetHardwareId;

/**
 * @brief Phase-0 initialization of the kernel debugger over network.
 *
 * Called by the kernel (e.g. ntoskrnl kd64/kdinit.c) with the loader block.
 * Windows implementation: parses LoaderOptions for HOST_IP, HOST_PORT, ENCRYPTION_KEY,
 * BUSPARAMS, etc.; builds KDNET_EXTENSIBILITY_IMPORTS; loads and calls the
 * extensibility module's KdInitializeLibrary(); allocates hardware context;
 * calls InitializeEncryption and InitializeController.
 *
 * @param LoaderBlock Optional loader parameter block (LoadOptions contain kdnet params).
 * @return STATUS_SUCCESS if debugger transport is ready; error code otherwise.
 */
static
NTSTATUS
KdNetInitializePhase0(_In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PCHAR LoaderOptions = NULL;
    NTSTATUS Status;

    if (KdNetInitialized)
        return STATUS_SUCCESS;

    if (LoaderBlock)
        FrLdrDbgPrint = (PVOID)LoaderBlock->u.I386.CommonDataArea;
    else
        FrLdrDbgPrint = NULL;

    if (FrLdrDbgPrint)
        FrLdrDbgPrint("kdnet: KdDebuggerInitialize0 LoaderBlock=%p LoadOptions=%p\n",
                      LoaderBlock, LoaderBlock ? LoaderBlock->LoadOptions : NULL);
    if (LoaderBlock)
        LoaderOptions = (PCHAR)LoaderBlock->LoadOptions;

    RtlZeroMemory(&g_KdNetExtExports, sizeof(g_KdNetExtExports));
    RtlZeroMemory(&g_KdNetDeviceDescriptor, sizeof(g_KdNetDeviceDescriptor));

    /*
     * Set the PCI match criteria for the debug NIC so HalpMatchDebuggingDevice
     * finds the right device. With an all-zero descriptor it would match Bus 0 /
     * Slot 0 (the host bridge). If BUSPARAMS=bus.dev.func is given, match that
     * exact location; otherwise match by class (network controller, 0x02) which
     * is what the "kd_02_8086" name encodes (class 02, vendor 8086).
     */
    {
        CHAR BusParams[64];
        ULONG Bus, Device, Function;

        if (LoaderOptions != NULL &&
            KdNetReadTokenCI(LoaderOptions, "BUSPARAMS", BusParams, sizeof(BusParams)))
        {
            KdNetParseBusParams(BusParams, &Bus, &Device, &Function);

            g_KdNetDeviceDescriptor.Bus = Bus;
            g_KdNetDeviceDescriptor.Slot = (Device & 0x1F) | ((Function & 0x7) << 5);
            if (FrLdrDbgPrint)
                FrLdrDbgPrint("kdnet: match BUSPARAMS bus=%lu dev=%lu fn=%lu (Slot=0x%lx)\n",
                              Bus, Device, Function, g_KdNetDeviceDescriptor.Slot);
        }
        else
        {
            /* Match the first network-class controller (e.g. the QEMU e1000). */
            g_KdNetDeviceDescriptor.Bus = 0xFFFFFFFF;
            g_KdNetDeviceDescriptor.Slot = 0xFFFFFFFF;
            g_KdNetDeviceDescriptor.VendorID = 0xFFFF;
            g_KdNetDeviceDescriptor.DeviceID = 0xFFFF;
            g_KdNetDeviceDescriptor.BaseClass = 0x02; /* PCI network controller */
            g_KdNetDeviceDescriptor.SubClass = 0x00;  /* ethernet */
            g_KdNetDeviceDescriptor.ProgIf = 0xFF;    /* any */
            if (FrLdrDbgPrint)
                FrLdrDbgPrint("kdnet: match by network class (BaseClass=2)\n");
        }
    }

    if (FrLdrDbgPrint && LoaderOptions)
    {
        const char* v;
        CHAR tok[128];
        ULONG ip;
        ULONG port;

        FrLdrDbgPrint("kdnet: LoaderOptions='%s'\n", LoaderOptions);

        if (KdNetReadTokenCI(LoaderOptions, "BUSPARAMS", tok, sizeof(tok)))
            FrLdrDbgPrint("kdnet: BUSPARAMS=%s\n", tok);
        else
            FrLdrDbgPrint("kdnet: BUSPARAMS=<absent>\n");

        v = KdNetFindValueCI(LoaderOptions, "HOST_IP");
        if (KdNetReadTokenCI(LoaderOptions, "HOST_IP", tok, sizeof(tok)) && KdNetParseIpv4(tok, &ip))
            FrLdrDbgPrint("kdnet: HOST_IP=%s (0x%08lx)\n", tok, ip);
        else if (v && KdNetReadTokenCI(LoaderOptions, "HOST_IP", tok, sizeof(tok)))
            FrLdrDbgPrint("kdnet: HOST_IP=%s (parse failed)\n", tok);
        else
            FrLdrDbgPrint("kdnet: HOST_IP=<absent>\n");

        v = KdNetFindValueCI(LoaderOptions, "TARGET_IP");
        if (KdNetReadTokenCI(LoaderOptions, "TARGET_IP", tok, sizeof(tok)) && KdNetParseIpv4(tok, &ip))
            FrLdrDbgPrint("kdnet: TARGET_IP=%s (0x%08lx)\n", tok, ip);
        else if (v && KdNetReadTokenCI(LoaderOptions, "TARGET_IP", tok, sizeof(tok)))
            FrLdrDbgPrint("kdnet: TARGET_IP=%s (parse failed)\n", tok);
        else
            FrLdrDbgPrint("kdnet: TARGET_IP=<absent>\n");

        v = KdNetFindValueCI(LoaderOptions, "HOST_PORT");
        if (KdNetReadTokenCI(LoaderOptions, "HOST_PORT", tok, sizeof(tok)) &&
            KdNetParseUlongDec(tok, &port) && port <= 65535)
            FrLdrDbgPrint("kdnet: HOST_PORT=%lu (0x%04lx)\n", port, port);
        else if (v && KdNetReadTokenCI(LoaderOptions, "HOST_PORT", tok, sizeof(tok)))
            FrLdrDbgPrint("kdnet: HOST_PORT=%s (parse failed)\n", tok);
        else
            FrLdrDbgPrint("kdnet: HOST_PORT=<absent>\n");

        FrLdrDbgPrint("kdnet: flags: NO_DHCP=%lu NO_KDNIC=%lu VERIFY_HOST_MAC=%lu SEND_KD_STATUS=%lu\n",
                      KdNetHasFlagCI(LoaderOptions, "NO_DHCP"),
                      KdNetHasFlagCI(LoaderOptions, "NO_KDNIC"),
                      KdNetHasFlagCI(LoaderOptions, "VERIFY_HOST_MAC"),
                      KdNetHasFlagCI(LoaderOptions, "SEND_KD_STATUS"));

        if (KdNetReadTokenCI(LoaderOptions, "ENCRYPTION_KEY", tok, sizeof(tok)))
            FrLdrDbgPrint("kdnet: ENCRYPTION_KEY=<present len=%lu>\n", (ULONG)strlen(tok));
        else
            FrLdrDbgPrint("kdnet: ENCRYPTION_KEY=<absent>\n");
    }

    Status = KdNetInitializeExtensibility(LoaderOptions,
                                          (struct _DEBUG_DEVICE_DESCRIPTOR*)&g_KdNetDeviceDescriptor,
                                          KdInitializeLibrary,
                                          &g_KdNetExtExports,
                                          NULL);
    FrLdrDbgPrint("KdNetInitializeExtensibility Status=%lx\n", Status);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Allocate hardware context and initialize the extension controller */
    if (!g_KdNetExtExports.KdGetHardwareContextSize || !g_KdNetExtExports.KdInitializeController)
        return STATUS_INVALID_PARAMETER;

    {
        ULONG ctxSize = g_KdNetExtExports.KdGetHardwareContextSize(&g_KdNetDeviceDescriptor);
        if (FrLdrDbgPrint)
            FrLdrDbgPrint("kdnet: KdGetHardwareContextSize=%lu (0x%lx)\n", ctxSize, ctxSize);

        if (!ctxSize)
            return STATUS_INVALID_PARAMETER;

        g_KdNetDeviceDescriptor.Memory.Length = ROUND_TO_PAGES(ctxSize);

        Status = STATUS_NOT_SUPPORTED;
        if (KdSetupPciDeviceForDebugging)
            Status = KdSetupPciDeviceForDebugging((PVOID)LoaderBlock, &g_KdNetDeviceDescriptor);

        if (FrLdrDbgPrint)
            FrLdrDbgPrint("kdnet: KdSetupPciDeviceForDebugging -> 0x%08lx VA=%p Len=0x%lx\n",
                          Status,
                          g_KdNetDeviceDescriptor.Memory.VirtualAddress,
                          g_KdNetDeviceDescriptor.Memory.Length);

        if (!NT_SUCCESS(Status))
            return Status;

        if (FrLdrDbgPrint)
            FrLdrDbgPrint("kdnet: PCI device Vendor=0x%04x Device=0x%04x Class=0x%02x/0x%02x Bus=%lu Slot=0x%lx\n",
                          g_KdNetDeviceDescriptor.VendorID, g_KdNetDeviceDescriptor.DeviceID,
                          g_KdNetDeviceDescriptor.BaseClass, g_KdNetDeviceDescriptor.SubClass,
                          g_KdNetDeviceDescriptor.Bus, g_KdNetDeviceDescriptor.Slot);

        /* Show the mapped register BARs: the extension does its MMIO through
         * BaseAddress[i].TranslatedAddress. A NULL/garbage VA here means the
         * stub's first register access faults/hangs in KdInitializeController. */
        if (FrLdrDbgPrint)
        {
            ULONG _bar;
            for (_bar = 0; _bar < MAXIMUM_DEBUG_BARS; _bar++)
            {
                if (!g_KdNetDeviceDescriptor.BaseAddress[_bar].Valid)
                    continue;
                FrLdrDbgPrint("kdnet: BAR[%lu] Type=%u Valid=%u Xlat=%p Len=0x%lx\n",
                              _bar,
                              (ULONG)g_KdNetDeviceDescriptor.BaseAddress[_bar].Type,
                              (ULONG)g_KdNetDeviceDescriptor.BaseAddress[_bar].Valid,
                              g_KdNetDeviceDescriptor.BaseAddress[_bar].TranslatedAddress,
                              g_KdNetDeviceDescriptor.BaseAddress[_bar].Length);
            }

            /* Probe the first memory BAR ourselves to prove the mapping reaches
             * the NIC. For an e1000 these are CTRL(0x00)/STATUS(0x08)/EECD(0x10);
             * sane values => the mapping is good and a hung KdInitializeController
             * is the stub's own polling, not a ReactOS mapping bug. */
            for (_bar = 0; _bar < MAXIMUM_DEBUG_BARS; _bar++)
            {
                PUCHAR _va;
                if (!g_KdNetDeviceDescriptor.BaseAddress[_bar].Valid ||
                    g_KdNetDeviceDescriptor.BaseAddress[_bar].Type != 3 /* CmResourceTypeMemory */)
                    continue;
                _va = (PUCHAR)g_KdNetDeviceDescriptor.BaseAddress[_bar].TranslatedAddress;
                if (!_va)
                    continue;
                FrLdrDbgPrint("kdnet: MMIO probe BAR[%lu]: +0x00=0x%08lx +0x08=0x%08lx +0x10=0x%08lx\n",
                              _bar,
                              READ_REGISTER_ULONG((PULONG)(_va + 0x00)),
                              READ_REGISTER_ULONG((PULONG)(_va + 0x08)),
                              READ_REGISTER_ULONG((PULONG)(_va + 0x10)));
                /* Device side-effect write test: CTRL.RST (bit 26) is self-clearing
                 * BY THE NIC. If our write reaches the device it resets and clears
                 * the bit; if the write is swallowed the bit stays set. Unambiguous
                 * (a cached mapping would read back our written value with RST set).
                 * The stub resets the chip itself first, so this is harmless. */
                WRITE_REGISTER_ULONG((PULONG)(_va + 0x00), 0x04000000);
                { volatile ULONG _d; for (_d = 0; _d < 2000000; _d++) { } }
                {
                    ULONG _ctrl = READ_REGISTER_ULONG((PULONG)(_va + 0x00));
                    FrLdrDbgPrint("kdnet: CTRL.RST test: CTRL=0x%08lx -> %s\n",
                                  _ctrl,
                                  (_ctrl & 0x04000000) ? "RST STUCK (write NOT reaching NIC)"
                                                       : "RST cleared (writes reach NIC)");
                }
                break;
            }

        }

        g_KdNetHardwareContext = g_KdNetDeviceDescriptor.Memory.VirtualAddress;
        if (!g_KdNetHardwareContext)
            return STATUS_INSUFFICIENT_RESOURCES;

        RtlZeroMemory(g_KdNetHardwareContext, g_KdNetDeviceDescriptor.Memory.Length);

        RtlZeroMemory(&g_KdNetSharedData, sizeof(g_KdNetSharedData));
        g_KdNetSharedData.Hardware = g_KdNetHardwareContext;
        g_KdNetSharedData.Device = &g_KdNetDeviceDescriptor;
        g_KdNetSharedData.TargetMacAddress = g_KdNetTargetMac;
        g_KdNetSharedData.LinkSpeed = 0;
        g_KdNetSharedData.LinkDuplex = 0;
        g_KdNetSharedData.LinkState = &g_KdNetLinkState;
        g_KdNetSharedData.SerialBaudRate = 0;
        g_KdNetSharedData.Flags = 0;
        g_KdNetSharedData.RestartKdnet = 0;

        g_KdNetLinkState = 0;
        RtlZeroMemory(g_KdNetTargetMac, sizeof(g_KdNetTargetMac));

        /*
         * Wire the runtime parameters + key/crypto BEFORE the controller is
         * brought up, so the encryption state is ready (and observable) even
         * while the NIC driver is being developed.
         */
        KdNetWireRuntimeParameters(LoaderOptions);

        if (FrLdrDbgPrint)
            FrLdrDbgPrint("kdnet: calling KdInitializeController...\n");
        KdNetInitializeCount++;
        Status = g_KdNetExtExports.KdInitializeController(&g_KdNetSharedData);
        if (FrLdrDbgPrint)
            FrLdrDbgPrint("kdnet: KdInitializeController -> 0x%08lx\n", Status);
        if (!NT_SUCCESS(Status))
        {
            if (FrLdrDbgPrint)
                FrLdrDbgPrint("kdnet: ext failed: HardwareID=0x%08lx ErrorString=%ws\n",
                              KdNetHardwareId, KdNetErrorString);
            return Status;
        }

        g_KdNetLinkSpeed = g_KdNetSharedData.LinkSpeed;
        g_KdNetLinkDuplex = g_KdNetSharedData.LinkDuplex;

        /* Publish the adapter/shared-data to the rest of the transport
         * (power transitions, hiber range, packet I/O). */
        KdNetHardwareContext = g_KdNetHardwareContext;
        KdNetSharedData = g_KdNetSharedData;
        KdNetInitialized = TRUE;

        if (FrLdrDbgPrint)
        {
            FrLdrDbgPrint("kdnet: LinkState=%u LinkSpeed=%lu LinkDuplex=%lu\n",
                          (ULONG)g_KdNetLinkState, g_KdNetLinkSpeed, g_KdNetLinkDuplex);
            FrLdrDbgPrint("kdnet: TargetMac=%02x-%02x-%02x-%02x-%02x-%02x\n",
                          g_KdNetTargetMac[0], g_KdNetTargetMac[1], g_KdNetTargetMac[2],
                          g_KdNetTargetMac[3], g_KdNetTargetMac[4], g_KdNetTargetMac[5]);
        }
    }

    /* Refresh TargetMac now that the controller has read it from the NIC. */
    if (KdNetData)
        RtlCopyMemory(KdNetDataStorage.TargetMac.Address, g_KdNetTargetMac, MAC_ADDRESS_SIZE);

    /* Bring up the network: assign target IP + ARP-resolve the host MAC. This is
     * the transport's first transmit. */
    {
        NTSTATUS netStatus = KdNetInitializeNetwork();
        if (FrLdrDbgPrint)
        {
            PUCHAR hm = KdNetParameters.HostMac.Address;
            FrLdrDbgPrint("kdnet: InitializeNetwork -> 0x%08lx TargetIP=0x%08lx\n",
                          netStatus, KdNetParameters.TargetIP);
            FrLdrDbgPrint("kdnet: HostMac=%02x-%02x-%02x-%02x-%02x-%02x\n",
                          hm[0], hm[1], hm[2], hm[3], hm[4], hm[5]);
        }
    }

    /*
     * The transport is up. KdNetInitializeNetwork above has already run the
     * offer and connect exchange with the host, so by this point either
     * Parameters->Connected is set or the offers timed out; a later
     * KdSendPacket works or does not on that basis.
     */
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Phase 0 of the debug transport, recording how it ended.
 *
 * @param[in] LoaderBlock
 * Optional loader parameter block; its LoadOptions carry the kdnet settings.
 *
 * @return
 * STATUS_SUCCESS when the transport is ready, otherwise the failure.
 */
NTSTATUS
NTAPI
KdDebuggerInitialize0(
    _In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    g_KdNetInitStatus = KdNetInitializePhase0(LoaderBlock);
    return g_KdNetInitStatus;
}

/* SERVICE KEY REPORTING ******************************************************/

/**
 * @brief
 * Opens the transport's own service key, creating it if it is not there.
 *
 * The real kdnet.dll creates this key rather than expecting an installer to
 * have made it: a machine with network debugging switched off has no
 * Services\kdnet key at all until the transport has run once.
 *
 * @param[out] KeyHandle
 * Receives the open key.
 *
 * @return
 * STATUS_SUCCESS, or the failure from the registry.
 */
static
NTSTATUS
KdNetOpenServiceKey(
    _Out_ PHANDLE KeyHandle)
{
    UNICODE_STRING KeyName = RTL_CONSTANT_STRING(
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\kdnet");
    OBJECT_ATTRIBUTES ObjectAttributes;

    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    return ZwCreateKey(KeyHandle,
                       KEY_SET_VALUE,
                       &ObjectAttributes,
                       0,
                       NULL,
                       REG_OPTION_NON_VOLATILE,
                       NULL);
}

/**
 * @brief
 * Writes one REG_DWORD under the service key.
 *
 * A failure is not reported. These values are a record of what happened, so
 * losing one is not worth abandoning the rest of the report for.
 *
 * @param[in] KeyHandle
 * The open service key.
 *
 * @param[in] Name
 * The value name.
 *
 * @param[in] Value
 * The value to write.
 */
static
VOID
KdNetReportDword(
    _In_ HANDLE KeyHandle,
    _In_ PCWSTR Name,
    _In_ ULONG Value)
{
    UNICODE_STRING ValueName;

    RtlInitUnicodeString(&ValueName, Name);
    ZwSetValueKey(KeyHandle, &ValueName, 0, REG_DWORD, &Value, sizeof(Value));
}

/**
 * @brief
 * Writes one REG_BINARY under the service key.
 *
 * @param[in] KeyHandle
 * The open service key.
 *
 * @param[in] Name
 * The value name.
 *
 * @param[in] Data
 * The bytes to write.
 *
 * @param[in] Length
 * How many bytes to write.
 */
static
VOID
KdNetReportBinary(
    _In_ HANDLE KeyHandle,
    _In_ PCWSTR Name,
    _In_reads_bytes_(Length) PVOID Data,
    _In_ ULONG Length)
{
    UNICODE_STRING ValueName;

    RtlInitUnicodeString(&ValueName, Name);
    ZwSetValueKey(KeyHandle, &ValueName, 0, REG_BINARY, Data, Length);
}

/**
 * @brief
 * Writes one REG_SZ under the service key.
 *
 * @param[in] KeyHandle
 * The open service key.
 *
 * @param[in] Name
 * The value name.
 *
 * @param[in] Value
 * The string to write.
 */
static
VOID
KdNetReportString(
    _In_ HANDLE KeyHandle,
    _In_ PCWSTR Name,
    _In_ PCWSTR Value)
{
    UNICODE_STRING ValueName;
    UNICODE_STRING Data;

    RtlInitUnicodeString(&ValueName, Name);
    RtlInitUnicodeString(&Data, Value);
    ZwSetValueKey(KeyHandle, &ValueName, 0, REG_SZ,
                  Data.Buffer, Data.Length + sizeof(UNICODE_NULL));
}

/**
 * @brief
 * Packs the debug device's PCI location the way a BDF is normally written.
 *
 * The descriptor carries the bus, and the device and function packed into Slot
 * the way PCI_SLOT_NUMBER does. This uses the conventional bus, device and
 * function encoding: the reference binary does not pin the layout down, and
 * nothing reads the value back here.
 *
 * @return
 * The packed location.
 */
static
ULONG
KdNetBusDeviceFunction(VOID)
{
    ULONG Device = g_KdNetDeviceDescriptor.Slot & 0x1F;
    ULONG Function = (g_KdNetDeviceDescriptor.Slot >> 5) & 0x7;

    return (g_KdNetDeviceDescriptor.Bus << 8) | (Device << 3) | Function;
}

/**
 * @brief
 * Records how the debug transport came up, under its own service key.
 *
 * The values are the ones the Windows transport writes: how phase 0 ended and
 * the string an extension left behind when it failed, the chip it recognized,
 * the address and MAC the target answers on, and the transfer counters.
 *
 * Nothing in ReactOS reads them back, because the kdnic driver that consumes
 * the other half of this key does not exist here. They are written anyway
 * because they are the only durable record of why a debug session did or did
 * not come up: the boot log is gone by the time anyone thinks to ask.
 *
 * @param[in] LoaderBlock
 * Unused. Everything reported here was captured during phase 0.
 *
 * @return
 * STATUS_SUCCESS once the values are written, otherwise the registry failure.
 */
NTSTATUS
NTAPI
KdDebuggerInitialize1(_In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    NTSTATUS Status;
    HANDLE KeyHandle;

    UNREFERENCED_PARAMETER(LoaderBlock);

    Status = KdNetOpenServiceKey(&KeyHandle);
    if (!NT_SUCCESS(Status))
        return Status;

    KdNetReportDword(KeyHandle, L"KdInitStatus", (ULONG)g_KdNetInitStatus);
    KdNetReportDword(KeyHandle, L"KdNetHardwareID", KdNetHardwareId);
    KdNetReportDword(KeyHandle, L"KdNetInitializeCount", KdNetInitializeCount);
    KdNetReportDword(KeyHandle, L"KdNetTxSuccess", KdNetTxSuccessCount);
    KdNetReportDword(KeyHandle, L"KdNetRxSuccess", KdNetRxSuccessCount);
    KdNetReportDword(KeyHandle, L"KdNetIPAddress", KdNetParameters.TargetIP);
    KdNetReportDword(KeyHandle, L"KdNet2PfBDF", KdNetBusDeviceFunction());

    KdNetReportBinary(KeyHandle, L"KdNetMacAddress",
                      g_KdNetTargetMac, sizeof(g_KdNetTargetMac));

    if (KdNetErrorString != NULL)
        KdNetReportString(KeyHandle, L"KdInitResultString", KdNetErrorString);

    ZwClose(KeyHandle);
    return STATUS_SUCCESS;
}
