#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>

EFI_GUID gEfiUfsLU0Guid, gEfiUfsLU1Guid, gEfiUfsLU2Guid, gEfiUfsLU3Guid;
EFI_GUID gEfiUfsLU4Guid, gEfiUfsLU5Guid, gEfiUfsLU6Guid, gEfiUfsLU7Guid;
EFI_GUID gEfiEmmcUserPartitionGuid, gEfiNvme0Guid, gEfiPartitionRecordGuid;
EFI_GUID gEfiMemCardInfoProtocolGuid;
EFI_GUID gEfiBootImgPartitionGuid = {0x20117f86, 0xe985, 0x4357, {0xb9, 0xee, 0x37, 0x4b, 0xc1, 0xd8, 0x48, 0x7d}};

#define __FASTBOOT_CMDS_H__
#include "../../../GblChainloadPkg/Application/GblChainload/SlotAttrs.c"

#define TEST_BLOCKS 256U
#define TEST_ENTRIES 128U

struct PartitionEntry PtnEntries[MAX_NUM_PARTITIONS];
struct StoragePartInfo Ptable[MAX_LUNS];
STATIC EFI_PARTITION_ENTRY mRecords[2];
STATIC EFI_BOOT_SERVICES mServices;
EFI_BOOT_SERVICES *gBS = &mServices;
STATIC EFI_BLOCK_IO_PROTOCOL mBlockIo;
STATIC EFI_BLOCK_IO_MEDIA mMedia;
STATIC EFI_BLOCK_IO_PROTOCOL mAblBlockIo;
STATIC EFI_BLOCK_IO_MEDIA mAblMedia;
STATIC UINT8 *mAblDisk;
STATIC UINT8 *mDisk;
STATIC UINT8 *mOriginal;
STATIC UINTN mDiskSize;
STATIC UINTN mAllocations;
STATIC UINTN mAllocateCalls;
STATIC UINTN mFailAllocation;
STATIC UINTN mWrites;
STATIC UINTN mFlushes;
STATIC UINTN mReads;
STATIC UINTN mFailRead;
STATIC UINTN mFailWrite;
STATIC UINTN mFailFlush;
STATIC UINTN mCorruptWrite;
STATIC BOOLEAN mPersistentFailure;
STATIC BOOLEAN mChangeMedia;
STATIC BOOLEAN mUnlocked;
STATIC BOOLEAN mIdle;
STATIC UINT32 mBootLun;
STATIC EFI_STATUS mBootLunStatus;
STATIC EFI_MEM_CARDINFO_PROTOCOL mCard;
STATIC EFI_USB_DEVICE_PROTOCOL mUsb;
STATIC FastbootDeviceData mFastboot;
STATIC CHAR8 mInfo[32][60];
STATIC UINTN mInfoCount;
STATIC UINTN mInfoMode;
STATIC UINTN mPolls;
STATIC UINTN mStalls;
STATIC BOOLEAN mInfoPending;
STATIC VirtualAbMergeStatus mMerge;
STATIC UINTN mRootCount;
STATIC BOOLEAN mOkay;
STATIC UINTN mReplies;
STATIC CHAR8 mResponse[64];
STATIC EFI_LBA mWrittenLbas[32];
STATIC VOID (*mShow)(CONST CHAR8 *, VOID *, UINT32);
STATIC VOID (*mSet)(CONST CHAR8 *, VOID *, UINT32);

VOID *EFIAPI CopyMem (VOID *Destination, CONST VOID *Source, UINTN Length) {
  return memcpy (Destination, Source, Length);
}
VOID *EFIAPI ZeroMem (VOID *Buffer, UINTN Length) { return memset (Buffer, 0, Length); }
INTN EFIAPI CompareMem (CONST VOID *First, CONST VOID *Second, UINTN Length) {
  return memcmp (First, Second, Length);
}
INTN EFIAPI AsciiStrCmp (CONST CHAR8 *First, CONST CHAR8 *Second) { return strcmp (First, Second); }
CHAR8 *EFIAPI AsciiStrStr (CONST CHAR8 *String, CONST CHAR8 *Search) { return strstr (String, Search); }
INTN EFIAPI StrnCmp (CONST CHAR16 *First, CONST CHAR16 *Second, UINTN Length) {
  UINTN Index;
  for (Index = 0; Index < Length; Index++) {
    if (First[Index] != Second[Index]) return (INTN)First[Index] - Second[Index];
    if (First[Index] == 0) return 0;
  }
  return 0;
}
VOID *EFIAPI AllocateZeroPool (UINTN Size) {
  VOID *Buffer;
  mAllocateCalls++;
  if (mAllocateCalls == mFailAllocation) return NULL;
  Buffer = calloc (1, Size);
  assert (Buffer != NULL);
  mAllocations++;
  return Buffer;
}
VOID EFIAPI FreePool (VOID *Buffer) { assert (Buffer != NULL && mAllocations > 0); mAllocations--; free (Buffer); }
UINTN EFIAPI AsciiSPrint (CHAR8 *Buffer, UINTN Size, CONST CHAR8 *Format, ...) {
  CHAR8 HostFormat[256];
  UINTN Source;
  UINTN Target = 0;
  int Result;
  va_list Arguments;
  for (Source = 0; Format[Source] != 0; Source++) {
    if (Format[Source] == '%' && Format[Source + 1] == 'a') {
      HostFormat[Target++] = '%'; HostFormat[Target++] = 's'; Source++;
    } else if (Format[Source] == '%' && Format[Source + 1] == 'r') {
      HostFormat[Target++] = '%'; HostFormat[Target++] = 'l'; HostFormat[Target++] = 'x'; Source++;
    } else HostFormat[Target++] = Format[Source];
  }
  HostFormat[Target] = 0;
  va_start (Arguments, Format);
  Result = vsnprintf (Buffer, Size, HostFormat, Arguments);
  va_end (Arguments);
  if (Result < 0 || (UINTN)Result >= Size) fprintf (stderr, "response overflow: %s (%d bytes)\n", Format, Result);
  assert (Result >= 0 && (UINTN)Result < Size);
  return (UINTN)Result;
}

