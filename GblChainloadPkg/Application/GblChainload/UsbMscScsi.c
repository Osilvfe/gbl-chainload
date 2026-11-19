#include "UsbMsc.h"
#include <Library/BaseMemoryLib.h>

STATIC UINT32
ReadLe32 (IN CONST UINT8 *Bytes)
{
  return (UINT32)Bytes[0] | ((UINT32)Bytes[1] << 8) |
         ((UINT32)Bytes[2] << 16) | ((UINT32)Bytes[3] << 24);
}

STATIC UINT32
ReadBe32 (IN CONST UINT8 *Bytes)
{
  return ((UINT32)Bytes[0] << 24) | ((UINT32)Bytes[1] << 16) |
         ((UINT32)Bytes[2] << 8) | Bytes[3];
}

STATIC VOID
WriteLe32 (OUT UINT8 *Bytes, IN UINT32 Value)
{
  UINTN Index;
  for (Index = 0; Index < 4; Index++) {
    Bytes[Index] = (UINT8)(Value >> (Index * 8));
  }
}

STATIC VOID
WriteBe32 (OUT UINT8 *Bytes, IN UINT32 Value)
{
  UINTN Index;
  for (Index = 0; Index < 4; Index++) {
    Bytes[Index] = (UINT8)(Value >> ((3 - Index) * 8));
  }
}

STATIC VOID
WriteBe24 (OUT UINT8 *Bytes, IN UINT32 Value)
{
  Bytes[0] = (UINT8)(Value >> 16);
  Bytes[1] = (UINT8)(Value >> 8);
  Bytes[2] = (UINT8)Value;
}

VOID
GblMscMakeDisk (OUT UINT8 *Disk)
{
  STATIC CONST CHAR8 Text[] =
    "gbl-chainload USB Mass Storage\r\n"
    "This is a read-only 1 MiB RAM disk.\r\n"
    "No internal UFS partitions are exported.\r\n"
    "Eject this disk, or press a volume key, to return to GBL Fastboot.\r\n";
  UINTN FatOffset;
  UINT8 *Root;

  ZeroMem (Disk, GBL_MSC_DISK_SIZE);
  Disk[0] = 0xeb;
  Disk[1] = 0x3c;
  Disk[2] = 0x90;
  CopyMem (Disk + 3, "GBL MSC ", 8);
  Disk[12] = 2;
  Disk[13] = 1;
  Disk[14] = 1;
  Disk[16] = 2;
  Disk[17] = 32;
  Disk[20] = 8;
  Disk[21] = 0xf8;
  Disk[22] = 6;
  Disk[24] = 32;
  Disk[26] = 64;
  Disk[36] = 0x80;
  Disk[38] = 0x29;
  WriteLe32 (Disk + 39, 0x47424c31);
  CopyMem (Disk + 43, "GBL RAMDISK", 11);
  CopyMem (Disk + 54, "FAT12   ", 8);
  Disk[510] = 0x55;
  Disk[511] = 0xaa;
  for (FatOffset = 512; FatOffset <= 3584; FatOffset += 3072) {
    Disk[FatOffset] = 0xf8;
    Disk[FatOffset + 1] = 0xff;
    Disk[FatOffset + 2] = 0xff;
    Disk[FatOffset + 3] = 0xff;
    Disk[FatOffset + 4] = 0x0f;
  }
  Root = Disk + 13 * GBL_MSC_BLOCK_SIZE;
  CopyMem (Root, "GBL RAMDISK", 11);
  Root[11] = 8;
  CopyMem (Root + 32, "README  TXT", 11);
  Root[43] = 1;
  Root[56] = 0x21;
  Root[58] = 2;
  WriteLe32 (Root + 60, sizeof (Text) - 1);
  CopyMem (Disk + 15 * GBL_MSC_BLOCK_SIZE, Text, sizeof (Text) - 1);
}

VOID
GblMscBotInit (OUT GBL_MSC_BOT *Bot, IN CONST UINT8 *Disk, IN UINT64 Size)
{
  ZeroMem (Bot, sizeof (*Bot));
  Bot->Disk = Disk;
  Bot->DiskSize = Size;
  Bot->BlockSize = GBL_MSC_BLOCK_SIZE;
  Bot->LunCount = 1;
  Bot->MediaPresent = TRUE;
}

VOID
GblMscBotInitMedia (OUT GBL_MSC_BOT *Bot, IN CONST GBL_MSC_MEDIA *Media)
{
  GblMscBotInit (Bot, NULL, Media->Size);
  Bot->Read = Media->Read;
  Bot->ReadContext = Media->Context;
  Bot->IsPartition = Media->IsPartition;
  Bot->Serial = Media->Serial;
  Bot->BlockSize = Media->BlockSize == 0 ? GBL_MSC_BLOCK_SIZE : Media->BlockSize;
  Bot->LunCount = 1;
}

