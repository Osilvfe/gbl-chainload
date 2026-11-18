#include "SlotAttrs.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/Recovery.h>
#include <Protocol/EFICardInfo.h>
#include "../../../edk2/QcomModulePkg/Library/FastbootLib/FastbootMain.h"

VOID FastbootRegister (CONST CHAR8 *Prefix, VOID (*Handler)(CONST CHAR8 *, VOID *, UINT32));
VOID FastbootOkay (CONST CHAR8 *Response);
VOID FastbootFail (CONST CHAR8 *Response);
VOID FastbootInfo (CONST CHAR8 *Response);
BOOLEAN FlashComplete (VOID);
BOOLEAN IsDynamicPartitionSupport (VOID);

#define SLOT_GPT_LIMIT 32768U
#define SLOT_COMMAND_LIMIT 64U
#define SLOT_RESPONSE_SIZE 60U

extern EFI_GUID gEfiPartitionRecordGuid;

typedef struct {
  CONST CHAR8 *Name;
  CONST CHAR8 *Alias;
  UINT64 Mask;
  UINT32 Shift;
  UINT32 Maximum;
} SLOT_FIELD;

STATIC CONST SLOT_FIELD mSlotFields[] = {
  {"active", "act", PART_ATT_ACTIVE_VAL, PART_ATT_ACTIVE_BIT, 1},
  {"priority", "pri", PART_ATT_PRIORITY_VAL, PART_ATT_PRIORITY_BIT, 3},
  {"retry", "ret", PART_ATT_MAX_RETRY_COUNT_VAL, PART_ATT_MAX_RETRY_CNT_BIT, 7},
  {"successful", "succ", PART_ATT_SUCCESSFUL_VAL, PART_ATT_SUCCESS_BIT, 1},
  {"unbootable", "unb", PART_ATT_UNBOOTABLE_VAL, PART_ATT_UNBOOTABLE_BIT, 1}
};

typedef struct {
  UINTN Slot;
  UINT64 Mask;
  UINT64 Value;
  BOOLEAN DryRun;
} SLOT_EDIT;

typedef struct {
  VOID *Allocation;
  UINT8 *Bytes;
} SLOT_BUFFER;

typedef struct {
  EFI_LBA HeaderLba;
  EFI_LBA EntriesLba;
  UINTN EntryBytes;
  UINTN Length;
  SLOT_BUFFER Original;
  SLOT_BUFFER Updated;
  SLOT_BUFFER Readback;
  BOOLEAN Touched;
} SLOT_GPT_COPY;

typedef struct {
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  EFI_BLOCK_IO_MEDIA Media;
  SLOT_GPT_COPY Copies[2];
  UINTN Offsets[2];
  struct PartitionEntry *Cached[2];
  EFI_PARTITION_ENTRY *Records[2];
} SLOT_GPT;

typedef struct {
  EFI_PARTITION_ENTRY Entry;
  UINT32 Lun;
} SLOT_GUID_ENTRY;

STATIC CONST CHAR16 *mSlotNames[] = {L"boot_a", L"boot_b"};
STATIC CONST CHAR16 *mGuidNames[] = {L"boot_a", L"boot_b", L"abl_a", L"abl_b"};
STATIC CONST CHAR8 *mGuidLabels[] = {"boot_a", "boot_b", "abl_a", "abl_b"};

