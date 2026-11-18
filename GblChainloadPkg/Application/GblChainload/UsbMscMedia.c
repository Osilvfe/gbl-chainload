#include "UsbMsc.h"
#include <Uefi/UefiGpt.h>
#include <Protocol/BlockIo.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DeviceInfo.h>

#define MSC_MEDIA_BUFFER 65536U

extern EFI_GUID gEfiPartitionRecordGuid;

typedef struct {
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  UINT32 MediaId;
  UINT32 BlockSize;
  UINT32 IoAlign;
  EFI_LBA LastBlock;
  UINT64 Size;
  VOID *Allocation;
  UINT8 *Buffer;
} MSC_PARTITION;

STATIC BOOLEAN
ValidName (IN CONST CHAR8 *Name)
{
  UINTN Index;
  CHAR8 Character;

  for (Index = 0; Index < GBL_MSC_NAME_SIZE; Index++) {
    Character = Name[Index];
    if (Character == 0) {
      return Index != 0;
    }
    if (!((Character >= 'a' && Character <= 'z') ||
          (Character >= 'A' && Character <= 'Z') ||
          (Character >= '0' && Character <= '9') ||
          Character == '_' || Character == '-' || Character == '.')) {
      return FALSE;
    }
  }
  return FALSE;
}

STATIC BOOLEAN
MatchName (IN CONST CHAR16 *Label, IN CONST CHAR8 *Name)
{
  UINTN Index;

  for (Index = 0; Index < GBL_MSC_NAME_SIZE - 1; Index++) {
    if (Label[Index] != (UINT8)Name[Index]) {
      return FALSE;
    }
    if (Name[Index] == 0) {
      return TRUE;
    }
  }
  return Name[Index] == 0;
}