STATIC VOID
FailCommand (IN OUT GBL_MSC_BOT *Bot, IN UINT8 Sense, IN UINT8 Asc)
{
  if (Bot->Status != 2) {
    Bot->Status = 1;
  }
  Bot->SenseKey = Sense;
  Bot->Asc = Asc;
  Bot->Ascq = 0;
}

STATIC UINT32
ScsiCommand (
  IN OUT GBL_MSC_BOT *Bot,
  IN CONST UINT8 *Command,
  IN UINT8 CommandSize,
  OUT GBL_MSC_DIRECTION *Direction
  )
{
  UINT8 Opcode = Command[0];
  UINT8 RequiredSize = 6;
  UINT32 Length = 0;
  UINT32 Blocks;
  UINT64 Lba;
  BOOLEAN Write = FALSE;
  UINT32 BlockSize = Bot->BlockSize == 0 ? GBL_MSC_BLOCK_SIZE : Bot->BlockSize;
  UINT64 LastBlock = Bot->DiskSize / BlockSize - 1;
  CONST CHAR8 *Serial;
  UINTN SerialLength;
  UINTN Index;

  *Direction = GblMscNoData;
  if ((Opcode >> 5) == 1 || (Opcode >> 5) == 2) {
    RequiredSize = 10;
  } else if ((Opcode >> 5) == 4) {
    RequiredSize = 16;
  } else if ((Opcode >> 5) == 5) {
    RequiredSize = 12;
  }
  if (CommandSize < RequiredSize) {
    FailCommand (Bot, 5, 0x24);
    return 0;
  }

  switch (Opcode) {
  case 0x00:
    break;
  case 0x03:
    *Direction = GblMscDataIn;
    Bot->Response[0] = 0x70;
    Bot->Response[2] = Bot->SenseKey;
    Bot->Response[7] = 10;
    Bot->Response[12] = Bot->Asc;
    Bot->Response[13] = Bot->Ascq;
    Bot->SenseKey = 0;
    Bot->Asc = 0;
    Bot->Ascq = 0;
    return MIN (18U, Command[4]);
  case 0x12:
    *Direction = GblMscDataIn;
    if (Command[1] == 1 && Command[2] == 0) {
      Bot->Response[3] = 2;
      Bot->Response[4] = 0;
      Bot->Response[5] = 0x80;
      Length = 6;
    } else if (Command[1] == 1 && Command[2] == 0x80) {
      Bot->Response[1] = 0x80;
      Serial = Bot->Serial == NULL ? "GBLRAM0001" : Bot->Serial;
      for (SerialLength = 0; Serial[SerialLength] != 0 && SerialLength < 63; SerialLength++) {
        Bot->Response[4 + SerialLength] = (UINT8)Serial[SerialLength];
      }
      Bot->Response[3] = (UINT8)SerialLength;
      Length = (UINT32)SerialLength + 4;
    } else if (Command[1] == 0 && Command[2] == 0) {
      Bot->Response[1] = 0x80;
      /*
       * INQUIRY version 0x05 (SPC-3). Hosts gate VPD page 0x80/0x83, REPORT
       * LUNS and other multi-LUN handling on the reported version; advertising
       * SPC-2 (0x04) makes Linux scan sequentially and never request the
       * per-LUN serial number page.
       */
      Bot->Response[2] = 5;
      Bot->Response[3] = 2;
      Bot->Response[4] = 31;
      CopyMem (Bot->Response + 8, Bot->IsPartition
               ? "GBL     PARTITION       0001" : "GBL     RAM DISK        0001", 28);
      Length = 36;
    } else {
      FailCommand (Bot, 5, 0x24);
    }
    return MIN (Length, ((UINT32)Command[3] << 8) | Command[4]);
  case 0x1a:
  case 0x5a:
    *Direction = GblMscDataIn;
    if ((Command[2] & 0x3f) != 0 && (Command[2] & 0x3f) != 0x3f) {
      FailCommand (Bot, 5, 0x24);
      return 0;
    }
    if (Opcode == 0x1a) {
      Bot->Response[0] = 3;
      Bot->Response[2] = 0x80;
      return MIN (4U, Command[4]);
    }
    Bot->Response[1] = 6;
    Bot->Response[3] = 0x80;
    return MIN (8U, ((UINT32)Command[7] << 8) | Command[8]);
  case 0x1b:
    if ((Command[4] & 3) == 2) {
      if (Bot->PreventRemoval) {
        FailCommand (Bot, 5, 0x53);
        Bot->Ascq = 2;
      } else {
        Bot->MediaPresent = FALSE;
        Bot->EjectRequested = TRUE;
      }
    } else if ((Command[4] & 1) != 0) {
      Bot->MediaPresent = TRUE;
    }
    return 0;
  case 0x1e:
    Bot->PreventRemoval = (Command[4] & 1) != 0;
    return 0;
  case 0x23:
    *Direction = GblMscDataIn;
    Bot->Response[3] = 8;
    WriteBe32 (Bot->Response + 4, (UINT32)MIN (Bot->DiskSize / BlockSize, MAX_UINT32));
    Bot->Response[8] = 2;
    WriteBe24 (Bot->Response + 9, BlockSize);
    Length = MIN (12U, ((UINT32)Command[7] << 8) | Command[8]);
    break;
  case 0x25:
    *Direction = GblMscDataIn;
    WriteBe32 (Bot->Response, (UINT32)MIN (LastBlock, MAX_UINT32));
    WriteBe32 (Bot->Response + 4, BlockSize);
    Length = 8;
    break;
  case 0xa0:
    *Direction = GblMscDataIn;
    if (Command[2] != 0 || Command[3] != 0) {
      FailCommand (Bot, 5, 0x24);
      return 0;
    }
    WriteBe32 (Bot->Response, (UINT32)(Bot->LunCount * 8));
    for (Index = 0; Index < Bot->LunCount; Index++) {
      /*
       * SAM-5 peripheral device addressing: byte 0 carries the bus (0) and
       * byte 1 the LUN. Hosts decode the list with that layout, so a LUN
       * written into byte 0 comes back as a different, unreachable number.
       */
      Bot->Response[4 + Index * 8] = (UINT8)(Index >> 8);
      Bot->Response[4 + Index * 8 + 1] = (UINT8)Index;
    }
    Length = MIN (4U + (UINT32)Bot->LunCount * 8U, ReadBe32 (Command + 6));
    break;
  case 0x9e:
    *Direction = GblMscDataIn;
    if (Command[1] != 0x10 || ReadBe32 (Command + 2) != 0 ||
        ReadBe32 (Command + 6) != 0 || Command[14] != 0) {
      FailCommand (Bot, 5, 0x24);
      return 0;
    }
    WriteBe32 (Bot->Response, (UINT32)(LastBlock >> 32));
    WriteBe32 (Bot->Response + 4, (UINT32)LastBlock);
    WriteBe32 (Bot->Response + 8, BlockSize);
    Length = MIN (32U, ReadBe32 (Command + 10));
    break;
  case 0x2f:
    *Direction = GblMscNoData;
    Lba = ReadBe32 (Command + 2);
    Blocks = ((UINT32)Command[7] << 8) | Command[8];
    if (Blocks > MAX_UINT32 / BlockSize ||
        Lba > Bot->DiskSize / BlockSize ||
        Blocks > Bot->DiskSize / BlockSize - Lba) {
      FailCommand (Bot, 5, 0x21);
    }
    return 0;
  case 0x35:
  case 0x91:
    return 0;
  case 0x08:
  case 0x0a:
  case 0x28:
  case 0x2a:
  case 0xa8:
  case 0xaa:
  case 0x88:
  case 0x8a:
    Write = Opcode == 0x0a || Opcode == 0x2a || Opcode == 0xaa || Opcode == 0x8a;
    *Direction = Write ? GblMscDataOut : GblMscDataIn;
    if (RequiredSize == 6) {
      Lba = ((UINT32)(Command[1] & 0x1f) << 16) |
            ((UINT32)Command[2] << 8) | Command[3];
      Blocks = Command[4] == 0 ? 256 : Command[4];
    } else if (RequiredSize == 16) {
      Lba = ((UINT64)ReadBe32 (Command + 2) << 32) | ReadBe32 (Command + 6);
      Blocks = ReadBe32 (Command + 10);
    } else {
      Lba = ReadBe32 (Command + 2);
      Blocks = RequiredSize == 10 ? ((UINT32)Command[7] << 8) | Command[8]
                                 : ReadBe32 (Command + 6);
    }
    if (Blocks > MAX_UINT32 / BlockSize) {
      FailCommand (Bot, 5, 0x21);
      return 0;
    }
    Length = Blocks * BlockSize;
    if (Write) {
      FailCommand (Bot, 7, 0x27);
    } else if (Lba > Bot->DiskSize / BlockSize ||
               Blocks > Bot->DiskSize / BlockSize - Lba) {
      FailCommand (Bot, 5, 0x21);
    } else {
      Bot->FromDisk = TRUE;
      Bot->DiskOffset = Lba * BlockSize;
    }
    break;
  default:
    FailCommand (Bot, 5, 0x20);
    return 0;
  }
  if (!Bot->MediaPresent) {
    FailCommand (Bot, 2, 0x3a);
  }
  return Length;
}

