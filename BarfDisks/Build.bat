@echo off

set Path=C:\TDM-gcc-64\bin

cls

gcc -std=c99 barfdisks.c ntfs.c utility.c -o BarfGptCrap.exe


pause
