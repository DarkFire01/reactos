/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     min and max for builds with NOMINMAX, where the windows.h macros would break <limits>
 */

#pragma once

/* Force included ahead of every header, so it cannot use SAL or Windows types */
#if defined(NOMINMAX) && defined(__cplusplus)

template <typename First, typename Second>
constexpr
auto
min(
    First Left,
    Second Right)
{
    return (Right < Left) ? Right : Left;
}

template <typename First, typename Second>
constexpr
auto
max(
    First Left,
    Second Right)
{
    return (Left < Right) ? Right : Left;
}

#endif
