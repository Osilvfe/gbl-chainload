#ifndef GBL_USB_MSC_H
#define GBL_USB_MSC_H

#include <Uefi.h>

#ifndef GBL_USB_MSC_PARTITION_EXPORT
#define GBL_USB_MSC_PARTITION_EXPORT 0
#endif

/*
 * Write support is off unless a build asks for it. GBL_USB_MSC_WRITE enables
 * the write backend at all (WRITE(6/10/12/16), SYNCHRONIZE CACHE and MODE
 * SELECT); GBL_USB_MSC_WRITE_PARTITIONS additionally allows the "rw:name"
 * opt-in for a real partition. A normal build therefore contains no block
 * write path and refuses every write with DATA PROTECT.
 */
#ifndef GBL_USB_MSC_WRITE
#define GBL_USB_MSC_WRITE 0
#endif

#ifndef GBL_USB_MSC_WRITE_PARTITIONS
#define GBL_USB_MSC_WRITE_PARTITIONS 0
#endif

#define GBL_MSC_BLOCK_SIZE 512U
#define GBL_MSC_DISK_SIZE (1024U * 1024U)
#define GBL_MSC_CBW_SIZE 31U
#define GBL_MSC_CSW_SIZE 13U
#define GBL_MSC_NAME_SIZE 37U
#define GBL_MSC_PATH_SIZE 192U
#define GBL_MSC_MAX_LUNS 4U
#define GBL_MSC_SPEC_SIZE (GBL_MSC_PATH_SIZE * GBL_MSC_MAX_LUNS + GBL_MSC_MAX_LUNS)
#define GBL_MSC_ISO_BLOCK_SIZE 2048U
#define GBL_MSC_FILE_BLOCK_SIZE 512U
#define GBL_MSC_PERIPHERAL_DIRECT 0U
#define GBL_MSC_PERIPHERAL_CDROM 5U
#define GBL_MSC_RAM_SIZE (1024U * 1024U)
#define GBL_MSC_RAM_MIN_SIZE (64U * 1024U)
#define GBL_MSC_RAM_MAX_SIZE (64U * 1024U * 1024U)

typedef EFI_STATUS (*GBL_MSC_MEDIA_READ)(IN VOID *Context, IN UINT64 Offset,
                                        OUT VOID *Buffer, IN UINTN Size);
typedef EFI_STATUS (*GBL_MSC_MEDIA_WRITE)(IN VOID *Context, IN UINT64 Offset,
                                          IN CONST VOID *Buffer, IN UINTN Size);
typedef EFI_STATUS (*GBL_MSC_MEDIA_FLUSH)(IN VOID *Context);

typedef struct {
  UINT64              Size;
  UINT32              BlockSize;
  GBL_MSC_MEDIA_READ  Read;
  GBL_MSC_MEDIA_WRITE Write;
  GBL_MSC_MEDIA_FLUSH Flush;
  VOID                *Context;
  BOOLEAN             IsPartition;
  BOOLEAN             IsFile;
  BOOLEAN             Writable;
  UINT8               PeripheralType;
  CHAR8               Name[GBL_MSC_NAME_SIZE];
  CHAR8               Serial[64];
} GBL_MSC_MEDIA;

typedef enum {
  GblMscNoData,
  GblMscDataIn,
  GblMscDataOut
} GBL_MSC_DIRECTION;

typedef struct {
  CONST UINT8        *Disk;
  UINT64             DiskSize;
  GBL_MSC_MEDIA_READ  Read;
  GBL_MSC_MEDIA_WRITE Write;
  GBL_MSC_MEDIA_FLUSH Flush;
  VOID               *ReadContext;
  CONST CHAR8        *Serial;
  BOOLEAN            IsPartition;
  BOOLEAN            IsFile;
  UINT8              PeripheralType;
  UINT32             Tag;
  UINT32             HostLength;
  UINT32             InLength;
  UINT32             OutLength;
  UINT64             DiskOffset;
  UINT8              Response[96];
  UINT8              Status;
  UINT8              SenseKey;
  UINT8              Asc;
  UINT8              Ascq;
  UINT32             BlockSize;
  UINT8              LunCount;
  BOOLEAN            FromDisk;
  BOOLEAN            ToDisk;
  UINT8              ParamList;
  BOOLEAN            Writable;
  BOOLEAN            Dirty;
  BOOLEAN            HaltIn;
  BOOLEAN            HaltOut;
  BOOLEAN            NeedsReset;
  BOOLEAN            MediaPresent;
  BOOLEAN            PreventRemoval;
  BOOLEAN            EjectRequested;
} GBL_MSC_BOT;

VOID GblMscMakeDisk (OUT UINT8 *Disk);
EFI_STATUS GblMscMediaOpen (IN CONST CHAR8 *Name, OUT GBL_MSC_MEDIA *Media);
VOID GblMscMediaClose (IN OUT GBL_MSC_MEDIA *Media);
VOID GblMscBotInit (OUT GBL_MSC_BOT *Bot, IN CONST UINT8 *Disk, IN UINT64 Size);
VOID GblMscBotInitMedia (OUT GBL_MSC_BOT *Bot, IN CONST GBL_MSC_MEDIA *Media);
EFI_STATUS GblMscBotBegin (IN OUT GBL_MSC_BOT *Bot, IN CONST UINT8 *Cbw, IN UINTN Size);
EFI_STATUS GblMscBotRead (IN OUT GBL_MSC_BOT *Bot, IN UINT32 Offset,
                         OUT VOID *Buffer, IN UINTN Size);
EFI_STATUS GblMscBotWrite (IN OUT GBL_MSC_BOT *Bot, IN UINT32 Offset,
                          IN CONST VOID *Buffer, IN UINTN Size);
EFI_STATUS GblMscBotFlush (IN OUT GBL_MSC_BOT *Bot);
VOID GblMscBotCsw (IN CONST GBL_MSC_BOT *Bot, IN UINT32 Processed,
                   OUT UINT8 *Csw);
VOID GblUsbMscRegisterCommands (VOID);
BOOLEAN GblUsbMscRequested (VOID);
EFI_STATUS GblUsbMscPrepare (IN CONST CHAR8 *Partition);
VOID GblUsbMscCancel (VOID);
EFI_STATUS GblUsbMscRun (OUT BOOLEAN *Released);

#endif
