/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Elapsed time measurement for the histogram statistics
 *
 * Two flavors because the callers pick their cost: the performance counter
 * is precise and reads a device, the tick counter is cheap and coarse.
 */

#pragma once

#include <KMacros.h>

class KStopwatch
{
public:

    NONPAGED
    void
    Start(
        void)
    {
        m_start = KeQueryPerformanceCounter(nullptr);
    }

    NONPAGED
    ULONG64
    Stop(
        void)
    {
        LARGE_INTEGER Now = KeQueryPerformanceCounter(nullptr);

        return (ULONG64)(Now.QuadPart - m_start.QuadPart);
    }

private:

    LARGE_INTEGER m_start = {};
};

class KTickCounterStopwatch
{
public:

    NONPAGED
    void
    Start(
        void)
    {
        m_start = KeQueryInterruptTime();
    }

    NONPAGED
    ULONG64
    Stop(
        void)
    {
        return KeQueryInterruptTime() - m_start;
    }

private:

    ULONG64 m_start = 0;
};
