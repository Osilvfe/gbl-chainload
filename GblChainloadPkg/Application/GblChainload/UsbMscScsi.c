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
  Bot->Write = Media->Write;
  Bot->Flush = Media->Flush;
  Bot->Writable = Media->Writable;
  Bot->ReadContext = Media->Context;
  Bot->IsPartition = Media->IsPartition;
  Bot->IsFile = Media->IsFile;
  Bot->PeripheralType = Media->PeripheralType;
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

#define GBL_MSC_MODE_HEADER_6 4U
#define GBL_MSC_MODE_HEADER_10 8U
#define GBL_MSC_MODE_BLOCK_DESCRIPTOR 8U
#define GBL_MSC_MODE_PAGE_RECOVERY 0x01U
#define GBL_MSC_MODE_PAGE_CACHING 0x08U
#define GBL_MSC_MODE_PAGE_CONTROL 0x0aU
#define GBL_MSC_MODE_PAGE_INFORMATIONAL 0x1cU
#define GBL_MSC_MODE_PAGE_CAPABILITIES 0x2aU
#define GBL_MSC_MODE_PAGE_ALL 0x3fU

/*
 * SBC-3 read/write error recovery page. Every recovery action stays off: the
 * LUN either returns CHECK CONDITION for a bad block or, on writable media,
 * reports the failure to the host, and never silently retries or reallocates
 * a block. That is the only honest answer for an image or a partition we
 * cannot spare, and it keeps a failed read from being hidden behind a zero
 * fill. The page exists so tools that probe for it do not treat the LUN as
 * SCSI-1.
 */
STATIC UINT32
RecoveryPage (OUT UINT8 *Page)
{
  ZeroMem (Page, 12);
  Page[0] = GBL_MSC_MODE_PAGE_RECOVERY;
  Page[1] = 10;
  return 12;
}

/*
 * SPC-3 informational exceptions control page. Nothing is logged and no
 * exception condition is generated: the medium cannot develop a predictive
 * failure the device could report, so every field is zero and TEST stays off.
 */
STATIC UINT32
InformationalPage (OUT UINT8 *Page)
{
  ZeroMem (Page, 10);
  Page[0] = GBL_MSC_MODE_PAGE_INFORMATIONAL;
  Page[1] = 8;
  return 10;
}

/*
 * SBC-3 caching page: no read cache, write cache disabled (WCE=0) so hosts
 * report "write through", and no prefetch. WCE stays 0 on writable media too:
 * a write that the host already saw acknowledged must survive a power cut, so
 * every write reaches the medium before the command completes and
 * SYNCHRONIZE CACHE only has the medium's own cache to flush. Advertising a
 * write-back cache we do not keep would be a promise this device cannot hold.
 * Every value is fixed; the page is the same for current and default values.
 */
STATIC UINT32
CachingPage (OUT UINT8 *Page)
{
  ZeroMem (Page, 20);
  Page[0] = GBL_MSC_MODE_PAGE_CACHING;
  Page[1] = 18;
  return 20;
}

/*
 * SPC-3 control page: fixed-format sense data (D_SENSE=0, which is what this
 * device returns) and no saved log parameters (GLTSD=1).
 */
STATIC UINT32
ControlPage (OUT UINT8 *Page)
{
  ZeroMem (Page, 10);
  Page[0] = GBL_MSC_MODE_PAGE_CONTROL;
  Page[1] = 8;
  Page[2] = 0x04;
  return 10;
}

/*
 * MMC "CD/DVD capabilities and mechanical status" page (0x2A). Optical tools
 * ask for it to learn what the drive can do; answering ILLEGAL REQUEST makes
 * them treat the LUN as a bare SCSI-1 drive and skip CD-specific handling.
 *
 * Only the properties of this LUN are described: a read-only data disc with
 * no CD-R/RW writing, no CD-DA audio, no C2 pointers, no DVD and no
 * sub-channel support. Every capability bit is therefore clear, which is the
 * truthful answer for an image that cannot be written, played or scanned for
 * C2 errors. The speed and mechanical-status fields are reported as zero
 * (not available) rather than guessed.
 */
STATIC UINT32
CapabilitiesPage (OUT UINT8 *Page)
{
  ZeroMem (Page, 20);
  Page[0] = GBL_MSC_MODE_PAGE_CAPABILITIES;
  Page[1] = 18;
  return 20;
}

