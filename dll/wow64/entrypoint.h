#pragma once

#include "ros_wow64_private.h"

NTSTATUS
Wow64InitEntrypointTranslation(VOID);

ULONG_PTR
Wow64TranslateEntrypoint32To64(ULONG Entrypoint);

ULONG
Wow64TranslateEntrypoint64To32(ULONG_PTR Entrypoint);
