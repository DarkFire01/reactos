#pragma once

#include "../wow64/ros_wow64_private.h"
NTSTATUS NTAPI BTCpuGetContext(HANDLE hThread, HANDLE hProcess, PVOID Unused, WOW64_CONTEXT* pCtx32);
NTSTATUS NTAPI BTCpuSetContext(HANDLE hThread, HANDLE hProcess, PVOID Unused, WOW64_CONTEXT* pCtx32);