/*
 * MODE SENSE(6)/(10). Only current and default values exist, so page control
 * 0 and 2 return the same bytes and 1 (changeable) / 3 (saved) are rejected.
 * Page code 0 returns the header and block descriptor only, page code 0x3f
 * returns every implemented page, and the block descriptor is suppressed by
 * the DBD bit.
 */
STATIC UINT32
ModeSense (
  IN OUT GBL_MSC_BOT *Bot,
  IN CONST UINT8 *Command,
  IN BOOLEAN Ten
  )
{
  UINT8 PageControl = (UINT8)(Command[2] >> 6);
  UINT8 PageCode = (UINT8)(Command[2] & 0x3f);
  BOOLEAN NoBlockDescriptor = (Command[1] & 0x08) != 0;
  UINT32 Allocation = Ten ? (((UINT32)Command[7] << 8) | Command[8]) : Command[4];
  UINT32 Header = Ten ? GBL_MSC_MODE_HEADER_10 : GBL_MSC_MODE_HEADER_6;
  UINT32 BlockDescriptor = NoBlockDescriptor ? 0 : GBL_MSC_MODE_BLOCK_DESCRIPTOR;
  UINT32 BlockSize = Bot->BlockSize == 0 ? GBL_MSC_BLOCK_SIZE : Bot->BlockSize;
  UINT64 Blocks;
  UINT32 Length;
  UINT8 *Descriptor;
  UINT8 *Page;

  if ((PageControl != 0 && PageControl != 2) ||
      (PageCode != 0 && PageCode != GBL_MSC_MODE_PAGE_RECOVERY &&
       PageCode != GBL_MSC_MODE_PAGE_CACHING && PageCode != GBL_MSC_MODE_PAGE_CONTROL &&
       PageCode != GBL_MSC_MODE_PAGE_INFORMATIONAL && PageCode != GBL_MSC_MODE_PAGE_ALL &&
       !(PageCode == GBL_MSC_MODE_PAGE_CAPABILITIES &&
         Bot->PeripheralType == GBL_MSC_PERIPHERAL_CDROM))) {
    FailCommand (Bot, 5, 0x24);
    return 0;
  }
  Page = Bot->Response + Header + BlockDescriptor;
  Length = 0;
  /* Pages are emitted in ascending page-code order, as hosts expect. */
  if (PageCode == GBL_MSC_MODE_PAGE_RECOVERY || PageCode == GBL_MSC_MODE_PAGE_ALL) {
    Length += RecoveryPage (Page + Length);
  }
  if (PageCode == GBL_MSC_MODE_PAGE_CACHING || PageCode == GBL_MSC_MODE_PAGE_ALL) {
    Length += CachingPage (Page + Length);
  }
  if (PageCode == GBL_MSC_MODE_PAGE_CONTROL || PageCode == GBL_MSC_MODE_PAGE_ALL) {
    Length += ControlPage (Page + Length);
  }
  if (PageCode == GBL_MSC_MODE_PAGE_INFORMATIONAL || PageCode == GBL_MSC_MODE_PAGE_ALL) {
    Length += InformationalPage (Page + Length);
  }
  if (Bot->PeripheralType == GBL_MSC_PERIPHERAL_CDROM &&
      (PageCode == GBL_MSC_MODE_PAGE_CAPABILITIES || PageCode == GBL_MSC_MODE_PAGE_ALL)) {
    Length += CapabilitiesPage (Page + Length);
  }
  if (Ten) {
    Bot->Response[0] = (UINT8)((Header + BlockDescriptor + Length - 2) >> 8);
    Bot->Response[1] = (UINT8)(Header + BlockDescriptor + Length - 2);
    Bot->Response[2] = 0;
    Bot->Response[3] = Bot->Writable ? 0 : 0x80;
    Bot->Response[4] = 0;
    Bot->Response[5] = 0;
    Bot->Response[6] = (UINT8)(BlockDescriptor >> 8);
    Bot->Response[7] = (UINT8)BlockDescriptor;
  } else {
    Bot->Response[0] = (UINT8)(Header + BlockDescriptor + Length - 1);
    Bot->Response[1] = 0;
    Bot->Response[2] = Bot->Writable ? 0 : 0x80;
    Bot->Response[3] = (UINT8)BlockDescriptor;
  }
  if (BlockDescriptor != 0) {
    Descriptor = Bot->Response + Header;
    Blocks = Bot->DiskSize / BlockSize;
    Descriptor[0] = 0;
    WriteBe24 (Descriptor + 1, (UINT32)MIN (Blocks, 0xffffffU));
    Descriptor[4] = 0;
    WriteBe24 (Descriptor + 5, BlockSize);
  }
  return MIN (Header + BlockDescriptor + Length, Allocation);
}

