#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include <Uefi/UefiGpt.h>
#include <Guid/FileInfo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include "../../../GblChainloadPkg/Application/GblChainload/UsbMscUsb.c"
#include "../../../GblChainloadPkg/Application/GblChainload/UsbMscFastboot.c"

EFI_GUID gEfiUsbfnIoProtocolGuid = EFI_USBFN_IO_PROTOCOL_GUID;
EFI_GUID gEfiBlockIoProtocolGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
EFI_GUID gEfiPartitionRecordGuid = {1, 2, 3, {4}};
EFI_GUID gEfiSimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_GUID gEfiFileInfoGuid = EFI_FILE_INFO_ID;
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
#define MOCK_VOLUMES 2
#define MOCK_FILES 3
#define MOCK_FILE_OBJECTS 8

typedef struct {
  BOOLEAN Present;
  BOOLEAN Directory;
  CHAR16 Path[64];
  UINT64 Size;
} MOCK_FILE;

typedef struct {
  EFI_FILE_PROTOCOL Protocol;
  BOOLEAN InUse;
  BOOLEAN Root;
  UINTN Volume;
  UINTN Slot;
  UINT64 Position;
} MOCK_FILE_OBJECT;

STATIC MOCK_FILE mFiles[MOCK_VOLUMES][MOCK_FILES];
STATIC MOCK_FILE_OBJECT mRootObjects[MOCK_VOLUMES];
STATIC MOCK_FILE_OBJECT mFileObjects[MOCK_FILE_OBJECTS];
STATIC EFI_SIMPLE_FILE_SYSTEM_PROTOCOL mVolumeFs[MOCK_VOLUMES];
STATIC UINT8 mFsTags[MOCK_VOLUMES];
STATIC UINTN mVolumeCount;
STATIC UINTN mFileReads;
STATIC EFI_STATUS mFileReadStatus;
STATIC UINTN mFileOpenCalls;

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
  if (memcmp (Guid, &gEfiBlockIoProtocolGuid, sizeof (*Guid)) == 0) {
    if (mPartitionCount == 0) return EFI_NOT_FOUND;
    *Handles = AllocateZeroPool (mPartitionCount * sizeof (**Handles));
    if (*Handles == NULL) return EFI_OUT_OF_RESOURCES;
    *Count = mPartitionCount;
    for (Index = 0; Index < *Count; Index++) (*Handles)[Index] = &mBlockIo[Index];
    return EFI_SUCCESS;
  }
  assert (memcmp (Guid, &gEfiSimpleFileSystemProtocolGuid, sizeof (*Guid)) == 0);
  if (mVolumeCount == 0) return EFI_NOT_FOUND;
  *Handles = AllocateZeroPool (mVolumeCount * sizeof (**Handles));
  if (*Handles == NULL) return EFI_OUT_OF_RESOURCES;
  *Count = mVolumeCount;
  for (Index = 0; Index < *Count; Index++) (*Handles)[Index] = &mFsTags[Index];
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI HandleProtocol (EFI_HANDLE Handle, EFI_GUID *Guid, VOID **Interface) {
  UINTN Index;
  for (Index = 0; Index < mPartitionCount; Index++) {
    if (Handle != &mBlockIo[Index]) continue;
    assert (memcmp (Guid, &gEfiBlockIoProtocolGuid, sizeof (*Guid)) == 0 ||
            memcmp (Guid, &gEfiPartitionRecordGuid, sizeof (*Guid)) == 0);
    if (memcmp (Guid, &gEfiBlockIoProtocolGuid, sizeof (*Guid)) == 0) {
      *Interface = &mBlockIo[Index];
    } else {
      *Interface = &mPartitionEntries[Index];
    }
    return EFI_SUCCESS;
  }
  for (Index = 0; Index < mVolumeCount; Index++) {
    if (Handle != &mFsTags[Index]) continue;
    assert (memcmp (Guid, &gEfiSimpleFileSystemProtocolGuid, sizeof (*Guid)) == 0);
    *Interface = &mVolumeFs[Index];
    return EFI_SUCCESS;
  }
  abort ();
}

/*
 * A 64 KiB overlay stands in for the partition's first blocks: writes land
 * there and reads prefer it, so a write followed by a read observes the write
 * exactly as the real BlockIo would. Anything outside the overlay is the
 * deterministic pattern the read mock generates.
 */
