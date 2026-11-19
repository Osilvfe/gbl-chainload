#include "UsbMsc.h"
#include <Uefi/UefiGpt.h>
#include <Guid/FileInfo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
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

typedef struct {
  EFI_FILE_PROTOCOL *File;
  UINT64 FileSize;
  UINT64 Size;
  UINTN BufferSize;
  UINT8 *Buffer;
} MSC_FILE;

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

STATIC BOOLEAN
HasPrefix (IN CONST CHAR8 *Text, IN CONST CHAR8 *Prefix)
{
  UINTN Index;

  for (Index = 0; Prefix[Index] != 0; Index++) {
    if (Text[Index] != Prefix[Index]) {
      return FALSE;
    }
  }
  return TRUE;
}

/*
 * Convert a command-line media spec into an EFI volume selector and a
 * normalised volume-relative path. An optional "fsN:" prefix selects one
 * SimpleFileSystem volume; otherwise every volume is searched in handle order.
 * Both '/' and '\\' separate path components, repeated separators are
 * collapsed, and a leading separator is dropped so the FAT driver always sees
 * a path relative to the volume root.
 */
STATIC EFI_STATUS
BuildFilePath (
  IN CONST CHAR8 *Spec,
  OUT UINTN *Volume,
  OUT CHAR16 *Path
  )
{
  UINTN Index = 0;
  UINTN Length = 0;
  UINTN Number = 0;
  BOOLEAN Digits = FALSE;
  UINT8 Character;

  *Volume = MAX_UINTN;
  if (Spec[0] == 'f' && Spec[1] == 's') {
    Index = 2;
    while (Spec[Index] >= '0' && Spec[Index] <= '9' && Number < 100) {
      Number = Number * 10 + (UINTN)(Spec[Index] - '0');
      Digits = TRUE;
      Index++;
    }
    if (!Digits || Spec[Index] != ':') {
      return EFI_INVALID_PARAMETER;
    }
    *Volume = Number;
    Index++;
  }
  while (Spec[Index] == '/' || Spec[Index] == '\\') {
    Index++;
  }
  for (; Spec[Index] != 0; Index++) {
    Character = (UINT8)Spec[Index];
    if (Character == '/' || Character == '\\') {
      if (Length == 0 || Path[Length - 1] == (CHAR16)L'\\') {
        continue;
      }
      Path[Length++] = (CHAR16)L'\\';
    } else {
      if (Character < 0x20 || Character > 0x7e) {
        return EFI_INVALID_PARAMETER;
      }
      Path[Length++] = (CHAR16)Character;
    }
    if (Length + 1 >= GBL_MSC_PATH_SIZE) {
      return EFI_INVALID_PARAMETER;
    }
  }
  if (Length == 0 || Path[Length - 1] == (CHAR16)L'\\') {
    return EFI_INVALID_PARAMETER;
  }
  Path[Length] = 0;
  return EFI_SUCCESS;
}

STATIC VOID
MakeFileSerial (
  OUT CHAR8 *Serial,
  IN CONST CHAR8 *Prefix,
  IN CONST CHAR16 *Path,
  IN UINTN Volume
  )
{
  STATIC CONST CHAR8 Hex[] = "0123456789abcdef";
  UINT32 Hash = 2166136261U;
  UINTN Index;
  UINTN Length;

  for (Length = 0; Prefix[Length] != 0; Length++) {
    Serial[Length] = Prefix[Length];
  }
  for (Index = 0; Path[Index] != 0; Index++) {
    Hash = (Hash ^ (UINT32)Path[Index]) * 16777619U;
  }
  Hash = (Hash ^ (UINT32)Volume) * 16777619U;
  for (Index = 0; Index < 8; Index++) {
    Serial[Length + Index] = Hex[(Hash >> ((7 - Index) * 4)) & 0xf];
  }
  Serial[Length + 8] = 0;
}