/*
 * Minimal MMC GET CONFIGURATION: no feature descriptors, a single CD-ROM
 * profile. Linux reads this before it treats the LUN as an optical drive.
 */
STATIC UINT32
GetConfiguration (IN OUT GBL_MSC_BOT *Bot, IN CONST UINT8 *Command)
{
  UINT8 RequestType = (UINT8)(Command[1] & 0x03);
  UINT16 Feature = ((UINT16)Command[2] << 8) | Command[3];
  UINT32 Allocation = ((UINT32)Command[7] << 8) | Command[8];

  if (Bot->PeripheralType != GBL_MSC_PERIPHERAL_CDROM) {
    FailCommand (Bot, 5, 0x20);
    return 0;
  }
  if (RequestType == 3) {
    FailCommand (Bot, 5, 0x24);
    return 0;
  }
  if (RequestType == 2 && Feature != 0) {
    /* The requested feature is not implemented: an empty feature list. */
    return MIN (4U, Allocation);
  }
  /*
   * The data length counts the bytes that follow it, so 4 covers the two
   * reserved bytes plus the current profile; there are no features.
   */
  Bot->Response[3] = 4;
  Bot->Response[6] = 0;
  Bot->Response[7] = 0x08;
  return MIN (8U, Allocation);
}

/*
 * MMC GET EVENT STATUS NOTIFICATION. The LUN has no asynchronous events: the
 * medium cannot be removed, never changes and the tray state is fixed, so
 * every poll is answered with "no event available" (NEA) and an event
 * descriptor length of 0. Reporting a media event instead would make the host
 * believe the image had been ejected or replaced and drop the cached capacity.
 *
 * The supported-event-class mask still advertises the media class, which is
 * what tells a poller that this drive would report such events at all.
 */
STATIC UINT32
GetEventStatusNotification (IN OUT GBL_MSC_BOT *Bot, IN CONST UINT8 *Command)
{
  UINT32 Allocation = ((UINT32)Command[7] << 8) | Command[8];

  if (Bot->PeripheralType != GBL_MSC_PERIPHERAL_CDROM) {
    FailCommand (Bot, 5, 0x20);
    return 0;
  }
  /* Event descriptor length 0, NEA set, notification class 0 (no event). */
  Bot->Response[2] = 0x80;
  /* Supported event classes: bit 4 of the 24-bit mask is the media class. */
  Bot->Response[5] = 0x10;
  /* No event code follows because no descriptor is returned. */
  return MIN (8U, Allocation);
}

/*
 * MODE SELECT(6)/(10) only validates: the LUN geometry, the page contents and
 * the write-cache policy are fixed, so a well-formed parameter list that asks
 * for the values already in effect is accepted and changes nothing. Anything
 * the device cannot honour (a different write-cache state, an unknown page, a
 * block descriptor that does not match the LUN) is INVALID FIELD IN PARAMETER
 * LIST, and a list that does not parse is a PARAMETER LIST LENGTH ERROR.
 *
 * The list arrives through GblMscBotWrite in transfer-sized chunks; the check
 * runs once the last byte is in, before the CSW is queued.
 */