EFI_STATUS
GblMscBotBegin (IN OUT GBL_MSC_BOT *Bot, IN CONST UINT8 *Cbw, IN UINTN Size)
{
  GBL_MSC_DIRECTION Direction;
  UINT32 DeviceLength;
  BOOLEAN HostIn;

  Bot->InLength = 0;
  Bot->OutLength = 0;
  Bot->Status = 0;
  Bot->FromDisk = FALSE;
  Bot->HaltIn = FALSE;
  Bot->HaltOut = FALSE;
  ZeroMem (Bot->Response, sizeof (Bot->Response));
  if (Bot->NeedsReset || Size != GBL_MSC_CBW_SIZE ||
      ReadLe32 (Cbw) != 0x43425355 ||
      (Cbw[12] & 0x7f) != 0 || Cbw[14] == 0 || Cbw[14] > 16) {
    Bot->NeedsReset = TRUE;
    Bot->HaltIn = TRUE;
    Bot->HaltOut = TRUE;
    return EFI_PROTOCOL_ERROR;
  }
  Bot->Tag = ReadLe32 (Cbw + 4);
  Bot->HostLength = ReadLe32 (Cbw + 8);
  if (Cbw[13] >= Bot->LunCount) {
    Bot->Status = 1;
    Bot->SenseKey = 5;
    Bot->Asc = 0x25;
    Bot->Ascq = 0;
    return EFI_SUCCESS;
  }
  HostIn = (Cbw[12] & 0x80) != 0;
  DeviceLength = ScsiCommand (Bot, Cbw + 15, Cbw[14], &Direction);
  if (DeviceLength == 0) {
    Direction = GblMscNoData;
  }
  if (Bot->HostLength == 0) {
    if (DeviceLength != 0) {
      Bot->Status = 2;
    }
  } else if (HostIn) {
    if (Direction == GblMscDataOut) {
      Bot->Status = 2;
      Bot->HaltIn = TRUE;
    } else if (Direction == GblMscNoData || DeviceLength == 0 || Bot->Status != 0) {
      Bot->HaltIn = TRUE;
    } else {
      Bot->InLength = MIN (DeviceLength, Bot->HostLength);
      if (Bot->HostLength < DeviceLength) {
        Bot->Status = 2;
      }
    }
  } else if (Direction == GblMscDataIn && DeviceLength != 0) {
    Bot->Status = 2;
    Bot->HaltOut = TRUE;
  } else {
    Bot->OutLength = Bot->HostLength;
    if (Bot->HostLength < DeviceLength) {
      Bot->Status = 2;
    }
  }
  return EFI_SUCCESS;
}