STATIC EFI_STATUS
FileRead (IN VOID *Context, IN UINT64 Offset, OUT VOID *Buffer, IN UINTN Size)
{
  MSC_FILE *Backing = Context;
  EFI_STATUS Status;
  UINT8 *Destination = Buffer;
  UINTN Remaining;
  UINTN Filled;
  UINTN Chunk;
  UINTN Done;

  if (Offset > Backing->Size || Size > Backing->Size - Offset) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (Offset >= Backing->FileSize) {
    ZeroMem (Destination, Size);
    return EFI_SUCCESS;
  }
  Remaining = (UINTN)MIN ((UINT64)Size, Backing->FileSize - Offset);
  Filled = 0;
  Status = Backing->File->SetPosition (Backing->File, Offset);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  while (Remaining != 0) {
    Chunk = MIN (Remaining, Backing->BufferSize);
    Done = Chunk;
    Status = Backing->File->Read (Backing->File, &Done, Backing->Buffer);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (Done == 0 || Done > Chunk) {
      break;
    }
    CopyMem (Destination, Backing->Buffer, Done);
    Destination += Done;
    Filled += Done;
    Remaining -= Done;
  }
  /*
   * A final logical block that extends past the end of the file, or a short
   * read, is completed with zeroes so the host always gets whole blocks.
   */
  if (Filled < Size) {
    ZeroMem (Destination, Size - Filled);
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
OpenFileMedia (
  IN CONST CHAR8 *Scheme,
  IN CONST CHAR8 *Spec,
  IN BOOLEAN Iso,
  OUT GBL_MSC_MEDIA *Media
  )
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0;
  UINTN Index;
  UINTN First;
  UINTN Last;
  UINTN Volume;
  UINTN LabelLength;
  CHAR16 Path[GBL_MSC_PATH_SIZE];
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root;
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO *Info;
  UINTN InfoSize;
  MSC_FILE *Backing;
  UINT32 BlockSize = Iso ? GBL_MSC_ISO_BLOCK_SIZE : GBL_MSC_FILE_BLOCK_SIZE;

  ZeroMem (Media, sizeof (*Media));
  Status = BuildFilePath (Spec, &Volume, Path);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                    NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || Handles == NULL || Count == 0) {
    if (Handles != NULL) {
      FreePool (Handles);
    }
    return EFI_NOT_FOUND;
  }
  if (Volume != MAX_UINTN) {
    if (Volume >= Count) {
      FreePool (Handles);
      return EFI_NOT_FOUND;
    }
    First = Volume;
    Last = Volume + 1;
  } else {
    First = 0;
    Last = Count;
  }
  File = NULL;
  for (Index = First; Index < Last && File == NULL; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid,
                                  (VOID **)&Fs);
    if (EFI_ERROR (Status) || Fs == NULL || Fs->OpenVolume == NULL) {
      continue;
    }
    Root = NULL;
    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status) || Root == NULL) {
      continue;
    }
    Status = Root->Open (Root, &File, Path, EFI_FILE_MODE_READ, 0);
    Root->Close (Root);
    if (EFI_ERROR (Status)) {
      File = NULL;
    }
  }
  FreePool (Handles);
  if (File == NULL) {
    return EFI_NOT_FOUND;
  }
  InfoSize = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL && EFI_ERROR (Status)) {
    File->Close (File);
    return EFI_UNSUPPORTED;
  }
  if (InfoSize < SIZE_OF_EFI_FILE_INFO) {
    InfoSize = SIZE_OF_EFI_FILE_INFO + 256;
  }
  Info = AllocateZeroPool (InfoSize);
  if (Info == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status) || (Info->Attribute & EFI_FILE_DIRECTORY) != 0 ||
      Info->FileSize == 0 || Info->FileSize > MAX_UINT64 - BlockSize) {
    FreePool (Info);
    File->Close (File);
    return EFI_UNSUPPORTED;
  }
  Backing = AllocateZeroPool (sizeof (*Backing));
  if (Backing == NULL) {
    FreePool (Info);
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }
  Backing->BufferSize = MSC_MEDIA_BUFFER;
  Backing->Buffer = AllocateZeroPool (Backing->BufferSize);
  if (Backing->Buffer == NULL) {
    FreePool (Backing);
    FreePool (Info);
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }
  Backing->File = File;
  Backing->FileSize = Info->FileSize;
  Backing->Size = ((Info->FileSize + BlockSize - 1) / BlockSize) * BlockSize;
  FreePool (Info);
  Media->Context = Backing;
  Media->IsFile = TRUE;
  Media->PeripheralType = Iso ? GBL_MSC_PERIPHERAL_CDROM : GBL_MSC_PERIPHERAL_DIRECT;
  Media->Size = Backing->Size;
  Media->BlockSize = BlockSize;
  Media->Read = FileRead;
  for (LabelLength = 0; Scheme[LabelLength] != 0 && LabelLength < GBL_MSC_NAME_SIZE - 1;
       LabelLength++) {
    Media->Name[LabelLength] = Scheme[LabelLength];
  }
  for (Index = 0; Spec[Index] != 0 && LabelLength < GBL_MSC_NAME_SIZE - 1; Index++) {
    Media->Name[LabelLength++] = Spec[Index];
  }
  MakeFileSerial (Media->Serial, Iso ? "GBLISO-" : "GBLFILE-", Path, Volume);
  return EFI_SUCCESS;
}

