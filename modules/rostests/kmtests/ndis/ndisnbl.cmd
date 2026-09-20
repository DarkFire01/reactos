@echo off
rem Run the NBL kmtest and pipe its output to the debug port, so an unattended
rem QEMU run reports results on the serial line. kmtest writes to stdout, which
rem nothing on a headless run would otherwise see.
cd /d "%SystemRoot%\bin"
dbgprint NDISNBL-BEGIN
dbgprint --process "kmtest.exe NdisNbl"
dbgprint --process "kmtest.exe NdisXlate"
dbgprint NDISNBL-END
