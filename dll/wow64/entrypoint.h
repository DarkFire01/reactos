#pragma once

#include "ros_wow64_private.h"

NTSTATUS
Wow64InitEntrypointTranslation(VOID);

NTSTATUS
Wow64TranslateEntrypoint32To64(IN  HANDLE hProcess,
                               OUT PCONTEXT pContext, 
                               IN  PI386_CONTEXT pContext32);

NTSTATUS
Wow64TranslateEntrypoint64To32(IN  HANDLE hProcess,
                               OUT PI386_CONTEXT pContext32, 
                               IN  PCONTEXT pContext);