#define MOCK_FLASH_BYTES 65536U
STATIC UINT8 mFlash[MOCK_FLASH_BYTES];
STATIC UINTN mFlashLength;
STATIC UINTN mBlockWrites;
STATIC UINTN mBlockFlushes;
STATIC EFI_STATUS mBlockWriteStatus;

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
  for (Index = 0; Index < Size; Index++) {
    UINT64 At = Lba * Media->BlockSize + Index;
    Bytes[Index] = At < mFlashLength ? mFlash[At] : PartitionByte (At);
  }
  if (mChangeMediaDuringRead) Media->MediaId++;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI WriteBlocks (EFI_BLOCK_IO_PROTOCOL *BlockIo, UINT32 MediaId,
                                      EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  EFI_BLOCK_IO_MEDIA *Media = BlockIo->Media;
  UINTN Offset = (UINTN)(Lba * Media->BlockSize);
  assert (MediaId == Media->MediaId && Media->MediaPresent && Media->LogicalPartition);
  assert (Size != 0 && Size <= 65536 && Size % Media->BlockSize == 0);
  assert (Media->IoAlign <= 1 || (UINTN)Buffer % Media->IoAlign == 0);
  mBlockWrites++;
  if (EFI_ERROR (mBlockWriteStatus)) return mBlockWriteStatus;
  assert (Offset + Size <= MOCK_FLASH_BYTES);
  memcpy (mFlash + Offset, Buffer, Size);
  if (Offset + Size > mFlashLength) mFlashLength = Offset + Size;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI ResetBlock (EFI_BLOCK_IO_PROTOCOL *BlockIo, BOOLEAN Extended) {
  (VOID)BlockIo; (VOID)Extended;
  abort ();
}

STATIC EFI_STATUS EFIAPI FlushBlock (EFI_BLOCK_IO_PROTOCOL *BlockIo) {
  (VOID)BlockIo;
  mBlockFlushes++;
  return EFI_SUCCESS;
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

STATIC UINT8 FileByte (UINTN Volume, UINT64 Offset) {
  return (UINT8)(Offset * 37U + Volume * 11U + 0x20U);
}

STATIC INTN ComparePath (CONST CHAR16 *Left, CONST CHAR16 *Right) {
  UINTN Index;
  for (Index = 0; Left[Index] != 0 && Right[Index] != 0; Index++) {
    if (Left[Index] != Right[Index]) return Left[Index] < Right[Index] ? -1 : 1;
  }
  if (Left[Index] == Right[Index]) return 0;
  return Left[Index] == 0 ? -1 : 1;
}

STATIC MOCK_FILE_OBJECT *ObjectFromProtocol (EFI_FILE_PROTOCOL *Protocol) {
  UINTN Index;
  for (Index = 0; Index < MOCK_VOLUMES; Index++) {
    if (&mRootObjects[Index].Protocol == Protocol) return &mRootObjects[Index];
  }
  for (Index = 0; Index < MOCK_FILE_OBJECTS; Index++) {
    if (&mFileObjects[Index].Protocol == Protocol) return &mFileObjects[Index];
  }
  abort ();
}

STATIC EFI_STATUS EFIAPI FileOpen (EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle,
                                   CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
STATIC EFI_STATUS EFIAPI FileClose (EFI_FILE_PROTOCOL *This);
STATIC EFI_STATUS EFIAPI FileDelete (EFI_FILE_PROTOCOL *This);
STATIC EFI_STATUS EFIAPI FileRead (EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
STATIC EFI_STATUS EFIAPI FileWrite (EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
STATIC EFI_STATUS EFIAPI FileGetPosition (EFI_FILE_PROTOCOL *This, UINT64 *Position);
STATIC EFI_STATUS EFIAPI FileSetPosition (EFI_FILE_PROTOCOL *This, UINT64 Position);
STATIC EFI_STATUS EFIAPI FileGetInfo (EFI_FILE_PROTOCOL *This, EFI_GUID *InfoType,
                                      UINTN *BufferSize, VOID *Buffer);
STATIC EFI_STATUS EFIAPI FileSetInfo (EFI_FILE_PROTOCOL *This, EFI_GUID *InfoType,
                                      UINTN BufferSize, VOID *Buffer);
STATIC EFI_STATUS EFIAPI FileFlush (EFI_FILE_PROTOCOL *This);

STATIC VOID FillFileProtocol (MOCK_FILE_OBJECT *Object) {
  Object->Protocol.Revision = EFI_FILE_PROTOCOL_REVISION;
  Object->Protocol.Open = FileOpen;
  Object->Protocol.Close = FileClose;
  Object->Protocol.Delete = FileDelete;
  Object->Protocol.Read = FileRead;
  Object->Protocol.Write = FileWrite;
  Object->Protocol.GetPosition = FileGetPosition;
  Object->Protocol.SetPosition = FileSetPosition;
  Object->Protocol.GetInfo = FileGetInfo;
  Object->Protocol.SetInfo = FileSetInfo;
  Object->Protocol.Flush = FileFlush;
}

STATIC EFI_STATUS EFIAPI OpenVolume (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                     EFI_FILE_PROTOCOL **Root) {
  UINTN Index;
  for (Index = 0; Index < mVolumeCount; Index++) {
    if (&mVolumeFs[Index] != This) continue;
    memset (&mRootObjects[Index], 0, sizeof (mRootObjects[Index]));
    mRootObjects[Index].InUse = TRUE;
    mRootObjects[Index].Root = TRUE;
    mRootObjects[Index].Volume = Index;
    FillFileProtocol (&mRootObjects[Index]);
    *Root = &mRootObjects[Index].Protocol;
    return EFI_SUCCESS;
  }
  abort ();
}

STATIC EFI_STATUS EFIAPI FileOpen (EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle,
                                   CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes) {
  MOCK_FILE_OBJECT *Object = ObjectFromProtocol (This);
  UINTN Slot;
  UINTN Index;
  assert (Object->Root && NewHandle != NULL && FileName != NULL);
  assert (OpenMode == EFI_FILE_MODE_READ && Attributes == 0);
  mFileOpenCalls++;
  for (Slot = 0; Slot < MOCK_FILES; Slot++) {
    if (!mFiles[Object->Volume][Slot].Present) continue;
    if (ComparePath (mFiles[Object->Volume][Slot].Path, FileName) != 0) continue;
    for (Index = 0; Index < MOCK_FILE_OBJECTS; Index++) {
      if (mFileObjects[Index].InUse) continue;
      memset (&mFileObjects[Index], 0, sizeof (mFileObjects[Index]));
      mFileObjects[Index].InUse = TRUE;
      mFileObjects[Index].Volume = Object->Volume;
      mFileObjects[Index].Slot = Slot;
      FillFileProtocol (&mFileObjects[Index]);
      *NewHandle = &mFileObjects[Index].Protocol;
      return EFI_SUCCESS;
    }
    abort ();
  }
  return EFI_NOT_FOUND;
}

STATIC EFI_STATUS EFIAPI FileClose (EFI_FILE_PROTOCOL *This) {
  MOCK_FILE_OBJECT *Object = ObjectFromProtocol (This);
  assert (Object->InUse);
  Object->InUse = FALSE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI FileDelete (EFI_FILE_PROTOCOL *This) {
  (VOID)This;
  abort ();
}

STATIC EFI_STATUS EFIAPI FileRead (EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer) {
  MOCK_FILE_OBJECT *Object = ObjectFromProtocol (This);
  MOCK_FILE *Entry = &mFiles[Object->Volume][Object->Slot];
  UINTN Index;
  UINTN Count;
  assert (!Object->Root && Object->InUse);
  assert (Object->Position <= Entry->Size);
  mFileReads++;
  if (EFI_ERROR (mFileReadStatus)) return mFileReadStatus;
  Count = (UINTN)MIN ((UINT64)*BufferSize, Entry->Size - Object->Position);
  for (Index = 0; Index < Count; Index++) {
    ((UINT8 *)Buffer)[Index] = FileByte (Object->Volume, Object->Position + Index);
  }
  Object->Position += Count;
  *BufferSize = Count;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI FileWrite (EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer) {
  (VOID)This; (VOID)BufferSize; (VOID)Buffer;
  abort ();
}

STATIC EFI_STATUS EFIAPI FileGetPosition (EFI_FILE_PROTOCOL *This, UINT64 *Position) {
  (VOID)This; (VOID)Position;
  abort ();
}

STATIC EFI_STATUS EFIAPI FileSetPosition (EFI_FILE_PROTOCOL *This, UINT64 Position) {
  MOCK_FILE_OBJECT *Object = ObjectFromProtocol (This);
  assert (!Object->Root && Object->InUse);
  assert (Position <= mFiles[Object->Volume][Object->Slot].Size);
  Object->Position = Position;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI FileGetInfo (EFI_FILE_PROTOCOL *This, EFI_GUID *InfoType,
                                      UINTN *BufferSize, VOID *Buffer) {
  MOCK_FILE_OBJECT *Object = ObjectFromProtocol (This);
  MOCK_FILE *Entry = Object->Root ? NULL : &mFiles[Object->Volume][Object->Slot];
  UINTN Required = SIZE_OF_EFI_FILE_INFO + 32;
  EFI_FILE_INFO *Info;

  assert (memcmp (InfoType, &gEfiFileInfoGuid, sizeof (*InfoType)) == 0);
  if (Buffer == NULL || *BufferSize < Required) {
    *BufferSize = Required;
    return EFI_BUFFER_TOO_SMALL;
  }
  Info = Buffer;
  memset (Info, 0, Required);
  Info->Size = Required;
  if (Entry == NULL) {
    Info->Attribute = EFI_FILE_DIRECTORY;
  } else {
    Info->Attribute = Entry->Directory ? EFI_FILE_DIRECTORY : 0;
    Info->FileSize = Entry->Size;
  }
  *BufferSize = Required;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI FileSetInfo (EFI_FILE_PROTOCOL *This, EFI_GUID *InfoType,
                                      UINTN BufferSize, VOID *Buffer) {
  (VOID)This; (VOID)InfoType; (VOID)BufferSize; (VOID)Buffer;
  abort ();
}

STATIC EFI_STATUS EFIAPI FileFlush (EFI_FILE_PROTOCOL *This) {
  (VOID)This;
  abort ();
}

STATIC VOID AddVolumeFile (UINTN Volume, CONST CHAR8 *Path, UINT64 Size, BOOLEAN Directory) {
  UINTN Slot;
  UINTN Index;
  for (Slot = 0; Slot < MOCK_FILES; Slot++) {
    if (mFiles[Volume][Slot].Present) continue;
    mFiles[Volume][Slot].Present = TRUE;
    mFiles[Volume][Slot].Directory = Directory;
    mFiles[Volume][Slot].Size = Size;
    for (Index = 0; Path[Index] != 0 && Index < 63; Index++) {
      mFiles[Volume][Slot].Path[Index] = (CHAR16)(UINT8)Path[Index];
    }
    return;
  }
  abort ();
}

STATIC VOID SetVolumeCount (UINTN Count) {
  UINTN Index;
  assert (Count <= MOCK_VOLUMES);
  mVolumeCount = Count;
  memset (mFiles, 0, sizeof (mFiles));
  memset (mRootObjects, 0, sizeof (mRootObjects));
  memset (mFileObjects, 0, sizeof (mFileObjects));
  mFileReads = 0;
  mFileOpenCalls = 0;
  mFileReadStatus = EFI_SUCCESS;
  for (Index = 0; Index < MOCK_VOLUMES; Index++) {
    memset (&mVolumeFs[Index], 0, sizeof (mVolumeFs[Index]));
    mVolumeFs[Index].Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    mVolumeFs[Index].OpenVolume = OpenVolume;
  }
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
  SetVolumeCount (0);
  mBlockReads = 0;
  mBlockReadStatus = EFI_SUCCESS;
  mChangeMediaDuringRead = FALSE;
  mBlockWrites = 0;
  mBlockFlushes = 0;
  mBlockWriteStatus = EFI_SUCCESS;
  mFlashLength = 0;
  memset (mFlash, 0, sizeof (mFlash));
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
  /* A command that fails before its data stage does not stall: the host must
   * still be able to read the CSW and the sense data. */
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 1 &&
          Bot.InLength == 0 && !Bot.HaltIn);
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

  /* A rejected answer: the data-in stage has to end with a short packet, then
   * CSW status 1 (CHECK CONDITION) with the sense reachable through REQUEST
   * SENSE. A zero-length transfer is not usable here: not every controller
   * emits one as a ZLP, and without the short packet the host reads the CSW as
   * payload and reports GOOD. */
  MakeCbw (mBuffers[2], 0x1a, 6, 63, TRUE); mBuffers[2][17] = 0x02; mBuffers[2][19] = 63;
  assert (CompleteSlot (&Msc, 2, 31, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (mDriverPending[3] && mDriverLength[3] == 63 && Msc.BulkState == MscReadShort);
  assert (mDriverLength[3] < Msc.BulkPacket);
  assert (CompleteSlot (&Msc, 3, 63, UsbTransferStatusComplete) == EFI_SUCCESS);
  assert (mDriverLength[3] == 13 && mBuffers[3][12] == 1);
  /* The data stage carried the whole host expectation, so the CSW must report
   * a residue of zero rather than pretending nothing was transferred. */
  assert (mBuffers[3][8] == 0 && mBuffers[3][9] == 0 && mBuffers[3][10] == 0 &&
          mBuffers[3][11] == 0);
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

STATIC VOID ModeSenseTests (VOID) {
  GBL_MSC_BOT Bot;
  UINT8 Cbw[31];
  UINT8 Data[96];

  GblMscMakeDisk (mDisk);
  GblMscBotInit (&Bot, mDisk, sizeof (mDisk));
  /* All pages, current values, block descriptor present:
   * 4 + 8 + 12 (0x01) + 20 (0x08) + 10 (0x0a) + 10 (0x1c). */
  MakeCbw (Cbw, 0x1a, 6, 96, TRUE);
  Cbw[17] = 0x3f;
  Cbw[19] = 96;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 0);
  assert (Bot.InLength == 64);
  assert (GblMscBotRead (&Bot, 0, Data, 64) == EFI_SUCCESS);
  assert (Data[0] == 63 && Data[1] == 0 && Data[2] == 0x80 && Data[3] == 8);
  /* Block descriptor: density 0, 2048 blocks of 512 bytes. */
  assert (Data[4] == 0 && Data[5] == 0 && Data[6] == 8 && Data[7] == 0);
  assert (Data[8] == 0 && Data[9] == 0 && Data[10] == 2 && Data[11] == 0);
  /* Read/write error recovery: every automatic action stays off. */
  assert (Data[12] == 0x01 && Data[13] == 10);
  assert (Data[14] == 0 && Data[15] == 0 && Data[23] == 0);
  /* Caching page: write cache disabled so hosts report "write through". */
  assert (Data[24] == 0x08 && Data[25] == 18 && Data[26] == 0 && Data[27] == 0);
  /* Control page: fixed-format sense data, no saved log parameters. */
  assert (Data[44] == 0x0a && Data[45] == 8 && Data[46] == 0x04);
  /* Informational exceptions: nothing logged, TEST off. */
  assert (Data[54] == 0x1c && Data[55] == 8 && Data[56] == 0);
  /* DBD suppresses the block descriptor. */
  Cbw[16] = 0x08;
  Cbw[17] = 0x3f;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 56);
  assert (GblMscBotRead (&Bot, 0, Data, 56) == EFI_SUCCESS);
  assert (Data[0] == 55 && Data[3] == 0 && Data[4] == 0x01 && Data[5] == 10);
  assert (Data[16] == 0x08 && Data[17] == 18);
  assert (Data[36] == 0x0a && Data[37] == 8);
  assert (Data[46] == 0x1c && Data[47] == 8);
  /* Page code 0 returns only the header and block descriptor. */
  Cbw[16] = 0;
  Cbw[17] = 0;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 12);
  assert (GblMscBotRead (&Bot, 0, Data, 12) == EFI_SUCCESS);
  assert (Data[0] == 11 && Data[3] == 8 && Data[4] == 0 && Data[10] == 2);
  /* Single pages, block descriptor still present (DBD is clear). */
  Cbw[17] = 0x08;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 32);
  Cbw[17] = 0x01;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 24);
  assert (GblMscBotRead (&Bot, 0, Data, 24) == EFI_SUCCESS);
  assert (Data[12] == 0x01 && Data[13] == 10);
  Cbw[17] = 0x1c;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 22);
  assert (GblMscBotRead (&Bot, 0, Data, 22) == EFI_SUCCESS);
  assert (Data[12] == 0x1c && Data[13] == 8 && Data[14] == 0);
  /* Default values are identical to current values. */
  Cbw[17] = 0x88;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 32);
  /* Changeable and saved values are not implemented. */
  Cbw[17] = 0x48;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 1 &&
          Bot.InLength == 0 && !Bot.HaltIn);
  assert (Bot.SenseKey == 5 && Bot.Asc == 0x24);
  Cbw[17] = 0xc8;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.SenseKey == 5 && Bot.Asc == 0x24);
  /* Unimplemented page codes are rejected. */
  Cbw[17] = 0x02;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.SenseKey == 5 && Bot.Asc == 0x24);
  /* Allocation length truncates the response. */
  Cbw[17] = 0x3f;
  Cbw[19] = 4;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 4);
  Cbw[19] = 0;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 0);
  /* MODE SENSE(10) uses the 8-byte header: 8 + 8 + 12 + 20 + 10 + 10. */
  MakeCbw (Cbw, 0x5a, 10, 96, TRUE);
  Cbw[17] = 0x3f;
  Cbw[23] = 96;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 68);
  assert (GblMscBotRead (&Bot, 0, Data, 68) == EFI_SUCCESS);
  assert (Data[0] == 0 && Data[1] == 66 && Data[2] == 0 && Data[3] == 0x80);
  assert (Data[4] == 0 && Data[5] == 0 && Data[6] == 0 && Data[7] == 8);
  assert (Data[8] == 0 && Data[9] == 0 && Data[10] == 8 && Data[11] == 0);
  assert (Data[12] == 0 && Data[13] == 0 && Data[14] == 2 && Data[15] == 0);
  assert (Data[16] == 0x01 && Data[17] == 10);
  assert (Data[28] == 0x08 && Data[29] == 18);
  assert (Data[48] == 0x0a && Data[49] == 8);
  assert (Data[58] == 0x1c && Data[59] == 8);
  Cbw[16] = 0x08;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 60);
  Cbw[16] = 0;
  Cbw[23] = 9;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 9);
  /* A writable LUN must clear the write-protect bit in both headers: Linux
   * reads the MODE SENSE WP bit before it exposes the medium for writes. */
  Bot.Writable = TRUE;
  MakeCbw (Cbw, 0x1a, 6, 96, TRUE);
  Cbw[17] = 0x3f;
  Cbw[19] = 96;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
  assert (GblMscBotRead (&Bot, 0, Data, 64) == EFI_SUCCESS);
  assert ((Data[2] & 0x80) == 0);
  MakeCbw (Cbw, 0x5a, 10, 96, TRUE);
  Cbw[17] = 0x3f;
  Cbw[23] = 96;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
  assert (GblMscBotRead (&Bot, 0, Data, 68) == EFI_SUCCESS);
  assert ((Data[3] & 0x80) == 0);
  Bot.Writable = FALSE;
}

