/*
 *
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/power.c
 * PURPOSE:         Power Management Functions
 * PROGRAMMER:      Dmitry Chapyshev <dmitry@reactos.org>
 *
 * UPDATE HISTORY:
 *                  01/15/2009 Created
 */

#include <k32.h>

#include <ndk/pofuncs.h>

#define NDEBUG
#include <debug.h>

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
GetSystemPowerStatus(IN LPSYSTEM_POWER_STATUS PowerStatus)
{
    NTSTATUS Status;
    SYSTEM_BATTERY_STATE BattState;
    ULONG Max, Current;

    Status = NtPowerInformation(SystemBatteryState,
                                NULL,
                                0,
                                &BattState,
                                sizeof(SYSTEM_BATTERY_STATE));
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    RtlZeroMemory(PowerStatus, sizeof(SYSTEM_POWER_STATUS));

    PowerStatus->BatteryLifeTime = BATTERY_LIFE_UNKNOWN;
    PowerStatus->BatteryFullLifeTime = BATTERY_LIFE_UNKNOWN;
    PowerStatus->BatteryLifePercent = BATTERY_PERCENTAGE_UNKNOWN;
    PowerStatus->ACLineStatus = AC_LINE_ONLINE;

    Max = BattState.MaxCapacity;
    Current = BattState.RemainingCapacity;
    if (Max)
    {
        if (Current <= Max)
        {
            PowerStatus->BatteryLifePercent = (UCHAR)((100 * Current + Max / 2) / Max);
        }
        else
        {
            PowerStatus->BatteryLifePercent = 100;
        }

        if (PowerStatus->BatteryLifePercent <= 4)
            PowerStatus->BatteryFlag |= BATTERY_FLAG_CRITICAL;

        if (PowerStatus->BatteryLifePercent <= 32)
            PowerStatus->BatteryFlag |= BATTERY_FLAG_LOW;

        if (PowerStatus->BatteryLifePercent >= 67)
            PowerStatus->BatteryFlag |= BATTERY_FLAG_HIGH;
    }

    if (!BattState.BatteryPresent)
        PowerStatus->BatteryFlag |= BATTERY_FLAG_NO_BATTERY;

    if (BattState.Charging)
        PowerStatus->BatteryFlag |= BATTERY_FLAG_CHARGING;

    if (!(BattState.AcOnLine) && (BattState.BatteryPresent))
        PowerStatus->ACLineStatus = AC_LINE_OFFLINE;

    if (BattState.EstimatedTime)
        PowerStatus->BatteryLifeTime = BattState.EstimatedTime;

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetSystemPowerState(IN BOOL fSuspend,
                    IN BOOL fForce)
{
    NTSTATUS Status;

    Status = NtInitiatePowerAction((fSuspend != FALSE) ? PowerActionSleep     : PowerActionHibernate,
                                   (fSuspend != FALSE) ? PowerSystemSleeping1 : PowerSystemHibernate,
                                   (fForce == FALSE),
                                   FALSE);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetDevicePowerState(IN HANDLE hDevice,
                    OUT BOOL *pfOn)
{
    DEVICE_POWER_STATE DevicePowerState;
    NTSTATUS Status;

    Status = NtGetDevicePowerState(hDevice, &DevicePowerState);
    if (NT_SUCCESS(Status))
    {
        *pfOn = (DevicePowerState == PowerDeviceUnspecified) ||
                (DevicePowerState == PowerDeviceD0);
        return TRUE;
    }

    BaseSetLastNTError(Status);
    return FALSE;
}

/*
 * @implemented
 */
BOOL
WINAPI
RequestDeviceWakeup(IN HANDLE hDevice)
{
    NTSTATUS Status;

    Status = NtRequestDeviceWakeup(hDevice);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
RequestWakeupLatency(IN LATENCY_TIME latency)
{
    NTSTATUS Status;

    Status = NtRequestWakeupLatency(latency);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
CancelDeviceWakeupRequest(IN HANDLE hDevice)
{
    NTSTATUS Status;

    Status = NtCancelDeviceWakeupRequest(hDevice);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
IsSystemResumeAutomatic(VOID)
{
    return (BOOL)NtIsSystemResumeAutomatic();
}

/*
 * @implemented
 */
BOOL
WINAPI
SetMessageWaitingIndicator(IN HANDLE hMsgIndicator,
                           IN ULONG ulMsgCount)
{
    /* This is the correct Windows implementation */
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

/*
 * @implemented
 */
EXECUTION_STATE
WINAPI
SetThreadExecutionState(EXECUTION_STATE esFlags)
{
    NTSTATUS Status;

    Status = NtSetThreadExecutionState(esFlags, &esFlags);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return 0;
    }

    return esFlags;
}


/* kernel32 targets NT 5.2, where potypes.h keeps these behind its Win7 guard */
#ifndef POWER_REQUEST_CONTEXT_VERSION
#define POWER_REQUEST_CONTEXT_VERSION            0
#define POWER_REQUEST_CONTEXT_SIMPLE_STRING      0x00000001
#define POWER_REQUEST_CONTEXT_DETAILED_STRING    0x00000002
#define PowerRequestMaximum                      3

typedef enum _POWER_REQUEST_TYPE {
  PowerRequestDisplayRequired,
  PowerRequestSystemRequired,
  PowerRequestAwayModeRequired
} POWER_REQUEST_TYPE, *PPOWER_REQUEST_TYPE;
#endif

/*
 * Power requests keep the machine, or just the display, awake while something
 * that matters to the user is going on. We do not act on them yet: the request
 * is validated and given a handle the caller can hold and close, but nothing
 * feeds into power policy. Callers treat a failure here as fatal, so refusing
 * outright is worse than accepting and ignoring.
 */

/*
 * @unimplemented
 */
HANDLE
WINAPI
PowerCreateRequest(IN PREASON_CONTEXT Context)
{
    HANDLE RequestHandle;
    NTSTATUS Status;

    if (Context == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    /* Only version 0 exists, and the reason is either a plain string or one
       pulled out of a module's resources, never both and never neither */
    if (Context->Version != POWER_REQUEST_CONTEXT_VERSION ||
        (Context->Flags & ~(POWER_REQUEST_CONTEXT_SIMPLE_STRING |
                            POWER_REQUEST_CONTEXT_DETAILED_STRING)) != 0 ||
        (Context->Flags & (POWER_REQUEST_CONTEXT_SIMPLE_STRING |
                           POWER_REQUEST_CONTEXT_DETAILED_STRING)) == 0 ||
        (Context->Flags & POWER_REQUEST_CONTEXT_SIMPLE_STRING &&
         Context->Flags & POWER_REQUEST_CONTEXT_DETAILED_STRING))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    /* The caller owns this handle and closes it with CloseHandle(), so it has
       to be a real kernel handle rather than something we made up */
    Status = NtCreateEvent(&RequestHandle,
                           EVENT_ALL_ACCESS,
                           NULL,
                           NotificationEvent,
                           FALSE);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return INVALID_HANDLE_VALUE;
    }

    DPRINT1("PowerCreateRequest(%p): power requests are not honoured yet\n", Context);

    return RequestHandle;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
PowerSetRequest(IN HANDLE PowerRequest,
                IN POWER_REQUEST_TYPE RequestType)
{
    if (PowerRequest == NULL || PowerRequest == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (RequestType >= PowerRequestMaximum)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    DPRINT1("PowerSetRequest(%p, %d): ignored\n", PowerRequest, RequestType);

    return TRUE;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
PowerClearRequest(IN HANDLE PowerRequest,
                  IN POWER_REQUEST_TYPE RequestType)
{
    if (PowerRequest == NULL || PowerRequest == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (RequestType >= PowerRequestMaximum)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    DPRINT1("PowerClearRequest(%p, %d): ignored\n", PowerRequest, RequestType);

    return TRUE;
}
