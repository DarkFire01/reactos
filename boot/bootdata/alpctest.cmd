@echo off
rem ALPC conformance suite auto-runner for the live image.
rem Output is mirrored to the kernel debug log (COM1) via dbgprint, framed by
rem sentinels so the host can extract results from the serial capture.
setlocal
set TESTS=AlpcHelpers NtAlpcCreatePort NtAlpcConnectPort NtAlpcConnectPortEx NtAlpcAcceptConnectPort SendReceiveSync SendReceiveAsync NtAlpcDisconnectPort NtAlpcCancelMessage MessageValidation NtAlpcQueryInformation AlpcView ViewTransfer NtAlpcOpenSender NtAlpcQueryInformationMessage NtAlpcImpersonate NtAlpcResourceReserve NtAlpcSecurityContext CompletionList ConnectPending CommPortReceive SectionRounding OpenSenderDatagram HandleTransfer ImpersonateDenied CancelRoundTrip DisconnectPeer ContextAttribute TokenAttribute SecurityAttribute CompletionPortAssoc LargeMessage ReserveEnforcement CompletionListDelivery LegacyLpc
dbgprint "===ALPC_TEST_BEGIN==="
for %%T in (%TESTS%) do (
    dbgprint "===TEST %%T==="
    dbgprint --process "%SystemRoot%\bin\alpc_apitest.exe %%T"
)
dbgprint "===ALPC_TEST_END==="
