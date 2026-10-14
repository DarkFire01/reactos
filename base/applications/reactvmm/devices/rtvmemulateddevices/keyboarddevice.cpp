/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The controller keys arrive through, and the keyboard behind it
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Two ports: one the bytes go through in both directions, the other the
 * controller's own status going out and its commands coming in. A byte written
 * to the first is for the keyboard unless the controller asked for it.
 *
 * Something polling a controller that is not there reads all ones, which says a
 * byte is waiting, and the poll never ends. Saying there is nothing waiting is
 * the first thing this is for.
 */

#include "keyboarddevice.h"

namespace rtvm
{

#define KBC_DATA            0x0060
#define KBC_STATUS          0x0064

/* The line the keyboard is wired to */
#define KBC_LINE            1

/* What the status register says */
#define KBC_OUTPUT_FULL     0x01
#define KBC_SYSTEM          0x04
#define KBC_WAS_COMMAND     0x08

/* What the configuration byte says */
#define KBC_CONFIG_IRQ      0x01
#define KBC_CONFIG_SYSTEM   0x04
#define KBC_CONFIG_NO_KEYS  0x10
#define KBC_CONFIG_NO_AUX   0x20
#define KBC_CONFIG_DEFAULT  0x45

/* The commands the controller takes for itself */
#define KBC_READ_CONFIG     0x20
#define KBC_WRITE_CONFIG    0x60
#define KBC_AUX_OFF         0xA7
#define KBC_AUX_ON          0xA8
#define KBC_AUX_TEST        0xA9
#define KBC_SELF_TEST       0xAA
#define KBC_KEYS_TEST       0xAB
#define KBC_KEYS_OFF        0xAD
#define KBC_KEYS_ON         0xAE
#define KBC_READ_INPUT      0xC0
#define KBC_READ_OUTPUT     0xD0
#define KBC_WRITE_OUTPUT    0xD1
#define KBC_ECHO_KEYS       0xD2
#define KBC_ECHO_AUX        0xD3
#define KBC_TO_AUX          0xD4

#define KBC_SELF_TEST_OK    0x55
#define KBC_OUTPUT_DEFAULT  0x01

/* And the ones the keyboard takes */
#define KEY_SET_LAMPS       0xED
#define KEY_ECHO            0xEE
#define KEY_CODE_SET        0xF0
#define KEY_IDENTIFY        0xF2
#define KEY_RATE            0xF3
#define KEY_ENABLE          0xF4
#define KEY_DISABLE         0xF5
#define KEY_DEFAULTS        0xF6
#define KEY_RESET           0xFF

#define KEY_ACK             0xFA
#define KEY_PASSED          0xAA

/* Set two, as a keyboard behind a translating controller reports it */
#define KEY_SET_SAID        0x41

/* What a key going up adds, and the prefixes of the keys that have them */
#define KEY_BREAK           0x80
#define KEY_PREFIX          0xE0
#define KEY_PREFIX1         0xE1

/* WHAT IT IS *****************************************************************/

KeyboardControllerDevice::KeyboardControllerDevice()
{
    InitializeCriticalSection(&m_Lock);
    Clear();
}

KeyboardControllerDevice::~KeyboardControllerDevice()
{
    PowerOff(VDEV_STATE_NONE);
    DeleteCriticalSection(&m_Lock);
}

STDMETHODIMP
KeyboardControllerDevice::QueryInterface(
    _In_ REFIID Interface,
    _Outptr_ void **Object)
{
    if (Object == nullptr)
        return E_POINTER;

    if (IsEqualIID(Interface, IID_IUnknown) ||
        IsEqualIID(Interface, IID_IVirtualDevice))
    {
        *Object = static_cast<IVirtualDevice *>(this);
    }
    else if (IsEqualIID(Interface, IID_IVndIoPortHandler))
    {
        *Object = static_cast<IVndIoPortHandler *>(this);
    }
    else if (IsEqualIID(Interface, IID_IVmKeyboardDevice))
    {
        *Object = static_cast<IVmKeyboardDevice *>(this);
    }
    else
    {
        *Object = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

/* Somewhere to hand the keyboard to, which a machine nobody types at may lack */
STDMETHODIMP
KeyboardControllerDevice::GetDependencies(
    _In_opt_ void *Repository,
    _Out_ ULONG *Count,
    _Outptr_ GUID **Services,
    _Out_ ULONG *Required)
{
    static const GUID *const Wanted[] =
    {
        &IID_IVmAmd64EmulationServices,
        &IID_IVmIoApic,
        &IID_IVmInputController
    };

    UNREFERENCED_PARAMETER(Repository);

    return PublishDependencies(Wanted, ARRAYSIZE(Wanted), 1,
                               Count, Services, Required);
}

STDMETHODIMP
KeyboardControllerDevice::StartReservingResources(
    _In_opt_ void *Repository,
    _In_ VDEV_STATE State)
{
    UNREFERENCED_PARAMETER(Repository);
    UNREFERENCED_PARAMETER(State);

    HRESULT Status = ReservePorts(KBC_DATA, KBC_DATA, this);

    if (SUCCEEDED(Status))
        Status = ReservePorts(KBC_STATUS, KBC_STATUS, this);

    return Status;
}

void KeyboardControllerDevice::Clear()
{
    m_Count = 0;
    m_Read = 0;
    m_Write = 0;
    m_Last = 0;

    m_Status = KBC_SYSTEM;
    m_Config = KBC_CONFIG_DEFAULT;
    m_Output = KBC_OUTPUT_DEFAULT;
    m_Lamps = 0;

    m_ControllerWants = 0;
    m_KeyboardWants = 0;
    m_Scanning = true;
}

STDMETHODIMP
KeyboardControllerDevice::Reset(
    _In_ VDEV_STATE State)
{
    UNREFERENCED_PARAMETER(State);

    EnterCriticalSection(&m_Lock);
    Clear();
    Raise();
    LeaveCriticalSection(&m_Lock);

    return S_OK;
}

STDMETHODIMP
KeyboardControllerDevice::PowerOnCold(
    _In_ VDEV_STATE State)
{
    if (m_Lines == nullptr)
        FindService(IID_IVmIoApic, reinterpret_cast<void **>(&m_Lines));

    /* The keyboard is handed over, so that what is typed has somewhere to go */
    if ((m_Input == nullptr) &&
        SUCCEEDED(FindService(IID_IVmInputController,
                              reinterpret_cast<void **>(&m_Input))))
    {
        m_Input->RegisterInputDevice(static_cast<IVmKeyboardDevice *>(this));
        m_Input->NotifyDeviceClass(static_cast<IVmKeyboardDevice *>(this),
                                   VDEV_INPUT_KEYBOARD);
    }

    return Reset(State);
}

STDMETHODIMP
KeyboardControllerDevice::PowerOff(
    _In_ VDEV_STATE State)
{
    UNREFERENCED_PARAMETER(State);

    EnterCriticalSection(&m_Lock);
    m_Count = 0;
    Raise();
    LeaveCriticalSection(&m_Lock);

    if (m_Input != nullptr)
    {
        m_Input->UnregisterInputDevice(static_cast<IVmKeyboardDevice *>(this));
        m_Input->Release();
        m_Input = nullptr;
    }

    if (m_Lines != nullptr)
    {
        m_Lines->Release();
        m_Lines = nullptr;
    }

    return S_OK;
}

/* THE BUFFER *****************************************************************/

/* A byte for the guest, from the controller or from the keyboard */
void
KeyboardControllerDevice::Put(
    _In_ UCHAR Byte)
{
    if (m_Count >= KEYBOARD_QUEUE)
        return;

    m_Queue[m_Write] = Byte;
    m_Write = (m_Write + 1) % KEYBOARD_QUEUE;
    m_Count++;

    m_Status |= KBC_OUTPUT_FULL;
    Raise();
}

/**
 * @brief
 * Puts the line where it should be for what is waiting.
 *
 * @remarks
 * Only while something is waiting and the guest asked to be told. Called with
 * the lock held.
 */
void KeyboardControllerDevice::Raise()
{
    const bool Wanted = (m_Count != 0) && (m_Config & KBC_CONFIG_IRQ);

    if ((Wanted == m_Raised) || (m_Lines == nullptr))
        return;

    m_Raised = Wanted;

    if (Wanted)
        m_Lines->AssertIrq(KBC_LINE, VDEV_IRQ_SOURCE_ONLY);
    else
        m_Lines->DeassertIrq(KBC_LINE, VDEV_IRQ_SOURCE_ONLY);
}

/* THE REGISTERS **************************************************************/

STDMETHODIMP
KeyboardControllerDevice::NotifyIoPortRead(
    _In_ USHORT Port,
    _In_ USHORT Width,
    _Out_ ULONG *Value)
{
    UNREFERENCED_PARAMETER(Width);

    if (Value == nullptr)
        return E_POINTER;

    EnterCriticalSection(&m_Lock);

    if (Port == KBC_STATUS)
    {
        *Value = m_Status;
        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    if (m_Count != 0)
    {
        m_Last = m_Queue[m_Read];
        m_Read = (m_Read + 1) % KEYBOARD_QUEUE;
        m_Count--;
    }

    if (m_Count == 0)
        m_Status &= (UCHAR)~KBC_OUTPUT_FULL;

    *Value = m_Last;

    /*
     * Let go and raised again if more is waiting. What carries the line is
     * edge triggered, so a line held up across a byte being taken would never
     * say that the next one is there.
     */
    if (m_Raised && (m_Lines != nullptr))
    {
        m_Raised = false;
        m_Lines->DeassertIrq(KBC_LINE, VDEV_IRQ_SOURCE_ONLY);
    }

    Raise();

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

STDMETHODIMP
KeyboardControllerDevice::NotifyIoPortWrite(
    _In_ USHORT Port,
    _In_ USHORT Width,
    _In_ ULONG Value)
{
    const UCHAR Byte = (UCHAR)(Value & 0xFF);

    UNREFERENCED_PARAMETER(Width);

    EnterCriticalSection(&m_Lock);

    if (Port == KBC_STATUS)
    {
        m_Status |= KBC_WAS_COMMAND;
        Command(Byte);
    }
    else
    {
        m_Status &= (UCHAR)~KBC_WAS_COMMAND;
        Data(Byte);
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

/* A command for the controller itself, called with the lock held */
void
KeyboardControllerDevice::Command(
    _In_ UCHAR Byte)
{
    m_ControllerWants = 0;

    switch (Byte)
    {
        case KBC_READ_CONFIG:
            Put(m_Config);
            break;

        case KBC_WRITE_CONFIG:
        case KBC_WRITE_OUTPUT:
        case KBC_ECHO_KEYS:
        case KBC_ECHO_AUX:
        case KBC_TO_AUX:
            m_ControllerWants = Byte;
            break;

        case KBC_AUX_OFF:
            m_Config |= KBC_CONFIG_NO_AUX;
            break;

        case KBC_AUX_ON:
            m_Config &= (UCHAR)~KBC_CONFIG_NO_AUX;
            break;

        case KBC_SELF_TEST:
            m_Status |= KBC_SYSTEM;
            Put(KBC_SELF_TEST_OK);
            break;

        case KBC_KEYS_TEST:
        case KBC_AUX_TEST:
            Put(0x00);
            break;

        case KBC_KEYS_OFF:
            m_Config |= KBC_CONFIG_NO_KEYS;
            break;

        case KBC_KEYS_ON:
            m_Config &= (UCHAR)~KBC_CONFIG_NO_KEYS;
            break;

        case KBC_READ_INPUT:
            Put(0x00);
            break;

        case KBC_READ_OUTPUT:
            Put(m_Output);
            break;

        default:
            /* Pulsing the output lines, and anything else, does nothing here */
            break;
    }
}

/* A byte on the data port, called with the lock held */
void
KeyboardControllerDevice::Data(
    _In_ UCHAR Byte)
{
    const UCHAR Controller = m_ControllerWants;
    const UCHAR Keyboard = m_KeyboardWants;

    m_ControllerWants = 0;
    m_KeyboardWants = 0;

    switch (Controller)
    {
        case KBC_WRITE_CONFIG:
            m_Config = Byte;
            Raise();
            return;

        case KBC_WRITE_OUTPUT:
            m_Output = Byte;
            return;

        case KBC_ECHO_KEYS:
            Put(Byte);
            return;

        /* There is nothing on the second port to send to or echo from */
        case KBC_ECHO_AUX:
        case KBC_TO_AUX:
            return;

        default:
            break;
    }

    /* The byte that goes with a keyboard command */
    if (Keyboard != 0)
    {
        if (Keyboard == KEY_SET_LAMPS)
            m_Lamps = Byte & 0x07;

        Put(KEY_ACK);

        if ((Keyboard == KEY_CODE_SET) && (Byte == 0))
            Put(KEY_SET_SAID);

        return;
    }

    switch (Byte)
    {
        case KEY_SET_LAMPS:
        case KEY_CODE_SET:
        case KEY_RATE:
            m_KeyboardWants = Byte;
            Put(KEY_ACK);
            break;

        case KEY_ECHO:
            Put(KEY_ECHO);
            break;

        case KEY_IDENTIFY:
            Put(KEY_ACK);
            Put(0xAB);
            Put(0x83);
            break;

        case KEY_ENABLE:
            m_Scanning = true;
            Put(KEY_ACK);
            break;

        case KEY_DISABLE:
        case KEY_DEFAULTS:
            m_Scanning = (Byte != KEY_DISABLE);
            Put(KEY_ACK);
            break;

        case KEY_RESET:
            m_Scanning = true;
            m_Lamps = 0;
            Put(KEY_ACK);
            Put(KEY_PASSED);
            break;

        default:
            /*
             * Acknowledged rather than refused. Asking to have it sent again
             * sends the sender round, and it is no better known the next time.
             */
            Put(KEY_ACK);
            break;
    }
}

/* THE KEYBOARD ***************************************************************/

STDMETHODIMP
KeyboardControllerDevice::DeviceClass(
    _Out_ PULONG Class)
{
    if (Class == nullptr)
        return E_POINTER;

    *Class = VDEV_INPUT_KEYBOARD;
    return S_OK;
}

STDMETHODIMP
KeyboardControllerDevice::DeviceState(
    _Out_ PULONG State)
{
    if (State == nullptr)
        return E_POINTER;

    *State = 0;
    return S_OK;
}

STDMETHODIMP
KeyboardControllerDevice::IsUnicodeSupported(
    _Out_ PINT Supported)
{
    if (Supported == nullptr)
        return E_POINTER;

    *Supported = 0;
    return S_OK;
}

STDMETHODIMP
KeyboardControllerDevice::LedState(
    _Out_ PUCHAR Lamps)
{
    if (Lamps == nullptr)
        return E_POINTER;

    EnterCriticalSection(&m_Lock);
    *Lamps = m_Lamps;
    LeaveCriticalSection(&m_Lock);

    return S_OK;
}

/**
 * @brief
 * A key going down or coming up, put into the buffer as the wire carries it.
 *
 * @remarks
 * Already the code the controller hands the guest, so nothing is translated.
 * A key coming up is the same code with its top bit set, after its prefix.
 */
STDMETHODIMP
KeyboardControllerDevice::SendKeystroke(
    _In_ PVDEV_KEYSTROKE Key)
{
    if (Key == nullptr)
        return E_POINTER;

    UCHAR Code = (UCHAR)(Key->Code & 0x7F);

    if (Code == 0)
        return E_INVALIDARG;

    if (Key->Flags & VDEV_KEY_UP)
        Code |= KEY_BREAK;

    EnterCriticalSection(&m_Lock);

    /* Nothing is sent while the guest has the keyboard switched off */
    if (!m_Scanning || (m_Config & KBC_CONFIG_NO_KEYS) ||
        ((m_Count + 2) > KEYBOARD_QUEUE))
    {
        LeaveCriticalSection(&m_Lock);
        return S_FALSE;
    }

    if (Key->Flags & VDEV_KEY_EXTENDED1)
        Put(KEY_PREFIX1);
    else if (Key->Flags & VDEV_KEY_EXTENDED)
        Put(KEY_PREFIX);

    Put(Code);

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

STDMETHODIMP
KeyboardControllerDevice::GetOutputBufferFreeSpace(
    _Out_ PULONG Free)
{
    if (Free == nullptr)
        return E_POINTER;

    EnterCriticalSection(&m_Lock);
    *Free = KEYBOARD_QUEUE - m_Count;
    LeaveCriticalSection(&m_Lock);

    return S_OK;
}

} /* namespace rtvm */
