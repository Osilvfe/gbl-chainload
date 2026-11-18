#include "UsbMsc.h"
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/EFIUsbfnIo.h>

#define MSC_CONTROL_BUFFER 1024U
#define MSC_BULK_BUFFER 65536U

#pragma pack(1)
typedef struct {
  EFI_USB_CONFIG_DESCRIPTOR Configuration;
  EFI_USB_INTERFACE_DESCRIPTOR Interface;
  EFI_USB_ENDPOINT_DESCRIPTOR Out;
  EFI_USB_ENDPOINT_DESCRIPTOR In;
} MSC_CONFIGURATION;

typedef struct {
  EFI_USB_CONFIG_DESCRIPTOR Configuration;
  EFI_USB_INTERFACE_DESCRIPTOR Interface;
  EFI_USB_ENDPOINT_DESCRIPTOR Out;
  EFI_USB_SS_ENDPOINT_COMPANION_DESCRIPTOR OutCompanion;
  EFI_USB_ENDPOINT_DESCRIPTOR In;
  EFI_USB_SS_ENDPOINT_COMPANION_DESCRIPTOR InCompanion;
} MSC_SS_CONFIGURATION;

typedef struct {
  EFI_USB_BOS_DESCRIPTOR Header;
  EFI_USB_USB_20_EXTENSION_DESCRIPTOR Usb2;
  EFI_USB_SUPERSPEED_USB_DESCRIPTOR Usb3;
} MSC_BOS;
#pragma pack()

typedef enum {
  MscWaitCbw,
  MscReadData,
  MscReadZlp,
  MscDiscardData,
  MscSendCsw,
  MscWaitClear,
  MscWaitReset
} MSC_BULK_STATE;

typedef enum {
  MscControlIdle,
  MscControlDataIn,
  MscControlZlp,
  MscControlDataOut,
  MscControlStatusIn,
  MscControlStatusOut
} MSC_CONTROL_STATE;

typedef struct {
  VOID *Buffer;
  UINTN Length;
  BOOLEAN Pending;
  BOOLEAN Aborting;
} MSC_TRANSFER;

typedef struct {
  EFI_USBFN_IO_PROTOCOL *Usb;
  GBL_MSC_MEDIA Media;
  GBL_MSC_BOT Bot;
  MSC_TRANSFER Transfers[4];
  MSC_BULK_STATE BulkState;
  MSC_CONTROL_STATE ControlState;
  EFI_USB_DEVICE_REQUEST Setup;
  UINTN TransferLimit;
  UINTN AbortTicks;
  UINT32 ReadOffset;
  UINT32 OutRemaining;
  UINT16 BulkPacket;
  UINT16 ControlPacket;
  UINT8 ClearMask;
  UINT8 PendingConfiguration;
  BOOLEAN Configured;
  BOOLEAN SetupPending;
  BOOLEAN ResetPending;
  BOOLEAN RecoveryRequired;
  BOOLEAN ResetSeen;
  BOOLEAN ApplyConfiguration;
  BOOLEAN ContinueCsw;
  BOOLEAN ControlStatusDeferred;
  BOOLEAN ControlZlp;
  BOOLEAN SeenBus;
  BOOLEAN ExitRequested;
  BOOLEAN SuperSpeed;
  BOOLEAN SupportsSuperSpeed;
  EFI_USB_DEVICE_DESCRIPTOR Device;
  EFI_USB_DEVICE_DESCRIPTOR SsDevice;
  EFI_USB_DEVICE_QUALIFIER_DESCRIPTOR Qualifier;
  MSC_CONFIGURATION Configuration;
  MSC_SS_CONFIGURATION SsConfiguration;
  MSC_BOS Bos;
  EFI_USB_ENDPOINT_DESCRIPTOR *Endpoints[2];
  EFI_USB_INTERFACE_INFO Interface;
  EFI_USB_INTERFACE_INFO *Interfaces[1];
  EFI_USB_CONFIG_INFO ConfigurationInfo;
  EFI_USB_CONFIG_INFO *Configurations[1];
  EFI_USB_DEVICE_INFO DeviceInfo;
  EFI_USB_SUPERSPEED_ENDPOINT_DESCRIPTOR SsEndpoints[2];
  EFI_USB_SUPERSPEED_ENDPOINT_DESCRIPTOR *SsEndpointPointers[2];
  EFI_USB_SUPERSPEED_INTERFACE_INFO SsInterface;
  EFI_USB_SUPERSPEED_INTERFACE_INFO *SsInterfaces[1];
  EFI_USB_SUPERSPEED_CONFIG_INFO SsConfigurationInfo;
  EFI_USB_SUPERSPEED_CONFIG_INFO *SsConfigurations[1];
  EFI_USB_SUPERSPEED_DEVICE_INFO SsDeviceInfo;
} MSC_USB;

STATIC UINT16 mMscVendor;
STATIC UINT16 mMscProduct;
STATIC UINTN mMscTransferLimit;
STATIC BOOLEAN mMscPrepared;
STATIC GBL_MSC_MEDIA mPreparedMedia;

STATIC EFI_STATUS QueueCbw (IN OUT MSC_USB *Msc);
STATIC EFI_STATUS QueueCsw (IN OUT MSC_USB *Msc);