EFI_STATUS
GblMscBotRead (IN OUT GBL_MSC_BOT *Bot, IN UINT32 Offset,
               OUT VOID *Buffer, IN UINTN Size)
{
  EFI_STATUS Status;

  if (Size != 0 && Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (Offset > Bot->InLength || Size > Bot->InLength - Offset) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (Bot->FromDisk) {
    if (Bot->DiskOffset > Bot->DiskSize || Offset > Bot->DiskSize - Bot->DiskOffset ||
        Size > Bot->DiskSize - Bot->DiskOffset - Offset) {
      return EFI_BAD_BUFFER_SIZE;
    }
    if (Bot->Read != NULL) {
      Status = Bot->Read (Bot->ReadContext, Bot->DiskOffset + Offset, Buffer, Size);
      if (EFI_ERROR (Status)) {
        FailCommand (Bot, Status == EFI_NO_MEDIA ? 2 : Status == EFI_MEDIA_CHANGED ? 6 : 3,
                          Status == EFI_NO_MEDIA ? 0x3a : Status == EFI_MEDIA_CHANGED ? 0x28 : 0x11);
      }
      return Status;
    }
    if (Bot->Disk == NULL || Bot->DiskOffset + Offset > MAX_UINTN) {
      return EFI_INVALID_PARAMETER;
    }
    CopyMem (Buffer, Bot->Disk + (UINTN)(Bot->DiskOffset + Offset), Size);
  } else {
    if (Offset > sizeof (Bot->Response) || Size > sizeof (Bot->Response) - Offset) {
      return EFI_BAD_BUFFER_SIZE;
    }
    CopyMem (Buffer, Bot->Response + Offset, Size);
  }
  return EFI_SUCCESS;
}

VOID
GblMscBotCsw (IN CONST GBL_MSC_BOT *Bot, IN UINT32 Processed, OUT UINT8 *Csw)
{
  WriteLe32 (Csw, 0x53425355);
  WriteLe32 (Csw + 4, Bot->Tag);
  WriteLe32 (Csw + 8, Bot->HostLength - MIN (Processed, Bot->HostLength));
  Csw[12] = Bot->Status;
}