STATIC VOID FileMediaTests (VOID) {
  GBL_MSC_MEDIA Media;
  UINT8 Buffer[4096];
  UINTN Index;
  UINTN Reads;
  CHAR8 Serial[64];

  ResetMocks ();
  assert (GblMscMediaOpen ("iso:", &Media) == EFI_INVALID_PARAMETER);
  assert (GblMscMediaOpen ("iso:/", &Media) == EFI_INVALID_PARAMETER);
  assert (GblMscMediaOpen ("iso:/trailing/", &Media) == EFI_INVALID_PARAMETER);
  assert (GblMscMediaOpen ("iso://", &Media) == EFI_INVALID_PARAMETER);
  assert (GblMscMediaOpen ("file:", &Media) == EFI_INVALID_PARAMETER);
  assert (GblMscMediaOpen ("fs0:", &Media) == EFI_INVALID_PARAMETER);
  assert (GblMscMediaOpen ("fsa:/x.iso", &Media) == EFI_INVALID_PARAMETER);
  assert (GblMscMediaOpen ("iso:/missing.iso", &Media) == EFI_NOT_FOUND);
  assert (GblMscMediaOpen ("file:/x.iso", &Media) == EFI_NOT_FOUND);

  SetVolumeCount (1);
  AddVolumeFile (0, "EFI\\BOOT\\TEST.ISO", 3000, FALSE);
  assert (GblMscMediaOpen ("iso:/EFI/BOOT/TEST.ISO", &Media) == EFI_SUCCESS);
  assert (Media.IsFile && !Media.IsPartition);
  assert (Media.PeripheralType == GBL_MSC_PERIPHERAL_CDROM);
  assert (Media.BlockSize == GBL_MSC_ISO_BLOCK_SIZE);
  assert (Media.Size == 4096);
  assert (strcmp (Media.Name, "iso:/EFI/BOOT/TEST.ISO") == 0);
  memcpy (Serial, Media.Serial, sizeof (Serial));
  assert (strncmp (Serial, "GBLISO-", 7) == 0 && strlen (Serial) == 15);
  /* Reads are block aligned and use the file contents. */
  assert (Media.Read (Media.Context, 0, Buffer, 2048) == EFI_SUCCESS);
  for (Index = 0; Index < 2048; Index++) {
    assert (Buffer[Index] == FileByte (0, Index));
  }
  /* The final block is zero padded past the end of the file. */
  assert (Media.Read (Media.Context, 2048, Buffer, 2048) == EFI_SUCCESS);
  for (Index = 0; Index < 952; Index++) {
    assert (Buffer[Index] == FileByte (0, 2048 + Index));
  }
  for (Index = 952; Index < 2048; Index++) {
    assert (Buffer[Index] == 0);
  }
  assert (Media.Read (Media.Context, 3000, Buffer, 512) == EFI_SUCCESS);
  for (Index = 0; Index < 512; Index++) {
    assert (Buffer[Index] == 0);
  }
  assert (Media.Read (Media.Context, Media.Size - 1, Buffer, 1) == EFI_SUCCESS);
  assert (Buffer[0] == 0);
  assert (Media.Read (Media.Context, Media.Size, Buffer, 1) == EFI_BAD_BUFFER_SIZE);
  assert (Media.Read (Media.Context, MAX_UINT64, Buffer, 4096) == EFI_BAD_BUFFER_SIZE);
  Reads = mFileReads;
  mFileReadStatus = EFI_DEVICE_ERROR;
  assert (Media.Read (Media.Context, 0, Buffer, 2048) == EFI_DEVICE_ERROR);
  mFileReadStatus = EFI_SUCCESS;
  assert (mFileReads == Reads + 1);
  GblMscMediaClose (&Media);
  assert (Media.Context == NULL && Media.Read == NULL);
  for (Index = 0; Index < MOCK_FILE_OBJECTS; Index++) {
    assert (!mFileObjects[Index].InUse);
  }
  /* The same file as a plain image uses 512-byte blocks and direct access. */
  assert (GblMscMediaOpen ("file:EFI/BOOT/TEST.ISO", &Media) == EFI_SUCCESS);
  assert (Media.PeripheralType == GBL_MSC_PERIPHERAL_DIRECT);
  assert (Media.BlockSize == GBL_MSC_FILE_BLOCK_SIZE && Media.Size == 3072);
  assert (strncmp (Media.Serial, "GBLFILE-", 8) == 0);
  assert (strcmp (Media.Serial, Serial) != 0);
  assert (Media.Read (Media.Context, 2048, Buffer, 1024) == EFI_SUCCESS);
  for (Index = 0; Index < 952; Index++) {
    assert (Buffer[Index] == FileByte (0, 2048 + Index));
  }
  for (Index = 952; Index < 1024; Index++) {
    assert (Buffer[Index] == 0);
  }
  GblMscMediaClose (&Media);
  /* Directories and empty files are not media. */
  SetVolumeCount (1);
  AddVolumeFile (0, "EFI\\BOOT", 4096, TRUE);
  assert (GblMscMediaOpen ("iso:/EFI/BOOT", &Media) == EFI_UNSUPPORTED);
  SetVolumeCount (1);
  AddVolumeFile (0, "EMPTY.ISO", 0, FALSE);
  assert (GblMscMediaOpen ("iso:/EMPTY.ISO", &Media) == EFI_UNSUPPORTED);
  /* A file on the second volume is found by search but not by fs0:. */
  SetVolumeCount (2);
  AddVolumeFile (1, "SECOND.ISO", 4096, FALSE);
  assert (GblMscMediaOpen ("iso:fs0:/SECOND.ISO", &Media) == EFI_NOT_FOUND);
  assert (GblMscMediaOpen ("iso:fs1:/SECOND.ISO", &Media) == EFI_SUCCESS);
  assert (Media.Size == 4096);
  GblMscMediaClose (&Media);
  assert (GblMscMediaOpen ("file:fs2:/SECOND.ISO", &Media) == EFI_NOT_FOUND);
  assert (GblMscMediaOpen ("iso:SECOND.ISO", &Media) == EFI_SUCCESS);
  GblMscMediaClose (&Media);
  ResetMocks ();
}

