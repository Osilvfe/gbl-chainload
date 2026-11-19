#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include <Uefi/UefiGpt.h>
#include <Protocol/BlockIo.h>
#include "../../../GblChainloadPkg/Application/GblChainload/UsbMscUsb.c"
#include "../../../GblChainloadPkg/Application/GblChainload/UsbMscFastboot.c"

EFI_GUID gEfiUsbfnIoProtocolGuid = EFI_USBFN_IO_PROTOCOL_GUID;
EFI_GUID gEfiBlockIoProtocolGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
EFI_GUID gEfiPartitionRecordGuid = {1, 2, 3, {4}};
STATIC EFI_BOOT_SERVICES mBootServices;
EFI_BOOT_SERVICES *gBS = &mBootServices;
EFI_SYSTEM_TABLE *gST;
STATIC EFI_USBFN_IO_PROTOCOL mUsb;
STATIC EFI_USB_DEVICE_PROTOCOL mHighUsb;
STATIC FastbootDeviceData mFastboot;
STATIC UINT8 mDisk[GBL_MSC_DISK_SIZE];
STATIC UINT8 mOriginalDisk[GBL_MSC_DISK_SIZE];
STATIC UINT8 mBuffers[4][MSC_BULK_BUFFER];
STATIC BOOLEAN mDriverPending[4];
STATIC BOOLEAN mHalted[4];
STATIC BOOLEAN mControllerActive;
STATIC VOID *mDriverBuffer[4];
STATIC UINTN mDriverLength[4];
STATIC VOID *mAllocations[16];
STATIC UINTN mAllocationAttempts;
STATIC UINTN mFailAllocation;
STATIC UINTN mStopCalls;
STATIC UINTN mRunStep;
STATIC UINTN mRunCommand;
STATIC UINTN mEventCalls;
STATIC UINTN mStalls;
STATIC UINTN mAckMode;
STATIC UINTN mControlStallCalls;
STATIC EFI_STATUS mStopStatus;
STATIC EFI_STATUS mConfigureStatus;
STATIC BOOLEAN mAck;
STATIC BOOLEAN mFail;
STATIC BOOLEAN mFlashComplete;
STATIC VOID (*mCommand)(CONST CHAR8 *, VOID *, UINT32);
STATIC EFI_BLOCK_IO_PROTOCOL mBlockIo[2];
STATIC EFI_BLOCK_IO_MEDIA mBlockMedia[2];
STATIC EFI_PARTITION_ENTRY mPartitionEntries[2];
STATIC UINTN mPartitionCount;
STATIC UINTN mBlockReads;
STATIC EFI_STATUS mBlockReadStatus;
STATIC BOOLEAN mChangeMediaDuringRead;
STATIC BOOLEAN mUnlocked;
STATIC EFI_SYSTEM_TABLE mSystemTable;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mConsole;
STATIC UINTN mStartCalls;
STATIC UINTN mKeyDelivered;
typedef enum {
  TestExitEject,
  TestExitDetach,
  TestExitUp,
  TestExitDown,
  TestExitEscape
} TEST_EXIT_MODE;
STATIC TEST_EXIT_MODE mExitMode;

BOOLEAN IsUnlocked (VOID) { return mUnlocked; }

STATIC UINT8 PartitionByte (UINT64 Offset) {
  return (UINT8)(Offset ^ (Offset >> 8) ^ (Offset >> 32));
}

VOID *EFIAPI CopyMem (VOID *Destination, CONST VOID *Source, UINTN Length) {
  return memcpy (Destination, Source, Length);
}

VOID *EFIAPI ZeroMem (VOID *Buffer, UINTN Length) {
  return memset (Buffer, 0, Length);
}

VOID *EFIAPI AllocateZeroPool (UINTN Size) {
  UINTN Index;
  VOID *Allocation;
  mAllocationAttempts++;
  if (mAllocationAttempts == mFailAllocation) return NULL;
  Allocation = calloc (1, Size);
  assert (Allocation != NULL);
  for (Index = 0; Index < ARRAY_SIZE (mAllocations); Index++) {
    if (mAllocations[Index] == NULL) {
      mAllocations[Index] = Allocation;
      return Allocation;
    }
  }
  abort ();
}

VOID EFIAPI FreePool (VOID *Buffer) {
  UINTN Index;
  for (Index = 0; Index < ARRAY_SIZE (mAllocations); Index++) {
    if (mAllocations[Index] == Buffer) {
      free (Buffer);
      mAllocations[Index] = NULL;
      return;
    }
  }
  abort ();
}

UINTN EFIAPI Print (CONST CHAR16 *Format, ...) {
  (VOID)Format;
  return 0;
}

