; A module built for Windows names KERNELBASE.dll among its imports, and this
; is that name. Nothing here is implemented: each export is handed on to the
; module in this tree that owns it, and the list grows as binaries ask for more.
@ stdcall -private LocalAlloc(long long) kernel32.LocalAlloc
@ stdcall -private LocalFree(long) kernel32.LocalFree
@ stdcall -private LocalReAlloc(long long long) kernel32.LocalReAlloc
@ stdcall -private Sleep(long) kernel32.Sleep