STATIC VOID IsoScsiTests (VOID) {
  GBL_MSC_MEDIA Media;
  GBL_MSC_BOT Bot;
  UINT8 Cbw[31];
  UINT8 Data[128];

  GblMscMakeDisk (mDisk);
  ResetMocks ();
  SetVolumeCount (1);
  AddVolumeFile (0, "TEST.ISO", 3000, FALSE);
  assert (GblMscMediaOpen ("iso:/TEST.ISO", &Media) == EFI_SUCCESS);
  GblMscBotInitMedia (&Bot, &Media);
  /* INQUIRY reports a read-only CD-ROM device. */
  MakeCbw (Cbw, 0x12, 6, 36, TRUE);
  Cbw[19] = 36;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 36);
  assert (GblMscBotRead (&Bot, 0, Data, 36) == EFI_SUCCESS);
  assert (Data[0] == 5 && Data[1] == 0x80 && Data[2] == 5);
  /* Vendor + product only: the 4-byte revision field is the on-device run
   * marker and is expected to change between validation builds. */
  assert (memcmp (Data + 8, "GBL     ISO IMAGE       ", 24) == 0);
  /* READ CAPACITY reports 2048-byte blocks. */
  MakeCbw (Cbw, 0x25, 10, 8, TRUE);
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 8);
  assert (GblMscBotRead (&Bot, 0, Data, 8) == EFI_SUCCESS);
  assert (Data[3] == 1 && Data[4] == 0 && Data[6] == 8 && Data[7] == 0);
  /* Block descriptor uses the ISO block size. All pages, DBD clear:
   * 4 + 8 + 12 + 20 + 10 + 10 + 20. */
  MakeCbw (Cbw, 0x1a, 6, 96, TRUE);
  Cbw[17] = 0x3f;
  Cbw[19] = 96;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 84);
  assert (GblMscBotRead (&Bot, 0, Data, 84) == EFI_SUCCESS);
  assert (Data[5] == 0 && Data[6] == 0 && Data[7] == 2);
  assert (Data[9] == 0 && Data[10] == 8 && Data[11] == 0);
  /* GET CONFIGURATION advertises the CD-ROM profile only: the data length
   * counts the reserved bytes and the profile, and no features follow. */
  MakeCbw (Cbw, 0x46, 10, 12, TRUE);
  Cbw[23] = 12;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 8);
  assert (GblMscBotRead (&Bot, 0, Data, 8) == EFI_SUCCESS);
  assert (Data[0] == 0 && Data[1] == 0 && Data[2] == 0 && Data[3] == 4);
  assert (Data[6] == 0 && Data[7] == 0x08);
  /* A feature code that is not implemented returns an empty feature list. */
  Cbw[16] = 2;
  Cbw[18] = 1;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 4);
  assert (Bot.Response[3] == 0);
  /* Request type 3 is reserved. */
  Cbw[16] = 3;
  Cbw[18] = 0;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.SenseKey == 5 && Bot.Asc == 0x24);
  /* The MMC capabilities page exists for optical LUNs: a read-only data disc
   * advertises no CD-R/RW writing, no CD-DA, no C2 and no DVD capability. */
  MakeCbw (Cbw, 0x1a, 6, 64, TRUE);
  Cbw[17] = 0x2a;
  Cbw[19] = 64;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 32);
  assert (GblMscBotRead (&Bot, 0, Data, 32) == EFI_SUCCESS);
  /* 4-byte header, 8-byte block descriptor, then the 20-byte page. */
  assert (Data[12] == 0x2a && Data[13] == 18);
  assert (Data[14] == 0 && Data[15] == 0 && Data[16] == 0 && Data[31] == 0);
  /* "All pages" lists it after the caching, control and informational pages,
   * in ascending page-code order. */
  MakeCbw (Cbw, 0x1a, 6, 96, TRUE);
  Cbw[17] = 0x3f;
  Cbw[19] = 96;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 84);
  assert (GblMscBotRead (&Bot, 0, Data, 84) == EFI_SUCCESS);
  assert (Data[12] == 0x01 && Data[24] == 0x08 && Data[44] == 0x0a &&
          Data[54] == 0x1c && Data[64] == 0x2a && Data[65] == 18);
  /* GET EVENT STATUS NOTIFICATION never queues a media event: the image
   * cannot be ejected or replaced, so the answer stays NEA. */
  MakeCbw (Cbw, 0x4a, 10, 8, TRUE);
  Cbw[16] = 1;
  Cbw[19] = 0x10;
  Cbw[23] = 8;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 8);
  assert (GblMscBotRead (&Bot, 0, Data, 8) == EFI_SUCCESS);
  assert (Data[0] == 0 && Data[1] == 0 && Data[2] == 0x80 && Data[5] == 0x10 && Data[7] == 0);
  /* READ TOC: one data track at LBA 0, then the lead-out. The lead-out carries
   * the address one block past the last block, which is how a host sizes the
   * disc (cdrom_get_last_written), so it must not be zero. */
  MakeCbw (Cbw, 0x43, 10, 20, TRUE);
  Cbw[23] = 20;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 20);
  assert (GblMscBotRead (&Bot, 0, Data, 20) == EFI_SUCCESS);
  assert (Data[0] == 0 && Data[1] == 18 && Data[2] == 1 && Data[3] == 1);
  assert (Data[4] == 0 && Data[5] == 0x14 && Data[6] == 1 && Data[7] == 0);
  assert (Data[8] == 0 && Data[9] == 0 && Data[10] == 0 && Data[11] == 0);
  assert (Data[12] == 0 && Data[13] == 0x14 && Data[14] == 0xaa && Data[15] == 0);
  assert (Data[16] == 0 && Data[17] == 0 && Data[18] == 0 && Data[19] == 2);
  /* CDB[6] = 0xAA asks for the lead-out alone and CDB[1] bit 1 selects MSF,
   * which counts 75 frames per second from 00:02:00. */
  Cbw[16] = 2;
  Cbw[21] = 0xaa;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 12);
  assert (GblMscBotRead (&Bot, 0, Data, 12) == EFI_SUCCESS);
  assert (Data[1] == 10 && Data[6] == 0xaa);
  assert (Data[8] == 0 && Data[9] == 0 && Data[10] == 2 && Data[11] == 2);
  Cbw[16] = 0;
  Cbw[21] = 0;
  /* Unsupported formats fall back to the TOC layout instead of failing. */
  Cbw[17] = 2;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 20);
  Cbw[17] = 0;
  GblMscMediaClose (&Media);
  GblMscBotInit (&Bot, mDisk, sizeof (mDisk));
  /* MMC commands are invalid for direct-access media. */
  MakeCbw (Cbw, 0x46, 10, 12, TRUE);
  Cbw[23] = 12;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 1);
  assert (Bot.SenseKey == 5 && Bot.Asc == 0x20);
  MakeCbw (Cbw, 0x43, 10, 12, TRUE);
  Cbw[23] = 12;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.SenseKey == 5 && Bot.Asc == 0x20);
  MakeCbw (Cbw, 0x4a, 10, 8, TRUE);
  Cbw[23] = 8;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.SenseKey == 5 && Bot.Asc == 0x20);
  /* The capabilities page is optical-only: a direct-access LUN rejects it. */
  MakeCbw (Cbw, 0x1a, 6, 32, TRUE);
  Cbw[17] = 0x2a;
  Cbw[19] = 32;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.SenseKey == 5 && Bot.Asc == 0x24);
  ResetMocks ();
}

