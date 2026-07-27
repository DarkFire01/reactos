/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared harness for the standalone d3dtest_* programs
 * COPYRIGHT:   Copyright 2026 The ReactOS Project
 *
 * Each test is a self-contained console program. It prints one line per check
 * and exits 0 only if every check passed, so the whole set can be driven from
 * a script. Nothing here allocates before main() or depends on the other
 * tests, so a crash in one says nothing about the others.
 */

#ifndef _D3DTEST_H_
#define _D3DTEST_H_

/* The tests drive COM interfaces from C, so they need the Iface_Method()
   convenience macros the DirectX headers only emit under COBJMACROS. */
#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Every test uses only part of this harness; do not make the rest an error. */
#if defined(__GNUC__)
# define D3DTEST_UNUSED __attribute__((unused))
#else
# define D3DTEST_UNUSED
#endif

static int D3DTEST_UNUSED d3dtest_checks = 0;
static int D3DTEST_UNUSED d3dtest_failures = 0;
static int D3DTEST_UNUSED d3dtest_skips = 0;
static const char * D3DTEST_UNUSED d3dtest_name = "test";

static void D3DTEST_UNUSED test_begin(const char *name)
{
    d3dtest_name = name;
    printf("=== %s ===\n", name);
    fflush(stdout);
}

/* A check that must hold for the test to be meaningful. */
static void D3DTEST_UNUSED ok_(int cond, const char *fmt, ...)
{
    va_list args;

    d3dtest_checks++;
    if (!cond)
        d3dtest_failures++;

    printf("%s: ", cond ? "PASS" : "FAIL");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

/* Something we could not exercise here -- no device, no driver support, an
   optional format. Reported, but does not fail the run: these tests have to be
   able to say "this machine cannot do that" without lying about it. */
static void D3DTEST_UNUSED skip_(const char *fmt, ...)
{
    va_list args;

    d3dtest_skips++;
    printf("SKIP: ");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

static void D3DTEST_UNUSED info_(const char *fmt, ...)
{
    va_list args;

    printf("info: ");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

static int D3DTEST_UNUSED test_end(void)
{
    printf("--- %s: %d checks, %d failed, %d skipped ---\n",
           d3dtest_name, d3dtest_checks, d3dtest_failures, d3dtest_skips);
    fflush(stdout);
    return d3dtest_failures ? 1 : 0;
}

/* Most of these tests need a window to point a device at. */
static HWND D3DTEST_UNUSED test_create_window(const char *title, int width, int height)
{
    WNDCLASSA wc;
    HWND hwnd;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "d3dtest_window";
    RegisterClassA(&wc);

    hwnd = CreateWindowExA(0, "d3dtest_window", title, WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                           NULL, NULL, wc.hInstance, NULL);
    return hwnd;
}

static void D3DTEST_UNUSED test_destroy_window(HWND hwnd)
{
    if (hwnd)
        DestroyWindow(hwnd);
    UnregisterClassA("d3dtest_window", GetModuleHandleA(NULL));
}

/* Pump the queue so the window manager does not think we are hung while a
   device is being created or a frame presented. */
static void D3DTEST_UNUSED test_pump(void)
{
    MSG msg;

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

#define D3DTEST_RELEASE(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

#endif /* _D3DTEST_H_ */
