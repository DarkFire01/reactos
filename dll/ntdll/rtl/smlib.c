/* Why is this imported like this?
 *
 * Mostly just to make win32ss from Longhorn 5048 -> Vista RTM happy
 * Apperently this is a very critical systemcall :D
 * thanks microsoft
 * */
#include <ntdll.h>
#define NTOS_MODE_USER
#include <ndk/lpctypes.h>
#include <ndk/lpcfuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>

#include <sm/ns.h>
#include <sm/smmsg.h>

#define NDEBUG
#include <debug.h>


NTSTATUS
NTAPI
RtlConnectToSm(
    _In_opt_ PUNICODE_STRING SbApiPortName,
    _In_opt_ HANDLE SbApiPort,
    _In_opt_ ULONG ImageType,
    _Out_ PHANDLE SmApiPort)
{
    NTSTATUS Status;
    SECURITY_QUALITY_OF_SERVICE SecurityQos;
    UNICODE_STRING PortName;
    SB_CONNECTION_INFO ConnectInfo = {0};
    ULONG ConnectInfoLength = sizeof(ConnectInfo);

    /* Setup the QoS structure */
    SecurityQos.ImpersonationLevel = SecurityIdentification;
    SecurityQos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
    SecurityQos.EffectiveOnly = TRUE;

    /* Set the SM API port name */
    RtlInitUnicodeString(&PortName, L"\\SmApiPort"); // SM_API_PORT_NAME

    /* Check if this is a client connecting to SMSS, or SMSS to itself */
    if (SbApiPortName)
    {
        /* A client SB port as well as an image type must be present */
        if (!SbApiPort || (ImageType == IMAGE_SUBSYSTEM_UNKNOWN))
            return STATUS_INVALID_PARAMETER_MIX;

        /* Validate SbApiPortName's length */
        if (SbApiPortName->Length >= sizeof(ConnectInfo.SbApiPortName))
            return STATUS_INVALID_PARAMETER;

        /* Copy the client port name, and NULL-terminate it */
        RtlCopyMemory(ConnectInfo.SbApiPortName,
                      SbApiPortName->Buffer,
                      SbApiPortName->Length);
        ConnectInfo.SbApiPortName[SbApiPortName->Length / sizeof(WCHAR)] = UNICODE_NULL;

        /* Save the subsystem type */
        ConnectInfo.SubsystemType = ImageType;
    }
    else
    {
        /* No client port, and the subsystem type is not set */
        ConnectInfo.SbApiPortName[0] = UNICODE_NULL;
        ConnectInfo.SubsystemType = IMAGE_SUBSYSTEM_UNKNOWN;
    }

    /* Connect to SMSS and exchange connection information */
    Status = NtConnectPort(SmApiPort,
                           &PortName,
                           &SecurityQos,
                           NULL,
                           NULL,
                           NULL,
                           &ConnectInfo,
                           &ConnectInfoLength);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SmConnectToSm: Connect to Sm failed %lx\n", Status);
    }
#if (NTDDI_VERSION < NTDDI_VISTA)
    else
    {
        /* Treat a warning or informational status as success */
        Status = STATUS_SUCCESS;
    }
#endif

    /* Return if the connection was successful or not */
    return Status;
}

/**
 * @brief
 * Sends a message to the SM via the SM API port.
 *
 * @param[in]   SmApiPort
 * Port handle returned by SmConnectToSm().
 *
 * @param[in,out]   SmApiMsg
 * Message to send to the SM. The API-specific data must be initialized,
 * and the SmApiMsg->ApiNumber must be specified accordingly.
 *
 * @return
 * Success status as handed by the SM reply; otherwise a failure
 * status code.
 *
 * @remark
 * Exported on Vista+ by NTDLL and called RtlSendMsgToSm().
 **/
NTSTATUS
NTAPI
RtlSendMsgToSm(
    _In_ HANDLE SmApiPort,
    _Inout_ PSM_API_MSG SmApiMsg)
{
    static ULONG RtlpSmMessageInfo[SmpMaxApiNumber] =
    {
        0 /*sizeof(SM_CREATE_FOREIGN_SESSION_MSG)*/,
        sizeof(SM_SESSION_COMPLETE_MSG),
        0 /*sizeof(SM_TERMINATE_FOREIGN_SESSION_MSG)*/,
        sizeof(SM_EXEC_PGM_MSG),
        sizeof(SM_LOAD_DEFERED_SUBSYSTEM_MSG),
        sizeof(SM_START_CSR_MSG),
        sizeof(SM_STOP_CSR_MSG),
    };

    NTSTATUS Status;
    ULONG DataLength;

    if (SmApiMsg->ApiNumber >= SmpMaxApiNumber)
        return STATUS_NOT_IMPLEMENTED;

    /* Obtain the necessary data length for this API */
    DataLength = RtlpSmMessageInfo[SmApiMsg->ApiNumber];

    /* Fill out the Port Message Header */
    // RtlZeroMemory(&SmApiMsg->h, sizeof(SmApiMsg->h));
    SmApiMsg->h.u2.ZeroInit = 0;
    /* DataLength = user_data_size + anything between
     * header and data, including intermediate padding */
    SmApiMsg->h.u1.s1.DataLength = (CSHORT)DataLength +
        FIELD_OFFSET(SM_API_MSG, u) - sizeof(SmApiMsg->h);
    /* TotalLength = sizeof(*SmApiMsg) on <= NT5.2, otherwise:
     * DataLength + header_size == user_data_size + FIELD_OFFSET(SM_API_MSG, u)
     * without structure trailing padding */
    SmApiMsg->h.u1.s1.TotalLength = SmApiMsg->h.u1.s1.DataLength + sizeof(SmApiMsg->h);

    /* Send the LPC message and wait for a reply */
    Status = NtRequestWaitReplyPort(SmApiPort, &SmApiMsg->h, &SmApiMsg->h);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SmSendMsgToSm: NtRequestWaitReplyPort failed, Status: 0x%08lx\n", Status);
    }
    else
    {
        /* Return the real status */
        Status = SmApiMsg->ReturnValue;
    }

    return Status;
}