/*
 * The write backend: which LUNs accept writes, that a write is visible to the
 * next read in command order, that SYNCHRONIZE CACHE is a real barrier, that
 * range checks and MODE SELECT behave, and that nothing outside a writable
 * LUN can be touched.
 */
STATIC VOID WriteBackendTests (VOID) {
  GBL_MSC_MEDIA Media;
  GBL_MSC_BOT Bot;
  UINT8 Cbw[31];
  UINT8 Payload[1024];
  UINT8 Data[1024];
#if GBL_USB_MSC_WRITE
  UINT8 List[32];
#endif
  UINTN Index;

  ResetMocks ();
  /* Sized in-memory media: the default 1 MiB image, an explicit KiB size, and
   * the bounds that must be refused before anything is allocated. */
  assert (GblMscMediaOpen ("ram:", &Media) == EFI_SUCCESS);
  assert (Media.Size == GBL_MSC_DISK_SIZE && Media.BlockSize == 512);
  assert (Media.Write == NULL || Media.Writable);
  GblMscMediaClose (&Media);
  assert (GblMscMediaOpen ("ram:64", &Media) == EFI_SUCCESS);
  assert (Media.Size == 65536);
  assert (Media.Read (Media.Context, Media.Size - 512, Data, 512) == EFI_SUCCESS);
  for (Index = 0; Index < 512; Index++) assert (Data[Index] == 0);
  GblMscMediaClose (&Media);
  assert (GblMscMediaOpen ("ram:1", &Media) == EFI_INVALID_PARAMETER);
  assert (GblMscMediaOpen ("ram:0", &Media) == EFI_INVALID_PARAMETER);
  assert (GblMscMediaOpen ("ram:65537", &Media) == EFI_INVALID_PARAMETER);
  assert (GblMscMediaOpen ("ram:16x", &Media) == EFI_INVALID_PARAMETER);
  assert (GblMscMediaOpen ("ram:512", &Media) == EFI_SUCCESS);
  assert (Media.Size == 512U * 1024U);
  GblMscMediaClose (&Media);

  /* A read-only LUN still refuses every write, whatever the build. */
  assert (GblMscMediaOpen ("ram:", &Media) == EFI_SUCCESS);
  GblMscBotInitMedia (&Bot, &Media);
  MakeCbw (Cbw, 0x28, 10, 512, TRUE);
  Cbw[20] = 64;
  Cbw[23] = 1;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 0);
  assert (GblMscBotRead (&Bot, 0, Data, 512) == EFI_SUCCESS);
  MakeCbw (Cbw, 0x2a, 10, 512, FALSE);
  Cbw[20] = 64;
  Cbw[23] = 1;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
  assert (Bot.Writable == Media.Writable);
  if (!Media.Writable) {
    assert (Bot.Status == 1 && Bot.SenseKey == 7 && Bot.Asc == 0x27);
    assert (!Bot.ToDisk && Bot.OutLength == 512);
    memset (Payload, 0xaa, sizeof (Payload));
    assert (GblMscBotWrite (&Bot, 0, Payload, 512) == EFI_SUCCESS);
    /* Nothing changed: read the same block again. */
    MakeCbw (Cbw, 0x28, 10, 512, TRUE);
    Cbw[20] = 64;
    Cbw[23] = 1;
    assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
    assert (GblMscBotRead (&Bot, 0, Payload, 512) == EFI_SUCCESS);
    assert (memcmp (Payload, Data, 512) == 0);
  }
