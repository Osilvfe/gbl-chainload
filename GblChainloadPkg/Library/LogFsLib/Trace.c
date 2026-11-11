/** @file Trace.c — synchronous boot trace that survives a hang/reset.

  UefiLog1.txt is produced by the platform's status-code handler and only
  flushed at "write final log" just before ExitBootServices. A boot that
  hangs or resets earlier leaves no record at all. LogFsTrace writes a single
  line straight to \GblTrace.txt and flushes it, so the last completed step
  is still on disk after a reset.

  The trace file is truncated on every LogFsInit() and capped so it can never
  grow without bound on the logfs partition.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/LogFsLib.h>
#include <Library/PrintLib.h>
#include <Protocol/SimpleFileSystem.h>

extern EFI_FILE_PROTOCOL *gLogFsRoot;

/* QcomModulePkg/Library/BootLib/LinuxLoaderLib.c. Lets a trace line carry the
   firmware uptime, which distinguishes a hang from a watchdog reset. */
extern UINT64 GetTimerCountms (VOID);

#define TRACE_FILE_PATH  L"\\GblTrace.txt"
#define TRACE_SIZE_LIMIT (48 * 1024)

STATIC
EFI_STATUS
OpenTraceFile (
  OUT EFI_FILE_PROTOCOL **Out
  )
{
  EFI_STATUS Status;

  Status = gLogFsRoot->Open (gLogFsRoot, Out, TRACE_FILE_PATH,
                             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                               EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR (Status)) {
    *Out = NULL;
    return Status;
  }

  /* Append: position at end of file. */
  Status = (*Out)->SetPosition (*Out, 0xFFFFFFFFFFFFFFFFULL);
  if (EFI_ERROR (Status)) {
    (*Out)->Close (*Out);
    *Out = NULL;
  }

  return Status;
}

/* Keep the file open across trace lines.  Re-opening + flushing on every line
   hammers the on-device FAT driver, and the far more common failure mode
   (the driver giving up mid-boot) would silently swallow the remaining
   milestones — exactly what we need to avoid while chasing a hang. */
STATIC EFI_FILE_PROTOCOL *mTraceFile = NULL;
STATIC UINT64             mTraceBytes = 0;

EFI_STATUS
EFIAPI
LogFsTraceReset (VOID)
{
  EFI_STATUS Status;

  if (mTraceFile != NULL) {
    mTraceFile->Close (mTraceFile);
    mTraceFile = NULL;
  }
  mTraceBytes = 0;
  if (gLogFsRoot == NULL) {
    return EFI_NOT_READY;
  }

  Status = gLogFsRoot->Open (gLogFsRoot, &mTraceFile, TRACE_FILE_PATH,
                             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (!EFI_ERROR (Status)) {
    mTraceFile->Delete (mTraceFile);
    mTraceFile = NULL;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
LogFsTraceClose (VOID)
{
  if (mTraceFile != NULL) {
    mTraceFile->Flush (mTraceFile);
    mTraceFile->Close (mTraceFile);
    mTraceFile = NULL;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
LogFsTrace (
  IN CONST CHAR8 *Text
  )
{
  EFI_STATUS Status;
  UINTN      Size;
  CHAR8      Line[160];

  if (Text == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (gLogFsRoot == NULL) {
    Status = LogFsTraceMount ();
    if (EFI_ERROR (Status)) {
      return Status;
    }
    /* Any handle we held was closed by LogFsClose(). */
    mTraceFile = NULL;
  }

  if (mTraceFile == NULL) {
    Status = OpenTraceFile (&mTraceFile);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if (mTraceBytes > TRACE_SIZE_LIMIT) {
    LogFsTraceReset ();
    Status = OpenTraceFile (&mTraceFile);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  AsciiSPrint (Line, sizeof (Line), "%8lu %a", GetTimerCountms (), Text);
  Size = AsciiStrLen (Line);
  Status = mTraceFile->Write (mTraceFile, &Size, Line);
  mTraceBytes += Size;
  if (!EFI_ERROR (Status)) {
    Status = mTraceFile->Flush (mTraceFile);
  }

  if (EFI_ERROR (Status)) {
    /* Drop the handle so the next line re-opens cleanly. */
    mTraceFile->Close (mTraceFile);
    mTraceFile = NULL;
  }

  return Status;
}