STATIC VOID
ModeSelectValidate (IN OUT GBL_MSC_BOT *Bot, IN BOOLEAN Ten)
{
  UINT32 Header = Ten ? GBL_MSC_MODE_HEADER_10 : GBL_MSC_MODE_HEADER_6;
  UINT32 BlockDescriptor;
  UINT32 Offset;
  UINT32 Size;
  UINT8 PageCode;

  if (Bot->OutLength < Header) {
    FailCommand (Bot, 5, 0x1a);
    return;
  }
  BlockDescriptor = Ten
                    ? (((UINT32)Bot->Response[6] << 8) | Bot->Response[7])
                    : Bot->Response[3];
  if (BlockDescriptor != 0 && BlockDescriptor != GBL_MSC_MODE_BLOCK_DESCRIPTOR) {
    FailCommand (Bot, 5, 0x26);
    return;
  }
  if (Header + BlockDescriptor > Bot->OutLength) {
    FailCommand (Bot, 5, 0x1a);
    return;
  }
  Offset = Header + BlockDescriptor;
  while (Offset + 2 <= Bot->OutLength) {
    PageCode = (UINT8)(Bot->Response[Offset] & 0x3f);
    Size = Bot->Response[Offset + 1];
    if (Offset + 2 + Size > Bot->OutLength) {
      FailCommand (Bot, 5, 0x1a);
      return;
    }
    switch (PageCode) {
    case GBL_MSC_MODE_PAGE_RECOVERY:
    case GBL_MSC_MODE_PAGE_CONTROL:
    case GBL_MSC_MODE_PAGE_INFORMATIONAL:
      /* Every field is fixed; the requested values are ignored. */
      break;
    case GBL_MSC_MODE_PAGE_CACHING:
      /* The write cache cannot be switched on: WCE must stay clear. */
      if (Size < 2 || (Bot->Response[Offset + 2] & 0x04) != 0) {
        FailCommand (Bot, 5, 0x26);
        return;
      }
      break;
    default:
      FailCommand (Bot, 5, 0x26);
      return;
    }
    Offset += 2 + Size;
  }
  if (Offset != Bot->OutLength) {
    FailCommand (Bot, 5, 0x1a);
  }
}

/*
 * TOC addresses are a plain LBA or MSF, which counts 75 frames per second from
 * 00:02:00 because of the 150-frame pregap.
 */
STATIC VOID
TocAddress (
  OUT UINT8 *Field,
  IN UINT32 Lba,
  IN BOOLEAN Msf
  )
{
  UINT32 Frames;

  if (!Msf) {
    WriteBe32 (Field, Lba);
    return;
  }
  Frames = Lba + 150;
  Field[0] = 0;
  Field[1] = (UINT8)(Frames / (75 * 60));
  Field[2] = (UINT8)((Frames / 75) % 60);
  Field[3] = (UINT8)(Frames % 75);
}

/*
 * MMC READ TOC/PMA/ATIP: the image is one data track in one session that
 * starts at LBA 0. The lead-out entry (track 0xAA) carries the address just
 * past the last block, and that is where Linux gets the medium size from:
 * cdrom_get_last_written asks for track 0xAA in MSF form and the resulting
 * value becomes the disc capacity. A wrong lead-out makes the host believe the
 * disc is larger than the image and probe blocks past its end.
 *
 * Format 0 (TOC) and format 1 (multi-session info) share this layout, so an
 * unimplemented format falls back to it instead of failing: hosts only need
 * the track and lead-out entries of a single-session data disc.
 */