#if GBL_USB_MSC_WRITE
  else {
    /* WRITE(10) of two blocks, delivered in two bulk-sized chunks. */
    for (Index = 0; Index < sizeof (Payload); Index++) {
      Payload[Index] = (UINT8)(Index * 7U + 3U);
    }
    MakeCbw (Cbw, 0x2a, 10, 1024, FALSE);
    Cbw[20] = 64;
    Cbw[23] = 2;
    assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
    assert (Bot.Status == 0 && Bot.ToDisk && Bot.OutLength == 1024 && !Bot.Dirty);
    assert (GblMscBotWrite (&Bot, 0, Payload, 512) == EFI_SUCCESS);
    assert (GblMscBotWrite (&Bot, 512, Payload + 512, 512) == EFI_SUCCESS);
    assert (Bot.Dirty);
    /* The next read sees the written bytes, in order. */
    MakeCbw (Cbw, 0x28, 10, 1024, TRUE);
    Cbw[20] = 64;
    Cbw[23] = 2;
    assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 1024);
    assert (GblMscBotRead (&Bot, 0, Data, 1024) == EFI_SUCCESS);
    assert (memcmp (Data, Payload, 1024) == 0);
    /* SYNCHRONIZE CACHE is a barrier and clears the dirty flag. */
    MakeCbw (Cbw, 0x35, 10, 0, FALSE);
    assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 0 && !Bot.Dirty);
    /* A write past the end is refused and stays clean. LBA bytes are CBW
     * bytes 17..20 for a ten-byte CDB. */
    MakeCbw (Cbw, 0x2a, 10, 512, FALSE);
    Cbw[17] = 0xff;
    Cbw[18] = 0xff;
    Cbw[19] = 0xff;
    Cbw[20] = 0xff;
    Cbw[23] = 1;
    assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
    assert (Bot.Status == 1 && Bot.SenseKey == 5 && Bot.Asc == 0x21 && !Bot.ToDisk);
    /* MODE SELECT(6): the caching page with WCE clear is accepted. */
    memset (List, 0, sizeof (List));
    List[0] = 24;
    List[4] = 0x08;
    List[5] = 0x12;
    MakeCbw (Cbw, 0x15, 6, 24, FALSE);
    Cbw[19] = 24;
    assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
    assert (Bot.Status == 0 && Bot.ParamList == 1 && Bot.OutLength == 24);
    assert (GblMscBotWrite (&Bot, 0, List, 24) == EFI_SUCCESS && Bot.Status == 0);
    /* Asking for the write cache to be switched on is refused. */
    List[6] = 0x04;
    assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 0);
    assert (GblMscBotWrite (&Bot, 0, List, 24) == EFI_SUCCESS);
    assert (Bot.Status == 1 && Bot.SenseKey == 5 && Bot.Asc == 0x26);
    /* An unknown page in the list is refused. */
    List[6] = 0;
    List[4] = 0x0e;
    assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
    assert (GblMscBotWrite (&Bot, 0, List, 24) == EFI_SUCCESS);
    assert (Bot.Status == 1 && Bot.SenseKey == 5 && Bot.Asc == 0x26);
    /* A page whose declared length runs past the list is a length error, and
     * a list longer than the buffer is refused before the data stage. A list
     * that stops short of its declared length is caught by the transport
     * residue instead, so it never reaches the validator. */
    List[4] = 0x08;
    List[5] = 0x20;
    assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
    assert (Bot.Status == 0);
    assert (GblMscBotWrite (&Bot, 0, List, 24) == EFI_SUCCESS);
    assert (Bot.Status == 1 && Bot.SenseKey == 5 && Bot.Asc == 0x1a);
    List[5] = 0x12;
    MakeCbw (Cbw, 0x15, 6, 200, FALSE);
    Cbw[19] = 200;
    assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.InLength == 0 &&
            Bot.SenseKey == 5 && Bot.Asc == 0x1a);
    /* MODE SELECT(10) with a valid list is accepted too: its header is eight
     * bytes, so the caching page starts at offset 8. */
    memset (List, 0, sizeof (List));
    List[8] = 0x08;
    List[9] = 0x12;
    MakeCbw (Cbw, 0x55, 10, 28, FALSE);
    Cbw[22] = 0;
    Cbw[23] = 28;
    assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.ParamList == 2);
    assert (GblMscBotWrite (&Bot, 0, List, 28) == EFI_SUCCESS && Bot.Status == 0);
  }
