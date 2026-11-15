
#include <stdarg.h>

#include "rpc.h"
#include "rpcndr.h"
#include "rpcasync.h"

#include "wine/debug.h"

#include "rpc_binding.h"
#include "rpc_message.h"
#include "ndr_stubless.h"

WINE_DEFAULT_DEBUG_CHANNEL(rpc);

/***********************************************************************
 *           RpcGetAuthorizationContextForClient [RPCRT4.@]
 *
 * Called by RpcFreeAuthorizationContext to return the Authz context.
 *
 * PARAMS
 *  ClientBinding        [I] Binding handle, represents a binding to a client on the server.
 *  ImpersonateOnReturn  [I] Directs this function to be represented the client on return.
 *  Reserved1            [I] Reserved, equal to null.
 *  expiration_time      [I] Points to the exact date and time when the token expires.
 *  Reserved2            [I] Reserved, equal to a LUID structure which has a members,
 *                           each of them is set to zero.
 *  Reserved3            [I] Reserved, equal to zero.
 *  Reserved4            [I] Reserved, equal to null.
 *  authz_client_context [I] Points to an AUTHZ_CLIENT_CONTEXT_HANDLE structure
 *                           that has direct pass to Authz functions.
 *
 * RETURNS
 *  Success: RPC_S_OK.
 *  Failure: Any error code.
 */
RPC_STATUS
WINAPI
RpcGetAuthorizationContextForClient(RPC_BINDING_HANDLE ClientBinding,
                                    BOOL ImpersonateOnReturn,
                                    void * Reserved1,
                                    PLARGE_INTEGER expiration_time,
                                    LUID Reserved2,
                                    DWORD Reserved3,
                                    PVOID Reserved4,
                                    PVOID *authz_client_context)
{
    FIXME("(%p, %d, %p, %p, (%d, %u), %u, %p, %p): stub\n",
          ClientBinding,
          ImpersonateOnReturn,
          Reserved1,
          expiration_time,
          Reserved2.HighPart,
          Reserved2.LowPart,
          Reserved3,
          Reserved4,
          authz_client_context);
    return RPC_S_NO_CONTEXT_AVAILABLE;
}

static
RPC_WSTR
RpcpProtseqName(
    _In_ ULONG ProtocolSequence)
{
    switch (ProtocolSequence)
    {
        case RPC_PROTSEQ_TCP:
            return (RPC_WSTR)L"ncacn_ip_tcp";
        case RPC_PROTSEQ_NMP:
            return (RPC_WSTR)L"ncacn_np";
        case RPC_PROTSEQ_LRPC:
            return (RPC_WSTR)L"ncalrpc";
        case RPC_PROTSEQ_HTTP:
            return (RPC_WSTR)L"ncacn_http";
        default:
            return NULL;
    }
}

/**
 * @brief Creates a binding handle from a template without contacting the server.
 */
RPC_STATUS
WINAPI
RpcBindingCreateW(
    _In_ RPC_BINDING_HANDLE_TEMPLATE_V1_W *Template,
    _In_opt_ RPC_BINDING_HANDLE_SECURITY_V1_W *Security,
    _In_opt_ RPC_BINDING_HANDLE_OPTIONS_V1 *Options,
    _Out_ RPC_BINDING_HANDLE *Binding)
{
    RPC_WSTR Protseq;
    RPC_WSTR Uuid = NULL;
    RPC_WSTR StringBinding = NULL;
    RPC_BINDING_HANDLE Handle = NULL;
    RPC_STATUS Status;

    TRACE("(%p, %p, %p, %p)\n", Template, Security, Options, Binding);

    if (!Template || !Binding || Template->Version != 1)
        return RPC_S_INVALID_ARG;
    if (Security && Security->Version != 1)
        return RPC_S_INVALID_ARG;
    if (Options && Options->Version != 1)
        return RPC_S_INVALID_ARG;

    Protseq = RpcpProtseqName(Template->ProtocolSequence);
    if (!Protseq)
        return RPC_S_PROTSEQ_NOT_SUPPORTED;

    if (Template->Flags & RPC_BHT_OBJECT_UUID_VALID)
    {
        Status = UuidToStringW(&Template->ObjectUuid, &Uuid);
        if (Status != RPC_S_OK)
            return Status;
    }

    Status = RpcStringBindingComposeW(Uuid,
                                      Protseq,
                                      Template->NetworkAddress,
                                      Template->StringEndpoint,
                                      NULL,
                                      &StringBinding);
    if (Status == RPC_S_OK)
        Status = RpcBindingFromStringBindingW(StringBinding, &Handle);

    if (Status == RPC_S_OK && Security)
    {
        Status = RpcBindingSetAuthInfoExW(Handle,
                                          Security->ServerPrincName,
                                          Security->AuthnLevel,
                                          Security->AuthnSvc,
                                          Security->AuthIdentity,
                                          RPC_C_AUTHZ_NONE,
                                          Security->SecurityQos);
    }

    if (Status == RPC_S_OK && Options)
        Status = RpcMgmtSetComTimeout(Handle, Options->ComTimeout);

    if (Status != RPC_S_OK && Handle)
        RpcBindingFree(&Handle);

    if (StringBinding)
        RpcStringFreeW(&StringBinding);
    if (Uuid)
        RpcStringFreeW(&Uuid);

    *Binding = Handle;
    return Status;
}

/**
 * @brief Binds a handle to its server. Connections are made on the first call, so there is nothing to do yet.
 */
RPC_STATUS
WINAPI
RpcBindingBind(
    _In_opt_ PRPC_ASYNC_STATE pAsync,
    _In_ RPC_BINDING_HANDLE Binding,
    _In_ RPC_IF_HANDLE IfSpec)
{
    TRACE("(%p, %p, %p)\n", pAsync, Binding, IfSpec);

    if (!Binding)
        return RPC_S_INVALID_BINDING;

    /* Asynchronous completion of a bind is not supported */
    if (pAsync)
        return RPC_S_CANNOT_SUPPORT;

    return RPC_S_OK;
}

/**
 * @brief Drops the server connection of a handle while keeping the handle usable.
 * Connections live only for the duration of a call here, so there is nothing to drop.
 */
RPC_STATUS
WINAPI
RpcBindingUnbind(
    _In_ RPC_BINDING_HANDLE Binding)
{
    TRACE("(%p)\n", Binding);

    if (!Binding)
        return RPC_S_INVALID_BINDING;

    return RPC_S_OK;
}