VOID
GblUsbMscCancel (VOID)
{
  mMscPrepared = FALSE;
  GblMscMediaClose (&mPreparedMedia);
}

EFI_STATUS
GblUsbMscPrepare (IN CONST CHAR8 *Partition)
{
  EFI_USBFN_IO_PROTOCOL *Usb;
  EFI_STATUS Status;
  UINTN Limit;

  GblUsbMscCancel ();
  Status = gBS->LocateProtocol (&gEfiUsbfnIoProtocolGuid, NULL, (VOID **)&Usb);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Usb->Revision < EFI_USBFN_IO_PROTOCOL_REVISION ||
      Usb->StartController == NULL || Usb->StopController == NULL ||
      Usb->ConfigureEnableEndpoints == NULL || Usb->GetVendorIdProductId == NULL ||
      Usb->GetMaxTransferSize == NULL || Usb->AllocateTransferBuffer == NULL ||
      Usb->FreeTransferBuffer == NULL || Usb->EventHandler == NULL ||
      Usb->Transfer == NULL || Usb->AbortTransfer == NULL ||
      Usb->SetEndpointStallState == NULL || Usb->GetEndpointStallState == NULL) {
    return EFI_UNSUPPORTED;
  }
  Status = Usb->GetVendorIdProductId (Usb, &mMscVendor, &mMscProduct);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Usb->GetMaxTransferSize (Usb, &Limit);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Limit < MSC_CONTROL_BUFFER) {
    return EFI_UNSUPPORTED;
  }
  mMscTransferLimit = MIN (Limit, MSC_BULK_BUFFER) & ~(UINTN)1023;
  if (Partition != NULL && Partition[0] != 0) {
    Status = GblMscMediaOpen (Partition, &mPreparedMedia);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  mMscPrepared = TRUE;
  return EFI_SUCCESS;
}

STATIC VOID
InitDescriptors (IN OUT MSC_USB *Msc)
{
  EFI_USB_DEVICE_DESCRIPTOR Device = {
    18, 1, 0x0210, 0, 0, 0, 64, 0, 0, 0x0100, 1, 2, 3, 1
  };
  MSC_CONFIGURATION Configuration = {
    {9, 2, sizeof (MSC_CONFIGURATION), 1, 1, 0, 0x80, 50},
    {9, 4, 0, 0, 2, 8, 6, 0x50, 0},
    {7, 5, 0x01, 2, 512, 0},
    {7, 5, 0x81, 2, 512, 0}
  };
  MSC_SS_CONFIGURATION SsConfiguration = {
    {9, 2, sizeof (MSC_SS_CONFIGURATION), 1, 1, 0, 0x80, 13},
    {9, 4, 0, 0, 2, 8, 6, 0x50, 0},
    {7, 5, 0x01, 2, 1024, 0},
    {6, 48, 0, 0, 0},
    {7, 5, 0x81, 2, 1024, 0},
    {6, 48, 0, 0, 0}
  };
  EFI_USB_DEVICE_QUALIFIER_DESCRIPTOR Qualifier = {10, 6, 0x0210, 0, 0, 0, 64, 1, 0};
  MSC_BOS Bos = {
    {5, 15, sizeof (MSC_BOS), 2},
    {7, 16, 2, 0},
    {10, 16, 3, 0, 0x0e, 1, 0, 0}
  };

  Msc->Device = Device;
  Msc->Device.IdVendor = mMscVendor;
  Msc->Device.IdProduct = mMscProduct;
  Msc->SsDevice = Msc->Device;
  Msc->SsDevice.BcdUSB = 0x0300;
  Msc->SsDevice.MaxPacketSize0 = 9;
  Msc->Qualifier = Qualifier;
  Msc->Configuration = Configuration;
  Msc->SsConfiguration = SsConfiguration;
  Msc->Bos = Bos;
  Msc->Endpoints[0] = &Msc->Configuration.Out;
  Msc->Endpoints[1] = &Msc->Configuration.In;
  Msc->Interface.InterfaceDescriptor = &Msc->Configuration.Interface;
  Msc->Interface.EndpointDescriptorTable = Msc->Endpoints;
  Msc->Interfaces[0] = &Msc->Interface;
  Msc->ConfigurationInfo.ConfigDescriptor = &Msc->Configuration.Configuration;
  Msc->ConfigurationInfo.InterfaceInfoTable = Msc->Interfaces;
  Msc->Configurations[0] = &Msc->ConfigurationInfo;
  Msc->DeviceInfo.DeviceDescriptor = &Msc->Device;
  Msc->DeviceInfo.ConfigInfoTable = Msc->Configurations;
  Msc->SsEndpoints[0].EndpointDescriptor = &Msc->SsConfiguration.Out;
  Msc->SsEndpoints[0].EndpointCompanionDescriptor = &Msc->SsConfiguration.OutCompanion;
  Msc->SsEndpoints[1].EndpointDescriptor = &Msc->SsConfiguration.In;
  Msc->SsEndpoints[1].EndpointCompanionDescriptor = &Msc->SsConfiguration.InCompanion;
  Msc->SsEndpointPointers[0] = &Msc->SsEndpoints[0];
  Msc->SsEndpointPointers[1] = &Msc->SsEndpoints[1];
  Msc->SsInterface.InterfaceDescriptor = &Msc->SsConfiguration.Interface;
  Msc->SsInterface.EndpointDescriptorTable = Msc->SsEndpointPointers;
  Msc->SsInterfaces[0] = &Msc->SsInterface;
  Msc->SsConfigurationInfo.ConfigDescriptor = &Msc->SsConfiguration.Configuration;
  Msc->SsConfigurationInfo.InterfaceInfoTable = Msc->SsInterfaces;
  Msc->SsConfigurations[0] = &Msc->SsConfigurationInfo;
  Msc->SsDeviceInfo.DeviceDescriptor = &Msc->SsDevice;
  Msc->SsDeviceInfo.ConfigInfoTable = Msc->SsConfigurations;
  Msc->SsDeviceInfo.BosDescriptor = &Msc->Bos.Header;
}