STATIC EFI_STATUS EFIAPI LocateProtocol (EFI_GUID *Guid, VOID *Registration, VOID **Interface) {
  (VOID)Registration;
  assert (memcmp (Guid, &gEfiUsbfnIoProtocolGuid, sizeof (*Guid)) == 0);
  *Interface = &mUsb;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Stall (UINTN Microseconds) {
  assert (Microseconds == 1000);
  mStalls++;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI LocateHandles (EFI_LOCATE_SEARCH_TYPE SearchType, EFI_GUID *Guid,
                                       VOID *Key, UINTN *Count, EFI_HANDLE **Handles) {
  UINTN Index;
  assert (SearchType == ByProtocol && Key == NULL);
  assert (memcmp (Guid, &gEfiBlockIoProtocolGuid, sizeof (*Guid)) == 0);
  if (mPartitionCount == 0) return EFI_NOT_FOUND;
  *Handles = AllocateZeroPool (mPartitionCount * sizeof (**Handles));
  if (*Handles == NULL) return EFI_OUT_OF_RESOURCES;
  *Count = mPartitionCount;
  for (Index = 0; Index < *Count; Index++) (*Handles)[Index] = &mBlockIo[Index];
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI HandleProtocol (EFI_HANDLE Handle, EFI_GUID *Guid, VOID **Interface) {
  UINTN Index;
  for (Index = 0; Index < mPartitionCount; Index++) {
    if (Handle != &mBlockIo[Index]) continue;
    if (memcmp (Guid, &gEfiBlockIoProtocolGuid, sizeof (*Guid)) == 0) {
      *Interface = &mBlockIo[Index];
    } else {
      assert (memcmp (Guid, &gEfiPartitionRecordGuid, sizeof (*Guid)) == 0);
      *Interface = &mPartitionEntries[Index];
    }
    return EFI_SUCCESS;
  }
  abort ();
}

STATIC EFI_STATUS EFIAPI ReadBlocks (EFI_BLOCK_IO_PROTOCOL *BlockIo, UINT32 MediaId,
                                     EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  UINTN Index;
  EFI_BLOCK_IO_MEDIA *Media = BlockIo->Media;
  UINT8 *Bytes = Buffer;
  assert (MediaId == Media->MediaId && Media->MediaPresent && Media->LogicalPartition);
  assert (Size != 0 && Size <= 65536 && Size % Media->BlockSize == 0);
  assert (Lba <= Media->LastBlock && Size / Media->BlockSize - 1 <= Media->LastBlock - Lba);
  assert (Media->IoAlign <= 1 || (UINTN)Buffer % Media->IoAlign == 0);
  mBlockReads++;
  if (EFI_ERROR (mBlockReadStatus)) return mBlockReadStatus;
  for (Index = 0; Index < Size; Index++) Bytes[Index] = PartitionByte (Lba * Media->BlockSize + Index);
  if (mChangeMediaDuringRead) Media->MediaId++;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI WriteBlocks (EFI_BLOCK_IO_PROTOCOL *BlockIo, UINT32 MediaId,
                                      EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  (VOID)BlockIo; (VOID)MediaId; (VOID)Lba; (VOID)Size; (VOID)Buffer;
  abort ();
}

STATIC EFI_STATUS EFIAPI ResetBlock (EFI_BLOCK_IO_PROTOCOL *BlockIo, BOOLEAN Extended) {
  (VOID)BlockIo; (VOID)Extended;
  abort ();
}

STATIC EFI_STATUS EFIAPI FlushBlock (EFI_BLOCK_IO_PROTOCOL *BlockIo) {
  (VOID)BlockIo;
  abort ();
}

STATIC VOID SetPartition (UINTN Index, CONST CHAR8 *Name, UINT32 BlockSize, UINT64 Size) {
  UINTN Position;
  memset (&mBlockIo[Index], 0, sizeof (mBlockIo[Index]));
  memset (&mBlockMedia[Index], 0, sizeof (mBlockMedia[Index]));
  memset (&mPartitionEntries[Index], 0, sizeof (mPartitionEntries[Index]));
  mBlockIo[Index].Media = &mBlockMedia[Index];
  mBlockIo[Index].ReadBlocks = ReadBlocks;
  mBlockIo[Index].WriteBlocks = WriteBlocks;
  mBlockIo[Index].Reset = ResetBlock;
  mBlockIo[Index].FlushBlocks = FlushBlock;
  mBlockMedia[Index].MediaId = 42;
  mBlockMedia[Index].BlockSize = BlockSize;
  mBlockMedia[Index].IoAlign = 4096;
  mBlockMedia[Index].MediaPresent = TRUE;
  mBlockMedia[Index].LogicalPartition = TRUE;
  mBlockMedia[Index].LastBlock = Size / BlockSize - 1;
  mPartitionEntries[Index].StartingLBA = 4096;
  mPartitionEntries[Index].EndingLBA = 4096 + mBlockMedia[Index].LastBlock;
  mPartitionEntries[Index].UniquePartitionGUID.Data1 = (UINT32)Index + 1;
  for (Position = 0; Name[Position] != 0 && Position < 36; Position++) {
    mPartitionEntries[Index].PartitionName[Position] = (CHAR16)Name[Position];
  }
}

STATIC EFI_STATUS TestRamRead (VOID *Context, UINT64 Offset, VOID *Buffer, UINTN Size) {
  assert (Offset <= GBL_MSC_DISK_SIZE && Size <= GBL_MSC_DISK_SIZE - Offset);
  memcpy (Buffer, (UINT8 *)Context + (UINTN)Offset, Size);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI StartController (EFI_USBFN_IO_PROTOCOL *Usb) {
  assert (Usb == &mUsb && !mControllerActive);
  mControllerActive = TRUE;
  mStartCalls++;
  mRunStep = 0;
  mRunCommand = 0;
  mEventCalls = 0;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI StopController (EFI_USBFN_IO_PROTOCOL *Usb) {
  assert (Usb == &mUsb);
  mStopCalls++;
  if (!EFI_ERROR (mStopStatus)) {
    mControllerActive = FALSE;
    memset (mDriverPending, 0, sizeof (mDriverPending));
  }
  return mStopStatus;
}

STATIC EFI_STATUS EFIAPI AllocateTransfer (EFI_USBFN_IO_PROTOCOL *Usb, UINTN Size, VOID **Buffer) {
  assert (Usb == &mUsb && mControllerActive);
  *Buffer = AllocateZeroPool (Size);
  return *Buffer == NULL ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI FreeTransfer (EFI_USBFN_IO_PROTOCOL *Usb, VOID *Buffer) {
  assert (Usb == &mUsb && !mControllerActive);
  FreePool (Buffer);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI VendorProduct (EFI_USBFN_IO_PROTOCOL *Usb, UINT16 *Vendor, UINT16 *Product) {
  assert (Usb == &mUsb);
  *Vendor = 0x1234;
  *Product = 0x5678;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI MaxTransfer (EFI_USBFN_IO_PROTOCOL *Usb, UINTN *Limit) {
  assert (Usb == &mUsb);
  *Limit = MSC_BULK_BUFFER;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Configure (EFI_USBFN_IO_PROTOCOL *Usb, EFI_USB_DEVICE_INFO *Info) {
  assert (Usb == &mUsb && mControllerActive);
  assert (Info->DeviceDescriptor->IdVendor == 0x1234);
  assert (Info->ConfigInfoTable[0]->InterfaceInfoTable[0]->InterfaceDescriptor->InterfaceClass == 8);
  return mConfigureStatus;
}

STATIC EFI_STATUS EFIAPI ConfigureEx (EFI_USBFN_IO_PROTOCOL *Usb, EFI_USB_DEVICE_INFO *Info,
                                     EFI_USB_SUPERSPEED_DEVICE_INFO *SsInfo) {
  assert (SsInfo->DeviceDescriptor->BcdUSB == 0x300);
  assert (SsInfo->DeviceDescriptor->MaxPacketSize0 == 9);
  return Configure (Usb, Info);
}

STATIC EFI_STATUS EFIAPI Transfer (EFI_USBFN_IO_PROTOCOL *Usb, UINT8 Endpoint,
                                  EFI_USBFN_ENDPOINT_DIRECTION Direction, UINTN *Size, VOID *Buffer) {
  UINTN Slot = Endpoint * 2 + Direction;
  assert (Usb == &mUsb && mControllerActive && Slot < 4);
  assert (!mDriverPending[Slot]);
  assert (*Size <= (Slot < 2 ? MSC_CONTROL_BUFFER : MSC_BULK_BUFFER));
  if (mHalted[Slot]) return EFI_DEVICE_ERROR;
  mDriverPending[Slot] = TRUE;
  mDriverBuffer[Slot] = Buffer;
  mDriverLength[Slot] = *Size;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI AbortTransfer (EFI_USBFN_IO_PROTOCOL *Usb, UINT8 Endpoint,
                                       EFI_USBFN_ENDPOINT_DIRECTION Direction) {
  assert (Usb == &mUsb && mDriverPending[Endpoint * 2 + Direction]);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI SetEndpointStall (EFI_USBFN_IO_PROTOCOL *Usb, UINT8 Endpoint,
                                          EFI_USBFN_ENDPOINT_DIRECTION Direction, BOOLEAN Halt) {
  assert (Usb == &mUsb && Endpoint < 2);
  if (Endpoint == 0) {
    mControlStallCalls++;
    mHalted[0] = TRUE;
    mHalted[1] = TRUE;
  } else {
    mHalted[Endpoint * 2 + Direction] = Halt;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI GetEndpointStall (EFI_USBFN_IO_PROTOCOL *Usb, UINT8 Endpoint,
                                          EFI_USBFN_ENDPOINT_DIRECTION Direction, BOOLEAN *Halt) {
  assert (Usb == &mUsb && Endpoint < 2);
  *Halt = mHalted[Endpoint * 2 + Direction];
  return EFI_SUCCESS;
}

STATIC VOID PutLe32 (UINT8 *Bytes, UINT32 Value) {
  UINTN Index;
  for (Index = 0; Index < 4; Index++) Bytes[Index] = (UINT8)(Value >> (Index * 8));
}

STATIC VOID PutBe32 (UINT8 *Bytes, UINT32 Value) {
  UINTN Index;
  for (Index = 0; Index < 4; Index++) Bytes[Index] = (UINT8)(Value >> ((3 - Index) * 8));
}

STATIC VOID PutBe64 (UINT8 *Bytes, UINT64 Value) {
  PutBe32 (Bytes, (UINT32)(Value >> 32));
  PutBe32 (Bytes + 4, (UINT32)Value);
}

STATIC VOID MakeCbw (UINT8 *Cbw, UINT8 Opcode, UINT8 CdbLength, UINT32 Length, BOOLEAN In) {
  memset (Cbw, 0, GBL_MSC_CBW_SIZE);
  PutLe32 (Cbw, 0x43425355);
  PutLe32 (Cbw + 4, 0x12345678);
  PutLe32 (Cbw + 8, Length);
  Cbw[12] = In ? 0x80 : 0;
  Cbw[14] = CdbLength;
  Cbw[15] = Opcode;
}

STATIC VOID CompletionPayload (UINTN Slot, EFI_USBFN_MESSAGE *Message,
                                EFI_USBFN_MESSAGE_PAYLOAD *Payload) {
  assert (mDriverPending[Slot]);
  *Message = (Slot & 1) ? EfiUsbMsgEndpointStatusChangedTx : EfiUsbMsgEndpointStatusChangedRx;
  Payload->utr.BytesTransferred = mDriverLength[Slot];
  Payload->utr.TransferStatus = UsbTransferStatusComplete;
  Payload->utr.EndpointIndex = (UINT8)(Slot / 2);
  Payload->utr.EndpointDirection = (EFI_USBFN_ENDPOINT_DIRECTION)(Slot & 1);
  Payload->utr.Buffer = mDriverBuffer[Slot];
  mDriverPending[Slot] = FALSE;
}

STATIC EFI_STATUS EFIAPI EventHandler (EFI_USBFN_IO_PROTOCOL *Usb, EFI_USBFN_MESSAGE *Message,
                                      UINTN *Size, EFI_USBFN_MESSAGE_PAYLOAD *Payload) {
  UINTN Slot;
  UINT8 *Cbw;
  assert (Usb == &mUsb && *Size == sizeof (*Payload));
  assert (++mEventCalls < 150);
  if (mExitMode == TestExitDetach) {
    *Message = mRunStep == 1 ? EfiUsbMsgBusEventAttach : EfiUsbMsgBusEventDetach;
    mRunStep++;
    return EFI_SUCCESS;
  }
  if (mRunStep == 0) {
    *Message = EfiUsbMsgBusEventSpeed;
    Payload->ubs = UsbBusSpeedHigh;
    mRunStep++;
  } else if (mRunStep == 1) {
    *Message = EfiUsbMsgBusEventReset;
    mRunStep++;
  } else if (mRunStep == 2) {
    *Message = EfiUsbMsgSetupPacket;
    Payload->udr = (EFI_USB_DEVICE_REQUEST){0, 9, 1, 0, 0};
    mRunStep++;
  } else {
    for (Slot = 0; Slot < 4; Slot++) {
      if (!mDriverPending[Slot]) continue;
      if (Slot == 2 && mDriverLength[Slot] == GBL_MSC_CBW_SIZE) {
        Cbw = mDriverBuffer[Slot];
        switch (mRunCommand++) {
        case 0:
          MakeCbw (Cbw, 0x12, 6, 36, TRUE);
          Cbw[19] = 36;
          break;
        case 1:
          MakeCbw (Cbw, 0x2a, 10, 512, FALSE);
          Cbw[23] = 1;
          break;
        case 2:
          MakeCbw (Cbw, 3, 6, 18, TRUE);
          Cbw[19] = 18;
          break;
        case 3:
          MakeCbw (Cbw, 0x28, 10, 512, TRUE);
          Cbw[23] = 1;
          break;
        case 4:
          MakeCbw (Cbw, 0x1b, 6, 0, FALSE);
          Cbw[19] = 2;
          break;
        default:
          abort ();
        }
      } else if (Slot == 2) {
        memset (mDriverBuffer[Slot], 0xa5, mDriverLength[Slot]);
      }
      CompletionPayload (Slot, Message, Payload);
      return EFI_SUCCESS;
    }
    abort ();
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI HighEvent (USB_DEVICE_EVENT *Message, UINTN *Size,
                                   USB_DEVICE_EVENT_DATA *Payload) {
  assert (*Size == sizeof (*Payload));
  if (mAckMode == 1) {
    *Message = UsbDeviceEventNoEvent;
  } else if (mAckMode == 2) {
    *Message = UsbDeviceEventDeviceStateChange;
    Payload->DeviceState = UsbDeviceStateDisconnected;
  } else {
    *Message = UsbDeviceEventTransferNotification;
    Payload->TransferOutcome.Status = UsbDeviceTransferStatusCompleteOK;
    Payload->TransferOutcome.EndpointIndex = 0x81;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS HighStop (VOID) { return EFI_SUCCESS; }
STATIC EFI_STATUS HighStart (USB_DEVICE_DESCRIPTOR_SET *Descriptors) {
  (VOID)Descriptors;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI ReadKeyStroke (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *Console,
                                       EFI_INPUT_KEY *Key) {
  assert (Console == &mConsole);
  if (mExitMode < TestExitUp || mRunCommand < 4) return EFI_NOT_READY;
  assert (mKeyDelivered++ == 0);
  Key->UnicodeChar = 0;
  Key->ScanCode = mExitMode == TestExitUp ? SCAN_UP
                  : (mExitMode == TestExitDown ? SCAN_DOWN : SCAN_ESC);
  return EFI_SUCCESS;
}

FastbootDeviceData *GetFastbootDeviceData (VOID) { return &mFastboot; }
VOID FastbootRegister (CONST CHAR8 *Prefix, VOID (*Handler)(CONST CHAR8 *, VOID *, UINT32)) {
  assert (strcmp (Prefix, "oem usb-msc") == 0);
  mCommand = Handler;
}
VOID GblSlotRegisterCommands (VOID) {}
VOID FastbootOkay (CONST CHAR8 *Response) { assert (Response != NULL); mAck = TRUE; }
VOID FastbootFail (CONST CHAR8 *Reason) { assert (Reason != NULL); mFail = TRUE; }
BOOLEAN FlashComplete (VOID) { return mFlashComplete; }

STATIC VOID ResetMocks (VOID) {
  UINTN Index;
  GblUsbMscCancel ();
  for (Index = 0; Index < ARRAY_SIZE (mAllocations); Index++) assert (mAllocations[Index] == NULL);
  memset (&mUsb, 0, sizeof (mUsb));
  memset (mDriverPending, 0, sizeof (mDriverPending));
  memset (mHalted, 0, sizeof (mHalted));
  mUsb.Revision = EFI_USBFN_IO_PROTOCOL_REVISION;
  mUsb.StartController = StartController;
  mUsb.StopController = StopController;
  mUsb.AllocateTransferBuffer = AllocateTransfer;
  mUsb.FreeTransferBuffer = FreeTransfer;
  mUsb.GetVendorIdProductId = VendorProduct;
  mUsb.GetMaxTransferSize = MaxTransfer;
  mUsb.ConfigureEnableEndpoints = Configure;
  mUsb.ConfigureEnableEndpointsEx = ConfigureEx;
  mUsb.Transfer = Transfer;
  mUsb.AbortTransfer = AbortTransfer;
  mUsb.SetEndpointStallState = SetEndpointStall;
  mUsb.GetEndpointStallState = GetEndpointStall;
  mUsb.EventHandler = EventHandler;
  mBootServices.LocateProtocol = LocateProtocol;
  mBootServices.Stall = Stall;
  mBootServices.LocateHandleBuffer = LocateHandles;
  mBootServices.HandleProtocol = HandleProtocol;
  mHighUsb.HandleEvent = HighEvent;
  mHighUsb.Stop = HighStop;
  mHighUsb.StartEx = HighStart;
  mFastboot.UsbDeviceProtocol = &mHighUsb;
  mControllerActive = FALSE;
  mStartCalls = 0;
  mExitMode = TestExitEject;
  mKeyDelivered = 0;
  memset (&mSystemTable, 0, sizeof (mSystemTable));
  memset (&mConsole, 0, sizeof (mConsole));
  mConsole.ReadKeyStroke = ReadKeyStroke;
  mSystemTable.ConIn = &mConsole;
  gST = NULL;
  mAllocationAttempts = 0;
  mFailAllocation = 0;
  mStopCalls = 0;
  mRunStep = 0;
  mRunCommand = 0;
  mEventCalls = 0;
  mStalls = 0;
  mStopStatus = EFI_SUCCESS;
  mConfigureStatus = EFI_SUCCESS;
  mAck = FALSE;
  mFail = FALSE;
  mFlashComplete = TRUE;
  mAckMode = 0;
  mControlStallCalls = 0;
  mMscPrepared = FALSE;
  mPartitionCount = 1;
  mBlockReads = 0;
  mBlockReadStatus = EFI_SUCCESS;
  mChangeMediaDuringRead = FALSE;
  mUnlocked = TRUE;
  SetPartition (0, "boot_a", 512, 96U * 1024U * 1024U);
}

STATIC VOID InitContext (MSC_USB *Msc) {
  UINTN Slot;
  ResetMocks ();
  assert (GblUsbMscPrepare (NULL) == EFI_SUCCESS);
  memset (Msc, 0, sizeof (*Msc));
  Msc->Usb = &mUsb;
  Msc->Media[0].Read = TestRamRead;
  Msc->Media[0].Context = mDisk;
  Msc->Media[0].Size = sizeof (mDisk);
  Msc->Media[0].BlockSize = 512;
  memcpy (Msc->Media[0].Serial, "GBLRAM0001-MSC", sizeof ("GBLRAM0001-MSC"));
  Msc->LunCount = 1;
  Msc->Lun = 0;
  Msc->TransferLimit = MSC_BULK_BUFFER;
  Msc->BulkPacket = 512;
  Msc->ControlPacket = 64;
  Msc->SupportsSuperSpeed = TRUE;
  GblMscBotInitMedia (&Msc->Bot[0], &Msc->Media[0]);
  Msc->Bot[0].LunCount = Msc->LunCount;
  InitDescriptors (Msc);
  for (Slot = 0; Slot < 4; Slot++) Msc->Transfers[Slot].Buffer = mBuffers[Slot];
  mControllerActive = TRUE;
}

STATIC EFI_STATUS CompleteSlot (MSC_USB *Msc, UINTN Slot, UINTN Length,
                                EFI_USBFN_TRANSFER_STATUS TransferStatus) {
  EFI_USBFN_MESSAGE Message;
  EFI_USBFN_MESSAGE_PAYLOAD Payload;
  EFI_STATUS Status;
  CompletionPayload (Slot, &Message, &Payload);
  Payload.utr.BytesTransferred = Length;
  Payload.utr.TransferStatus = TransferStatus;
  Status = HandleMessage (Msc, Message, &Payload);
  if (!EFI_ERROR (Status)) Status = AdvancePending (Msc);
  return Status;
}

STATIC VOID Setup (MSC_USB *Msc, UINT8 Type, UINT8 Request, UINT16 Value, UINT16 Index, UINT16 Length) {
  EFI_USBFN_MESSAGE_PAYLOAD Payload;
  Payload.udr = (EFI_USB_DEVICE_REQUEST){Type, Request, Value, Index, Length};
  mHalted[0] = FALSE;
  mHalted[1] = FALSE;
  assert (HandleMessage (Msc, EfiUsbMsgSetupPacket, &Payload) == EFI_SUCCESS);
  assert (AdvancePending (Msc) == EFI_SUCCESS);
}

STATIC VOID FinishControl (MSC_USB *Msc) {
  UINTN Attempt;
  UINTN Slot;
  for (Attempt = 0; Attempt < 8; Attempt++) {
    Slot = mDriverPending[1] ? 1 : 0;
    if (!mDriverPending[Slot]) return;
    assert (CompleteSlot (Msc, Slot, mDriverLength[Slot], UsbTransferStatusComplete) == EFI_SUCCESS);
  }
  abort ();
}

STATIC VOID ConfigureContext (MSC_USB *Msc) {
  Setup (Msc, 0, 9, 1, 0, 0);
  assert (!Msc->Configured);
  FinishControl (Msc);
  assert (Msc->Configured && mDriverPending[2]);
}

STATIC VOID CoreTests (VOID) {
  GBL_MSC_BOT Bot;
  UINT8 Cbw[31];
  UINT8 Csw[13];
  UINT8 Data[512];
  UINTN Index;
  UINT32 Random = 0x42ac;
  STATIC CONST struct {
    UINT8 Opcode;
    UINT32 HostLength;
    BOOLEAN In;
    UINT8 Status;
    UINT32 InLength;
    UINT32 OutLength;
    BOOLEAN HaltIn;
    BOOLEAN HaltOut;
  } Cases[] = {
    {0x00,    0, FALSE, 0,   0,    0, FALSE, FALSE},
    {0x28,    0, FALSE, 2,   0,    0, FALSE, FALSE},
    {0x2a,    0, FALSE, 2,   0,    0, FALSE, FALSE},
    {0x00,  512, TRUE,  0,   0,    0, TRUE,  FALSE},
    {0x28, 1024, TRUE,  0, 512,    0, FALSE, FALSE},
    {0x28,  512, TRUE,  0, 512,    0, FALSE, FALSE},
    {0x28,  256, TRUE,  2, 256,    0, FALSE, FALSE},
    {0x2a,  512, TRUE,  2,   0,    0, TRUE,  FALSE},
    {0x00,  512, FALSE, 0,   0,  512, FALSE, FALSE},
    {0x28,  512, FALSE, 2,   0,    0, FALSE, TRUE },
    {0x2a, 1024, FALSE, 1,   0, 1024, FALSE, FALSE},
    {0x2a,  512, FALSE, 1,   0,  512, FALSE, FALSE},
    {0x2a,  256, FALSE, 2,   0,  256, FALSE, FALSE}
  };
  GblMscMakeDisk (mDisk);
  memcpy (mOriginalDisk, mDisk, sizeof (mDisk));
  assert (mDisk[510] == 0x55 && mDisk[511] == 0xaa);
  assert (memcmp (mDisk + 13 * 512 + 32, "README  TXT", 11) == 0);
  GblMscBotInit (&Bot, mDisk, sizeof (mDisk));
  MakeCbw (Cbw, 0x12, 6, 36, TRUE); Cbw[19] = 36;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 36);
  assert (GblMscBotRead (&Bot, 0, Data, 36) == EFI_SUCCESS && Data[1] == 0x80);
  assert (memcmp (Data + 8, "GBL     ", 8) == 0);
  GblMscBotCsw (&Bot, 36, Csw);
  assert (Csw[0] == 'U' && Csw[3] == 'S' && Csw[4] == 0x78 && Csw[8] == 0 && Csw[12] == 0);
  /* SPC-3 version: hosts gate VPD pages and multi-LUN scans on this byte. */
  assert (Data[2] == 5 && Data[3] == 2 && Data[4] == 31);
  MakeCbw (Cbw, 0x12, 6, 36, TRUE);
  Cbw[16] = 1; Cbw[17] = 0x00; Cbw[18] = 0; Cbw[19] = 36;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 6);
  assert (GblMscBotRead (&Bot, 0, Data, 6) == EFI_SUCCESS);
  assert (Data[1] == 0x00 && Data[2] == 0 && Data[3] == 2 && Data[4] == 0x00 && Data[5] == 0x80);
  GblMscBotCsw (&Bot, 6, Csw);
  assert (Csw[8] == 30 && Csw[12] == 0);
  MakeCbw (Cbw, 0x12, 6, 40, TRUE);
  Cbw[16] = 1; Cbw[17] = 0x80; Cbw[18] = 0; Cbw[19] = 40;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 14);
  assert (GblMscBotRead (&Bot, 0, Data, 14) == EFI_SUCCESS);
  assert (Data[1] == 0x80 && Data[2] == 0 && Data[3] == 10);
  assert (memcmp (Data + 4, "GBLRAM0001", 10) == 0);
  GblMscBotCsw (&Bot, 14, Csw);
  assert (Csw[8] == 26 && Csw[12] == 0);
  MakeCbw (Cbw, 0x25, 10, 8, TRUE);
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
  assert (GblMscBotRead (&Bot, 0, Data, 8) == EFI_SUCCESS);
  assert (Data[2] == 7 && Data[3] == 0xff && Data[6] == 2 && Data[7] == 0);
  MakeCbw (Cbw, 0x1a, 6, 4, TRUE); Cbw[19] = 4;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Response[2] == 0x80);
  MakeCbw (Cbw, 0x28, 10, 512, TRUE); Cbw[23] = 1;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.FromDisk);
  assert (GblMscBotRead (&Bot, 0, Data, sizeof (Data)) == EFI_SUCCESS);
  assert (memcmp (Data, mDisk, sizeof (Data)) == 0);
  assert (GblMscBotRead (&Bot, 512, Data, 1) == EFI_BAD_BUFFER_SIZE);
  Cbw[19] = 8;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 1 && Bot.HaltIn);
  MakeCbw (Cbw, 0x2a, 10, 512, FALSE); Cbw[23] = 1;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 1 && Bot.OutLength == 512);
  assert (Bot.SenseKey == 7 && Bot.Asc == 0x27);
  MakeCbw (Cbw, 3, 6, 18, TRUE); Cbw[19] = 18;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
  assert (Bot.Response[2] == 7 && Bot.Response[12] == 0x27);
  MakeCbw (Cbw, 0x12, 6, 8, TRUE); Cbw[19] = 36;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 2 && Bot.InLength == 8);
  MakeCbw (Cbw, 0x12, 6, 36, FALSE); Cbw[19] = 36;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 2 && Bot.HaltOut);
  MakeCbw (Cbw, 0, 6, 0, FALSE);
  assert (GblMscBotBegin (&Bot, Cbw, 30) == EFI_PROTOCOL_ERROR && Bot.NeedsReset);
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_PROTOCOL_ERROR);
  for (Index = 0; Index < ARRAY_SIZE (Cases); Index++) {
    GblMscBotInit (&Bot, mDisk, sizeof (mDisk));
    MakeCbw (Cbw, Cases[Index].Opcode, Cases[Index].Opcode == 0 ? 6 : 10,
             Cases[Index].HostLength, Cases[Index].In);
    Cbw[23] = 1;
    assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
    assert (Bot.Status == Cases[Index].Status && Bot.InLength == Cases[Index].InLength);
    assert (Bot.OutLength == Cases[Index].OutLength && Bot.HaltIn == Cases[Index].HaltIn);
    assert (Bot.HaltOut == Cases[Index].HaltOut);
  }
  for (Index = 0; Index < 20000; Index++) {
    UINTN Position;
    GblMscBotInit (&Bot, mDisk, sizeof (mDisk));
    for (Position = 0; Position < sizeof (Cbw); Position++) {
      Random = Random * 1664525U + 1013904223U;
      Cbw[Position] = (UINT8)(Random >> 24);
    }
    if ((Index & 1) != 0) {
      PutLe32 (Cbw, 0x43425355); Cbw[12] &= 0x80; Cbw[13] = 0; Cbw[14] = 16;
    }
    if (!EFI_ERROR (GblMscBotBegin (&Bot, Cbw, (Index & 3) == 0 ? Index % 31 : 31))) {
      UINTN Length = MIN ((UINTN)Bot.InLength, sizeof (Data));
      assert (GblMscBotRead (&Bot, 0, Data, Length) == EFI_SUCCESS);
      GblMscBotCsw (&Bot, (UINT32)Length, Csw);
    }
  }
  assert (memcmp (mDisk, mOriginalDisk, sizeof (mDisk)) == 0);
}

STATIC VOID EnumerationTests (VOID) {
  MSC_USB Msc;
  EFI_USBFN_MESSAGE_PAYLOAD Payload;
  UINTN Index;

  InitContext (&Msc);
  Payload.ubs = UsbBusSpeedHigh;
  assert (HandleMessage (&Msc, EfiUsbMsgBusEventSpeed, &Payload) == EFI_SUCCESS);
  Setup (&Msc, 0x80, 6, 0x100, 0, 64);
  assert (mDriverLength[1] == 18 && mBuffers[1][0] == 18 && mBuffers[1][7] == 64);
  assert (mControlStallCalls == 0 && !mHalted[0] && !mHalted[1]);
  FinishControl (&Msc);
  Setup (&Msc, 0, 5, 4, 0, 0);
  assert (Msc.ControlState == MscControlStatusIn && mDriverLength[1] == 0);
  FinishControl (&Msc);
  Setup (&Msc, 0x80, 6, 0x100, 0, 18);
  assert (mDriverLength[1] == 18);
  FinishControl (&Msc);
  Setup (&Msc, 0x80, 6, 0x200, 0, 9);
  assert (mDriverLength[1] == 9 && mBuffers[1][2] == sizeof (MSC_CONFIGURATION));
  FinishControl (&Msc);
  Setup (&Msc, 0x80, 6, 0x200, 0, sizeof (MSC_CONFIGURATION));
  assert (mDriverLength[1] == sizeof (MSC_CONFIGURATION));
  assert (((MSC_CONFIGURATION *)mBuffers[1])->Interface.InterfaceClass == 8);
  FinishControl (&Msc);
  Setup (&Msc, 0x80, 6, 0x300, 0, 255);
  assert (mDriverLength[1] == 4 && mBuffers[1][2] == 9 && mBuffers[1][3] == 4);
  FinishControl (&Msc);
  for (Index = 1; Index <= 3; Index++) {
    Setup (&Msc, 0x80, 6, (UINT16)(0x300 + Index), 0x409, 255);
    assert (mDriverLength[1] == mBuffers[1][0] && mBuffers[1][1] == 3);
    FinishControl (&Msc);
  }
  assert (mControlStallCalls == 0);
  Setup (&Msc, 0x80, 6, 0x3ee, 0, 255);
  assert (mControlStallCalls == 1 && mHalted[0] && mHalted[1]);
  assert (!mDriverPending[0] && !mDriverPending[1]);
  Setup (&Msc, 0x80, 6, 0x100, 0, 18);
  assert (mControlStallCalls == 1 && !mHalted[0] && !mHalted[1]);
  FinishControl (&Msc);
  ConfigureContext (&Msc);
  assert (mControlStallCalls == 1);
}

STATIC VOID TransportTests (VOID) {
  MSC_USB Msc;
  EFI_USBFN_MESSAGE_PAYLOAD Payload;
  InitContext (&Msc);
  Payload.ubs = UsbBusSpeedFull;
  assert (HandleMessage (&Msc, EfiUsbMsgBusEventSpeed, &Payload) == EFI_SUCCESS);
  assert (Msc.Configuration.Out.MaxPacketSize == 64);
  Setup (&Msc, 0x80, 6, 0x100, 0, 64);
  assert (mDriverLength[1] == 18 && mBuffers[1][8] == 0x34);
  FinishControl (&Msc);
  Setup (&Msc, 0x80, 6, 0x700, 0, 255);
  assert (((MSC_CONFIGURATION *)mBuffers[1])->Out.MaxPacketSize == 512);
  FinishControl (&Msc);
  ConfigureContext (&Msc);
  Setup (&Msc, 0xa1, 0xfe, 0, 0, 1);
  assert (mDriverLength[1] == 1 && mBuffers[1][0] == 0);
  FinishControl (&Msc);
  memset (mBuffers[2], 0, 31);
  assert (CompleteSlot (&Msc, 2, 31, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (mHalted[2] && mHalted[3] && Msc.RecoveryRequired);
  Setup (&Msc, 2, 1, 0, 0x81, 0); FinishControl (&Msc);
  assert (Msc.RecoveryRequired && !mDriverPending[2]);
  Setup (&Msc, 0x21, 0xff, 0, 0, 0); FinishControl (&Msc);
  Setup (&Msc, 2, 1, 0, 1, 0); FinishControl (&Msc);
  assert (Msc.RecoveryRequired);
  Setup (&Msc, 2, 1, 0, 0x81, 0); FinishControl (&Msc);
  assert (!Msc.RecoveryRequired && mDriverPending[2]);

  MakeCbw (mBuffers[2], 0x28, 10, 512, TRUE); mBuffers[2][23] = 1;
  assert (CompleteSlot (&Msc, 2, 31, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (mDriverPending[3]);
  Setup (&Msc, 0x21, 0xff, 0, 0, 0);
  assert (Msc.Transfers[3].Aborting && Msc.ResetPending);
  FinishControl (&Msc);
  assert (!mDriverPending[2]);
  assert (CompleteSlot (&Msc, 3, 0, UsbTransferStatusAborted) == EFI_SUCCESS);
  assert (!mDriverPending[2]);
  FinishControl (&Msc);
  assert (mDriverPending[2] && !Msc.ResetPending);

  Setup (&Msc, 0x80, 6, 0x100, 0, 18);
  Setup (&Msc, 0xa1, 0xfe, 0, 0, 1);
  assert (Msc.SetupPending && Msc.Transfers[1].Aborting);
  assert (CompleteSlot (&Msc, 1, 0, UsbTransferStatusAborted) == EFI_SUCCESS);
  assert (!Msc.SetupPending && mDriverLength[1] == 1);
  FinishControl (&Msc);

  Payload.ubs = UsbBusSpeedHigh;
  assert (HandleMessage (&Msc, EfiUsbMsgBusEventSpeed, &Payload) == EFI_SUCCESS);
  MakeCbw (mBuffers[2], 0x28, 10, 1024, TRUE); mBuffers[2][23] = 1;
  assert (CompleteSlot (&Msc, 2, 31, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (mDriverLength[3] == 512);
  assert (CompleteSlot (&Msc, 3, 512, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (mDriverPending[3] && mDriverLength[3] == 0 && Msc.BulkState == MscReadZlp);
  assert (CompleteSlot (&Msc, 3, 0, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (mDriverLength[3] == 13 && mBuffers[3][9] == 2 && mBuffers[3][12] == 0);
  assert (CompleteSlot (&Msc, 3, 13, UsbTransferStatusComplete) == EFI_SUCCESS);

  MakeCbw (mBuffers[2], 0x1b, 6, 0, FALSE); mBuffers[2][19] = 2;
  assert (CompleteSlot (&Msc, 2, 31, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (!Msc.ExitRequested);
  assert (CompleteSlot (&Msc, 3, 13, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (Msc.ExitRequested && !mDriverPending[2]);
  assert (memcmp (mDisk, mOriginalDisk, sizeof (mDisk)) == 0);
}

STATIC VOID SuperSpeedAndCancellationTests (VOID) {
  MSC_USB Msc;
  EFI_USBFN_MESSAGE_PAYLOAD Payload;
  UINTN Attempt;
  InitContext (&Msc);
  Payload.ubs = UsbBusSpeedSuper;
  assert (HandleMessage (&Msc, EfiUsbMsgBusEventSpeed, &Payload) == EFI_SUCCESS);
  assert (Msc.ControlPacket == 512 && Msc.BulkPacket == 1024);
  Setup (&Msc, 0x80, 6, 0x100, 0, 18);
  assert (mBuffers[1][3] == 3 && mBuffers[1][7] == 9);
  FinishControl (&Msc);
  Setup (&Msc, 0x80, 6, 0x200, 0, 255);
  assert (mDriverLength[1] == sizeof (MSC_SS_CONFIGURATION));
  assert (((MSC_SS_CONFIGURATION *)mBuffers[1])->OutCompanion.DescriptorType == 48);
  FinishControl (&Msc);
  ConfigureContext (&Msc);
  Setup (&Msc, 0xa1, 0xfe, 0, 0, 2);
  assert (mHalted[0] && mHalted[1]);
  Setup (&Msc, 0xa1, 0xfe, 0, 0, 1);
  assert (!mHalted[0] && !mHalted[1]);
  FinishControl (&Msc);
  Setup (&Msc, 0, 9, 1, 0, 0);
  assert (Msc.Transfers[2].Aborting && !mDriverPending[1]);
  assert (CompleteSlot (&Msc, 2, 0, UsbTransferStatusAborted) == EFI_SUCCESS);
  assert (mDriverPending[1] && !mDriverPending[2]);
  FinishControl (&Msc);
  assert (mDriverPending[2]);
  Setup (&Msc, 0x21, 0xff, 0, 0, 0);
  assert (Msc.Transfers[2].Aborting);
  for (Attempt = 0; Attempt < 2000; Attempt++) {
    EFI_STATUS Status = AdvancePending (&Msc);
    if (Status == EFI_TIMEOUT) break;
    assert (Status == EFI_SUCCESS);
  }
  assert (AdvancePending (&Msc) == EFI_TIMEOUT);
  assert (CompleteSlot (&Msc, 2, 0, UsbTransferStatusAborted) == EFI_SUCCESS);
  FinishControl (&Msc);
  Payload.ubs = UsbBusSpeedSuper;
  assert (HandleMessage (&Msc, EfiUsbMsgBusEventDetach, &Payload) == EFI_SUCCESS);
  assert (!Msc.ExitRequested);
  assert (HandleMessage (&Msc, EfiUsbMsgBusEventAttach, &Payload) == EFI_SUCCESS);
  assert (HandleMessage (&Msc, EfiUsbMsgBusEventDetach, &Payload) == EFI_SUCCESS);
  assert (Msc.ExitRequested);
}

STATIC VOID PartitionMediaTests (VOID) {
  GBL_MSC_MEDIA Media;
  UINT8 Buffer[65536 + 512];
  UINTN Index;
  UINTN Reads;
  UINTN Failure;
  CHAR8 Serial[64];
  CHAR8 LongName[38];

  ResetMocks ();
  assert (GblMscMediaOpen ("boot", &Media) == EFI_NOT_FOUND);
  assert (GblMscMediaOpen ("boot_a ", &Media) == EFI_INVALID_PARAMETER);
  assert (GblMscMediaOpen ("../boot_a", &Media) == EFI_INVALID_PARAMETER);
  memset (LongName, 'x', sizeof (LongName));
  LongName[37] = 0;
  assert (GblMscMediaOpen (LongName, &Media) == EFI_INVALID_PARAMETER);
  mUnlocked = FALSE;
  assert (GblMscMediaOpen ("boot_a", &Media) == EFI_ACCESS_DENIED);
  assert (mBlockReads == 0);
  assert (GblMscMediaOpen (NULL, &Media) == EFI_SUCCESS && !Media.IsPartition);
  assert (Media.Size == GBL_MSC_DISK_SIZE);
  GblMscMediaClose (&Media);
  mUnlocked = TRUE;
  assert (GblMscMediaOpen ("boot_a", &Media) == EFI_SUCCESS && Media.IsPartition);
  assert (Media.Size == 96U * 1024U * 1024U && mBlockReads == 1);
  memcpy (Serial, Media.Serial, sizeof (Serial));
  assert (strcmp (Media.Name, "boot_a") == 0);
  assert (Media.Read (Media.Context, 4096 - 30, Buffer, sizeof (Buffer)) == EFI_SUCCESS);
  for (Index = 0; Index < sizeof (Buffer); Index++) {
    assert (Buffer[Index] == PartitionByte (4096 - 30 + Index));
  }
  assert (Media.Read (Media.Context, Media.Size - 1, Buffer, 1) == EFI_SUCCESS);
  assert (Buffer[0] == PartitionByte (Media.Size - 1));
  Reads = mBlockReads;
  assert (Media.Read (Media.Context, Media.Size, Buffer, 1) == EFI_BAD_BUFFER_SIZE);
  assert (Media.Read (Media.Context, MAX_UINT64, Buffer, 512) == EFI_BAD_BUFFER_SIZE);
  assert (Media.Read (Media.Context, Media.Size, Buffer, 0) == EFI_SUCCESS);
  assert (mBlockReads == Reads);
  mBlockMedia[0].MediaId++;
  assert (Media.Read (Media.Context, 0, Buffer, 512) == EFI_MEDIA_CHANGED);
  assert (mBlockReads == Reads);
  mBlockMedia[0].MediaId--;
  mBlockMedia[0].MediaPresent = FALSE;
  assert (Media.Read (Media.Context, 0, Buffer, 512) == EFI_NO_MEDIA);
  mBlockMedia[0].MediaPresent = TRUE;
  mBlockReadStatus = EFI_DEVICE_ERROR;
  assert (Media.Read (Media.Context, 0, Buffer, 512) == EFI_DEVICE_ERROR);
  mBlockReadStatus = EFI_SUCCESS;
  mChangeMediaDuringRead = TRUE;
  assert (Media.Read (Media.Context, 0, Buffer, 512) == EFI_MEDIA_CHANGED);
  GblMscMediaClose (&Media);
  assert (Media.Context == NULL && Media.Read == NULL);

  ResetMocks ();
  mPartitionCount = 2;
  SetPartition (1, "boot_a", 4096, 96U * 1024U * 1024U);
  assert (GblMscMediaOpen ("boot_a", &Media) == EFI_NO_MAPPING && mBlockReads == 0);
  SetPartition (1, "boot_b", 512, 4096);
  assert (GblMscMediaOpen ("boot_b", &Media) == EFI_SUCCESS);
  assert (Media.Size == 4096 && strcmp (Media.Serial, Serial) != 0);
  GblMscMediaClose (&Media);

  ResetMocks ();
  mBlockMedia[0].LogicalPartition = FALSE;
  assert (GblMscMediaOpen ("boot_a", &Media) == EFI_UNSUPPORTED);
  mBlockMedia[0].LogicalPartition = TRUE;
  mBlockMedia[0].RemovableMedia = TRUE;
  assert (GblMscMediaOpen ("boot_a", &Media) == EFI_UNSUPPORTED);
  mBlockMedia[0].RemovableMedia = FALSE;
  mBlockMedia[0].MediaPresent = FALSE;
  assert (GblMscMediaOpen ("boot_a", &Media) == EFI_NO_MEDIA);
  mBlockMedia[0].MediaPresent = TRUE;
  mBlockMedia[0].IoAlign = 3;
  assert (GblMscMediaOpen ("boot_a", &Media) == EFI_UNSUPPORTED);
  mBlockMedia[0].IoAlign = 131072;
  assert (GblMscMediaOpen ("boot_a", &Media) == EFI_UNSUPPORTED);
  mBlockMedia[0].IoAlign = 4096;
  mBlockMedia[0].BlockSize = 1000;
  assert (GblMscMediaOpen ("boot_a", &Media) == EFI_UNSUPPORTED);
  mBlockMedia[0].BlockSize = 4096;
  mBlockMedia[0].LastBlock = MAX_UINT64;
  assert (GblMscMediaOpen ("boot_a", &Media) == EFI_UNSUPPORTED);
  SetPartition (0, "boot_a", 4096, 96U * 1024U * 1024U);
  mPartitionEntries[0].EndingLBA++;
  assert (GblMscMediaOpen ("boot_a", &Media) == EFI_UNSUPPORTED);
  SetPartition (0, "boot_a", 4096, 96U * 1024U * 1024U);
  mBlockReadStatus = EFI_DEVICE_ERROR;
  assert (GblMscMediaOpen ("boot_a", &Media) == EFI_DEVICE_ERROR && Media.Context == NULL);
  mBlockReadStatus = EFI_SUCCESS;
  mChangeMediaDuringRead = TRUE;
  assert (GblMscMediaOpen ("boot_a", &Media) == EFI_MEDIA_CHANGED && Media.Context == NULL);

  ResetMocks ();
  memset (LongName, 'x', 36);
  LongName[36] = 0;
  SetPartition (0, LongName, 65536, 131072);
  assert (GblMscMediaOpen (LongName, &Media) == EFI_SUCCESS);
  assert (strcmp (Media.Name, LongName) == 0);
  assert (Media.Read (Media.Context, 1, Buffer, sizeof (Buffer)) == EFI_SUCCESS);
  GblMscMediaClose (&Media);
  for (Failure = 1; Failure <= 3; Failure++) {
    ResetMocks ();
    mFailAllocation = Failure;
    assert (GblMscMediaOpen ("boot_a", &Media) == EFI_OUT_OF_RESOURCES);
    assert (Media.Context == NULL);
  }
  ResetMocks ();
}

STATIC VOID PartitionScsiTests (VOID) {
  GBL_MSC_MEDIA Media;
  GBL_MSC_BOT Bot;
  UINT8 Cbw[31];
  UINT8 Buffer[4096];
  UINT64 Size = 5ULL * 1024 * 1024 * 1024 * 1024;
  UINT64 Lba = 0x10000001ULL;
  UINTN Reads;
  UINTN Index;

  ResetMocks ();
  SetPartition (0, "boot_a", 4096, Size);
  assert (GblMscMediaOpen ("boot_a", &Media) == EFI_SUCCESS);
  assert (Media.Size == Size);
  GblMscBotInitMedia (&Bot, &Media);
  MakeCbw (Cbw, 0x25, 10, 8, TRUE);
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 0);
  assert (GblMscBotRead (&Bot, 0, Buffer, 8) == EFI_SUCCESS);
  assert (Buffer[0] == 0x4f && Buffer[1] == 255 && Buffer[2] == 255 && Buffer[3] == 255);
  MakeCbw (Cbw, 0x9e, 16, 32, TRUE);
  Cbw[16] = 0x10;
  PutBe32 (Cbw + 25, 32);
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 32);
  assert (GblMscBotRead (&Bot, 0, Buffer, 32) == EFI_SUCCESS);
  assert (Buffer[3] == 0 && Buffer[4] == 0x4f && Buffer[7] == 0xff && Buffer[10] == 0x10);
  MakeCbw (Cbw, 0x23, 10, 12, TRUE);
  Cbw[23] = 12;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 0);
  assert (GblMscBotRead (&Bot, 0, Buffer, 12) == EFI_SUCCESS);
  assert (Buffer[3] == 8 && Buffer[9] == 0 && Buffer[10] == 0x10 && Buffer[11] == 0);
  MakeCbw (Cbw, 0x2f, 10, 0, FALSE);
  Cbw[23] = 1;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 0);
  Cbw[17] = 0xff;
  Cbw[18] = 0xff;
  Cbw[19] = 0xff;
  Cbw[20] = 0xff;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 1 && Bot.Asc == 0x21);
  Cbw[16] = 0x11;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 1);
  MakeCbw (Cbw, 0x88, 16, 4096, TRUE);
  PutBe64 (Cbw + 17, Lba);
  PutBe32 (Cbw + 25, 1);
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 0);
  assert (Bot.DiskOffset == Lba * 4096);
  assert (GblMscBotRead (&Bot, 0, Buffer, sizeof (Buffer)) == EFI_SUCCESS);
  for (Index = 0; Index < sizeof (Buffer); Index++) assert (Buffer[Index] == PartitionByte (Lba * 4096 + Index));
  Reads = mBlockReads;
  PutBe64 (Cbw + 17, Size / 4096);
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 1 && Bot.Asc == 0x21);
  assert (mBlockReads == Reads);
  MakeCbw (Cbw, 0x8a, 16, 4096, FALSE);
  PutBe32 (Cbw + 25, 1);
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 1);
  assert (Bot.SenseKey == 7 && Bot.Asc == 0x27 && mBlockReads == Reads);
  GblMscMediaClose (&Media);
  ResetMocks ();
}

STATIC VOID PartitionTransportTests (VOID) {
  MSC_USB Msc;
  EFI_USBFN_MESSAGE_PAYLOAD Payload;

  InitContext (&Msc);
  SetPartition (0, "boot_a", 4096, 96U * 1024U * 1024U);
  assert (GblMscMediaOpen ("boot_a", &Msc.Media[0]) == EFI_SUCCESS);
  Msc.Media[0].BlockSize = 4096;
  GblMscBotInitMedia (&Msc.Bot[0], &Msc.Media[0]);
  Msc.Bot[0].LunCount = 1;
  Msc.LunCount = 1;
  Msc.Lun = 0;
  ConfigureContext (&Msc);
  assert (Msc.Bot[0].IsPartition && Msc.Bot[0].DiskSize == 96U * 1024U * 1024U);
  Payload.ubs = UsbBusSpeedHigh;
  assert (HandleMessage (&Msc, EfiUsbMsgBusEventReset, &Payload) == EFI_SUCCESS);
  assert (CompleteSlot (&Msc, 2, 0, UsbTransferStatusAborted) == EFI_SUCCESS);
  assert (Msc.Bot[0].IsPartition && Msc.Bot[0].DiskSize == Msc.Media[0].Size);
  ConfigureContext (&Msc);
  mBlockReadStatus = EFI_DEVICE_ERROR;
  MakeCbw (mBuffers[2], 0x28, 10, 4096, TRUE); mBuffers[2][23] = 1;
  assert (CompleteSlot (&Msc, 2, 31, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (mHalted[3] && Msc.BulkState == MscWaitClear && !mDriverPending[3]);
  assert (Msc.Bot[0].Status == 1 && Msc.Bot[0].SenseKey == 3 && Msc.Bot[0].Asc == 0x11);
  Setup (&Msc, 2, 1, 0, 0x81, 0); FinishControl (&Msc);
  assert (mDriverPending[3] && mDriverLength[3] == 13);
  assert (mBuffers[3][9] == 0x10 && mBuffers[3][12] == 1);
  assert (CompleteSlot (&Msc, 3, 13, UsbTransferStatusComplete) == EFI_SUCCESS);
  MakeCbw (mBuffers[2], 3, 6, 18, TRUE); mBuffers[2][19] = 18;
  assert (CompleteSlot (&Msc, 2, 31, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (mBuffers[3][2] == 3 && mBuffers[3][12] == 0x11);
  assert (CompleteSlot (&Msc, 3, 18, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (CompleteSlot (&Msc, 3, 13, UsbTransferStatusComplete) == EFI_SUCCESS);
  mBlockReadStatus = EFI_SUCCESS;
  MakeCbw (mBuffers[2], 0x28, 10, 4096, TRUE); mBuffers[2][23] = 1;
  assert (CompleteSlot (&Msc, 2, 31, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (mDriverLength[3] == 4096 && mBuffers[3][1] == PartitionByte (1));
  GblMscMediaClose (&Msc.Media[0]);
  ResetMocks ();
}

STATIC VOID PartitionCommandTests (VOID) {
  BOOLEAN Released;
  UINTN Index;
  STATIC CONST CHAR8 *Invalid[] = {"boot_a", " boot_a extra", " /dev/sde", " boot", " boot_b"};

  for (Index = 0; Index < ARRAY_SIZE (Invalid); Index++) {
    ResetMocks ();
    GblUsbMscRegisterCommands ();
    mCommand (Invalid[Index], NULL, 0);
    assert (mFail && !mAck && !GblUsbMscRequested () && !mMscPrepared);
    assert (mStopCalls == 0 && mPreparedMedia[0].Context == NULL);
  }
  if (!GBL_USB_MSC_PARTITION_EXPORT) {
    ResetMocks ();
    GblUsbMscRegisterCommands ();
    mCommand (" boot_a", NULL, 0);
    assert (mFail && !mAck && !GblUsbMscRequested () && !mMscPrepared);
    assert (mBlockReads == 0 && mStopCalls == 0 && mStalls == 0);
    assert (mPreparedMedia[0].Context == NULL);
    ResetMocks ();
    return;
  }
  ResetMocks ();
  GblUsbMscRegisterCommands ();
  mUnlocked = FALSE;
  mCommand (" boot_a", NULL, 0);
  assert (mFail && !mAck && !GblUsbMscRequested ());
  ResetMocks ();
  GblUsbMscRegisterCommands ();
  mCommand (" boot_a ", NULL, 0);
  assert (mAck && !mFail && GblUsbMscRequested () && mPreparedMedia[0].IsPartition);
  assert (GblUsbMscRun (&Released) == EFI_SUCCESS && Released);
  assert (mRunCommand == 5 && mBlockReads > 1 && mPreparedMedia[0].Context == NULL);
  ResetMocks ();
  mPartitionCount = 2;
  SetPartition (1, "boot_b", 512, 4096);
  GblUsbMscRegisterCommands ();
  mCommand (" boot_a,boot_b", NULL, 0);
  assert (mAck && !mFail && mPreparedLunCount == 2 && mPreparedMedia[1].IsPartition);
  assert (GblUsbMscRun (&Released) == EFI_SUCCESS && Released);
  assert (mPreparedMedia[0].Context == NULL && mPreparedMedia[1].Context == NULL);
  ResetMocks ();
  GblUsbMscRegisterCommands ();
  mAckMode = 1;
  mCommand (" boot_a", NULL, 0);
  assert (mAck && !GblUsbMscRequested () && mPreparedMedia[0].Context == NULL && !mMscPrepared);
  mAckMode = 2;
  mCommand (" boot_a", NULL, 0);
  assert (!GblUsbMscRequested () && mPreparedMedia[0].Context == NULL && !mMscPrepared);
  ResetMocks ();
}

STATIC VOID MultiLunTests (VOID) {
  MSC_USB Msc;
  GBL_MSC_BOT Bot;
  UINT8 Cbw[31];
  UINT8 Data[512];

  InitContext (&Msc);
  mPartitionCount = 2;
  SetPartition (1, "boot_b", 512, 4096);
  assert (GblMscMediaOpen ("boot_b", &Msc.Media[1]) == EFI_SUCCESS);
  Msc.LunCount = 2;
  Msc.Lun = 0;
  Msc.Bot[0].LunCount = 2;
  GblMscBotInitMedia (&Msc.Bot[1], &Msc.Media[1]);
  Msc.Bot[1].LunCount = 2;
  ConfigureContext (&Msc);

  /* GET MAX LUN must report the highest LUN index, not the count. */
  Setup (&Msc, 0xa1, 0xfe, 0, 0, 1);
  assert (mDriverLength[1] == 1 && mBuffers[1][0] == 1);
  FinishControl (&Msc);

  MakeCbw (mBuffers[2], 0x12, 6, 36, TRUE);
  mBuffers[2][13] = 1;
  mBuffers[2][19] = 36;
  assert (CompleteSlot (&Msc, 2, 31, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (Msc.Lun == 1 && mDriverPending[3] && mDriverLength[3] == 36);
  assert (mBuffers[3][0] == 0 && mBuffers[3][1] == 0x80);
  assert (CompleteSlot (&Msc, 3, 36, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (CompleteSlot (&Msc, 3, 13, UsbTransferStatusComplete) == EFI_SUCCESS);

  MakeCbw (mBuffers[2], 0x28, 10, 512, TRUE);
  mBuffers[2][13] = 1;
  mBuffers[2][23] = 1;
  assert (CompleteSlot (&Msc, 2, 31, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (Msc.Lun == 1 && mDriverLength[3] == 512);
  assert (mBuffers[3][0] == PartitionByte (0) && mBuffers[3][1] == PartitionByte (1));
  assert (CompleteSlot (&Msc, 3, 512, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (CompleteSlot (&Msc, 3, 13, UsbTransferStatusComplete) == EFI_SUCCESS);

  GblMscBotInit (&Bot, mDisk, sizeof (mDisk));
  Bot.LunCount = 4;
  MakeCbw (Cbw, 0xa0, 12, 36, TRUE);
  PutBe32 (Cbw + 21, 36);
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 0);
  assert (GblMscBotRead (&Bot, 0, Data, 36) == EFI_SUCCESS);
  /* SAM-5 peripheral device addressing: bus in byte 0, LUN in byte 1. */
  assert (Data[3] == 32);
  assert (Data[4] == 0 && Data[5] == 0 && Data[6] == 0 && Data[7] == 0);
  assert (Data[12] == 0 && Data[13] == 1 && Data[14] == 0);
  assert (Data[20] == 0 && Data[21] == 2 && Data[22] == 0);
  assert (Data[28] == 0 && Data[29] == 3 && Data[30] == 0);

  MakeCbw (Cbw, 0x12, 6, 36, TRUE);
  Cbw[13] = 4;
  Cbw[19] = 36;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
  assert (Bot.Status == 1 && Bot.SenseKey == 5 && Bot.Asc == 0x25);

  GblMscMediaClose (&Msc.Media[1]);
  Msc.Media[0].Context = NULL;
  ResetMocks ();
}

STATIC VOID LifecycleTests (VOID) {
  BOOLEAN Released;
  UINTN Failure;
  UINTN Index;
  ResetMocks ();
  assert (GblUsbMscPrepare (NULL) == EFI_SUCCESS);
  assert (GblUsbMscRun (&Released) == EFI_SUCCESS && Released);
  assert (mStopCalls == 1 && mRunCommand == 5 && !mControllerActive);
  for (Failure = 1; Failure <= 6; Failure++) {
    ResetMocks ();
    mFailAllocation = Failure;
    assert (GblUsbMscPrepare (NULL) == EFI_SUCCESS);
    assert (GblUsbMscRun (&Released) == EFI_OUT_OF_RESOURCES && Released);
    assert (mStopCalls == (Failure > 2 ? 1U : 0U));
  }
  ResetMocks ();
  mConfigureStatus = EFI_DEVICE_ERROR;
  mStopStatus = EFI_DEVICE_ERROR;
  assert (GblUsbMscPrepare (NULL) == EFI_SUCCESS);
  assert (GblUsbMscRun (&Released) == EFI_DEVICE_ERROR && !Released);
  assert (mControllerActive);
  for (Index = 0; Index < ARRAY_SIZE (mAllocations); Index++) {
    free (mAllocations[Index]); mAllocations[Index] = NULL;
  }
  ResetMocks ();
  mUsb.Revision = 0;
  assert (GblUsbMscPrepare (NULL) == EFI_UNSUPPORTED);
  assert (GblUsbMscRun (&Released) == EFI_NOT_READY && Released);
  ResetMocks ();
}

STATIC VOID RepeatedSessionTests (VOID) {
  BOOLEAN Released;
  TEST_EXIT_MODE ExitMode;
  UINTN Cycle;
  UINTN Index;
  UINTN Sessions = 0;

  ResetMocks ();
  for (ExitMode = TestExitEject; ExitMode <= TestExitEscape; ExitMode++) {
    for (Cycle = 0; Cycle < 3; Cycle++) {
      mExitMode = ExitMode;
      mKeyDelivered = 0;
      gST = &mSystemTable;
      assert (GblUsbMscPrepare (Cycle == 1 ? "boot_a" : NULL) == EFI_SUCCESS);
      assert (GblUsbMscRun (&Released) == EFI_SUCCESS && Released);
      Sessions++;
      assert (mStartCalls == Sessions && mStopCalls == Sessions);
      assert (!mControllerActive && !mMscPrepared && mPreparedMedia[0].Context == NULL);
      if (ExitMode == TestExitDetach) {
        assert (mRunStep == 3 && mRunCommand == 0 && mKeyDelivered == 0);
      } else if (ExitMode == TestExitEject) {
        assert (mRunCommand == 5 && mKeyDelivered == 0);
      } else {
        assert (mRunCommand == 4 && mKeyDelivered == 1);
      }
      for (Index = 0; Index < ARRAY_SIZE (mAllocations); Index++) {
        assert (mAllocations[Index] == NULL);
      }
      for (Index = 0; Index < ARRAY_SIZE (mDriverPending); Index++) {
        assert (!mDriverPending[Index]);
      }
      assert (GblUsbMscRun (&Released) == EFI_NOT_READY && Released);
    }
  }
  assert (Sessions == 15 && mBlockReads != 0);
  ResetMocks ();
}

STATIC VOID CommandTests (VOID) {
  ResetMocks ();
  GblUsbMscRegisterCommands ();
  assert (mCommand != NULL && !GblUsbMscRequested ());
  mCommand ("-unsafe", NULL, 0);
  assert (mFail && !mAck && !GblUsbMscRequested ());
  mFail = FALSE;
  mFlashComplete = FALSE;
  mCommand ("", NULL, 0);
  assert (mFail && !mAck && !GblUsbMscRequested ());
  mFail = FALSE;
  mFlashComplete = TRUE;
  mCommand ("", NULL, 0);
  assert (mAck && !mFail && GblUsbMscRequested () && !GblUsbMscRequested ());
  mAckMode = 1;
  mCommand ("", NULL, 0);
  assert (!GblUsbMscRequested () && mStalls == 5000);
  mAckMode = 2;
  mCommand ("", NULL, 0);
  assert (!GblUsbMscRequested ());
  assert (mStopCalls == 0);
}

int main (int argc, char **argv) {
  if (argc == 2) {
    FILE *Output;
    GblMscMakeDisk (mDisk);
    Output = fopen (argv[1], "wb");
    assert (Output != NULL && fwrite (mDisk, 1, sizeof (mDisk), Output) == sizeof (mDisk));
    assert (fclose (Output) == 0);
    return 0;
  }
  CoreTests ();
  EnumerationTests ();
  TransportTests ();
  SuperSpeedAndCancellationTests ();
  PartitionMediaTests ();
  PartitionScsiTests ();
  PartitionTransportTests ();
  PartitionCommandTests ();
  MultiLunTests ();
  LifecycleTests ();
  RepeatedSessionTests ();
  CommandTests ();
  puts ("ok USB MSC: read-only RAM/partitions, multi-LUN REPORT LUNS, 512/4096 logical blocks, 64-bit capacity, VERIFY, Qualcomm EP0, SCSI/BOT reset recovery, repeated exits, DMA lifetime, OOM and Fastboot ACK");
  return 0;
}