typedef struct {
  UINT64 Size;
} MSC_RAM;

STATIC EFI_STATUS
RamRead (IN VOID *Context, IN UINT64 Offset, OUT VOID *Buffer, IN UINTN Size)
{
  MSC_RAM *Backing = Context;

  if (Offset > Backing->Size || Size > Backing->Size - Offset) {
    return EFI_BAD_BUFFER_SIZE;
  }
  CopyMem (Buffer, (UINT8 *)Backing + sizeof (*Backing) + (UINTN)Offset, Size);
  return EFI_SUCCESS;
}

#if GBL_USB_MSC_WRITE
/*
 * Writes land in the same buffer the reads come from, in command order, so a
 * read issued after a write always sees it. There is no write-back cache: the
 * RAM is the medium, and nothing is left unwritten when a command completes.
 */
STATIC EFI_STATUS
RamWrite (IN VOID *Context, IN UINT64 Offset, IN CONST VOID *Buffer, IN UINTN Size)
{
  MSC_RAM *Backing = Context;

  if (Offset > Backing->Size || Size > Backing->Size - Offset) {
    return EFI_BAD_BUFFER_SIZE;
  }
  CopyMem ((UINT8 *)Backing + sizeof (*Backing) + (UINTN)Offset, Buffer, Size);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
RamFlush (IN VOID *Context)
{
  (VOID)Context;
  /* Volatile memory with no cache in front of it: nothing to commit. */
  return EFI_SUCCESS;
}
#endif

/*
 * "ram:" is an explicit in-memory LUN, optionally sized in KiB ("ram:4096").
 * The medium is private to this session: it starts as the same FAT12 example
 * image the default RAM disk carries (1 MiB) or blank, and it is discarded
 * when the session ends. Nothing it holds can reach real storage.
 */
STATIC EFI_STATUS
OpenRamMedia (IN CONST CHAR8 *Spec, OUT GBL_MSC_MEDIA *Media)
{
  UINT64 Size = GBL_MSC_DISK_SIZE;
  UINT64 Value = 0;
  UINTN Index = 0;
  MSC_RAM *Backing;
  UINT8 *Disk;
  STATIC CONST CHAR8 Hex[] = "0123456789abcdef";

  if (Spec[0] != 0) {
    while (Spec[Index] >= '0' && Spec[Index] <= '9') {
      Value = Value * 10 + (UINT64)(Spec[Index] - '0');
      if (Value > GBL_MSC_RAM_MAX_SIZE) {
        return EFI_INVALID_PARAMETER;
      }
      Index++;
    }
    if (Spec[Index] != 0 || Index == 0) {
      return EFI_INVALID_PARAMETER;
    }
    Size = Value * 1024;
    if (Size < GBL_MSC_RAM_MIN_SIZE || Size > GBL_MSC_RAM_MAX_SIZE ||
        Size % GBL_MSC_BLOCK_SIZE != 0) {
      return EFI_INVALID_PARAMETER;
    }
  }
  ZeroMem (Media, sizeof (*Media));
  Backing = AllocateZeroPool ((UINTN)(sizeof (*Backing) + Size));
  if (Backing == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Backing->Size = Size;
  Disk = (UINT8 *)Backing + sizeof (*Backing);
  if (Size == GBL_MSC_DISK_SIZE) {
    GblMscMakeDisk (Disk);
  }
  Media->Context = Backing;
  Media->Size = Size;
  Media->BlockSize = GBL_MSC_BLOCK_SIZE;
  Media->Read = RamRead;
  Media->Name[0] = 0;
  CopyMem (Media->Name, "RAM disk", sizeof ("RAM disk"));
  CopyMem (Media->Serial, "GBLRAM", 6);
  for (Index = 0; Index < 6; Index++) {
    Media->Serial[6 + Index] = Hex[((Size / 1024) >> ((5 - Index) * 4)) & 0xf];
  }
  CopyMem (Media->Serial + 12, "-MSC", 5);
#if GBL_USB_MSC_WRITE
  Media->Write = RamWrite;
  Media->Flush = RamFlush;
  Media->Writable = TRUE;
#endif
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

#if GBL_USB_MSC_WRITE_PARTITIONS
/*
 * Partition writes. Only compiled into builds that opt in, so a normal image
 * has no block write call at all. Two properties matter:
 *
 *  - Every write is re-validated against the partition record it was opened
 *    with. If the table changed (MediaId, block size, last block) the write is
 *    abandoned with MEDIA CHANGED instead of landing on whatever now sits at
 *    that LBA. The GPT itself is never in range: the LUN starts at the
 *    partition's StartingLBA, and that is required to be at least 34, so the
 *    primary and backup headers and entry arrays are unreachable.
 *  - A block is never left half updated. A chunk that does not cover whole
 *    logical blocks is read-modify-written, so a power cut leaves either the
 *    old block or the new block, never a mixture of the two.
 */
STATIC EFI_STATUS
PartitionWrite (IN VOID *Context, IN UINT64 Offset, IN CONST VOID *Buffer, IN UINTN Size)
{
  MSC_PARTITION *Partition = Context;
  EFI_STATUS Status;
  UINTN Skip;
  UINTN Length;
  CONST UINT8 *Source = Buffer;

  if (Offset > Partition->Size || Size > Partition->Size - Offset) {
    return EFI_BAD_BUFFER_SIZE;
  }
  Status = CheckMedia (Partition);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  while (Size != 0) {
    Skip = (UINTN)(Offset % Partition->BlockSize);
    if (Skip == 0) {
      /* Whole blocks that fit the bounce buffer go out in one call. */
      Length = (MIN (Size, MSC_MEDIA_BUFFER) / Partition->BlockSize) * Partition->BlockSize;
      if (Length != 0) {
        CopyMem (Partition->Buffer, Source, Length);
        Status = Partition->BlockIo->WriteBlocks (Partition->BlockIo, Partition->MediaId,
                     Offset / Partition->BlockSize, Length, Partition->Buffer);
        if (EFI_ERROR (Status)) {
          return Status;
        }
        Status = CheckMedia (Partition);
        if (EFI_ERROR (Status)) {
          return Status;
        }
        Source += Length;
        Offset += Length;
        Size -= Length;
        continue;
      }
    }
    /* One partial block: read it, patch the covered bytes, write it back. */
    Length = MIN (Size, Partition->BlockSize - Skip);
    Status = Partition->BlockIo->ReadBlocks (Partition->BlockIo, Partition->MediaId,
                 Offset / Partition->BlockSize, Partition->BlockSize, Partition->Buffer);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    CopyMem (Partition->Buffer + Skip, Source, Length);
    Status = Partition->BlockIo->WriteBlocks (Partition->BlockIo, Partition->MediaId,
                 Offset / Partition->BlockSize, Partition->BlockSize, Partition->Buffer);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = CheckMedia (Partition);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Source += Length;
    Offset += Length;
    Size -= Length;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
PartitionFlush (IN VOID *Context)
{
  MSC_PARTITION *Partition = Context;
  EFI_STATUS Status;

  Status = CheckMedia (Partition);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Partition->BlockIo->FlushBlocks == NULL) {
    /* The medium has no cache of its own: nothing to commit. */
    return EFI_SUCCESS;
  }
  return Partition->BlockIo->FlushBlocks (Partition->BlockIo);
}
#endif

VOID
GblMscMediaClose (IN OUT GBL_MSC_MEDIA *Media)
{
  MSC_PARTITION *Partition;
  MSC_FILE *File;

  if (Media->Context != NULL) {
    /*
     * Last barrier before the medium is released: a writable LUN that was
     * closed without an eject must not lose an acknowledged write.
     */
    if (Media->Writable && Media->Flush != NULL) {
      (VOID)Media->Flush (Media->Context);
    }
    if (Media->IsPartition) {
      Partition = Media->Context;
      if (Partition->Allocation != NULL) {
        FreePool (Partition->Allocation);
      }
    } else if (Media->IsFile) {
      File = Media->Context;
      if (File->File != NULL) {
        File->File->Close (File->File);
      }
      if (File->Buffer != NULL) {
        FreePool (File->Buffer);
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
  BOOLEAN Writable = FALSE;
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
    return OpenRamMedia ("", Media);
  }
  if (HasPrefix (Name, "ram:")) {
    return OpenRamMedia (Name + 4, Media);
  }
  if (HasPrefix (Name, "iso:")) {
    return OpenFileMedia ("iso:", Name + 4, TRUE, Media);
  }
  if (HasPrefix (Name, "file:")) {
    return OpenFileMedia ("file:", Name + 5, FALSE, Media);
  }
  if (HasPrefix (Name, "rw:")) {
#if GBL_USB_MSC_WRITE_PARTITIONS
    /* Explicit opt-in, and only in a build that carries the write backend. */
    Writable = TRUE;
    Name += 3;
#else
    /*
     * A normal build has no block write path at all, so the spec is refused
     * here instead of being accepted as a read-only LUN: a name that says
     * "rw" must never silently behave like "ro".
     */
    return EFI_SECURITY_VIOLATION;
#endif
  }
  if (HasPrefix (Name, "ro:")) {
    Name += 3;
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
#if GBL_USB_MSC_WRITE_PARTITIONS
  if (Writable && BlockIo->WriteBlocks == NULL) {
    return EFI_UNSUPPORTED;
  }
#endif
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
  if (Writable && SelectedEntry.StartingLBA < 34) {
    /*
     * The first 34 LBAs hold the protective MBR, the GPT header and the entry
     * array. A partition that starts inside them is not something this export
     * will hand out with write access: the table would be one stray write away
     * from being destroyed.
     */
    DEBUG ((DEBUG_ERROR, "USB MSC: refusing writable partition at LBA %Lu\n",
            SelectedEntry.StartingLBA));
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
  Media->BlockSize = Partition->BlockSize;
  Media->Read = PartitionRead;
#if GBL_USB_MSC_WRITE_PARTITIONS
  if (Writable) {
    Media->Write = PartitionWrite;
    Media->Flush = PartitionFlush;
    Media->Writable = TRUE;
  }
#endif
  for (Index = 0; Name[Index] != 0; Index++) {
    Media->Name[Index] = Name[Index];
  }
  CopyMem (Media->Serial, Writable ? "GBLRW-" : "GBLRO-", 6);
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