STATIC EFI_STATUS
QueueTransfer (IN OUT MSC_USB *Msc, IN UINTN Slot, IN UINTN Length)
{
  EFI_STATUS Status;
  MSC_TRANSFER *Transfer = &Msc->Transfers[Slot];
  UINTN Actual = Length;

  if (Transfer->Pending || Length > (Slot < 2 ? MSC_CONTROL_BUFFER : Msc->TransferLimit)) {
    return EFI_ALREADY_STARTED;
  }
  Status = Msc->Usb->Transfer (Msc->Usb, (UINT8)(Slot / 2),
                              (EFI_USBFN_ENDPOINT_DIRECTION)(Slot & 1),
                              &Actual, Transfer->Buffer);
  if (!EFI_ERROR (Status)) {
    Transfer->Pending = TRUE;
    Transfer->Aborting = FALSE;
    Transfer->Length = Length;
    if (Actual != Length) {
      return EFI_BAD_BUFFER_SIZE;
    }
  }
  return Status;
}

STATIC EFI_STATUS
AbortSlot (IN OUT MSC_USB *Msc, IN UINTN Slot)
{
  EFI_STATUS Status;
  MSC_TRANSFER *Transfer = &Msc->Transfers[Slot];

  if (!Transfer->Pending || Transfer->Aborting) {
    return EFI_SUCCESS;
  }
  Status = Msc->Usb->AbortTransfer (Msc->Usb, (UINT8)(Slot / 2),
                                   (EFI_USBFN_ENDPOINT_DIRECTION)(Slot & 1));
  if (!EFI_ERROR (Status)) {
    Transfer->Aborting = TRUE;
    Msc->AbortTicks = 0;
  }
  return Status;
}

STATIC EFI_STATUS
SetHalt (IN OUT MSC_USB *Msc, IN UINTN Slot, IN BOOLEAN Halt)
{
  return Msc->Usb->SetEndpointStallState (Msc->Usb, (UINT8)(Slot / 2),
                                         (EFI_USBFN_ENDPOINT_DIRECTION)(Slot & 1), Halt);
}

STATIC EFI_STATUS
BeginReset (IN OUT MSC_USB *Msc)
{
  EFI_STATUS Status;
  Msc->ResetPending = TRUE;
  Status = AbortSlot (Msc, 2);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return AbortSlot (Msc, 3);
}

STATIC EFI_STATUS
QueueCbw (IN OUT MSC_USB *Msc)
{
  if (!Msc->Configured || Msc->ResetPending || Msc->RecoveryRequired || Msc->ControlStatusDeferred) {
    return EFI_SUCCESS;
  }
  Msc->BulkState = MscWaitCbw;
  return QueueTransfer (Msc, 2, GBL_MSC_CBW_SIZE);
}

STATIC EFI_STATUS
QueueCsw (IN OUT MSC_USB *Msc)
{
  Msc->BulkState = MscSendCsw;
  GblMscBotCsw (&Msc->Bot, Msc->ReadOffset, Msc->Transfers[3].Buffer);
  return QueueTransfer (Msc, 3, GBL_MSC_CSW_SIZE);
}

STATIC EFI_STATUS
QueueRead (IN OUT MSC_USB *Msc)
{
  EFI_STATUS Status;
  UINTN Length = MIN ((UINTN)(Msc->Bot.InLength - Msc->ReadOffset), Msc->TransferLimit);

  if (Length == 0) {
    if (Msc->Bot.HostLength > Msc->Bot.InLength &&
        (Msc->Bot.InLength % Msc->BulkPacket) == 0) {
      Msc->BulkState = MscReadZlp;
      return QueueTransfer (Msc, 3, 0);
    }
    return QueueCsw (Msc);
  }
  Status = GblMscBotRead (&Msc->Bot, Msc->ReadOffset, Msc->Transfers[3].Buffer, Length);
  if (EFI_ERROR (Status)) {
    if (Msc->Bot.Status != 2) {
      Msc->Bot.Status = 1;
    }
    Msc->BulkState = MscWaitClear;
    return SetHalt (Msc, 3, TRUE);
  }
  Msc->BulkState = MscReadData;
  return QueueTransfer (Msc, 3, Length);
}