STATIC UINT32
ReadToc (IN OUT GBL_MSC_BOT *Bot, IN CONST UINT8 *Command)
{
  UINT8 Format = (UINT8)(Command[2] & 0x0f);
  BOOLEAN Msf = (Command[1] & 0x02) != 0;
  UINT8 StartTrack = Command[6];
  UINT32 Allocation = ((UINT32)Command[7] << 8) | Command[8];
  UINT32 BlockSize = Bot->BlockSize == 0 ? GBL_MSC_BLOCK_SIZE : Bot->BlockSize;
  UINT32 LeadOut = (UINT32)MIN (Bot->DiskSize / BlockSize, MAX_UINT32);
  UINT32 Length = 0;
  UINT8 *Entry;

  if (Bot->PeripheralType != GBL_MSC_PERIPHERAL_CDROM) {
    FailCommand (Bot, 5, 0x20);
    return 0;
  }
  (VOID)Format;
  /* First/last track (format 0) and first/last session (format 1). */
  Bot->Response[2] = 1;
  Bot->Response[3] = 1;
  Entry = Bot->Response + 4;
  if (StartTrack != 0xaa) {
    /* The only track: data (CONTROL=4), positioned by LBA (ADR=1), at LBA 0. */
    Entry[0] = 0;
    Entry[1] = 0x14;
    Entry[2] = 1;
    Entry[3] = 0;
    TocAddress (Entry + 4, 0, Msf);
    Entry += 8;
    Length += 8;
  }
  Entry[0] = 0;
  Entry[1] = 0x14;
  Entry[2] = 0xaa;
  Entry[3] = 0;
  TocAddress (Entry + 4, LeadOut, Msf);
  Length += 8;
  /* The data length counts everything after the length field. */
  Bot->Response[0] = (UINT8)((2 + Length) >> 8);
  Bot->Response[1] = (UINT8)(2 + Length);
  return MIN (4U + Length, Allocation);
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
      Bot->Response[0] = Bot->PeripheralType;
      Bot->Response[2] = 5;
      Bot->Response[3] = 2;
      Bot->Response[4] = 31;
      if (Bot->PeripheralType == GBL_MSC_PERIPHERAL_CDROM) {
        CopyMem (Bot->Response + 8, "GBL     ISO IMAGE       0010", 28);
      } else if (Bot->IsFile) {
        CopyMem (Bot->Response + 8, "GBL     FILE IMAGE      0010", 28);
      } else if (Bot->IsPartition) {
        CopyMem (Bot->Response + 8, "GBL     PARTITION       0010", 28);
      } else {
        CopyMem (Bot->Response + 8, "GBL     RAM DISK        0010", 28);
      }
      Length = 36;
    } else {
      FailCommand (Bot, 5, 0x24);
    }
    return MIN (Length, ((UINT32)Command[3] << 8) | Command[4]);
  case 0x1a:
    *Direction = GblMscDataIn;
    return ModeSense (Bot, Command, FALSE);
  case 0x5a:
    *Direction = GblMscDataIn;
    return ModeSense (Bot, Command, TRUE);
  case 0x15:
  case 0x55: {
    /*
     * The parameter list is small (a header, an optional block descriptor and
     * a few pages), so it is buffered whole and validated once it is complete.
     * A list longer than the buffer is refused before the data stage, which
     * keeps a host from streaming an unbounded amount into the device.
     */
    UINT32 ListLength = Opcode == 0x15
                        ? Command[4]
                        : (((UINT32)Command[7] << 8) | Command[8]);

    if (ListLength > sizeof (Bot->Response)) {
      FailCommand (Bot, 5, 0x1a);
      return 0;
    }
    if (ListLength != 0) {
      Bot->ParamList = Opcode == 0x15 ? 1 : 2;
      *Direction = GblMscDataOut;
    }
    return ListLength;
  }
  case 0x43:
    *Direction = GblMscDataIn;
    return ReadToc (Bot, Command);
  case 0x46:
    *Direction = GblMscDataIn;
    return GetConfiguration (Bot, Command);
  case 0x4a:
    *Direction = GblMscDataIn;
    return GetEventStatusNotification (Bot, Command);
  case 0x1b:
    if ((Command[4] & 3) == 2) {
      if (Bot->PreventRemoval) {
        FailCommand (Bot, 5, 0x53);
        Bot->Ascq = 2;
      } else if (EFI_ERROR (GblMscBotFlush (Bot))) {
        /* The sense data is set: the media stays until the flush succeeds. */
        return 0;
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
    /*
     * A real barrier: on writable media this is the host telling us to commit
     * everything it has already had acknowledged. For a read-only LUN there is
     * nothing to flush and the command is a no-op success, which is what SBC
     * requires.
     */
    (VOID)GblMscBotFlush (Bot);
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
    /*
     * A refused command keeps its declared length: the transport then sends a
     * short (or zero-length) data stage so the host reads the CSW and the sense
     * data, instead of seeing a stalled endpoint. The data is dropped in
     * GblMscBotWrite because ToDisk/FromDisk stay clear.
     */
    if (Lba > Bot->DiskSize / BlockSize ||
        Blocks > Bot->DiskSize / BlockSize - Lba) {
      FailCommand (Bot, 5, 0x21);
    } else if (Write && !Bot->Writable) {
      FailCommand (Bot, 7, 0x27);
    } else {
      Bot->FromDisk = TRUE;
      Bot->DiskOffset = Lba * BlockSize;
      if (Write) {
        Bot->ToDisk = TRUE;
      }
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
  Bot->ToDisk = FALSE;
  Bot->ParamList = 0;
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
    } else if (Bot->Status != 0) {
      /*
       * The command failed before producing data. Do not stall the IN
       * endpoint: hosts treat a stalled data phase as a transport error and
       * run bulk-only reset recovery, which hides the sense data (Linux only
       * reports "I/O error", never the ILLEGAL REQUEST). Send a zero-length
       * data stage instead so the host reads the CSW and then REQUEST SENSE.
       */
      Bot->InLength = 0;
    } else if (Direction == GblMscNoData || DeviceLength == 0) {
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

/*
 * Map a media-layer status onto sense data. A missing or replaced medium has
 * its own key so a host does not treat it as a plain hardware fault; anything
 * else is an unrecovered read error or a write error, depending on direction.
 */
STATIC VOID
MediaSense (IN OUT GBL_MSC_BOT *Bot, IN EFI_STATUS Status, IN BOOLEAN Write)
{
  if (Status == EFI_NO_MEDIA) {
    FailCommand (Bot, 2, 0x3a);
  } else if (Status == EFI_MEDIA_CHANGED) {
    FailCommand (Bot, 6, 0x28);
  } else {
    FailCommand (Bot, 3, Write ? 0x0c : 0x11);
  }
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
        MediaSense (Bot, Status, FALSE);
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

EFI_STATUS
GblMscBotWrite (IN OUT GBL_MSC_BOT *Bot, IN UINT32 Offset,
                IN CONST VOID *Buffer, IN UINTN Size)
{
  EFI_STATUS Status;

  if (Size != 0 && Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (Offset > Bot->OutLength || Size > Bot->OutLength - Offset) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (Bot->ParamList != 0) {
    /*
     * MODE SELECT parameter list: buffer the chunk and validate once the
     * declared length has arrived. The list is bounded by sizeof (Response)
     * before the data stage starts, so this copy cannot overflow.
     */
    if (Offset + Size > sizeof (Bot->Response)) {
      FailCommand (Bot, 5, 0x1a);
      return EFI_BAD_BUFFER_SIZE;
    }
    CopyMem (Bot->Response + Offset, Buffer, Size);
    if (Offset + Size == Bot->OutLength && Bot->Status == 0) {
      ModeSelectValidate (Bot, Bot->ParamList == 2);
    }
    return EFI_SUCCESS;
  }
  if (!Bot->ToDisk) {
    /* Nothing consumes the data: a rejected or unsupported write is drained by
     * the transport and only the CSW reports the failure. */
    return EFI_SUCCESS;
  }
  if (!Bot->Writable || Bot->Write == NULL) {
    FailCommand (Bot, 7, 0x27);
    return EFI_WRITE_PROTECTED;
  }
  if (Bot->DiskOffset > Bot->DiskSize || Offset > Bot->DiskSize - Bot->DiskOffset ||
      Size > Bot->DiskSize - Bot->DiskOffset - Offset) {
    FailCommand (Bot, 5, 0x21);
    return EFI_BAD_BUFFER_SIZE;
  }
  Status = Bot->Write (Bot->ReadContext, Bot->DiskOffset + Offset, Buffer, Size);
  if (EFI_ERROR (Status)) {
    MediaSense (Bot, Status, TRUE);
    return Status;
  }
  Bot->Dirty = TRUE;
  return EFI_SUCCESS;
}

/*
 * SYNCHRONIZE CACHE, the eject path and teardown all funnel through here. A
 * read-only LUN has nothing to flush and always succeeds; a writable one asks
 * its backend for a real barrier, so a host that trusted an acknowledged write
 * gets it on the medium before the command completes.
 */
EFI_STATUS
GblMscBotFlush (IN OUT GBL_MSC_BOT *Bot)
{
  EFI_STATUS Status;

  if (Bot->Flush == NULL || !Bot->Writable) {
    Bot->Dirty = FALSE;
    return EFI_SUCCESS;
  }
  Status = Bot->Flush (Bot->ReadContext);
  if (EFI_ERROR (Status)) {
    if (Status == EFI_NO_MEDIA || Status == EFI_MEDIA_CHANGED) {
      MediaSense (Bot, Status, TRUE);
    } else {
      /* The medium is still there, so this is a hardware error, not a
       * medium error. */
      FailCommand (Bot, 4, 0x08);
    }
    return Status;
  }
  Bot->Dirty = FALSE;
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
