#pragma once

BOOLEAN 
Rs232PortInitialize(IN ULONG ComPort,
                    IN ULONG BaudRate);

VOID 
Rs232PortPutByte(UCHAR ByteToSend);
