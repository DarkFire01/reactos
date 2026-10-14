/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The controller keys arrive through, and the keyboard behind it
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "vdevbase.h"

#include <string.h>

namespace rtvm
{

/* As many bytes as can wait for the guest to read them */
#define KEYBOARD_QUEUE 64

class KeyboardControllerDevice : public VirtualDeviceBase,
                                 public IVndIoPortHandler,
                                 public IVmKeyboardDevice
{
public:
    KeyboardControllerDevice();
    ~KeyboardControllerDevice() override;

    KeyboardControllerDevice(const KeyboardControllerDevice &) = delete;
    KeyboardControllerDevice &operator=(const KeyboardControllerDevice &) = delete;

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return VirtualDeviceBase::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return VirtualDeviceBase::Release(); }

    STDMETHODIMP GetDependencies(void *Repository, ULONG *Count,
                                 GUID **Services, ULONG *Required) override;
    STDMETHODIMP StartReservingResources(void *Repository, VDEV_STATE State) override;
    STDMETHODIMP PowerOnCold(VDEV_STATE State) override;
    STDMETHODIMP PowerOff(VDEV_STATE State) override;
    STDMETHODIMP Reset(VDEV_STATE State) override;

    STDMETHODIMP NotifyUnregistered() override { return S_OK; }
    STDMETHODIMP NotifyIoPortRead(USHORT Port, USHORT Width, ULONG *Value) override;
    STDMETHODIMP NotifyIoPortWrite(USHORT Port, USHORT Width, ULONG Value) override;

    /* The keyboard, as whatever the operator is typing at sees it */
    STDMETHODIMP DeviceClass(PULONG Class) override;
    STDMETHODIMP DeviceState(PULONG State) override;
    STDMETHODIMP IsUnicodeSupported(PINT Supported) override;
    STDMETHODIMP LedState(PUCHAR Lamps) override;
    STDMETHODIMP SendKeystroke(PVDEV_KEYSTROKE Key) override;
    STDMETHODIMP GetOutputBufferFreeSpace(PULONG Free) override;

private:
    void Clear();
    void Put(_In_ UCHAR Byte);
    void Raise();
    void Command(_In_ UCHAR Byte);
    void Data(_In_ UCHAR Byte);

    CRITICAL_SECTION m_Lock = {};

    UCHAR m_Queue[KEYBOARD_QUEUE] = {};
    ULONG m_Count = 0;
    ULONG m_Read = 0;
    ULONG m_Write = 0;

    /* What was last read, which is what an empty buffer goes on saying */
    UCHAR m_Last = 0;

    UCHAR m_Status = 0;
    UCHAR m_Config = 0;
    UCHAR m_Output = 0;
    UCHAR m_Lamps = 0;

    /* A command waiting for the byte that goes with it, at either end */
    UCHAR m_ControllerWants = 0;
    UCHAR m_KeyboardWants = 0;

    bool m_Scanning = true;
    bool m_Raised = false;

    IVmIoApic *m_Lines = nullptr;
    IVmInputController *m_Input = nullptr;
};

} /* namespace rtvm */