STATIC EFI_STATUS EFIAPI Crc32 (VOID *Data, UINTN Length, UINT32 *Result) {
  UINT32 Crc = 0xffffffffU;
  UINTN Index;
  UINTN Bit;
  for (Index = 0; Index < Length; Index++) {
    Crc ^= ((UINT8 *)Data)[Index];
    for (Bit = 0; Bit < 8; Bit++) Crc = (Crc >> 1) ^ ((Crc & 1) ? 0xedb88320U : 0);
  }
  *Result = ~Crc;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI ReadBlocks (EFI_BLOCK_IO_PROTOCOL *Io, UINT32 MediaId,
                                     EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  EFI_BLOCK_IO_MEDIA *Media = Io == &mBlockIo ? &mMedia : &mAblMedia;
  UINT8 *Disk = Io == &mBlockIo ? mDisk : mAblDisk;
  assert ((Io == &mBlockIo || Io == &mAblBlockIo) && MediaId == Media->MediaId);
  assert (Disk != NULL && (UINTN)Buffer % Media->IoAlign == 0);
  assert (Lba < TEST_BLOCKS && Size <= mDiskSize - Lba * Media->BlockSize);
  mReads++;
  if (mReads == mFailRead) return EFI_DEVICE_ERROR;
  memcpy (Buffer, Disk + Lba * Media->BlockSize, Size);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI WriteBlocks (EFI_BLOCK_IO_PROTOCOL *Io, UINT32 MediaId,
                                      EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  UINTN Offset = (UINTN)Lba * mMedia.BlockSize;
  assert (Io == &mBlockIo && MediaId == mMedia.MediaId);
  assert ((UINTN)Buffer % mMedia.IoAlign == 0 && Size == mMedia.BlockSize);
  assert (Lba < TEST_BLOCKS && mWrites < ARRAY_SIZE (mWrittenLbas));
  mWrittenLbas[mWrites++] = Lba;
  if (mWrites == mFailWrite || (mPersistentFailure && mWrites >= mFailWrite)) {
    memcpy (mDisk + Offset, Buffer, Size / 2);
    return EFI_DEVICE_ERROR;
  }
  memcpy (mDisk + Offset, Buffer, Size);
  if (mWrites == mCorruptWrite) mDisk[Offset + 17] ^= 1;
  if (mChangeMedia) { mMedia.MediaId++; mChangeMedia = FALSE; }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI FlushBlocks (EFI_BLOCK_IO_PROTOCOL *Io) {
  assert (Io == &mBlockIo);
  mFlushes++;
  return mFlushes == mFailFlush ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI HandleProtocol (EFI_HANDLE Handle, EFI_GUID *Guid, VOID **Interface) {
  if (Guid == &gEfiMemCardInfoProtocolGuid) {
    assert (Handle == &mCard);
    *Interface = &mCard;
    return EFI_SUCCESS;
  }
  assert (Guid == &gEfiPartitionRecordGuid);
  assert (Handle == &mRecords[0] || Handle == &mRecords[1]);
  *Interface = Handle;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI GetBlkIOHandles (UINT32 Selection, PartiSelectFilter *Filter,
                                  HandleInfo *Handles, UINT32 *Count) {
  assert (Selection == BLK_IO_SEL_SELECT_ROOT_DEVICE_ONLY);
  assert (*Count == 2);
  assert (Filter->RootDeviceType == &gEfiUfsLU4Guid || Filter->RootDeviceType == &gEfiUfsLU0Guid ||
          Filter->RootDeviceType == &gEfiUfsLU3Guid);
  Handles[0].BlkIo = Filter->RootDeviceType == &gEfiUfsLU3Guid ? &mAblBlockIo : &mBlockIo;
  Handles[0].Handle = (EFI_HANDLE *)&mCard;
  *Count = (UINT32)mRootCount;
  return EFI_SUCCESS;
}
VOID GetRootDeviceType (CHAR8 *Type, UINT32 Length) { assert (Length >= 4); memcpy (Type, "UFS", 4); }
STATIC EFI_STATUS EFIAPI GetBootLun (EFI_MEM_CARDINFO_PROTOCOL *Card, UINT32 *BootLun) {
  assert (Card == &mCard);
  *BootLun = mBootLun;
  return mBootLunStatus;
}
INT32 GetPartitionIndex (CHAR16 *Name) {
  if (!StrnCmp (Name, L"boot_a", 7)) return 0;
  if (!StrnCmp (Name, L"boot_b", 7)) return 1;
  if (!StrnCmp (Name, L"abl_a", 6)) return 2;
  if (!StrnCmp (Name, L"abl_b", 6)) return 3;
  return INVALID_PTN;
}
BOOLEAN IsUnlocked (VOID) { return mUnlocked; }
BOOLEAN FlashComplete (VOID) { return mIdle; }
BOOLEAN IsDynamicPartitionSupport (VOID) { return TRUE; }
VirtualAbMergeStatus GetSnapshotMergeStatus (VOID) { return mMerge; }
VOID FastbootRegister (CONST CHAR8 *Prefix, VOID (*Handler)(CONST CHAR8 *, VOID *, UINT32)) {
  if (!strcmp (Prefix, "oem slot-show")) mShow = Handler;
  else { assert (!strcmp (Prefix, "oem slot-set")); mSet = Handler; }
}
VOID FastbootOkay (CONST CHAR8 *Response) {
  assert (!mInfoPending);
  assert (strlen (Response) < 60);
  strcpy (mResponse, Response); mOkay = TRUE; mReplies++;
}
VOID FastbootFail (CONST CHAR8 *Response) {
  assert (!mInfoPending);
  assert (strlen (Response) < 60);
  strcpy (mResponse, Response); mOkay = FALSE; mReplies++;
}

FastbootDeviceData *GetFastbootDeviceData (VOID) { return &mFastboot; }
VOID FastbootInfo (CONST CHAR8 *Response) {
  assert (!mInfoPending && strlen (Response) < 60 && mInfoCount < ARRAY_SIZE (mInfo));
  strcpy (mInfo[mInfoCount++], Response);
  mInfoPending = TRUE;
  mPolls = 0;
}
STATIC EFI_STATUS HandleEvent (USB_DEVICE_EVENT *Message, UINTN *Size, USB_DEVICE_EVENT_DATA *Payload) {
  assert (mInfoPending && *Size == sizeof (*Payload));
  mPolls++;
  if (mInfoMode == 4) return EFI_DEVICE_ERROR;
  if (mInfoMode == 1 || mPolls == 1) return EFI_NOT_READY;
  if (mInfoMode == 2) {
    *Message = UsbDeviceEventDeviceStateChange;
    Payload->DeviceState = UsbDeviceStateDisconnected;
    return EFI_SUCCESS;
  }
  *Message = UsbDeviceEventTransferNotification;
  Payload->TransferOutcome.EndpointIndex = mPolls == 2 ? 0x80 : 0x81;
  Payload->TransferOutcome.Status = mInfoMode == 3
    ? UsbDeviceTransferStatusCancelled : UsbDeviceTransferStatusCompleteOK;
  if (mPolls > 2) mInfoPending = FALSE;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Stall (UINTN Microseconds) {
  assert (Microseconds == 1000);
  mStalls++;
  return EFI_SUCCESS;
}

STATIC VOID RecalculateDisk (UINT8 *Disk, UINT32 BlockSize, UINT8 *Header) {
  UINT32 Crc;
  UINT64 EntryLba = GET_LLWORD_FROM_BYTE (Header + PARTITION_ENTRIES_OFFSET);
  Crc32 (Disk + EntryLba * BlockSize, TEST_ENTRIES * PARTITION_ENTRY_SIZE, &Crc);
  PUT_LONG (Header + PARTITION_CRC_OFFSET, Crc);
  PUT_LONG (Header + HEADER_CRC_OFFSET, 0);
  Crc32 (Header, GPT_HEADER_SIZE, &Crc);
  PUT_LONG (Header + HEADER_CRC_OFFSET, Crc);
}

STATIC VOID Recalculate (UINT8 *Header) { RecalculateDisk (mDisk, mMedia.BlockSize, Header); }

STATIC VOID Setup (UINT32 BlockSize) {
  UINTN Index;
  UINTN Slot;
  UINTN EntryBlocks = (TEST_ENTRIES * PARTITION_ENTRY_SIZE) / BlockSize;
  UINTN First = 2 + EntryBlocks;

  assert (mAllocations == 0);
  free (mDisk); free (mOriginal); free (mAblDisk); mAblDisk = NULL;
  mDiskSize = TEST_BLOCKS * BlockSize;
  mDisk = malloc (mDiskSize); mOriginal = malloc (mDiskSize);
  assert (mDisk && mOriginal);
  for (Index = 0; Index < mDiskSize; Index++) mDisk[Index] = (UINT8)(Index ^ (Index >> 9));
  memset (&mMedia, 0, sizeof (mMedia));
  mMedia.MediaId = 42; mMedia.BlockSize = BlockSize; mMedia.IoAlign = BlockSize;
  mMedia.LastBlock = TEST_BLOCKS - 1; mMedia.MediaPresent = TRUE;
  mBlockIo.Media = &mMedia; mBlockIo.ReadBlocks = ReadBlocks;
  mBlockIo.WriteBlocks = WriteBlocks; mBlockIo.FlushBlocks = FlushBlocks;
  mServices.CalculateCrc32 = Crc32; mServices.HandleProtocol = HandleProtocol;
  mServices.Stall = Stall;
  mUsb.HandleEvent = HandleEvent;
  mFastboot.UsbDeviceProtocol = &mUsb;
  mCard.Revision = EFI_MEM_CARD_INFO_PROTOCOL_REVISION_3;
  mCard.GetBootLU = GetBootLun;
  memset (PtnEntries, 0, sizeof (PtnEntries)); memset (Ptable, 0, sizeof (Ptable));
  for (Index = 0; Index < 2; Index++) {
    UINT8 *Header = mDisk + (Index == 0 ? 1 : TEST_BLOCKS - 1) * BlockSize;
    UINT64 EntryLba = Index == 0 ? 2 : TEST_BLOCKS - 1 - EntryBlocks;
    memset (Header, 0, GPT_HEADER_SIZE);
    memcpy (Header, "EFI PART", 8);
    PUT_LONG (Header + 8, 0x10000);
    PUT_LONG (Header + HEADER_SIZE_OFFSET, GPT_HEADER_SIZE);
    PUT_LONG_LONG (Header + PRIMARY_HEADER_OFFSET, (UINT64)(Index == 0 ? 1 : TEST_BLOCKS - 1));
    PUT_LONG_LONG (Header + BACKUP_HEADER_OFFSET, (UINT64)(Index == 0 ? TEST_BLOCKS - 1 : 1));
    PUT_LONG_LONG (Header + FIRST_USABLE_LBA_OFFSET, First);
    PUT_LONG_LONG (Header + LAST_USABLE_LBA_OFFSET, TEST_BLOCKS - 2 - EntryBlocks);
    Header[56] = 31;
    PUT_LONG_LONG (Header + PARTITION_ENTRIES_OFFSET, EntryLba);
    PUT_LONG (Header + PARTITION_COUNT_OFFSET, TEST_ENTRIES);
    PUT_LONG (Header + PENTRY_SIZE_OFFSET, PARTITION_ENTRY_SIZE);
    memset (mDisk + EntryLba * BlockSize, 0, TEST_ENTRIES * PARTITION_ENTRY_SIZE);
    for (Slot = 0; Slot < 4; Slot++) {
      EFI_PARTITION_ENTRY Entry;
      UINTN NameIndex;
      UINTN EntryIndex = Slot % 2 == 0 ? 5 : 37;
      if (Slot >= 2) EntryIndex++;
      memset (&Entry, 0, sizeof (Entry));
      Entry.PartitionTypeGUID.Data1 = 11;
      Entry.UniquePartitionGUID.Data1 = (UINT32)Slot + 100;
      Entry.StartingLBA = First + 4 + Slot * 10;
      Entry.EndingLBA = Entry.StartingLBA + 4;
      Entry.Attributes = 0x0800000000000005ULL;
      if (Slot == 0) Entry.Attributes |= PART_ATT_SUCCESSFUL_VAL | PART_ATT_MAX_RETRY_COUNT_VAL;
      for (NameIndex = 0; mGuidNames[Slot][NameIndex] != 0; NameIndex++) Entry.PartitionName[NameIndex] = mGuidNames[Slot][NameIndex];
      memcpy (mDisk + EntryLba * BlockSize + EntryIndex * PARTITION_ENTRY_SIZE, &Entry, sizeof (Entry));
      PtnEntries[Slot].PartEntry = Entry; PtnEntries[Slot].lun = 4;
      if (Slot < 2) {
        mRecords[Slot] = Entry;
        Ptable[4].HandleInfoList[Slot].Handle = (EFI_HANDLE *)&mRecords[Slot];
      }
    }
    Recalculate (Header);
  }
  Ptable[4].MaxHandles = 2;
  memcpy (mOriginal, mDisk, mDiskSize);
  mWrites = mFlushes = mReads = mAllocateCalls = mReplies = 0;
  mFailWrite = mFailFlush = mCorruptWrite = mFailRead = mFailAllocation = 0;
  mPersistentFailure = mChangeMedia = FALSE;
  mUnlocked = mIdle = TRUE; mBootLun = 1; mMerge = SNAPSHOTTED; mRootCount = 1;
  mBootLunStatus = EFI_SUCCESS;
  mInfoPending = FALSE; mInfoCount = mInfoMode = mStalls = 0;
}

STATIC VOID Run (CONST CHAR8 *Argument, BOOLEAN Writing, BOOLEAN Success) {
  UINTN Replies = mReplies;
  mInfoCount = 0;
  if (Writing) mSet (Argument, NULL, 0); else mShow (Argument, NULL, 0);
  assert (mReplies == Replies + 1 && mAllocations == 0);
  if (mOkay != Success) fprintf (stderr, "unexpected response for %s: %s\n", Argument, mResponse);
  assert (mOkay == Success);
}
STATIC VOID Unchanged (VOID) {
  assert (mWrites == 0 && !memcmp (mDisk, mOriginal, mDiskSize));
}

STATIC VOID SetDiskAttributes (UINTN Slot, UINT64 Attributes) {
  UINTN Index;
  UINTN EntryIndex = Slot % 2 == 0 ? 5 : 37;
  if (Slot >= 2) EntryIndex++;
  for (Index = 0; Index < 2; Index++) {
    UINT8 *Header = mDisk + (Index == 0 ? 1 : TEST_BLOCKS - 1) * mMedia.BlockSize;
    UINT64 EntryLba = GET_LLWORD_FROM_BYTE (Header + PARTITION_ENTRIES_OFFSET);
    UINTN Offset = (UINTN)EntryLba * mMedia.BlockSize + EntryIndex * PARTITION_ENTRY_SIZE;
    PUT_LONG_LONG (mDisk + Offset + 48, Attributes);
    Recalculate (Header);
  }
  PtnEntries[Slot].PartEntry.Attributes = Attributes;
  if (Slot < 2) mRecords[Slot].Attributes = Attributes;
  memcpy (mOriginal, mDisk, mDiskSize);
}

STATIC VOID SetDiskType (UINTN Partition, CONST EFI_GUID *Type) {
  UINTN Index;
  UINTN EntryIndex = Partition % 2 == 0 ? 5 : 37;
  if (Partition >= 2) EntryIndex++;
  for (Index = 0; Index < 2; Index++) {
    UINT8 *Header = mDisk + (Index == 0 ? 1 : TEST_BLOCKS - 1) * mMedia.BlockSize;
    UINT64 EntryLba = GET_LLWORD_FROM_BYTE (Header + PARTITION_ENTRIES_OFFSET);
    memcpy (mDisk + EntryLba * mMedia.BlockSize + EntryIndex * PARTITION_ENTRY_SIZE, Type, sizeof (*Type));
    Recalculate (Header);
  }
  PtnEntries[Partition].PartEntry.PartitionTypeGUID = *Type;
  if (Partition < 2) mRecords[Partition].PartitionTypeGUID = *Type;
  memcpy (mOriginal, mDisk, mDiskSize);
}

STATIC VOID SetupAblLun (VOID) {
  STATIC CONST EFI_GUID Type = {0x12345678, 0x9abc, 0xdef0, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0}};
  UINTN Index;
  assert (mAblDisk == NULL);
  mAblDisk = malloc (mDiskSize);
  assert (mAblDisk != NULL);
  memcpy (mAblDisk, mDisk, mDiskSize);
  mAblMedia = mMedia; mAblMedia.MediaId++;
  mAblBlockIo = mBlockIo; mAblBlockIo.Media = &mAblMedia;
  PtnEntries[2].lun = PtnEntries[3].lun = 3;
  for (Index = 0; Index < 2; Index++) {
    UINT8 *Header = mAblDisk + (Index == 0 ? 1 : TEST_BLOCKS - 1) * mMedia.BlockSize;
    UINT64 EntryLba = GET_LLWORD_FROM_BYTE (Header + PARTITION_ENTRIES_OFFSET);
    memcpy (mAblDisk + EntryLba * mMedia.BlockSize + 6 * PARTITION_ENTRY_SIZE, &Type, sizeof (Type));
    RecalculateDisk (mAblDisk, mMedia.BlockSize, Header);
  }
}

STATIC VOID TestMissingBootBAttributes (VOID) {
  STATIC CONST EFI_GUID InactiveType = {0x77036cd4, 0x03d5, 0x42bb, {0x8e, 0xd1, 0x37, 0xe5, 0xa8, 0x8b, 0xaa, 0x34}};
  STATIC CONST EFI_GUID AblType = {0xbd6928a1, 0x4ce0, 0xa038, {0x4f, 0x3a, 0x14, 0x95, 0xe3, 0xed, 0xdf, 0xfb}};
  STATIC CONST UINT64 Expected = 0x003f000000000000ULL;
  struct PartitionEntry Cache[4];
  EFI_PARTITION_ENTRY Records[2];
  SLOT_GPT Gpt;
  UINTN Index;

  Setup (4096);
  SetDiskAttributes (0, 0x007b000000000000ULL);
  SetDiskAttributes (1, 0);
  SetDiskAttributes (2, 0x107b000000000000ULL);
  SetDiskAttributes (3, 0x103f000000000000ULL);
  SetDiskType (0, &InactiveType);
  SetDiskType (1, &gEfiBootImgPartitionGuid);
  SetDiskType (2, &InactiveType);
  SetDiskType (3, &AblType);
  mBootLun = 2;
  memcpy (Cache, PtnEntries, sizeof (Cache));
  memcpy (Records, mRecords, sizeof (Records));
  Run ("", FALSE, TRUE); Unchanged ();
  assert (!strcmp (mInfo[20], "standard boot type GUID: b"));
  assert (!strcmp (mInfo[21], "a boot/abl type GUIDs: same"));
  assert (!strcmp (mInfo[22], "b boot/abl type GUIDs: different"));
  Run (" b act=1 pri=3 ret=7 succ=0 unb=0 dry-run", TRUE, TRUE); Unchanged ();
  assert (strstr (mInfo[0], "0000000000000000 -> 003f000000000000"));
  assert (!memcmp (Cache, PtnEntries, sizeof (Cache)) && !memcmp (Records, mRecords, sizeof (Records)));
  Run (" b act=1 pri=3 ret=7 succ=0 unb=0", TRUE, TRUE);
  assert (mWrites == 4 && mFlushes == 2 && mBootLun == 2);
  assert (!EFI_ERROR (SlotOpen (&Gpt)));
  assert (SlotAttributes (&Gpt, 0) == Cache[0].PartEntry.Attributes && SlotAttributes (&Gpt, 1) == Expected);
  for (Index = 0; Index < 2; Index++) {
    SLOT_GPT_COPY *Copy = &Gpt.Copies[Index];
    UINTN Offset = (UINTN)Copy->EntriesLba * mMedia.BlockSize + Gpt.Offsets[1] + 48;
    UINT8 *Header = mOriginal + Copy->HeaderLba * mMedia.BlockSize;
    PUT_LONG_LONG (mOriginal + Offset, Expected);
    RecalculateDisk (mOriginal, mMedia.BlockSize, Header);
  }
  SlotClose (&Gpt);
  Cache[1].PartEntry.Attributes = Expected;
  Records[1].Attributes = Expected;
  assert (!memcmp (Cache, PtnEntries, sizeof (Cache)) && !memcmp (Records, mRecords, sizeof (Records)));
  assert (!memcmp (mDisk, mOriginal, mDiskSize));
  puts ("ok observed boot_b=0 repair: preview unchanged; only boot_b attributes and GPT CRCs written; A/ABL/GUID/LUN preserved");
}

int main (void) {
  STATIC CONST CHAR8 *Invalid[] = {"", " a", " a dry-run", " a retry=8", " a pri=-1",
    " a pri=4", " a active=2", " a ret=01", " a retry=1=2", " a ret=1 retry=2",
    " a unb=1 unbootable=0", " a typo=1", "a ret=7", " c ret=7",
    " a ret=7 dry-run dry-run", " a successful=4294967297", " a retry="};
  UINTN Index;
  UINT32 BlockSize;
  SLOT_GPT Gpt;
  SLOT_EDIT Edit;
  EFI_STATUS Status;
  BOOLEAN Restored;
  UINT64 Updated;

  GblSlotRegisterCommands ();
  assert (mSet && mShow);
  TestMissingBootBAttributes ();
  for (BlockSize = 512; BlockSize <= 4096; BlockSize *= 8) {
    Setup (BlockSize);
    Run ("", FALSE, TRUE);
    assert (mInfoCount == 23 && strstr (mInfo[0], "crc=ok") && strstr (mInfo[1], "boot-lun=1"));
    assert (strstr (mInfo[2], "act=0") && strstr (mInfo[3], "ret=0"));
    assert (!strcmp (mInfo[4], "boot_a lun=4 attrs=0878000000000005"));
    assert (!strcmp (mInfo[5], "boot_a type=0000000b-0000-0000-0000-000000000000"));
    assert (!strcmp (mInfo[6], "boot_a uuid=00000064-0000-0000-0000-000000000000"));
    assert (strstr (mInfo[13], "abl_a type=") && strstr (mInfo[20], "neither"));
    Run (" a", FALSE, TRUE); Run (" _b", FALSE, TRUE);
    assert (mInfoCount == 13 && strstr (mInfo[3], "boot_b lun=4") && strstr (mInfo[7], "abl_b lun=4"));
    Run (" a extra", FALSE, FALSE); Run ("x", FALSE, FALSE); Unchanged ();
    for (Index = 0; Index < ARRAY_SIZE (Invalid); Index++) {
      Run (Invalid[Index], TRUE, FALSE); Unchanged ();
    }
    Run (" a act=1 pri=3 ret=7 succ=1 unb=0 dry-run", TRUE, TRUE); Unchanged ();
    assert (mInfoCount == 1 && strstr (mInfo[0], "dry-run boot_a"));
    assert (!(PtnEntries[0].PartEntry.Attributes & PART_ATT_ACTIVE_VAL));
    Run (" a act=1 pri=3 ret=7 succ=1 unb=0", TRUE, TRUE);
    assert (mWrites == 4 && mFlushes == 2);
    assert (mWrittenLbas[1] == TEST_BLOCKS - 1 && mWrittenLbas[3] == 1);
    assert (PtnEntries[0].PartEntry.Attributes == (0x0800000000000005ULL | 0x007f000000000000ULL));
    assert (mRecords[0].Attributes == PtnEntries[0].PartEntry.Attributes);
    assert (!EFI_ERROR (SlotOpen (&Gpt)));
    assert (SlotAttributes (&Gpt, 1) == 0x0800000000000005ULL);
    for (Index = 0; Index < 2; Index++) {
      SLOT_GPT_COPY *Copy = &Gpt.Copies[Index];
      UINT8 *Header = mDisk + Copy->HeaderLba * BlockSize;
      UINT8 *OriginalHeader = mOriginal + Copy->HeaderLba * BlockSize;
      UINTN AttributeOffset = Copy->EntriesLba * BlockSize + Gpt.Offsets[0] + 48;
      UINT32 Crc;
      PUT_LONG_LONG (mOriginal + AttributeOffset, PtnEntries[0].PartEntry.Attributes);
      Crc32 (mOriginal + Copy->EntriesLba * BlockSize, Copy->EntryBytes, &Crc);
      PUT_LONG (OriginalHeader + PARTITION_CRC_OFFSET, Crc);
      PUT_LONG (OriginalHeader + HEADER_CRC_OFFSET, 0);
      Crc32 (OriginalHeader, GPT_HEADER_SIZE, &Crc);
      PUT_LONG (OriginalHeader + HEADER_CRC_OFFSET, Crc);
      assert (!memcmp (Header, OriginalHeader, BlockSize));
    }
    assert (!memcmp (mDisk, mOriginal, mDiskSize)); SlotClose (&Gpt);
    Run (" a act=1 pri=3 ret=7 succ=1 unb=0", TRUE, TRUE); assert (mWrites == 4);
    mWrites = 0;
    Run (" b act=1 pri=3 ret=7", TRUE, FALSE); Unchanged ();
    assert (strstr (mResponse, "peer boot_a is active"));
    Run (" a act=0", TRUE, FALSE); Unchanged ();
    Run (" a pri=0", TRUE, FALSE); Unchanged ();
    Run (" a succ=0 ret=0", TRUE, FALSE); Unchanged ();
    Run (" a unb=1", TRUE, FALSE); Unchanged ();
  }
  Setup (4096);
  SetDiskAttributes (0, 0x007b000000000000ULL);
  SetDiskAttributes (1, 0);
  SetDiskType (0, &gEfiBootImgPartitionGuid); mBootLun = 2;
  Run ("", FALSE, TRUE); Unchanged ();
  assert (!strcmp (mInfo[2], "a:007b000000000000 act=0 pri=3 ret=7 succ=1 unb=0"));
  assert (!strcmp (mInfo[3], "b:0000000000000000 act=0 pri=0 ret=0 succ=0 unb=0"));
  assert (!strcmp (mInfo[5], "boot_a type=20117f86-e985-4357-b9ee-374bc1d8487d"));
  assert (!strcmp (mInfo[20], "standard boot type GUID: a"));
  assert (!strcmp (mInfo[21], "a boot/abl type GUIDs: different"));
  assert (!strcmp (mInfo[22], "b boot/abl type GUIDs: same"));
  Setup (4096); SetDiskType (1, &gEfiBootImgPartitionGuid);
  Run ("", FALSE, TRUE); Unchanged ();
  assert (!strcmp (mInfo[20], "standard boot type GUID: b"));
  SetDiskType (0, &gEfiBootImgPartitionGuid);
  Run ("", FALSE, TRUE); Unchanged ();
  assert (!strcmp (mInfo[20], "standard boot type GUID: both"));
  Setup (4096); SetupAblLun ();
  Run ("", FALSE, TRUE); Unchanged ();
  assert (strstr (mInfo[12], "abl_a lun=3"));
  assert (!strcmp (mInfo[13], "abl_a type=12345678-9abc-def0-1234-56789abcdef0"));
  assert (PtnEntries[2].PartEntry.PartitionTypeGUID.Data1 == 11);
  mAblDisk[mMedia.BlockSize + 40] ^= 1;
  Run ("", FALSE, FALSE); Unchanged ();
  assert (strstr (mResponse, "abl_a GUID check"));
  for (Index = 7; Index <= 12; Index++) {
    Setup (4096); SetupAblLun (); mFailAllocation = Index;
    Run ("", FALSE, FALSE); Unchanged ();
    assert (strstr (mResponse, "abl_a GUID check"));
  }
  Setup (4096); PtnEntries[2].lun = MAX_LUNS;
  Run ("", FALSE, FALSE); Unchanged ();
  assert (strstr (mResponse, "abl_a GUID check"));
  Setup (4096); mBootLun = 2;
  Run (" a act=1 pri=3", TRUE, FALSE); Unchanged ();
  assert (strstr (mResponse, "boot-lun=2; target=a requires=1"));
  Setup (4096); mBootLunStatus = EFI_DEVICE_ERROR;
  Run (" a act=1 pri=3", TRUE, FALSE); Unchanged ();
  assert (strstr (mResponse, "boot-LUN read failed"));
  Run ("", FALSE, TRUE); assert (strstr (mInfo[1], "boot-LUN read failed"));
  Setup (4096); mBootLun = 2;
  Run ("", FALSE, TRUE); assert (strstr (mInfo[1], "boot-lun=2")); Unchanged ();
  for (Index = 1; Index <= 4; Index++) {
    Setup (4096); mInfoMode = Index;
    mShow ("", NULL, 0);
    assert (mReplies == 0 && mAllocations == 0);
    if (Index == 1) assert (mStalls == 3000);
    Unchanged ();
  }
  Setup (4096); mUnlocked = FALSE;
  Run (" a pri=3", TRUE, FALSE); Unchanged ();
  Setup (4096); mIdle = FALSE;
  Run (" a pri=3", TRUE, FALSE); Unchanged ();
  Setup (4096); mMerge = MERGING;
  Run (" a pri=3", TRUE, FALSE); Unchanged ();
  Setup (4096); mMedia.ReadOnly = TRUE;
  Run ("", FALSE, TRUE); Run (" a pri=3", TRUE, FALSE); Unchanged ();
  Setup (4096); mRootCount = 2;
  Run (" a pri=3", TRUE, FALSE); Unchanged ();
  Setup (4096);
  SetDiskAttributes (0, PART_ATT_UNBOOTABLE_VAL);
  SetDiskAttributes (1, PART_ATT_UNBOOTABLE_VAL | PART_ATT_MAX_RETRY_COUNT_VAL);
  Run (" a ret=7", TRUE, FALSE); Unchanged ();
  Run (" b unbootable=0", TRUE, TRUE);
  assert (PtnEntries[1].PartEntry.Attributes == PART_ATT_MAX_RETRY_COUNT_VAL);
  assert (PtnEntries[0].PartEntry.Attributes == PART_ATT_UNBOOTABLE_VAL);
  Setup (4096);
  PtnEntries[1].PartEntry.Attributes = PART_ATT_UNBOOTABLE_VAL;
  mRecords[1].Attributes = PART_ATT_UNBOOTABLE_VAL;
  Run (" a act=1 pri=3", TRUE, TRUE);
  assert (PtnEntries[1].PartEntry.Attributes == 0x0800000000000005ULL);
  assert (mRecords[1].Attributes == 0x0800000000000005ULL);
  Setup (4096); PtnEntries[0].PartEntry.StartingLBA++;
  Run (" a pri=3", TRUE, FALSE); Unchanged ();
  for (Index = 1; Index <= 6; Index++) {
    Setup (4096); mFailAllocation = Index;
    Run (" a pri=3", TRUE, FALSE); Unchanged ();
  }
  for (Index = 1; Index <= 14; Index++) {
    Setup (4096); mFailRead = Index;
    Run (" a pri=3", TRUE, FALSE);
    if (Index <= 8) Unchanged ();
    else {
      assert (strstr (mResponse, "rollback verified"));
      assert (!memcmp (mDisk, mOriginal, mDiskSize));
    }
  }
  for (Index = 1; Index <= 4; Index++) {
    Setup (4096); mFailWrite = Index;
    Run (" a act=1 pri=3", TRUE, FALSE);
    assert (strstr (mResponse, "rollback verified"));
    assert (!memcmp (mDisk, mOriginal, mDiskSize));
    assert (!(PtnEntries[0].PartEntry.Attributes & PART_ATT_ACTIVE_VAL));
    Setup (4096); mCorruptWrite = Index;
    Run (" a pri=3", TRUE, FALSE);
    assert (strstr (mResponse, "rollback verified"));
    assert (!memcmp (mDisk, mOriginal, mDiskSize));
  }
  for (Index = 1; Index <= 2; Index++) {
    Setup (4096); mFailFlush = Index;
    Run (" a pri=3", TRUE, FALSE);
    assert (strstr (mResponse, "rollback verified"));
    assert (!memcmp (mDisk, mOriginal, mDiskSize));
  }
  Setup (4096); mFailWrite = 1; mPersistentFailure = TRUE;
  Run (" a pri=3", TRUE, FALSE); assert (strstr (mResponse, "rollback FAILED"));
  Setup (4096); mChangeMedia = TRUE;
  Run (" a pri=3", TRUE, FALSE); assert (mWrites == 1 && strstr (mResponse, "rollback FAILED"));
  for (Index = 0; Index < 4; Index++) {
    Setup (4096);
    mDisk[(Index < 2 ? 1 : TEST_BLOCKS - 1) * 4096 + (Index % 2 ? 16 : 40)] ^= 1;
    memcpy (mOriginal, mDisk, mDiskSize);
    Run (" a pri=3", TRUE, FALSE); Unchanged ();
  }
  Setup (4096);
  mDisk[2 * 4096 + 700] ^= 1; memcpy (mOriginal, mDisk, mDiskSize);
  Run (" a pri=3", TRUE, FALSE); Unchanged ();
  Setup (4096);
  mDisk[(TEST_BLOCKS - 5) * 4096 + 700] ^= 1;
  Recalculate (mDisk + (TEST_BLOCKS - 1) * 4096);
  memcpy (mOriginal, mDisk, mDiskSize);
  Run (" a pri=3", TRUE, FALSE); Unchanged ();
  Setup (4096);
  assert (!EFI_ERROR (SlotOpen (&Gpt)));
  assert (!EFI_ERROR (SlotParse (" a pri=3", TRUE, &Edit)));
  Updated = (SlotAttributes (&Gpt, 0) & ~Edit.Mask) | Edit.Value;
  mDisk[2 * 4096 + 1000] ^= 1;
  Status = SlotCommit (&Gpt, 0, Updated, &Restored);
  assert (EFI_ERROR (Status) && mWrites == 0); SlotClose (&Gpt);
  Setup (4096); mFailRead = 3;
  Run (" a pri=3", TRUE, FALSE); Unchanged ();
  free (mDisk); free (mOriginal); free (mAblDisk);
  puts ("ok 104_slot_attrs: live multi-LUN GPT/GUID diagnostics, INFO/ACK sequencing, aligned write bounds, CRCs, guards, dry-run and rollback");
  return 0;
}