STATIC EFI_STATUS
SlotSendInfo (IN CONST CHAR8 *Text)
{
  FastbootDeviceData *Device = GetFastbootDeviceData ();
  EFI_USB_DEVICE_PROTOCOL *Usb;
  USB_DEVICE_EVENT Message;
  USB_DEVICE_EVENT_DATA Payload;
  UINTN PayloadSize;
  UINTN Attempt;
  EFI_STATUS Status;

  if (Device == NULL || Device->UsbDeviceProtocol == NULL) return EFI_NOT_READY;
  Usb = Device->UsbDeviceProtocol;
  if (Usb->HandleEvent == NULL) return EFI_UNSUPPORTED;
  FastbootInfo (Text);
  for (Attempt = 0; Attempt < 3000; Attempt++) {
    Message = UsbDeviceEventNoEvent;
    PayloadSize = sizeof (Payload);
    ZeroMem (&Payload, sizeof (Payload));
    Status = Usb->HandleEvent (&Message, &PayloadSize, &Payload);
    if (EFI_ERROR (Status) && Status != EFI_NOT_READY) return Status;
    if (Message == UsbDeviceEventDeviceStateChange &&
        Payload.DeviceState == UsbDeviceStateDisconnected) return EFI_NO_RESPONSE;
    if (Message == UsbDeviceEventTransferNotification &&
        Payload.TransferOutcome.EndpointIndex == 0x81) {
      return Payload.TransferOutcome.Status == UsbDeviceTransferStatusCompleteOK
               ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    }
    gBS->Stall (1000);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
SlotReadBootLun (OUT UINT32 *BootLun)
{
  CHAR8 DeviceType[BOOT_DEV_NAME_SIZE_MAX] = {0};
  PartiSelectFilter Filter;
  HandleInfo Handles[2];
  UINT32 Count = ARRAY_SIZE (Handles);
  EFI_MEM_CARDINFO_PROTOCOL *Card;
  EFI_STATUS Status;

  *BootLun = 0;
  GetRootDeviceType (DeviceType, sizeof (DeviceType));
  if (AsciiStrCmp (DeviceType, "UFS")) return EFI_UNSUPPORTED;
  ZeroMem (&Filter, sizeof (Filter));
  Filter.RootDeviceType = &gEfiUfsLU0Guid;
  Status = GetBlkIOHandles (BLK_IO_SEL_SELECT_ROOT_DEVICE_ONLY, &Filter, Handles, &Count);
  if (EFI_ERROR (Status)) return Status;
  if (Count != 1) return EFI_NO_MAPPING;
  Status = gBS->HandleProtocol (Handles[0].Handle, &gEfiMemCardInfoProtocolGuid, (VOID **)&Card);
  if (EFI_ERROR (Status)) return Status;
  if (Card == NULL || Card->Revision < EFI_MEM_CARD_INFO_PROTOCOL_REVISION_3 ||
      Card->GetBootLU == NULL) return EFI_UNSUPPORTED;
  return Card->GetBootLU (Card, BootLun);
}

STATIC EFI_STATUS
SlotNextToken (IN OUT CONST CHAR8 **Cursor, OUT CHAR8 *Token, IN UINTN Size)
{
  UINTN Length = 0;

  while (**Cursor == ' ') {
    (*Cursor)++;
  }
  if (**Cursor == 0) {
    return EFI_NOT_FOUND;
  }
  while (**Cursor != 0 && **Cursor != ' ') {
    if (Length + 1 >= Size) {
      return EFI_BAD_BUFFER_SIZE;
    }
    Token[Length++] = *(*Cursor)++;
  }
  Token[Length] = 0;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
SlotParse (IN CONST CHAR8 *Argument, IN BOOLEAN Writing, OUT SLOT_EDIT *Edit)
{
  CONST CHAR8 *Cursor = Argument;
  CHAR8 Token[24];
  CHAR8 *Equals;
  UINTN Index;
  UINT32 Value;
  EFI_STATUS Status;

  ZeroMem (Edit, sizeof (*Edit));
  Edit->Slot = 2;
  if (Argument == NULL || (*Argument != 0 && *Argument != ' ')) {
    return EFI_INVALID_PARAMETER;
  }
  for (Index = 0; Index < SLOT_COMMAND_LIMIT && Argument[Index] != 0; Index++) {}
  if (Index == SLOT_COMMAND_LIMIT) {
    return EFI_BAD_BUFFER_SIZE;
  }
  Status = SlotNextToken (&Cursor, Token, sizeof (Token));
  if (Status == EFI_NOT_FOUND && !Writing) {
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) {
    return EFI_INVALID_PARAMETER;
  }
  if (!AsciiStrCmp (Token, "a") || !AsciiStrCmp (Token, "_a")) {
    Edit->Slot = 0;
  } else if (!AsciiStrCmp (Token, "b") || !AsciiStrCmp (Token, "_b")) {
    Edit->Slot = 1;
  } else {
    return EFI_INVALID_PARAMETER;
  }
  while ((Status = SlotNextToken (&Cursor, Token, sizeof (Token))) != EFI_NOT_FOUND) {
    if (EFI_ERROR (Status) || !Writing) {
      return EFI_INVALID_PARAMETER;
    }
    if (!AsciiStrCmp (Token, "dry-run")) {
      if (Edit->DryRun) {
        return EFI_INVALID_PARAMETER;
      }
      Edit->DryRun = TRUE;
      continue;
    }
    Equals = AsciiStrStr (Token, "=");
    if (Equals == NULL || Equals[1] < '0' || Equals[1] > '7' || Equals[2] != 0) {
      return EFI_INVALID_PARAMETER;
    }
    *Equals = 0;
    Value = (UINT32)(Equals[1] - '0');
    for (Index = 0; Index < ARRAY_SIZE (mSlotFields); Index++) {
      CONST SLOT_FIELD *Field = &mSlotFields[Index];
      if (AsciiStrCmp (Token, Field->Name) && AsciiStrCmp (Token, Field->Alias)) {
        continue;
      }
      if ((Edit->Mask & Field->Mask) != 0 || Value > Field->Maximum) {
        return EFI_INVALID_PARAMETER;
      }
      Edit->Mask |= Field->Mask;
      Edit->Value |= (UINT64)Value << Field->Shift;
      break;
    }
    if (Index == ARRAY_SIZE (mSlotFields)) {
      return EFI_INVALID_PARAMETER;
    }
  }
  return Writing && Edit->Mask == 0 ? EFI_INVALID_PARAMETER : EFI_SUCCESS;
}

STATIC EFI_STATUS
SlotAllocate (OUT SLOT_BUFFER *Buffer, IN UINTN Size, IN UINTN Alignment)
{
  Buffer->Allocation = AllocateZeroPool (Size + Alignment - 1);
  if (Buffer->Allocation == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Buffer->Bytes = (UINT8 *)ALIGN_VALUE ((UINTN)Buffer->Allocation, Alignment);
  return EFI_SUCCESS;
}

STATIC VOID
SlotClose (IN OUT SLOT_GPT *Gpt)
{
  UINTN Index;

  for (Index = 0; Index < ARRAY_SIZE (Gpt->Copies); Index++) {
    SLOT_GPT_COPY *Copy = &Gpt->Copies[Index];
    if (Copy->Original.Allocation != NULL) FreePool (Copy->Original.Allocation);
    if (Copy->Updated.Allocation != NULL) FreePool (Copy->Updated.Allocation);
    if (Copy->Readback.Allocation != NULL) FreePool (Copy->Readback.Allocation);
  }
}

STATIC EFI_STATUS
SlotCheckMedia (IN CONST SLOT_GPT *Gpt)
{
  EFI_BLOCK_IO_MEDIA *Media = Gpt->BlockIo->Media;

  if (Media == NULL || !Media->MediaPresent) return EFI_NO_MEDIA;
  if (Media->MediaId != Gpt->Media.MediaId || Media->BlockSize != Gpt->Media.BlockSize ||
      Media->LastBlock != Gpt->Media.LastBlock || Media->IoAlign != Gpt->Media.IoAlign ||
      Media->LogicalPartition || Media->RemovableMedia || Media->ReadOnly != Gpt->Media.ReadOnly) {
    return EFI_MEDIA_CHANGED;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
SlotRead (IN SLOT_GPT *Gpt, IN EFI_LBA Lba, IN UINTN Size, OUT VOID *Buffer)
{
  EFI_STATUS Status = SlotCheckMedia (Gpt);

  if (EFI_ERROR (Status)) return Status;
  if (Lba > Gpt->Media.LastBlock || Size == 0 || Size % Gpt->Media.BlockSize != 0 ||
      Size / Gpt->Media.BlockSize - 1 > Gpt->Media.LastBlock - Lba) {
    return EFI_INVALID_PARAMETER;
  }
  Status = Gpt->BlockIo->ReadBlocks (Gpt->BlockIo, Gpt->Media.MediaId, Lba, Size, Buffer);
  return EFI_ERROR (Status) ? Status : SlotCheckMedia (Gpt);
}

STATIC EFI_STATUS
SlotLoadCopy (IN OUT SLOT_GPT *Gpt, IN UINTN Index)
{
  SLOT_GPT_COPY *Copy = &Gpt->Copies[Index];
  UINT32 BlockSize = Gpt->Media.BlockSize;
  UINTN Capacity = BlockSize + ALIGN_VALUE (SLOT_GPT_LIMIT, BlockSize);
  UINTN Alignment = MAX (Gpt->Media.IoAlign, 1U);
  UINT8 *Header;
  UINT32 HeaderSize;
  UINT32 Crc;
  UINT32 StoredCrc;
  UINT32 Count;
  UINT64 First;
  UINT64 Last;
  UINTN EntryBlocks;
  EFI_STATUS Status;

  Status = SlotAllocate (&Copy->Original, Capacity, Alignment);
  if (EFI_ERROR (Status)) return Status;
  Status = SlotAllocate (&Copy->Updated, Capacity, Alignment);
  if (EFI_ERROR (Status)) return Status;
  Status = SlotAllocate (&Copy->Readback, Capacity, Alignment);
  if (EFI_ERROR (Status)) return Status;
  Copy->HeaderLba = Index == 0 ? 1 : Gpt->Media.LastBlock;
  Header = Copy->Original.Bytes;
  Status = SlotRead (Gpt, Copy->HeaderLba, BlockSize, Header);
  if (EFI_ERROR (Status)) return Status;
  HeaderSize = GET_LWORD_FROM_BYTE (Header + HEADER_SIZE_OFFSET);
  if (CompareMem (Header, "EFI PART", 8) ||
      GET_LWORD_FROM_BYTE (Header + 8) != 0x00010000 ||
      HeaderSize < GPT_HEADER_SIZE || HeaderSize > BlockSize ||
      GET_LWORD_FROM_BYTE (Header + 20) != 0 ||
      GET_LLWORD_FROM_BYTE (Header + PRIMARY_HEADER_OFFSET) != Copy->HeaderLba ||
      GET_LLWORD_FROM_BYTE (Header + BACKUP_HEADER_OFFSET) !=
        (Index == 0 ? Gpt->Media.LastBlock : 1)) return EFI_VOLUME_CORRUPTED;
  StoredCrc = GET_LWORD_FROM_BYTE (Header + HEADER_CRC_OFFSET);
  PUT_LONG (Header + HEADER_CRC_OFFSET, 0);
  Status = gBS->CalculateCrc32 (Header, HeaderSize, &Crc);
  PUT_LONG (Header + HEADER_CRC_OFFSET, StoredCrc);
  if (EFI_ERROR (Status) || Crc != StoredCrc) return EFI_CRC_ERROR;
  Count = GET_LWORD_FROM_BYTE (Header + PARTITION_COUNT_OFFSET);
  if (Count == 0 || Count > SLOT_GPT_LIMIT / PARTITION_ENTRY_SIZE ||
      GET_LWORD_FROM_BYTE (Header + PENTRY_SIZE_OFFSET) != PARTITION_ENTRY_SIZE) {
    return EFI_UNSUPPORTED;
  }
  Copy->EntryBytes = Count * PARTITION_ENTRY_SIZE;
  EntryBlocks = (Copy->EntryBytes + BlockSize - 1) / BlockSize;
  Copy->Length = BlockSize + EntryBlocks * BlockSize;
  Copy->EntriesLba = GET_LLWORD_FROM_BYTE (Header + PARTITION_ENTRIES_OFFSET);
  First = GET_LLWORD_FROM_BYTE (Header + FIRST_USABLE_LBA_OFFSET);
  Last = GET_LLWORD_FROM_BYTE (Header + LAST_USABLE_LBA_OFFSET);
  if (First < 2 || First > Last || Last >= Gpt->Media.LastBlock ||
      Copy->EntriesLba < 2 || Copy->EntriesLba >= Gpt->Media.LastBlock ||
      EntryBlocks > Gpt->Media.LastBlock - Copy->EntriesLba ||
      (Index == 0 && Copy->EntriesLba + EntryBlocks > First) ||
      (Index == 1 && Copy->EntriesLba <= Last)) return EFI_VOLUME_CORRUPTED;
  Status = SlotRead (Gpt, Copy->EntriesLba, EntryBlocks * BlockSize, Header + BlockSize);
  if (EFI_ERROR (Status)) return Status;
  Status = gBS->CalculateCrc32 (Header + BlockSize, Copy->EntryBytes, &Crc);
  if (EFI_ERROR (Status) || Crc != GET_LWORD_FROM_BYTE (Header + PARTITION_CRC_OFFSET)) {
    return EFI_CRC_ERROR;
  }
  CopyMem (Copy->Updated.Bytes, Header, Copy->Length);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
SlotLoadLun (IN OUT SLOT_GPT *Gpt, IN UINT32 Lun)
{
  EFI_GUID *UfsGuids[] = {&gEfiUfsLU0Guid, &gEfiUfsLU1Guid, &gEfiUfsLU2Guid,
    &gEfiUfsLU3Guid, &gEfiUfsLU4Guid, &gEfiUfsLU5Guid, &gEfiUfsLU6Guid, &gEfiUfsLU7Guid};
  HandleInfo Handles[2];
  PartiSelectFilter Filter;
  CHAR8 DeviceType[BOOT_DEV_NAME_SIZE_MAX] = {0};
  UINT32 Count = ARRAY_SIZE (Handles);
  UINTN Index;
  EFI_STATUS Status;
  UINT8 *Primary;
  UINT8 *Backup;
  UINT32 BlockSize;

  if (Lun >= MAX_LUNS) return EFI_UNSUPPORTED;
  ZeroMem (&Filter, sizeof (Filter));
  GetRootDeviceType (DeviceType, sizeof (DeviceType));
  if (!AsciiStrCmp (DeviceType, "UFS")) Filter.RootDeviceType = UfsGuids[Lun];
  else if (!AsciiStrCmp (DeviceType, "EMMC")) Filter.RootDeviceType = &gEfiEmmcUserPartitionGuid;
  else if (!AsciiStrCmp (DeviceType, "NVME")) Filter.RootDeviceType = &gEfiNvme0Guid;
  else return EFI_UNSUPPORTED;
  Status = GetBlkIOHandles (BLK_IO_SEL_SELECT_ROOT_DEVICE_ONLY, &Filter, Handles, &Count);
  if (EFI_ERROR (Status)) return Status;
  if (Count != 1 || Handles[0].BlkIo == NULL || Handles[0].BlkIo->Media == NULL) return EFI_NO_MAPPING;
  Gpt->BlockIo = Handles[0].BlkIo;
  Gpt->Media = *Gpt->BlockIo->Media;
  BlockSize = Gpt->Media.BlockSize;
  if (BlockSize < 512 || BlockSize > 65536 || (BlockSize & (BlockSize - 1)) ||
      Gpt->Media.IoAlign > BlockSize ||
      (Gpt->Media.IoAlign > 1 && (Gpt->Media.IoAlign & (Gpt->Media.IoAlign - 1))) ||
      Gpt->Media.LastBlock < 8 || Gpt->BlockIo->ReadBlocks == NULL) return EFI_UNSUPPORTED;
  for (Index = 0; Index < 2; Index++) {
    Status = SlotLoadCopy (Gpt, Index);
    if (EFI_ERROR (Status)) return Status;
  }
  Primary = Gpt->Copies[0].Original.Bytes;
  Backup = Gpt->Copies[1].Original.Bytes;
  if (Gpt->Copies[0].EntryBytes != Gpt->Copies[1].EntryBytes ||
      GET_LWORD_FROM_BYTE (Primary + HEADER_SIZE_OFFSET) != GET_LWORD_FROM_BYTE (Backup + HEADER_SIZE_OFFSET) ||
      CompareMem (Primary + FIRST_USABLE_LBA_OFFSET, Backup + FIRST_USABLE_LBA_OFFSET, 32) ||
      CompareMem (Primary + BlockSize, Backup + BlockSize, Gpt->Copies[0].EntryBytes)) {
    return EFI_VOLUME_CORRUPTED;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
SlotOpen (OUT SLOT_GPT *Gpt)
{
  STATIC CONST EFI_GUID EmptyGuid = {0};
  UINTN Slot;
  UINTN Index;
  UINT32 Lun;
  INT32 CacheIndex;
  UINT8 *Primary;
  UINT32 BlockSize;
  EFI_STATUS Status;

  ZeroMem (Gpt, sizeof (*Gpt));
  for (Slot = 0; Slot < 2; Slot++) {
    CacheIndex = GetPartitionIndex ((CHAR16 *)mSlotNames[Slot]);
    if (CacheIndex < 0 || CacheIndex >= MAX_NUM_PARTITIONS) return EFI_NOT_FOUND;
    Gpt->Cached[Slot] = &PtnEntries[CacheIndex];
  }
  Lun = Gpt->Cached[0]->lun;
  if (Lun != Gpt->Cached[1]->lun) return EFI_UNSUPPORTED;
  Status = SlotLoadLun (Gpt, Lun);
  if (EFI_ERROR (Status)) return Status;
  Primary = Gpt->Copies[0].Original.Bytes;
  BlockSize = Gpt->Media.BlockSize;
  for (Slot = 0; Slot < 2; Slot++) {
    UINTN Matches = 0;
    EFI_PARTITION_ENTRY *DiskEntry = NULL;
    for (Index = 0; Index < Gpt->Copies[0].EntryBytes; Index += PARTITION_ENTRY_SIZE) {
      EFI_PARTITION_ENTRY *Entry = (EFI_PARTITION_ENTRY *)(Primary + BlockSize + Index);
      if (!StrnCmp (Entry->PartitionName, mSlotNames[Slot], 7)) {
        DiskEntry = Entry;
        Gpt->Offsets[Slot] = Index;
        Matches++;
      }
    }
    if (Matches != 1) return EFI_NO_MAPPING;
    if (!CompareMem (&DiskEntry->PartitionTypeGUID, &EmptyGuid, sizeof (EmptyGuid)) ||
        !CompareMem (&DiskEntry->UniquePartitionGUID, &EmptyGuid, sizeof (EmptyGuid)) ||
        CompareMem (DiskEntry, &Gpt->Cached[Slot]->PartEntry, 48) ||
        CompareMem ((UINT8 *)DiskEntry + 56, (UINT8 *)&Gpt->Cached[Slot]->PartEntry + 56, 72) ||
        DiskEntry->StartingLBA > DiskEntry->EndingLBA ||
        DiskEntry->StartingLBA < GET_LLWORD_FROM_BYTE (Primary + FIRST_USABLE_LBA_OFFSET) ||
        DiskEntry->EndingLBA > GET_LLWORD_FROM_BYTE (Primary + LAST_USABLE_LBA_OFFSET)) return EFI_VOLUME_CORRUPTED;
    Matches = 0;
    if (Ptable[Lun].MaxHandles > MAX_NUM_PARTITIONS) return EFI_VOLUME_CORRUPTED;
    for (Index = 0; Index < Ptable[Lun].MaxHandles; Index++) {
      EFI_PARTITION_ENTRY *Record;
      Status = gBS->HandleProtocol (Ptable[Lun].HandleInfoList[Index].Handle,
                                    &gEfiPartitionRecordGuid, (VOID **)&Record);
      if (EFI_ERROR (Status) || Record == NULL) continue;
      if (!CompareMem (Record, DiskEntry, 48) &&
          !CompareMem ((UINT8 *)Record + 56, (UINT8 *)DiskEntry + 56, 72)) {
        Gpt->Records[Slot] = Record;
        Matches++;
      }
    }
    if (Matches != 1) return EFI_NO_MAPPING;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
SlotCollectGuids (IN SLOT_GPT *BootGpt, OUT SLOT_GUID_ENTRY *Entries, OUT UINTN *Problem)
{
  SLOT_GPT Extra = {0};
  SLOT_GPT *Source;
  UINT32 ExtraLun = MAX_LUNS;
  UINT32 Lun;
  INT32 CacheIndex;
  UINTN Index;
  UINTN Offset;
  UINTN Matches;
  EFI_STATUS Status = EFI_SUCCESS;

  for (Index = 0; Index < ARRAY_SIZE (mGuidNames); Index++) {
    *Problem = Index;
    CacheIndex = GetPartitionIndex ((CHAR16 *)mGuidNames[Index]);
    if (CacheIndex < 0 || CacheIndex >= MAX_NUM_PARTITIONS) {
      Status = EFI_NOT_FOUND;
      break;
    }
    Lun = PtnEntries[CacheIndex].lun;
    if (Lun >= MAX_LUNS) {
      Status = EFI_UNSUPPORTED;
      break;
    }
    if (Lun == BootGpt->Cached[0]->lun) {
      Source = BootGpt;
    } else {
      if (Lun != ExtraLun) {
        SlotClose (&Extra);
        ZeroMem (&Extra, sizeof (Extra));
        Status = SlotLoadLun (&Extra, Lun);
        if (EFI_ERROR (Status)) break;
        ExtraLun = Lun;
      }
      Source = &Extra;
    }
    Entries[Index].Lun = Lun;
    Matches = 0;
    for (Offset = 0; Offset < Source->Copies[0].EntryBytes; Offset += PARTITION_ENTRY_SIZE) {
      EFI_PARTITION_ENTRY *Entry = (EFI_PARTITION_ENTRY *)(Source->Copies[0].Original.Bytes +
                                                        Source->Media.BlockSize + Offset);
      if (!StrnCmp (Entry->PartitionName, mGuidNames[Index], ARRAY_SIZE (Entry->PartitionName))) {
        CopyMem (&Entries[Index].Entry, Entry, sizeof (*Entry));
        Matches++;
      }
    }
    if (Matches != 1) {
      Status = Matches == 0 ? EFI_NOT_FOUND : EFI_NO_MAPPING;
      break;
    }
  }
  SlotClose (&Extra);
  return Status;
}

STATIC VOID
SlotGuidText (OUT CHAR8 *Text, IN CONST EFI_GUID *Guid)
{
  AsciiSPrint (Text, 37, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
               Guid->Data1, Guid->Data2, Guid->Data3,
               Guid->Data4[0], Guid->Data4[1], Guid->Data4[2], Guid->Data4[3],
               Guid->Data4[4], Guid->Data4[5], Guid->Data4[6], Guid->Data4[7]);
}

STATIC EFI_STATUS
SlotShowGuids (IN CONST SLOT_GUID_ENTRY *Entries, IN UINTN SelectedSlot)
{
  CHAR8 Response[SLOT_RESPONSE_SIZE];
  CHAR8 Guid[37];
  UINTN Index;
  BOOLEAN BootA;
  BOOLEAN BootB;
  CONST CHAR8 *Mapping;

  for (Index = 0; Index < ARRAY_SIZE (mGuidNames); Index++) {
    CONST SLOT_GUID_ENTRY *Entry = &Entries[Index];
    if (SelectedSlot < 2 && SelectedSlot != Index % 2) continue;
    AsciiSPrint (Response, sizeof (Response), "%a lun=%u attrs=%016lx",
                 mGuidLabels[Index], Entry->Lun, Entry->Entry.Attributes);
    if (EFI_ERROR (SlotSendInfo (Response))) return EFI_DEVICE_ERROR;
    SlotGuidText (Guid, &Entry->Entry.PartitionTypeGUID);
    AsciiSPrint (Response, sizeof (Response), "%a type=%a", mGuidLabels[Index], Guid);
    if (EFI_ERROR (SlotSendInfo (Response))) return EFI_DEVICE_ERROR;
    SlotGuidText (Guid, &Entry->Entry.UniquePartitionGUID);
    AsciiSPrint (Response, sizeof (Response), "%a uuid=%a", mGuidLabels[Index], Guid);
    if (EFI_ERROR (SlotSendInfo (Response))) return EFI_DEVICE_ERROR;
    AsciiSPrint (Response, sizeof (Response), "%a lba=%lx..%lx", mGuidLabels[Index],
                 Entry->Entry.StartingLBA, Entry->Entry.EndingLBA);
    if (EFI_ERROR (SlotSendInfo (Response))) return EFI_DEVICE_ERROR;
  }
  BootA = CompareMem (&Entries[0].Entry.PartitionTypeGUID, &gEfiBootImgPartitionGuid, sizeof (EFI_GUID)) == 0;
  BootB = CompareMem (&Entries[1].Entry.PartitionTypeGUID, &gEfiBootImgPartitionGuid, sizeof (EFI_GUID)) == 0;
  Mapping = BootA ? (BootB ? "both" : "a") : (BootB ? "b" : "neither");
  AsciiSPrint (Response, sizeof (Response), "standard boot type GUID: %a", Mapping);
  if (EFI_ERROR (SlotSendInfo (Response))) return EFI_DEVICE_ERROR;
  for (Index = 0; Index < 2; Index++) {
    if (SelectedSlot < 2 && SelectedSlot != Index) continue;
    AsciiSPrint (Response, sizeof (Response), "%c boot/abl type GUIDs: %a", (CHAR8)('a' + Index),
                 CompareMem (&Entries[Index].Entry.PartitionTypeGUID,
                   &Entries[Index + 2].Entry.PartitionTypeGUID, sizeof (EFI_GUID)) ? "different" : "same");
    if (EFI_ERROR (SlotSendInfo (Response))) return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

STATIC UINT64
SlotAttributes (IN CONST SLOT_GPT *Gpt, IN UINTN Slot)
{
  return GET_LLWORD_FROM_BYTE (Gpt->Copies[0].Original.Bytes + Gpt->Media.BlockSize + Gpt->Offsets[Slot] + 48);
}

STATIC EFI_STATUS
SlotValidateEdit (IN CONST SLOT_EDIT *Edit, IN UINT64 Original, IN UINT64 Peer,
                  OUT CHAR8 *Reason)
{
  UINT64 Updated = (Original & ~Edit->Mask) | Edit->Value;
  UINT32 BootLun = 0;
  CHAR8 DeviceType[BOOT_DEV_NAME_SIZE_MAX] = {0};
  EFI_STATUS Status;
  CONST CHAR8 *Failure = NULL;

  if ((Updated & PART_ATT_ACTIVE_VAL) && (Peer & PART_ATT_ACTIVE_VAL)) {
    AsciiSPrint (Reason, SLOT_RESPONSE_SIZE, "slot-set: peer boot_%c is active; no writes",
                 (CHAR8)('a' + 1 - Edit->Slot));
    return EFI_ACCESS_DENIED;
  }
  if ((Updated & PART_ATT_UNBOOTABLE_VAL) && (Peer & PART_ATT_UNBOOTABLE_VAL))
    Failure = "both slots would be unbootable";
  else if ((Edit->Mask & PART_ATT_ACTIVE_VAL) && !(Updated & PART_ATT_ACTIVE_VAL) &&
           !(Peer & PART_ATT_ACTIVE_VAL)) Failure = "would clear last active slot";
  else if ((Updated & PART_ATT_ACTIVE_VAL) && (Updated & PART_ATT_UNBOOTABLE_VAL))
    Failure = "active slot has unbootable=1";
  else if ((Updated & PART_ATT_ACTIVE_VAL) && !(Updated & PART_ATT_PRIORITY_VAL))
    Failure = "active slot has priority=0";
  else if ((Updated & PART_ATT_ACTIVE_VAL) &&
           !(Updated & (PART_ATT_SUCCESSFUL_VAL | PART_ATT_MAX_RETRY_COUNT_VAL)))
    Failure = "active slot needs successful=1 or retry>0";
  else if ((Updated & PART_ATT_UNBOOTABLE_VAL) && (Updated & PART_ATT_SUCCESSFUL_VAL))
    Failure = "unbootable=1 conflicts with successful=1";
  if (Failure != NULL) {
    AsciiSPrint (Reason, SLOT_RESPONSE_SIZE, "slot-set: %a", Failure);
    return EFI_ACCESS_DENIED;
  }
  if ((Edit->Mask & Edit->Value & PART_ATT_ACTIVE_VAL) != 0) {
    GetRootDeviceType (DeviceType, sizeof (DeviceType));
    if (!AsciiStrCmp (DeviceType, "UFS")) {
      Status = SlotReadBootLun (&BootLun);
      if (EFI_ERROR (Status)) {
        AsciiSPrint (Reason, SLOT_RESPONSE_SIZE, "slot-set: boot-LUN read failed: %r", Status);
        return Status;
      }
      if (BootLun != Edit->Slot + 1) {
        AsciiSPrint (Reason, SLOT_RESPONSE_SIZE, "slot-set: boot-lun=%u; target=%c requires=%u",
                     BootLun, (CHAR8)('a' + Edit->Slot), (UINT32)(Edit->Slot + 1));
        return EFI_ACCESS_DENIED;
      }
    }
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
SlotVerifyCopy (IN OUT SLOT_GPT *Gpt, IN UINTN Index, IN CONST UINT8 *Expected)
{
  SLOT_GPT_COPY *Copy = &Gpt->Copies[Index];
  EFI_STATUS Status;
  UINT32 BlockSize = Gpt->Media.BlockSize;

  Status = SlotRead (Gpt, Copy->HeaderLba, BlockSize, Copy->Readback.Bytes);
  if (EFI_ERROR (Status)) return Status;
  Status = SlotRead (Gpt, Copy->EntriesLba, Copy->Length - BlockSize, Copy->Readback.Bytes + BlockSize);
  if (EFI_ERROR (Status)) return Status;
  return CompareMem (Copy->Readback.Bytes, Expected, Copy->Length) ? EFI_CRC_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS
SlotPrepareEdit (IN OUT SLOT_GPT *Gpt, IN UINTN Slot, IN UINT64 Attributes)
{
  UINTN Index;
  UINT32 Crc;
  EFI_STATUS Status;

  for (Index = 0; Index < 2; Index++) {
    SLOT_GPT_COPY *Copy = &Gpt->Copies[Index];
    UINT8 *Header = Copy->Updated.Bytes;
    PUT_LONG_LONG (Header + Gpt->Media.BlockSize + Gpt->Offsets[Slot] + 48, Attributes);
    Status = gBS->CalculateCrc32 (Header + Gpt->Media.BlockSize, Copy->EntryBytes, &Crc);
    if (EFI_ERROR (Status)) return Status;
    PUT_LONG (Header + PARTITION_CRC_OFFSET, Crc);
    PUT_LONG (Header + HEADER_CRC_OFFSET, 0);
    Status = gBS->CalculateCrc32 (Header, GET_LWORD_FROM_BYTE (Header + HEADER_SIZE_OFFSET), &Crc);
    if (EFI_ERROR (Status)) return Status;
    PUT_LONG (Header + HEADER_CRC_OFFSET, Crc);
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
SlotWriteCopy (IN OUT SLOT_GPT *Gpt, IN UINTN Index, IN UINTN Slot, IN UINT8 *Bytes)
{
  SLOT_GPT_COPY *Copy = &Gpt->Copies[Index];
  UINT32 BlockSize = Gpt->Media.BlockSize;
  UINTN EntryBlock = Gpt->Offsets[Slot] / BlockSize;
  EFI_STATUS Status;

  Status = SlotCheckMedia (Gpt);
  if (EFI_ERROR (Status)) return Status;
  Copy->Touched = TRUE;
  Status = Gpt->BlockIo->WriteBlocks (Gpt->BlockIo, Gpt->Media.MediaId,
               Copy->EntriesLba + EntryBlock, BlockSize, Bytes + BlockSize + EntryBlock * BlockSize);
  if (EFI_ERROR (Status)) return Status;
  Status = SlotCheckMedia (Gpt);
  if (EFI_ERROR (Status)) return Status;
  Status = Gpt->BlockIo->WriteBlocks (Gpt->BlockIo, Gpt->Media.MediaId,
                                     Copy->HeaderLba, BlockSize, Bytes);
  if (EFI_ERROR (Status)) return Status;
  Status = SlotCheckMedia (Gpt);
  if (EFI_ERROR (Status)) return Status;
  Status = Gpt->BlockIo->FlushBlocks (Gpt->BlockIo);
  if (EFI_ERROR (Status)) return Status;
  return SlotVerifyCopy (Gpt, Index, Bytes);
}

STATIC EFI_STATUS
SlotCommit (IN OUT SLOT_GPT *Gpt, IN UINTN Slot, IN UINT64 Attributes, OUT BOOLEAN *Restored)
{
  EFI_STATUS Status;
  EFI_STATUS RecoveryStatus;
  UINTN Index;

  *Restored = FALSE;
  if (Gpt->Media.ReadOnly || Gpt->BlockIo->WriteBlocks == NULL ||
      Gpt->BlockIo->FlushBlocks == NULL) return EFI_WRITE_PROTECTED;
  Status = SlotPrepareEdit (Gpt, Slot, Attributes);
  if (EFI_ERROR (Status)) return Status;
  for (Index = 0; Index < 2; Index++) {
    Status = SlotVerifyCopy (Gpt, Index, Gpt->Copies[Index].Original.Bytes);
    if (EFI_ERROR (Status)) return Status;
  }
  Status = SlotWriteCopy (Gpt, 1, Slot, Gpt->Copies[1].Updated.Bytes);
  if (!EFI_ERROR (Status)) {
    Status = SlotWriteCopy (Gpt, 0, Slot, Gpt->Copies[0].Updated.Bytes);
  }
  if (!EFI_ERROR (Status)) {
    Status = SlotVerifyCopy (Gpt, 1, Gpt->Copies[1].Updated.Bytes);
  }
  if (EFI_ERROR (Status)) {
    *Restored = TRUE;
    for (Index = 0; Index < 2; Index++) {
      if (Gpt->Copies[Index].Touched) {
        RecoveryStatus = SlotWriteCopy (Gpt, Index, Slot, Gpt->Copies[Index].Original.Bytes);
      } else {
        RecoveryStatus = SlotVerifyCopy (Gpt, Index, Gpt->Copies[Index].Original.Bytes);
      }
      if (EFI_ERROR (RecoveryStatus)) *Restored = FALSE;
    }
    return Status;
  }
  for (Index = 0; Index < 2; Index++) {
    UINT64 Current = Index == Slot ? Attributes : SlotAttributes (Gpt, Index);
    Gpt->Cached[Index]->PartEntry.Attributes = Current;
    Gpt->Records[Index]->Attributes = Current;
  }
  return EFI_SUCCESS;
}

STATIC VOID
CmdSlotShow (IN CONST CHAR8 *Argument, IN VOID *Data, IN UINT32 Size)
{
  SLOT_EDIT Edit;
  SLOT_GPT Gpt;
  EFI_STATUS Status;
  CHAR8 Response[SLOT_RESPONSE_SIZE];
  UINT64 Attributes[2];
  UINT32 BootLun;
  UINTN Slot;
  SLOT_GUID_ENTRY GuidEntries[4];
  UINTN Problem;

  (VOID)Data;
  (VOID)Size;
  Status = SlotParse (Argument, FALSE, &Edit);
  if (EFI_ERROR (Status)) {
    FastbootFail ("usage: oem slot-show [a|b]");
    return;
  }
  if (!FlashComplete ()) {
    FastbootFail ("slot-show: wait for flash to finish");
    return;
  }
  Status = SlotOpen (&Gpt);
  if (EFI_ERROR (Status)) {
    AsciiSPrint (Response, sizeof (Response), "slot-show: GPT check failed: %r", Status);
    SlotClose (&Gpt);
    FastbootFail (Response);
    return;
  }
  Attributes[0] = SlotAttributes (&Gpt, 0);
  Attributes[1] = SlotAttributes (&Gpt, 1);
  Status = SlotCollectGuids (&Gpt, GuidEntries, &Problem);
  SlotClose (&Gpt);
  if (EFI_ERROR (Status)) {
    AsciiSPrint (Response, sizeof (Response), "slot-show: %a GUID check: %r", mGuidLabels[Problem], Status);
    FastbootFail (Response);
    return;
  }
  if (EFI_ERROR (SlotSendInfo ("GPT primary/backup: crc=ok, entries=identical"))) return;
  Status = SlotReadBootLun (&BootLun);
  if (EFI_ERROR (Status)) {
    AsciiSPrint (Response, sizeof (Response), "UFS boot-LUN read failed: %r", Status);
  } else {
    AsciiSPrint (Response, sizeof (Response), "UFS boot-lun=%u (a=1 b=2)", BootLun);
  }
  if (EFI_ERROR (SlotSendInfo (Response))) return;
  for (Slot = 0; Slot < 2; Slot++) {
    UINT64 Attribute = Attributes[Slot];
    if (Edit.Slot < 2 && Edit.Slot != Slot) continue;
    AsciiSPrint (Response, sizeof (Response), "%c:%016lx act=%d pri=%d ret=%d succ=%d unb=%d",
      (CHAR8)('a' + Slot), Attribute, (Attribute & PART_ATT_ACTIVE_VAL) != 0,
      (UINT32)((Attribute & PART_ATT_PRIORITY_VAL) >> PART_ATT_PRIORITY_BIT),
      (UINT32)((Attribute & PART_ATT_MAX_RETRY_COUNT_VAL) >> PART_ATT_MAX_RETRY_CNT_BIT),
      (Attribute & PART_ATT_SUCCESSFUL_VAL) != 0, (Attribute & PART_ATT_UNBOOTABLE_VAL) != 0);
    if (EFI_ERROR (SlotSendInfo (Response))) return;
  }
  if (!EFI_ERROR (SlotShowGuids (GuidEntries, Edit.Slot))) FastbootOkay ("");
}

STATIC VOID
CmdSlotSet (IN CONST CHAR8 *Argument, IN VOID *Data, IN UINT32 Size)
{
  SLOT_EDIT Edit;
  SLOT_GPT Gpt;
  EFI_STATUS Status;
  UINT64 Original;
  UINT64 Updated;
  BOOLEAN Restored = FALSE;
  BOOLEAN Attempted = FALSE;
  CHAR8 Response[SLOT_RESPONSE_SIZE];

  (VOID)Data;
  (VOID)Size;
  Status = SlotParse (Argument, TRUE, &Edit);
  if (EFI_ERROR (Status)) {
    FastbootFail ("usage: oem slot-set a act=1 pri=3 ret=7 succ=1 unb=0");
    return;
  }
  if (!IsUnlocked () || !FlashComplete ()) {
    FastbootFail ("slot-set: requires unlocked device and idle flash");
    return;
  }
  if (IsDynamicPartitionSupport () && GetSnapshotMergeStatus () == MERGING) {
    FastbootFail ("slot-set: snapshot merge in progress");
    return;
  }
  Status = SlotOpen (&Gpt);
  if (EFI_ERROR (Status)) {
    AsciiSPrint (Response, sizeof (Response), "slot-set: GPT check failed: %r", Status);
    goto Done;
  }
  Original = SlotAttributes (&Gpt, Edit.Slot);
  Updated = (Original & ~Edit.Mask) | Edit.Value;
  Status = SlotValidateEdit (&Edit, Original, SlotAttributes (&Gpt, 1 - Edit.Slot), Response);
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  if (!Edit.DryRun && Updated != Original) {
    Attempted = TRUE;
    Status = SlotCommit (&Gpt, Edit.Slot, Updated, &Restored);
    if (EFI_ERROR (Status)) {
      if (!Gpt.Copies[0].Touched && !Gpt.Copies[1].Touched) {
        AsciiSPrint (Response, sizeof (Response), "slot-set: preflight failed; no writes: %r", Status);
      } else {
        AsciiSPrint (Response, sizeof (Response), Restored
          ? "slot-set: write failed; rollback verified"
          : "slot-set: write failed; rollback FAILED; keep fastboot");
      }
      goto Done;
    }
  }
  AsciiSPrint (Response, sizeof (Response), "%a boot_%c %016lx -> %016lx",
               Edit.DryRun ? "dry-run" : (Attempted ? "verified" : "unchanged"),
               (CHAR8)('a' + Edit.Slot), Original, Updated);
Done:
  SlotClose (&Gpt);
  if (EFI_ERROR (Status)) FastbootFail (Response);
  else if (!EFI_ERROR (SlotSendInfo (Response))) FastbootOkay ("");
}

VOID
GblSlotRegisterCommands (VOID)
{
  FastbootRegister ("oem slot-show", CmdSlotShow);
  FastbootRegister ("oem slot-set", CmdSlotSet);
}
