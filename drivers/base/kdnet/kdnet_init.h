

#ifndef _KDNET_INIT_H_
#define _KDNET_INIT_H_

#include <reactos/kdnetextensibility.h>

NTSTATUS NTAPI KdDebuggerInitialize0(_In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock);
NTSTATUS NTAPI KdDebuggerInitialize1(_In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock);

#endif /* _KDNET_INIT_H_ */
