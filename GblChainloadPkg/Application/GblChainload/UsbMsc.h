#ifndef GBL_USB_MSC_H
#define GBL_USB_MSC_H

#include <Uefi.h>

#ifndef GBL_USB_MSC_PARTITION_EXPORT
#define GBL_USB_MSC_PARTITION_EXPORT 0
#endif

#define GBL_MSC_BLOCK_SIZE 512U
#define GBL_MSC_DISK_SIZE (1024U * 1024U)
#define GBL_MSC_CBW_SIZE 31U
#define GBL_MSC_CSW_SIZE 13U
#define GBL_MSC_NAME_SIZE 37U
#define GBL_MSC_MAX_LUNS 4U
#define GBL_MSC_SPEC_SIZE (GBL_MSC_NAME_SIZE * GBL_MSC_MAX_LUNS + GBL_MSC_MAX_LUNS)

typedef EFI_STATUS (*GBL_MSC_MEDIA_READ)(IN VOID *Context, IN UINT64 Offset,
                                        OUT VOID *Buffer, IN UINTN Size);

typedef struct {
  UINT64             Size;
  UINT32             BlockSize;
  GBL_MSC_MEDIA_READ Read;
  VOID               *Context;
  BOOLEAN            IsPartition;
  CHAR8              Name[GBL_MSC_NAME_SIZE];
  CHAR8              Serial[64];
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
  VOID               *ReadContext;
  CONST CHAR8        *Serial;
  BOOLEAN            IsPartition;
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
VOID GblMscBotCsw (IN CONST GBL_MSC_BOT *Bot, IN UINT32 Processed,
                   OUT UINT8 *Csw);
VOID GblUsbMscRegisterCommands (VOID);
BOOLEAN GblUsbMscRequested (VOID);
EFI_STATUS GblUsbMscPrepare (IN CONST CHAR8 *Partition);
VOID GblUsbMscCancel (VOID);
EFI_STATUS GblUsbMscRun (OUT BOOLEAN *Released);

#endif
