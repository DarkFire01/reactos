#include "ros_wow64_private.h"

/**********************************************************************
 *           wow64_NtSecureConnectPort
 */
NTSTATUS WINAPI wow64_NtSecureConnectPort(UINT *pArgs)
{
    ULONG *PortHandle = get_ptr(&pArgs);
    UNICODE_STRING32 *PortName32 = get_ptr(&pArgs);
    PSECURITY_QUALITY_OF_SERVICE SecurityQos = get_ptr(&pArgs);
    PORT_VIEW *ClientView = get_ptr(&pArgs);
    SID* ServerSid = get_ptr(&pArgs);
    REMOTE_PORT_VIEW *SecureView = get_ptr(&pArgs);
    PULONG MaxMessageLength = get_ptr(&pArgs);
    PVOID ConnectionInformation = get_ptr(&pArgs);
    PULONG ConnectionInformationLength = get_ptr(&pArgs);
    
    NTSTATUS status;
    HANDLE result;
    WCHAR buffer[MAX_PATH];
    UNICODE_STRING portName;
    
    portName.MaximumLength = sizeof(buffer);
    portName.Buffer = buffer;
    
    unicode_str_32to64(&portName, PortName32);

    
    status = NtSecureConnectPort(&result, 
                                 &portName, 
                                 SecurityQos, 
                                 ClientView,
                                 ServerSid, 
                                 SecureView,
                                 MaxMessageLength,
                                 ConnectionInformation,
                                 ConnectionInformationLength);
    
    *PortHandle = HandleToULong(result);
    
    return status;
}