STATIC EFI_STATUS
StartCommand (IN OUT MSC_USB *Msc, IN UINTN Size)
{
  EFI_STATUS Status;

  Msc->ReadOffset = 0;
  Status = GblMscBotBegin (&Msc->Bot, Msc->Transfers[2].Buffer, Size);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "USB MSC: invalid CBW, reset recovery required\n"));
    Msc->RecoveryRequired = TRUE;
    Msc->ResetSeen = FALSE;
    Msc->ClearMask = 0;
    Msc->BulkState = MscWaitReset;
    Status = SetHalt (Msc, 2, TRUE);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    return SetHalt (Msc, 3, TRUE);
  }
  if (Msc->Bot.HaltOut) {
    Status = SetHalt (Msc, 2, TRUE);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  if (Msc->Bot.HaltIn) {
    Msc->BulkState = MscWaitClear;
    return SetHalt (Msc, 3, TRUE);
  }
  if (Msc->Bot.OutLength != 0) {
    Msc->OutRemaining = Msc->Bot.OutLength;
    Msc->BulkState = MscDiscardData;
    return QueueTransfer (Msc, 2, MIN ((UINTN)Msc->OutRemaining, Msc->TransferLimit));
  }
  if (Msc->Bot.InLength != 0) {
    return QueueRead (Msc);
  }
  return QueueCsw (Msc);
}

STATIC EFI_STATUS
ControlReply (IN OUT MSC_USB *Msc, IN CONST VOID *Data, IN UINTN Length)
{
  Length = MIN (Length, (UINTN)Msc->Setup.Length);
  if ((Msc->Setup.RequestType & 0x80) == 0 || Msc->Setup.Length == 0) {
    Msc->ControlState = MscControlStatusIn;
    return QueueTransfer (Msc, 1, 0);
  }
  if (Length != 0 && Data != Msc->Transfers[1].Buffer) {
    CopyMem (Msc->Transfers[1].Buffer, Data, Length);
  }
  Msc->ControlZlp = Length != 0 && Length < Msc->Setup.Length &&
                    (Length % Msc->ControlPacket) == 0;
  Msc->ControlState = MscControlDataIn;
  return QueueTransfer (Msc, 1, Length);
}

STATIC EFI_STATUS
StallControl (IN OUT MSC_USB *Msc)
{
  DEBUG ((DEBUG_INFO, "USB MSC: stalled setup %02x/%02x value=%04x index=%04x len=%u\n",
          Msc->Setup.RequestType, Msc->Setup.Request, Msc->Setup.Value,
          Msc->Setup.Index, Msc->Setup.Length));
  Msc->ControlState = MscControlIdle;
  return SetHalt (Msc, 0, TRUE);
}

STATIC EFI_STATUS
GetDescriptor (IN OUT MSC_USB *Msc)
{
  EFI_USB_DEVICE_REQUEST *Request = &Msc->Setup;
  UINT8 Type = (UINT8)(Request->Value >> 8);
  UINT8 Index = (UINT8)Request->Value;
  UINT8 *Buffer = Msc->Transfers[1].Buffer;
  CONST CHAR8 *Text;
  UINTN Position;
  UINTN Length;

  if (Type != 3 && (Index != 0 || Request->Index != 0)) {
    return StallControl (Msc);
  }
  switch (Type) {
  case 1:
    return ControlReply (Msc, Msc->SuperSpeed ? &Msc->SsDevice : &Msc->Device, 18);
  case 2:
    return Msc->SuperSpeed
      ? ControlReply (Msc, &Msc->SsConfiguration, sizeof (Msc->SsConfiguration))
      : ControlReply (Msc, &Msc->Configuration, sizeof (Msc->Configuration));
  case 3:
    if (Index == 0 && Request->Index == 0) {
      STATIC CONST UINT8 Languages[] = {4, 3, 9, 4};
      return ControlReply (Msc, Languages, sizeof (Languages));
    }
    if (Request->Index != 0x409) {
      return StallControl (Msc);
    }
    if (Index == 1) {
      Text = "GBL";
    } else if (Index == 2) {
      Text = Msc->Media.IsPartition ? "GBL read-only partition" : "GBL read-only RAM disk";
    } else if (Index == 3) {
      Text = Msc->Media.Serial;
    } else {
      return StallControl (Msc);
    }
    for (Position = 0; Text[Position] != 0; Position++) {
      Buffer[2 + Position * 2] = (UINT8)Text[Position];
      Buffer[3 + Position * 2] = 0;
    }
    Length = 2 + Position * 2;
    Buffer[0] = (UINT8)Length;
    Buffer[1] = 3;
    return ControlReply (Msc, Buffer, Length);
  case 6:
    return Msc->SuperSpeed ? StallControl (Msc)
                          : ControlReply (Msc, &Msc->Qualifier, sizeof (Msc->Qualifier));
  case 7:
    if (Msc->SuperSpeed) {
      return StallControl (Msc);
    }
    CopyMem (Buffer, &Msc->Configuration, sizeof (Msc->Configuration));
    Buffer[1] = 7;
    ((MSC_CONFIGURATION *)Buffer)->Out.MaxPacketSize = Msc->BulkPacket == 64 ? 512 : 64;
    ((MSC_CONFIGURATION *)Buffer)->In.MaxPacketSize = Msc->BulkPacket == 64 ? 512 : 64;
    return ControlReply (Msc, Buffer, sizeof (Msc->Configuration));
  case 15:
    return ControlReply (Msc, &Msc->Bos, Msc->Bos.Header.TotalLength);
  default:
    return StallControl (Msc);
  }
}