STATIC EFI_STATUS
RamRead (IN VOID *Context, IN UINT64 Offset, OUT VOID *Buffer, IN UINTN Size)
{
  if (Offset > GBL_MSC_DISK_SIZE || Size > GBL_MSC_DISK_SIZE - Offset) {
    return EFI_BAD_BUFFER_SIZE;
  }
  CopyMem (Buffer, (UINT8 *)Context + (UINTN)Offset, Size);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
CheckMedia (IN MSC_PARTITION *Partition)
{
  EFI_BLOCK_IO_MEDIA *Media = Partition->BlockIo->Media;

  if (Media == NULL || !Media->MediaPresent) {
    return EFI_NO_MEDIA;
  }
  if (Media->MediaId != Partition->MediaId ||
      Media->BlockSize != Partition->BlockSize || Media->IoAlign != Partition->IoAlign ||
      Media->LastBlock != Partition->LastBlock || !Media->LogicalPartition ||
      Media->RemovableMedia) {
    return EFI_MEDIA_CHANGED;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
PartitionRead (IN VOID *Context, IN UINT64 Offset, OUT VOID *Buffer, IN UINTN Size)
{
  MSC_PARTITION *Partition = Context;
  EFI_STATUS Status;
  UINTN Skip;
  UINTN Length;
  UINTN ReadSize;
  UINT8 *Destination = Buffer;

  if (Offset > Partition->Size || Size > Partition->Size - Offset) {
    return EFI_BAD_BUFFER_SIZE;
  }
  Status = CheckMedia (Partition);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  while (Size != 0) {
    Skip = (UINTN)(Offset % Partition->BlockSize);
    Length = MIN (Size, MSC_MEDIA_BUFFER - Skip);
    ReadSize = ((Skip + Length + Partition->BlockSize - 1) / Partition->BlockSize) * Partition->BlockSize;
    Status = Partition->BlockIo->ReadBlocks (Partition->BlockIo, Partition->MediaId,
                 Offset / Partition->BlockSize, ReadSize, Partition->Buffer);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = CheckMedia (Partition);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    CopyMem (Destination, Partition->Buffer + Skip, Length);
    Destination += Length;
    Offset += Length;
    Size -= Length;
  }
  return EFI_SUCCESS;
}

VOID
GblMscMediaClose (IN OUT GBL_MSC_MEDIA *Media)
{
  MSC_PARTITION *Partition;

  if (Media->Context != NULL) {
    if (Media->IsPartition) {
      Partition = Media->Context;
      if (Partition->Allocation != NULL) {
        FreePool (Partition->Allocation);
      }
    }
    FreePool (Media->Context);
  }
  ZeroMem (Media, sizeof (*Media));
}

EFI_STATUS
GblMscMediaOpen (IN CONST CHAR8 *Name, OUT GBL_MSC_MEDIA *Media)
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN Index;
  UINTN Matches = 0;
  EFI_PARTITION_ENTRY *Entry;
  EFI_PARTITION_ENTRY SelectedEntry;
  EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
  EFI_BLOCK_IO_MEDIA *BlockMedia;
  MSC_PARTITION *Partition;
  UINTN Alignment;
  UINTN Address;
  CONST UINT8 *GuidBytes;
  STATIC CONST CHAR8 Hex[] = "0123456789abcdef";

  ZeroMem (Media, sizeof (*Media));
  if (Name == NULL || Name[0] == 0) {
    Media->Context = AllocateZeroPool (GBL_MSC_DISK_SIZE);
    if (Media->Context == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    GblMscMakeDisk (Media->Context);
    Media->Size = GBL_MSC_DISK_SIZE;
    Media->Read = RamRead;
    CopyMem (Media->Name, "RAM disk", sizeof ("RAM disk"));
    CopyMem (Media->Serial, "GBLRAM0001-MSC", sizeof ("GBLRAM0001-MSC"));
    return EFI_SUCCESS;
  }
  if (!ValidName (Name)) {
    return EFI_INVALID_PARAMETER;
  }
  if (!IsUnlocked ()) {
    return EFI_ACCESS_DENIED;
  }
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiPartitionRecordGuid, (VOID **)&Entry);
    if (EFI_ERROR (Status) || Entry == NULL || !MatchName (Entry->PartitionName, Name)) {
      continue;
    }
    Matches++;
    SelectedEntry = *Entry;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status)) {
      BlockIo = NULL;
    }
  }
  FreePool (Handles);
  if (Matches == 0) {
    return EFI_NOT_FOUND;
  }
  if (Matches != 1) {
    return EFI_NO_MAPPING;
  }
  if (BlockIo == NULL || BlockIo->Media == NULL || BlockIo->ReadBlocks == NULL) {
    return EFI_UNSUPPORTED;
  }
  BlockMedia = BlockIo->Media;
  if (!BlockMedia->MediaPresent) {
    return EFI_NO_MEDIA;
  }
  if (!BlockMedia->LogicalPartition || BlockMedia->RemovableMedia ||
      BlockMedia->BlockSize < GBL_MSC_BLOCK_SIZE || BlockMedia->BlockSize > MSC_MEDIA_BUFFER ||
      (BlockMedia->BlockSize & (BlockMedia->BlockSize - 1)) != 0 ||
      BlockMedia->LastBlock >= MAX_UINT64 / BlockMedia->BlockSize ||
      SelectedEntry.StartingLBA == 0 || SelectedEntry.EndingLBA < SelectedEntry.StartingLBA ||
      SelectedEntry.EndingLBA - SelectedEntry.StartingLBA != BlockMedia->LastBlock) {
    return EFI_UNSUPPORTED;
  }
  Alignment = MAX ((UINTN)BlockMedia->IoAlign, (UINTN)1);
  if (Alignment > MSC_MEDIA_BUFFER || (Alignment & (Alignment - 1)) != 0) {
    return EFI_UNSUPPORTED;
  }
  Partition = AllocateZeroPool (sizeof (*Partition));
  if (Partition == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Media->Context = Partition;
  Media->IsPartition = TRUE;
  Partition->Allocation = AllocateZeroPool (MSC_MEDIA_BUFFER + Alignment - 1);
  if (Partition->Allocation == NULL) {
    GblMscMediaClose (Media);
    return EFI_OUT_OF_RESOURCES;
  }
  Address = ((UINTN)Partition->Allocation + Alignment - 1) & ~(Alignment - 1);
  Partition->Buffer = (UINT8 *)Address;
  Partition->BlockIo = BlockIo;
  Partition->MediaId = BlockMedia->MediaId;
  Partition->BlockSize = BlockMedia->BlockSize;
  Partition->IoAlign = BlockMedia->IoAlign;
  Partition->LastBlock = BlockMedia->LastBlock;
  Partition->Size = (BlockMedia->LastBlock + 1) * BlockMedia->BlockSize;
  Media->Size = Partition->Size;
  Media->Read = PartitionRead;
  for (Index = 0; Name[Index] != 0; Index++) {
    Media->Name[Index] = Name[Index];
  }
  CopyMem (Media->Serial, "GBLRO-", 6);
  GuidBytes = (CONST UINT8 *)&SelectedEntry.UniquePartitionGUID;
  for (Index = 0; Index < sizeof (SelectedEntry.UniquePartitionGUID); Index++) {
    Media->Serial[6 + Index * 2] = Hex[GuidBytes[Index] >> 4];
    Media->Serial[7 + Index * 2] = Hex[GuidBytes[Index] & 15];
  }
  Status = PartitionRead (Partition, 0, Partition->Buffer, Partition->BlockSize);
  if (EFI_ERROR (Status)) {
    GblMscMediaClose (Media);
  }
  return Status;
}
