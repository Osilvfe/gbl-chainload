/** @file LogFsLib.h
  Mount the device's `logfs` partition (located by GPT label) and rotate
  UefiLog1.txt → UefiLogSaved{0..4}.txt across boots so BDS log history
  is preserved.

  The GblChainload_BootN.txt auto-mirror mechanism has been removed.
  UefiLog<N>.txt — written by QCOM's BSP-installed status-code handler
  (gBS->ReportStatusCode → handler → UART log buffer → BDS flush, the
  same path PeiDxeDebugLibReportStatusCode targets) — is the sole
  persistent log destination.
**/
#ifndef GBL_CHAINLOAD_LOGFSLIB_H
#define GBL_CHAINLOAD_LOGFSLIB_H

#include <Uefi.h>

/** Mount logfs and rotate UefiLog1.txt → UefiLogSaved{0..4}.txt.

    @retval EFI_SUCCESS    logfs mounted and rotation attempted
    @retval EFI_NOT_FOUND  logfs partition absent
    @retval other          partition lookup / mount errors propagated
**/
EFI_STATUS
EFIAPI
LogFsInit (VOID);

/** Close the logfs root volume handle. Call before any chain-load handoff
    so the next EFI image can mount the partition without finding it bound
    to our driver instance. No-op if LogFsInit was not called or already
    closed. **/
EFI_STATUS
EFIAPI
LogFsClose (VOID);

/** Mount logfs without rotating UefiLog1.txt. Used by the boot trace once
    LogFsClose() has released the volume for a fastboot handoff. **/
EFI_STATUS
EFIAPI
LogFsTraceMount (VOID);

/** Append one ASCII line to \GblTrace.txt and flush it immediately.

    The normal UEFI log path only reaches logfs at "write final log", just
    before ExitBootServices, so a hang earlier in the boot loses everything.
    This writer keeps the last completed step across a reset. The file is
    truncated by LogFsInit() on every boot, so it describes the last boot.

    @retval EFI_NOT_READY  logfs is not mounted and cannot be mounted.
**/
EFI_STATUS
EFIAPI
LogFsTrace (IN CONST CHAR8 *Text);

/** Delete \GblTrace.txt so the current boot starts with an empty trace. **/
EFI_STATUS
EFIAPI
LogFsTraceReset (VOID);

/** Close the trace file handle but keep logfs mounted.

    Call before ExitBootServices: leaving an open FAT file behind while the
    block-IO stack is torn down is avoidable risk. The next LogFsTrace()
    reopens the file in append mode. **/
EFI_STATUS
EFIAPI
LogFsTraceClose (VOID);

#endif /* GBL_CHAINLOAD_LOGFSLIB_H */