STATIC EFI_STATUS
HandleSetup (IN OUT MSC_USB *Msc)
{
  EFI_USB_DEVICE_REQUEST *Request = &Msc->Setup;
  UINT8 *Buffer = Msc->Transfers[1].Buffer;
  EFI_STATUS Status;
  UINTN Slot;
  BOOLEAN Halt;

  Msc->SetupPending = FALSE;
  Msc->ApplyConfiguration = FALSE;
  Msc->ContinueCsw = FALSE;
  Msc->ControlStatusDeferred = FALSE;
  if (Request->RequestType == 0x80 && Request->Request == 6) {
    return GetDescriptor (Msc);
  }
  if (Request->RequestType == 0 && Request->Request == 5 && Request->Value < 128 &&
      Request->Index == 0 && Request->Length == 0 && !Msc->Configured) {
    return ControlReply (Msc, NULL, 0);
  }
  if (Request->RequestType == 0 && Request->Request == 9 && Request->Value <= 1 &&
      Request->Index == 0 && Request->Length == 0) {
    Msc->PendingConfiguration = (UINT8)Request->Value;
    Msc->ApplyConfiguration = TRUE;
    Msc->ControlStatusDeferred = TRUE;
    return BeginReset (Msc);
  }
  if (Request->RequestType == 0x80 && Request->Request == 8 && Request->Value == 0 &&
      Request->Index == 0 && Request->Length == 1) {
    Buffer[0] = Msc->Configured ? 1 : 0;
    return ControlReply (Msc, Buffer, 1);
  }
  if (Request->RequestType >= 0x80 && Request->RequestType <= 0x82 &&
      Request->Request == 0 && Request->Value == 0 && Request->Length == 2) {
    Buffer[0] = 0;
    Buffer[1] = 0;
    if (Request->RequestType == 0x82) {
      if (Request->Index != 0 && Request->Index != 0x80 &&
          Request->Index != 1 && Request->Index != 0x81) {
        return StallControl (Msc);
      }
      Slot = ((Request->Index & 15) * 2) + ((Request->Index >> 7) & 1);
      Status = Msc->Usb->GetEndpointStallState (Msc->Usb, (UINT8)(Slot / 2),
                   (EFI_USBFN_ENDPOINT_DIRECTION)(Slot & 1), &Halt);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      Buffer[0] = Halt ? 1 : 0;
    } else if (Request->Index != 0) {
      return StallControl (Msc);
    }
    return ControlReply (Msc, Buffer, 2);
  }
  if (Request->RequestType == 2 && (Request->Request == 1 || Request->Request == 3) &&
      Request->Value == 0 && Request->Length == 0 && Msc->Configured &&
      (Request->Index == 1 || Request->Index == 0x81)) {
    Slot = Request->Index == 1 ? 2 : 3;
    Halt = Request->Request == 3;
    Status = SetHalt (Msc, Slot, Halt);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (!Halt) {
      if (Msc->RecoveryRequired && Msc->ResetSeen) {
        Msc->ClearMask |= Slot == 2 ? 1 : 2;
      }
      if (Slot == 3 && Msc->BulkState == MscWaitClear) {
        Msc->ContinueCsw = TRUE;
      }
    }
    return ControlReply (Msc, NULL, 0);
  }
  if (Msc->Configured && Request->Index == 0 && Request->Value == 0) {
    if (Request->RequestType == 0xa1 && Request->Request == 0xfe && Request->Length == 1) {
      Buffer[0] = 0;
      return ControlReply (Msc, Buffer, 1);
    }
    if (Request->RequestType == 0x21 && Request->Request == 0xff && Request->Length == 0) {
      Msc->ResetSeen = TRUE;
      Msc->ClearMask = 0;
      Msc->ControlStatusDeferred = TRUE;
      return BeginReset (Msc);
    }
    if (Request->RequestType == 0x81 && Request->Request == 10 && Request->Length == 1) {
      Buffer[0] = 0;
      return ControlReply (Msc, Buffer, 1);
    }
    if (Request->RequestType == 1 && Request->Request == 11 && Request->Length == 0) {
      return ControlReply (Msc, NULL, 0);
    }
  }
  if (Msc->SuperSpeed && Request->RequestType == 0 && Request->Index == 0) {
    if (Request->Request == 0x30 && Request->Value == 0 && Request->Length == 6) {
      Msc->ControlState = MscControlDataOut;
      return QueueTransfer (Msc, 0, 6);
    }
    if (Request->Request == 0x31 && Request->Length == 0) {
      return ControlReply (Msc, NULL, 0);
    }
  }
  return StallControl (Msc);
}

