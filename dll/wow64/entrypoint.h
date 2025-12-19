#pragma once

#include "ros_wow64_private.h"

NTSTATUS
Wow64InitEntrypointTranslation(VOID);

ULONG_PTR
Wow64TranslateEntrypoint32To64(PCONTEXT pContext, PI386_CONTEXT pContext32);

ULONG
Wow64TranslateEntrypoint64To32(PI386_CONTEXT pContext32, PCONTEXT pContext);