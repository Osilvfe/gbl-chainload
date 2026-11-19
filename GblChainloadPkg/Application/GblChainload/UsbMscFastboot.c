#include "UsbMsc.h"
#include "SlotAttrs.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Protocol/EFIUsbDevice.h>
#include "../../../edk2/QcomModulePkg/Library/FastbootLib/FastbootMain.h"

VOID FastbootRegister (CONST CHAR8 *Prefix,
                      VOID (*Handler)(CONST CHAR8 *Argument, VOID *Data, UINT32 Size));
VOID FastbootOkay (CONST CHAR8 *Response);
VOID FastbootFail (CONST CHAR8 *Reason);
BOOLEAN FlashComplete (VOID);

STATIC BOOLEAN mMscRequested;

STATIC CONST CHAR8 *const mMediaUsage =
  "usage: oem usb-msc [name[,name...]] "
  "(partition name, iso:path or file:path)";

STATIC BOOLEAN
IsFileSpec (IN CONST CHAR8 *Spec)
{
  UINTN Index;

  for (Index = 0; Index < 5 && Spec[Index] != 0; Index++) {
    if (Spec[Index] == ':') {
      return Index != 0;
    }
  }
  return FALSE;
}

STATIC VOID
CmdUsbMsc (IN CONST CHAR8 *Argument, IN VOID *Data, IN UINT32 Size)
{
  EFI_STATUS Status;
  USB_DEVICE_EVENT Message;
  USB_DEVICE_EVENT_DATA Payload;
  UINTN PayloadSize;
  UINTN Attempt;
  UINTN NameLength = 0;
  CHAR8 Partition[GBL_MSC_SPEC_SIZE] = {0};
  EFI_USB_DEVICE_PROTOCOL *Usb = GetFastbootDeviceData ()->UsbDeviceProtocol;

  (VOID)Data;
  (VOID)Size;
  if (*Argument != '\0' && *Argument != ' ') {
    FastbootFail (mMediaUsage);
    return;
  }
  while (*Argument == ' ') {
    Argument++;
  }
  while (*Argument != '\0' && *Argument != ' ' && NameLength < sizeof (Partition) - 1) {
    Partition[NameLength++] = *Argument++;
  }
  while (*Argument == ' ') {
    Argument++;
  }
  if (*Argument != '\0') {
    FastbootFail (mMediaUsage);
    return;
  }
  if (NameLength != 0 && !GBL_USB_MSC_PARTITION_EXPORT) {
    FastbootFail ("media export is disabled in this build; use oem usb-msc");
    return;
  }
  if (!FlashComplete ()) {
    FastbootFail ("wait for the current flash operation to finish");
    return;
  }
  if (Usb == NULL || Usb->HandleEvent == NULL || Usb->Stop == NULL || Usb->StartEx == NULL) {
    FastbootFail ("USB device mode switching is unavailable");
    return;
  }
  Status = GblUsbMscPrepare (Partition);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "USB MSC: preflight failed: %r\n", Status));
    if (Status == EFI_INVALID_PARAMETER) {
      FastbootFail (mMediaUsage);
    } else if (Status == EFI_ACCESS_DENIED) {
      FastbootFail ("partition export requires an unlocked device");
    } else if (Status == EFI_NO_MAPPING) {
      FastbootFail ("partition name is ambiguous");
    } else if (Status == EFI_NOT_FOUND && NameLength != 0) {
      FastbootFail ("media not found; use an exact partition name, "
                    "iso:<path> or file:<path>");
    } else {
      FastbootFail ("USB or media unavailable, unreadable or unsupported");
    }
    return;
  }
  FastbootOkay (NameLength == 0 ? "switching to read-only RAM disk"
             : (IsFileSpec (Partition) ? "switching to read-only file media"
                                       : "switching to read-only partition"));
  for (Attempt = 0; Attempt < 5000; Attempt++) {
    PayloadSize = sizeof (Payload);
    Message = UsbDeviceEventNoEvent;
    Status = Usb->HandleEvent (&Message, &PayloadSize, &Payload);
    if (EFI_ERROR (Status) && Status != EFI_NOT_READY) {
      GblUsbMscCancel ();
      return;
    }
    if (Message == UsbDeviceEventDeviceStateChange &&
        Payload.DeviceState == UsbDeviceStateDisconnected) {
      GblUsbMscCancel ();
      return;
    }
    if (Message == UsbDeviceEventTransferNotification &&
        Payload.TransferOutcome.EndpointIndex == 0x81) {
      mMscRequested = Payload.TransferOutcome.Status == UsbDeviceTransferStatusCompleteOK;
      if (!mMscRequested) {
        GblUsbMscCancel ();
      }
      return;
    }
    gBS->Stall (1000);
  }
  DEBUG ((DEBUG_ERROR, "USB MSC: Fastboot acknowledgement timed out\n"));
  GblUsbMscCancel ();
}

VOID
GblUsbMscRegisterCommands (VOID)
{
  mMscRequested = FALSE;
  GblUsbMscCancel ();
  FastbootRegister ("oem usb-msc", CmdUsbMsc);
  GblSlotRegisterCommands ();
}

BOOLEAN
GblUsbMscRequested (VOID)
{
  BOOLEAN Requested = mMscRequested;
  mMscRequested = FALSE;
  return Requested;
}