#endif
  GblMscMediaClose (&Media);
  assert (Media.Context == NULL && Media.Write == NULL);

#if GBL_USB_MSC_WRITE_PARTITIONS
  /* Opt-in partition writes go through BlockIo and are flushed for real. */
  ResetMocks ();
  assert (GblMscMediaOpen ("rw:boot_a", &Media) == EFI_SUCCESS);
  assert (Media.IsPartition && Media.Writable);
  assert (memcmp (Media.Serial, "GBLRW-", 6) == 0);
  GblMscBotInitMedia (&Bot, &Media);
  memset (Payload, 0x5a, 512);
  MakeCbw (Cbw, 0x2a, 10, 512, FALSE);
  Cbw[23] = 1;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 0 && Bot.ToDisk);
  assert (GblMscBotWrite (&Bot, 0, Payload, 512) == EFI_SUCCESS);
  assert (mBlockWrites == 1 && mBlockFlushes == 0);
  MakeCbw (Cbw, 0x28, 10, 512, TRUE);
  Cbw[23] = 1;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
  assert (GblMscBotRead (&Bot, 0, Data, 512) == EFI_SUCCESS);
  assert (memcmp (Data, Payload, 512) == 0);
  /* A partial block is read-modify-written, never half updated. */
  memset (Payload, 0xa5, 100);
  MakeCbw (Cbw, 0x2a, 10, 100, FALSE);
  Cbw[23] = 1;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
  assert (GblMscBotWrite (&Bot, 0, Payload, 100) == EFI_SUCCESS);
  assert (mBlockWrites == 2 && mBlockReads >= 2);
  /* SYNCHRONIZE CACHE reaches BlockIo->FlushBlocks. */
  MakeCbw (Cbw, 0x35, 10, 0, FALSE);
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS && Bot.Status == 0);
  assert (mBlockFlushes == 1 && !Bot.Dirty);
  /* A table change mid-session aborts the write with MEDIA CHANGED. */
  mBlockMedia[0].MediaId++;
  MakeCbw (Cbw, 0x2a, 10, 512, FALSE);
  Cbw[23] = 1;
  assert (GblMscBotBegin (&Bot, Cbw, 31) == EFI_SUCCESS);
  assert (GblMscBotWrite (&Bot, 0, Payload, 512) == EFI_MEDIA_CHANGED);
  assert (Bot.SenseKey == 6 && Bot.Asc == 0x28);
  mBlockMedia[0].MediaId--;
  GblMscMediaClose (&Media);
  /* Closing a writable LUN flushes it. */
  assert (mBlockFlushes == 2);

  /* A writable partition must stay away from the GPT. */
  ResetMocks ();
  mPartitionEntries[0].StartingLBA = 2;
  mPartitionEntries[0].EndingLBA = 2 + mBlockMedia[0].LastBlock;
  assert (GblMscMediaOpen ("rw:boot_a", &Media) == EFI_UNSUPPORTED);
  assert (Media.Context == NULL);

  /* The read-only spelling of the same partition stays writable-free. */
  ResetMocks ();
  assert (GblMscMediaOpen ("ro:boot_a", &Media) == EFI_SUCCESS);
  assert (!Media.Writable && Media.Write == NULL);
  assert (memcmp (Media.Serial, "GBLRO-", 6) == 0);
  GblMscMediaClose (&Media);
#else
  /* Without the partition write backend the opt-in is refused outright. */
  ResetMocks ();
  assert (GblMscMediaOpen ("rw:boot_a", &Media) == EFI_SECURITY_VIOLATION);
  assert (GblMscMediaOpen ("ro:boot_a", &Media) == EFI_SUCCESS && !Media.Writable);
  GblMscMediaClose (&Media);
#endif
  ResetMocks ();
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
  ModeSenseTests ();
  WriteBackendTests ();
  FileMediaTests ();
  IsoScsiTests ();
  LifecycleTests ();
  RepeatedSessionTests ();
  CommandTests ();
  puts ("ok USB MSC: read-only RAM/partition/ISO/file media, multi-LUN REPORT LUNS, 512/2048/4096 logical blocks, MODE SENSE headers/block descriptors/caching/control pages, MMC GET CONFIGURATION and READ TOC, 64-bit capacity, VERIFY, Qualcomm EP0, SCSI/BOT reset recovery, repeated exits, DMA lifetime, OOM and Fastboot ACK");
  return 0;
}