STATIC EFI_STATUS
ControlComplete (IN OUT MSC_USB *Msc, IN UINTN Slot)
{
  EFI_STATUS Status;

  if (Slot == 1 && Msc->ControlState == MscControlDataIn && Msc->ControlZlp) {
    Msc->ControlState = MscControlZlp;
    return QueueTransfer (Msc, 1, 0);
  }
  if (Slot == 1 && (Msc->ControlState == MscControlDataIn ||
                     Msc->ControlState == MscControlZlp)) {
    Msc->ControlState = MscControlStatusOut;
    return QueueTransfer (Msc, 0, 0);
  }
  if (Slot == 0 && Msc->ControlState == MscControlDataOut) {
    Msc->ControlState = MscControlStatusIn;
    return QueueTransfer (Msc, 1, 0);
  }
  if ((Slot == 0 && Msc->ControlState != MscControlStatusOut) ||
      (Slot == 1 && Msc->ControlState != MscControlStatusIn)) {
    return EFI_PROTOCOL_ERROR;
  }
  Msc->ControlState = MscControlIdle;
  if (Msc->ApplyConfiguration) {
    Msc->ApplyConfiguration = FALSE;
    Msc->Configured = Msc->PendingConfiguration != 0;
    Msc->RecoveryRequired = FALSE;
    Msc->ResetSeen = FALSE;
    DEBUG ((DEBUG_INFO, "USB MSC: configuration=%u\n", Msc->PendingConfiguration));
    Status = SetHalt (Msc, 2, FALSE);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = SetHalt (Msc, 3, FALSE);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    return QueueCbw (Msc);
  }
  if (Msc->ContinueCsw) {
    Msc->ContinueCsw = FALSE;
    return QueueCsw (Msc);
  }
  if (Msc->RecoveryRequired && Msc->ResetSeen && Msc->ClearMask == 3 && !Msc->ResetPending) {
    Msc->RecoveryRequired = FALSE;
    Msc->Bot.NeedsReset = FALSE;
    return QueueCbw (Msc);
  }
  if (Msc->BulkState == MscWaitReset && !Msc->RecoveryRequired && !Msc->ResetPending) {
    return QueueCbw (Msc);
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
TransferComplete (IN OUT MSC_USB *Msc, IN EFI_USBFN_TRANSFER_RESULT *Result)
{
  UINTN Slot;
  MSC_TRANSFER *Transfer;

  if (Result->EndpointIndex > 1 || Result->EndpointDirection > EfiUsbEndpointDirectionHostIn) {
    return EFI_PROTOCOL_ERROR;
  }
  Slot = Result->EndpointIndex * 2 + Result->EndpointDirection;
  Transfer = &Msc->Transfers[Slot];
  if (Result->TransferStatus == UsbTransferStatusActive) {
    return EFI_SUCCESS;
  }
  if (!Transfer->Pending || Result->Buffer != Transfer->Buffer) {
    return EFI_PROTOCOL_ERROR;
  }
  Transfer->Pending = FALSE;
  if (Transfer->Aborting) {
    Transfer->Aborting = FALSE;
    return EFI_SUCCESS;
  }
  if (Result->TransferStatus != UsbTransferStatusComplete ||
      Result->BytesTransferred > Transfer->Length) {
    return EFI_DEVICE_ERROR;
  }
  if (Slot < 2) {
    if (Result->BytesTransferred != Transfer->Length) {
      return StallControl (Msc);
    }
    return ControlComplete (Msc, Slot);
  }
  if (Slot == 2 && Msc->BulkState == MscWaitCbw) {
    return StartCommand (Msc, Result->BytesTransferred);
  }
  if (Result->BytesTransferred != Transfer->Length) {
    if (Msc->BulkState == MscSendCsw) {
      return EFI_PROTOCOL_ERROR;
    }
    if (Slot == 3 && Msc->BulkState == MscReadData) {
      Msc->ReadOffset += (UINT32)Result->BytesTransferred;
    }
    Msc->Bot.Status = 2;
    return QueueCsw (Msc);
  }
  if (Slot == 2 && Msc->BulkState == MscDiscardData) {
    Msc->OutRemaining -= (UINT32)Result->BytesTransferred;
    if (Msc->OutRemaining != 0) {
      return QueueTransfer (Msc, 2, MIN ((UINTN)Msc->OutRemaining, Msc->TransferLimit));
    }
    return QueueCsw (Msc);
  }
  if (Slot == 3 && Msc->BulkState == MscReadData) {
    Msc->ReadOffset += (UINT32)Result->BytesTransferred;
    return QueueRead (Msc);
  }
  if (Slot == 3 && Msc->BulkState == MscReadZlp) {
    return QueueCsw (Msc);
  }
  if (Slot == 3 && Msc->BulkState == MscSendCsw) {
    if (Msc->Bot.Status == 2) {
      Msc->RecoveryRequired = TRUE;
      Msc->ResetSeen = FALSE;
      Msc->ClearMask = 0;
      Msc->Bot.NeedsReset = TRUE;
      Msc->BulkState = MscWaitReset;
      return EFI_SUCCESS;
    }
    if (Msc->Bot.EjectRequested && Msc->Bot.Status == 0) {
      Msc->ExitRequested = TRUE;
      return EFI_SUCCESS;
    }
    return QueueCbw (Msc);
  }
  return EFI_PROTOCOL_ERROR;
}

STATIC EFI_STATUS
AdvancePending (IN OUT MSC_USB *Msc)
{
  EFI_STATUS Status;
  BOOLEAN MediaPresent;
  BOOLEAN PreventRemoval;
  UINTN Slot;

  if (Msc->SetupPending && !Msc->Transfers[0].Pending && !Msc->Transfers[1].Pending) {
    Status = HandleSetup (Msc);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  for (Slot = 0; Slot < 4; Slot++) {
    if (Msc->Transfers[Slot].Aborting) {
      if (++Msc->AbortTicks > 2000) {
        return EFI_TIMEOUT;
      }
      break;
    }
  }
  if (Msc->ResetPending && !Msc->Transfers[2].Pending && !Msc->Transfers[3].Pending) {
    MediaPresent = Msc->Bot.MediaPresent;
    PreventRemoval = Msc->Bot.PreventRemoval;
    GblMscBotInitMedia (&Msc->Bot, &Msc->Media);
    Msc->Bot.MediaPresent = MediaPresent;
    Msc->Bot.PreventRemoval = PreventRemoval;
    Msc->ResetPending = FALSE;
    Msc->ReadOffset = 0;
    Msc->OutRemaining = 0;
    Msc->BulkState = MscWaitReset;
    if (!Msc->RecoveryRequired || (Msc->ResetSeen && Msc->ClearMask == 3)) {
      Msc->RecoveryRequired = FALSE;
      Status = QueueCbw (Msc);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }
  if (Msc->ControlStatusDeferred && !Msc->ResetPending) {
    Msc->ControlStatusDeferred = FALSE;
    return ControlReply (Msc, NULL, 0);
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
HandleMessage (IN OUT MSC_USB *Msc, IN EFI_USBFN_MESSAGE Message,
               IN EFI_USBFN_MESSAGE_PAYLOAD *Payload)
{
  EFI_STATUS Status;
  UINTN Slot;

  switch (Message) {
  case EfiUsbMsgSetupPacket:
    Msc->ControlStatusDeferred = FALSE;
    Msc->ApplyConfiguration = FALSE;
    Msc->ContinueCsw = FALSE;
    Msc->Setup = Payload->udr;
    Msc->SetupPending = TRUE;
    for (Slot = 0; Slot < 2; Slot++) {
      Status = AbortSlot (Msc, Slot);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
    break;
  case EfiUsbMsgEndpointStatusChangedRx:
  case EfiUsbMsgEndpointStatusChangedTx:
    return TransferComplete (Msc, &Payload->utr);
  case EfiUsbMsgBusEventReset:
    DEBUG ((DEBUG_INFO, "USB MSC: bus reset\n"));
    Msc->SeenBus = TRUE;
    Msc->Configured = FALSE;
    Msc->SetupPending = FALSE;
    Msc->ApplyConfiguration = FALSE;
    Msc->ContinueCsw = FALSE;
    Msc->ControlStatusDeferred = FALSE;
    Msc->RecoveryRequired = FALSE;
    Msc->ControlState = MscControlIdle;
    for (Slot = 0; Slot < 4; Slot++) {
      Status = AbortSlot (Msc, Slot);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
    Msc->ResetPending = TRUE;
    break;
  case EfiUsbMsgBusEventAttach:
    Msc->SeenBus = TRUE;
    break;
  case EfiUsbMsgBusEventDetach:
    Msc->ExitRequested = Msc->SeenBus;
    break;
  case EfiUsbMsgBusEventSpeed:
    DEBUG ((DEBUG_INFO, "USB MSC: bus speed=%u\n", Payload->ubs));
    Msc->SuperSpeed = Payload->ubs == UsbBusSpeedSuper || Payload->ubs == UsbBusSpeedSuperPlus;
    if (Msc->SuperSpeed && !Msc->SupportsSuperSpeed) {
      return EFI_UNSUPPORTED;
    }
    if (Payload->ubs == UsbBusSpeedFull) {
      Msc->BulkPacket = 64;
    } else if (Payload->ubs == UsbBusSpeedHigh) {
      Msc->BulkPacket = 512;
    } else if (Msc->SuperSpeed) {
      Msc->BulkPacket = 1024;
    } else {
      return EFI_UNSUPPORTED;
    }
    Msc->ControlPacket = Msc->SuperSpeed ? 512 : 64;
    Msc->Configuration.Out.MaxPacketSize = Msc->BulkPacket == 64 ? 64 : 512;
    Msc->Configuration.In.MaxPacketSize = Msc->Configuration.Out.MaxPacketSize;
    break;
  default:
    break;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
GblUsbMscRun (OUT BOOLEAN *Released)
{
  MSC_USB *Msc;
  EFI_STATUS Status;
  EFI_STATUS StopStatus;
  EFI_USBFN_MESSAGE Message;
  EFI_USBFN_MESSAGE_PAYLOAD Payload;
  UINTN Size;
  UINTN Slot;
  EFI_INPUT_KEY Key;
  BOOLEAN DisableZlp = FALSE;

  *Released = TRUE;
  if (!mMscPrepared) {
    return EFI_NOT_READY;
  }
  mMscPrepared = FALSE;
  Msc = AllocateZeroPool (sizeof (*Msc));
  if (Msc == NULL) {
    GblUsbMscCancel ();
    return EFI_OUT_OF_RESOURCES;
  }
  Msc->Media = mPreparedMedia;
  ZeroMem (&mPreparedMedia, sizeof (mPreparedMedia));
  Msc->TransferLimit = mMscTransferLimit;
  Msc->BulkPacket = 512;
  Msc->ControlPacket = 64;
  Status = gBS->LocateProtocol (&gEfiUsbfnIoProtocolGuid, NULL, (VOID **)&Msc->Usb);
  if (EFI_ERROR (Status)) {
    GblMscMediaClose (&Msc->Media);
    FreePool (Msc);
    return Status;
  }
  if (Msc->Media.Read == NULL) {
    Status = GblMscMediaOpen (NULL, &Msc->Media);
    if (EFI_ERROR (Status)) {
      FreePool (Msc);
      return Status;
    }
  }
  GblMscBotInitMedia (&Msc->Bot, &Msc->Media);
  InitDescriptors (Msc);
  *Released = FALSE;
  Status = Msc->Usb->StartController (Msc->Usb);
  if (EFI_ERROR (Status)) {
    goto Stop;
  }
  for (Slot = 0; Slot < 4; Slot++) {
    Size = Slot < 2 ? MSC_CONTROL_BUFFER : Msc->TransferLimit;
    Status = Msc->Usb->AllocateTransferBuffer (Msc->Usb, Size, &Msc->Transfers[Slot].Buffer);
    if (EFI_ERROR (Status)) {
      goto Stop;
    }
  }
  Msc->SupportsSuperSpeed = Msc->Usb->ConfigureEnableEndpointsEx != NULL;
  if (Msc->SupportsSuperSpeed) {
    Status = Msc->Usb->ConfigureEnableEndpointsEx (Msc->Usb, &Msc->DeviceInfo, &Msc->SsDeviceInfo);
    if (Status == EFI_UNSUPPORTED) {
      Msc->SupportsSuperSpeed = FALSE;
    }
  }
  if (!Msc->SupportsSuperSpeed) {
    Msc->Bos.Header.NumDeviceCaps = 1;
    Msc->Bos.Header.TotalLength = sizeof (Msc->Bos.Header) + sizeof (Msc->Bos.Usb2);
    Status = Msc->Usb->ConfigureEnableEndpoints (Msc->Usb, &Msc->DeviceInfo);
  }
  if (EFI_ERROR (Status)) {
    goto Stop;
  }
  if (Msc->Usb->SetEndpointPolicy != NULL) {
    Status = Msc->Usb->SetEndpointPolicy (Msc->Usb, 1, EfiUsbEndpointDirectionHostIn,
                                         EfiUsbPolicyZeroLengthTermination,
                                         sizeof (DisableZlp), &DisableZlp);
    if (EFI_ERROR (Status) && Status != EFI_UNSUPPORTED) {
      goto Stop;
    }
  } else {
    Status = EFI_SUCCESS;
  }
  Print (L"GBL USB Mass Storage: read-only %a (%Lu bytes)\n"
         L"All host writes are rejected.\n"
         L"Eject the disk or press a volume key to return to Fastboot.\n",
         Msc->Media.Name, Msc->Media.Size);
  DEBUG ((DEBUG_INFO, "USB MSC: started, read-only %a, VID=%04x PID=%04x\n",
          Msc->Media.Name, mMscVendor, mMscProduct));
  while (!Msc->ExitRequested) {
    Size = sizeof (Payload);
    ZeroMem (&Payload, sizeof (Payload));
    Message = EfiUsbMsgNone;
    Status = Msc->Usb->EventHandler (Msc->Usb, &Message, &Size, &Payload);
    if (EFI_ERROR (Status) && Status != EFI_NOT_READY) {
      break;
    }
    Status = HandleMessage (Msc, Message, &Payload);
    if (EFI_ERROR (Status)) {
      break;
    }
    Status = AdvancePending (Msc);
    if (EFI_ERROR (Status)) {
      break;
    }
    if (gST != NULL && gST->ConIn != NULL &&
        !EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key)) &&
        (Key.ScanCode == SCAN_UP || Key.ScanCode == SCAN_DOWN || Key.ScanCode == SCAN_ESC)) {
      Msc->ExitRequested = TRUE;
    }
    gBS->Stall (1000);
  }

Stop:
  StopStatus = Msc->Usb->StopController (Msc->Usb);
  DEBUG ((DEBUG_INFO, "USB MSC: leaving status=%r stop=%r\n", Status, StopStatus));
  if (EFI_ERROR (StopStatus)) {
    return StopStatus;
  }
  *Released = TRUE;
  for (Slot = 0; Slot < 4; Slot++) {
    if (Msc->Transfers[Slot].Buffer != NULL) {
      Msc->Usb->FreeTransferBuffer (Msc->Usb, Msc->Transfers[Slot].Buffer);
    }
  }
  GblMscMediaClose (&Msc->Media);
  FreePool (Msc);
  return Status;
}
