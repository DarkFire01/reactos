;/*
; * PROJECT:     ReactOS HID Stack
; * LICENSE:     MIT (https://spdx.org/licenses/MIT)
; * PURPOSE:     HID over I2C problem descriptions
; * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
; */

MessageIdTypedef=NTSTATUS

SeverityNames=(Success=0x0:HIDI2C_SEVERITY_SUCCESS
               Informational=0x1:HIDI2C_SEVERITY_INFORMATIONAL
               Warning=0x2:HIDI2C_SEVERITY_WARNING
               Error=0x3:HIDI2C_SEVERITY_ERROR
              )

FacilityNames=(Problem=0x1:HIDI2C_FACILITY_PROBLEM
              )

LanguageNames=(English=0x409:MSG00409
              )

MessageId=0x0000
Severity=Error
Facility=Problem
SymbolicName=HIDI2C_PROBLEM_ACPI
Language=English
The firmware did not describe the HID descriptor address through the ACPI _DSM method, or described it incorrectly.
Contact the system manufacturer for help.
.

MessageId=0x0001
Severity=Error
Facility=Problem
SymbolicName=HIDI2C_PROBLEM_DESCRIPTOR_READ
Language=English
The device did not return its HID descriptor. The device firmware may be faulty.
Contact the system manufacturer for help.
.

MessageId=0x0002
Severity=Error
Facility=Problem
SymbolicName=HIDI2C_PROBLEM_DESCRIPTOR_INVALID
Language=English
The device returned a HID descriptor that is not valid. The device firmware may be faulty.
Contact the system manufacturer for help.
.

MessageId=0x0003
Severity=Error
Facility=Problem
SymbolicName=HIDI2C_PROBLEM_SET_POWER
Language=English
The device did not accept the SET_POWER command. The device firmware may be faulty.
Contact the system manufacturer for help.
.

MessageId=0x0004
Severity=Error
Facility=Problem
SymbolicName=HIDI2C_PROBLEM_RESET
Language=English
The device did not accept the RESET command. The device firmware may be faulty.
Contact the system manufacturer for help.
.

MessageId=0x0005
Severity=Error
Facility=Problem
SymbolicName=HIDI2C_PROBLEM_RESOURCES
Language=English
The firmware did not assign the device an I2C connection, an interrupt, or both.
Contact the system manufacturer for help.
.
