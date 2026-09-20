/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     User mode part of the ndis.sys NET_BUFFER_LIST test suite
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kmt_test.h>

#include "ndisnbl.h"

START_TEST(NdisNbl)
{
    DWORD Error;

    Error = KmtLoadAndOpenDriver(L"NdisNbl", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    Error = KmtSendToDriver(IOCTL_TEST_NBL);
    ok_eq_ulong(Error, ERROR_SUCCESS);

    KmtCloseDriver();
    KmtUnloadDriver();
}
