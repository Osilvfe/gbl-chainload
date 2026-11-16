#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>

#define __PARTITION_TABLE_H__
#define MAX_GPT_NAME_SIZE 36
typedef struct {
    CHAR16 Suffix[3];
} Slot;
Slot GetCurrentSlotSuffix (VOID);

#include "../../../GblChainloadPkg/Library/ProtocolHookLib/ProfileOverlay.c"
#include "../../../GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c"
#include "../../../GblChainloadPkg/Application/GblChainload/BootFlow.c"

struct GblManifest gManifest;
EFI_HANDLE gImageHandle = (EFI_HANDLE)1;
EFI_GUID gQcomQseecomProtocolGuid = EFI_QSEECOM_PROTOCOL_GUID;
static EFI_BOOT_SERVICES mBootServices;
EFI_BOOT_SERVICES *gBS = &mBootServices;

static struct gbl_mode2_profile mProfile;
static BOOLEAN mMode2;
static EFI_STATUS mProfileStatus;
static EFI_STATUS mCachedStatus;
static EFI_STATUS mHookStatus;
static EFI_STATUS mLoadStatus;
static PATCH_WORST mPatchWorst;
static unsigned mProfileCalls;
static unsigned mCachedCalls;
static unsigned mDynamicCalls;
static unsigned mPatchCalls;
static unsigned mHookCalls;
static unsigned mLoadCalls;
static unsigned mStartCalls;
static unsigned mFreeCalls;
static int mPatchMode;
static char mWarning[128];
static QCOM_QSEECOM_PROTOCOL mQseecom;
static EFI_STATUS mStartStatus;
static UINT32 mStartHandle;
static UINT8 mLastSend[GBL_KM_LEN_SET_BOOT_STATE];
static UINT32 mLastSendLen;
static unsigned mSendCalls;

VOID *EFIAPI CopyMem(VOID *Destination, CONST VOID *Source, UINTN Length) {
    return memcpy(Destination, Source, Length);
}

VOID *EFIAPI ZeroMem(VOID *Buffer, UINTN Length) {
    return memset(Buffer, 0, Length);
}

INTN EFIAPI CompareMem(CONST VOID *FirstBuffer, CONST VOID *SecondBuffer,
                       UINTN Length) {
    return memcmp(FirstBuffer, SecondBuffer, Length);
}

INTN EFIAPI AsciiStrCmp(CONST CHAR8 *FirstString, CONST CHAR8 *SecondString) {
    return strcmp(FirstString, SecondString);
}

RETURN_STATUS EFIAPI AsciiStrCpyS(CHAR8 *Destination, UINTN DestMax,
                                  CONST CHAR8 *Source) {
    assert(strlen(Source) < DestMax);
    strcpy(Destination, Source);
    return RETURN_SUCCESS;
}

UINTN EFIAPI __StrLen(CONST CHAR16 *String) {
    UINTN Length = 0;
    while (String[Length] != 0) Length++;
    return Length;
}

VOID FortifyFail(CONST CHAR8 *Name, CONST UINTN Line) {
    (VOID)Name;
    (VOID)Line;
    abort();
}

UINT32 EFIAPI CalculateCrc32(VOID *Buffer, UINTN Length) {
    (VOID)Buffer;
    (VOID)Length;
    return 0;
}

RETURN_STATUS EFIAPI __StrnCpyS(CHAR16 *Destination, UINTN DestMax,
                                CONST CHAR16 *Source, UINTN Length) {
    UINTN Index = 0;
    while (Index < Length && Source[Index] != 0) {
        assert(Index + 1 < DestMax);
        Destination[Index] = Source[Index];
        Index++;
    }
    Destination[Index] = 0;
    return RETURN_SUCCESS;
}

RETURN_STATUS EFIAPI __StrnCatS(CHAR16 *Destination, UINTN DestMax,
                                CONST CHAR16 *Source, UINTN Length) {
    UINTN Used = StrLen(Destination);
    return __StrnCpyS(Destination + Used, DestMax - Used, Source, Length);
}

UINTN EFIAPI Print(CONST CHAR16 *Format, ...) {
    (VOID)Format;
    return 0;
}

UINTN EFIAPI AsciiPrint(CONST CHAR8 *Format, ...) {
    (VOID)Format;
    return 0;
}

VOID EFIAPI FreePool(VOID *Buffer) {
    mFreeCalls++;
    free(Buffer);
}

Slot GetCurrentSlotSuffix(VOID) {
    Slot Active = {{'_', 'a', 0}};
    return Active;
}

EFI_STATUS EFIAPI LogFsInit(VOID) {
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI LogFsClose(VOID) {
    return EFI_SUCCESS;
}

VOID EFIAPI GblPayload_LogProvenance(EFI_HANDLE ImageHandle) {
    (VOID)ImageHandle;
}

EFI_STATUS EFIAPI GblPayload_LoadManifest(EFI_HANDLE ImageHandle,
                                          struct GblManifest *Manifest) {
    (VOID)ImageHandle;
    Manifest->WantFakelockHook = FALSE;
    Manifest->WantProfileSpoof = mMode2;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI GblPayload_LoadMode2Profile(EFI_HANDLE ImageHandle,
                                              struct gbl_mode2_profile *Profile) {
    (VOID)ImageHandle;
    mProfileCalls++;
    if (!EFI_ERROR(mProfileStatus)) *Profile = mProfile;
    return mProfileStatus;
}

static VOID AllocateAbl(VOID **Pe, UINT32 *PeSize) {
    *PeSize = 16;
    *Pe = calloc(1, *PeSize);
    assert(*Pe != NULL);
}

EFI_STATUS EFIAPI GblPayload_LoadCachedAbl(EFI_HANDLE ImageHandle,
                                           VOID **Pe, UINT32 *PeSize) {
    (VOID)ImageHandle;
    mCachedCalls++;
    assert(!mMode2 || mProfileCalls == 1);
    if (!EFI_ERROR(mCachedStatus)) AllocateAbl(Pe, PeSize);
    return mCachedStatus;
}

EFI_STATUS EFIAPI AblUnwrap_LoadFromPartition(CHAR16 *PartitionName,
                                              VOID **Pe, UINT32 *PeSize) {
    (VOID)PartitionName;
    mDynamicCalls++;
    assert(!mMode2 || mProfileCalls == 1);
    AllocateAbl(Pe, PeSize);
    return EFI_SUCCESS;
}

void DynamicPatchLib_EnsureInitWithMode2(int EnableMode2) {
    mPatchMode = EnableMode2;
}

void DynamicPatch_Apply(uint8_t *Buffer, uint32_t Size, PATCH_RESULT *Result) {
    (VOID)Buffer;
    (VOID)Size;
    mPatchCalls++;
    memset(Result, 0, sizeof(*Result));
    Result->WorstOutcome = mPatchWorst;
}

EFI_STATUS EFIAPI ProtocolHook_InstallAll(HOOK_INSTALL_RESULT *Result) {
    (VOID)Result;
    mHookCalls++;
    assert(gMode2HasProfile == mMode2);
    return mHookStatus;
}

VOID GblFastbootSetMode2Warning(CONST CHAR8 *Warning) {
    snprintf(mWarning, sizeof(mWarning), "%s", Warning != NULL ? Warning : "");
}

BOOLEAN FakelockOverlay_ShouldDropQseeOplusSec(UINT32 CmdId,
                                               EFI_STATUS *FakeStatus) {
    (VOID)CmdId;
    (VOID)FakeStatus;
    assert(!gManifest.WantFakelockHook);
    return FALSE;
}

BOOLEAN FakelockOverlay_ShouldDropKmDeviceStateWrite(UINT32 CmdId,
                                                     EFI_STATUS *FakeStatus) {
    (VOID)CmdId;
    (VOID)FakeStatus;
    assert(!gManifest.WantFakelockHook);
    return FALSE;
}

static EFI_STATUS EFIAPI MockLoadImage(BOOLEAN BootPolicy, EFI_HANDLE Parent,
    EFI_DEVICE_PATH_PROTOCOL *Path, VOID *Buffer, UINTN Size, EFI_HANDLE *Handle) {
    (VOID)BootPolicy;
    (VOID)Parent;
    (VOID)Path;
    assert(Buffer != NULL && Size == 16);
    assert(gMode2HasProfile == mMode2);
    mLoadCalls++;
    *Handle = (EFI_HANDLE)2;
    return mLoadStatus;
}

static EFI_STATUS EFIAPI MockStartImage(EFI_HANDLE Handle, UINTN *ExitSize,
                                         CHAR16 **ExitData) {
    (VOID)ExitSize;
    (VOID)ExitData;
    assert(Handle == (EFI_HANDLE)2);
    assert(gMode2HasProfile == mMode2);
    mStartCalls++;
    return EFI_ABORTED;
}

static EFI_STATUS EFIAPI MockUnloadImage(EFI_HANDLE Handle) {
    assert(Handle == (EFI_HANDLE)2);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockStartApp(QCOM_QSEECOM_PROTOCOL *This,
                                      CHAR8 *AppName, UINT32 *Handle) {
    (VOID)This;
    (VOID)AppName;
    if (Handle != NULL) *Handle = mStartHandle;
    return mStartStatus;
}

static EFI_STATUS EFIAPI MockSendCmd(QCOM_QSEECOM_PROTOCOL *This, UINT32 Handle,
    UINT8 *SendBuf, UINT32 SendLen, UINT8 *RspBuf, UINT32 RspLen) {
    (VOID)This;
    (VOID)Handle;
    (VOID)RspBuf;
    (VOID)RspLen;
    assert(SendLen <= sizeof(mLastSend));
    memcpy(mLastSend, SendBuf, SendLen);
    mLastSendLen = SendLen;
    mSendCalls++;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockLocateProtocol(EFI_GUID *Protocol,
                                             VOID *Registration, VOID **Interface) {
    (VOID)Registration;
    assert(Protocol == &gQcomQseecomProtocolGuid);
    *Interface = &mQseecom;
    return EFI_SUCCESS;
}

static VOID ResetFlow(BOOLEAN Mode2) {
    mMode2 = Mode2;
    mProfileStatus = EFI_SUCCESS;
    mCachedStatus = EFI_SUCCESS;
    mHookStatus = EFI_SUCCESS;
    mLoadStatus = EFI_SUCCESS;
    mPatchWorst = 0;
    mProfileCalls = mCachedCalls = mDynamicCalls = mPatchCalls = 0;
    mHookCalls = mLoadCalls = mStartCalls = mFreeCalls = 0;
    mPatchMode = -1;
}

static VOID TestBootFlow(VOID) {
    EFI_STATUS Errors[] = {EFI_NOT_FOUND, EFI_LOAD_ERROR, EFI_DEVICE_ERROR};
    for (UINTN Index = 0; Index < sizeof(Errors) / sizeof(Errors[0]); Index++) {
        ResetFlow(TRUE);
        ProfileOverlay_SetProfile(&mProfile);
        mProfileStatus = Errors[Index];
        assert(BootFlowChainLoad() == Errors[Index]);
        assert(mProfileCalls == 1 && mCachedCalls == 0 && mDynamicCalls == 0);
        assert(mPatchCalls == 0 && mHookCalls == 0);
        assert(mLoadCalls == 0 && mStartCalls == 0 && mFreeCalls == 0);
        assert(!gMode2HasProfile);
        assert(strstr(mWarning, Index == 0 ? "MISSING" : "INVALID") != NULL);
        assert(strstr(mWarning, "chainload aborted") != NULL);
    }

    ResetFlow(TRUE);
    assert(BootFlowChainLoad() == EFI_LOAD_ERROR);
    assert(mProfileCalls == 1 && mCachedCalls == 1 && mDynamicCalls == 0);
    assert(mHookCalls == 1 && mLoadCalls == 1 && mStartCalls == 1);
    assert(mFreeCalls == 1 && mWarning[0] == 0 && !gMode2HasProfile);

    ResetFlow(TRUE);
    mCachedStatus = EFI_NOT_FOUND;
    assert(BootFlowChainLoad() == EFI_LOAD_ERROR);
    assert(mDynamicCalls == 1 && mPatchCalls == 1 && mPatchMode == 1);
    assert(mStartCalls == 1 && mFreeCalls == 1 && !gMode2HasProfile);

    ResetFlow(TRUE);
    mCachedStatus = EFI_NOT_FOUND;
    mPatchWorst = PATCH_RESULT_MANDATORY_MISS;
    assert(BootFlowChainLoad() == EFI_NOT_READY);
    assert(mPatchCalls == 1 && mHookCalls == 0 && mLoadCalls == 0);
    assert(mFreeCalls == 1 && !gMode2HasProfile);

    ResetFlow(TRUE);
    mHookStatus = EFI_NOT_READY;
    assert(BootFlowChainLoad() == EFI_NOT_READY);
    assert(mLoadCalls == 0 && mFreeCalls == 1 && !gMode2HasProfile);

    ResetFlow(TRUE);
    mLoadStatus = EFI_LOAD_ERROR;
    assert(BootFlowChainLoad() == EFI_LOAD_ERROR);
    assert(mStartCalls == 0 && mFreeCalls == 1 && !gMode2HasProfile);

    ResetFlow(FALSE);
    mProfileStatus = EFI_LOAD_ERROR;
    mCachedStatus = EFI_NOT_FOUND;
    ProfileOverlay_SetProfile(&mProfile);
    GblFastbootSetMode2Warning("stale warning");
    assert(BootFlowChainLoad() == EFI_LOAD_ERROR);
    assert(mProfileCalls == 0 && mStartCalls == 1 && mPatchMode == 0);
    assert(mWarning[0] == 0 && !gMode2HasProfile);
}

static VOID CheckSend(UINT32 Handle, UINT32 CmdId, UINT32 Length,
                       BOOLEAN ExpectRewrite) {
    UINT8 Buffer[GBL_KM_LEN_SET_BOOT_STATE];
    UINT8 Expected[GBL_KM_LEN_SET_BOOT_STATE];
    unsigned Before = mSendCalls;
    memset(Buffer, 0xAA, sizeof(Buffer));
    memcpy(Buffer, &CmdId, sizeof(CmdId));
    memcpy(Expected, Buffer, sizeof(Buffer));
    if (ExpectRewrite) {
        assert(gbl_profile_rewrite_km(CmdId, Expected, Length, &mProfile) == 1);
    }
    assert(mQseecom.QseecomSendCmd(&mQseecom, Handle, Buffer, Length, NULL, 0)
           == EFI_SUCCESS);
    assert(mSendCalls == Before + 1 && mLastSendLen == Length);
    assert(memcmp(Buffer, Expected, Length) == 0);
    assert(memcmp(mLastSend, Expected, Length) == 0);
    assert(gQseecomSendGuard.Depth == 0);
}

static VOID TestQseecom(VOID) {
    UINT32 Handle = 0;
    const UINT32 Commands[] = {GBL_KM_CMD_SET_ROT, GBL_KM_CMD_SET_VERSION,
                              GBL_KM_CMD_SET_BOOT_STATE, GBL_KM_CMD_SET_VBH};
    const UINT32 Lengths[] = {GBL_KM_LEN_SET_ROT, GBL_KM_LEN_SET_VERSION,
                             GBL_KM_LEN_SET_BOOT_STATE, GBL_KM_LEN_SET_VBH};
    gManifest.WantProfileSpoof = TRUE;
    ProfileOverlay_SetProfile(&mProfile);
    mQseecom.QseecomStartApp = MockStartApp;
    mQseecom.QseecomSendCmd = MockSendCmd;
    assert(InstallQseecomHook() == EFI_SUCCESS);

    CheckSend(9, GBL_KM_CMD_SET_BOOT_STATE, GBL_KM_LEN_SET_BOOT_STATE, FALSE);
    assert(gKeymasterHandle == 9 && gProfileKeymasterHandle == (UINT32)-1);

    mStartStatus = EFI_DEVICE_ERROR;
    mStartHandle = 9;
    assert(mQseecom.QseecomStartApp(&mQseecom, "keymaster", &Handle)
           == EFI_DEVICE_ERROR);
    CheckSend(9, GBL_KM_CMD_SET_ROT, GBL_KM_LEN_SET_ROT, FALSE);

    mStartStatus = EFI_SUCCESS;
    assert(mQseecom.QseecomStartApp(&mQseecom, "keymaster", NULL) == EFI_SUCCESS);
    CheckSend(0, GBL_KM_CMD_SET_VERSION, GBL_KM_LEN_SET_VERSION, FALSE);
    for (UINTN Index = 0; Index < 2; Index++) {
        mStartHandle = Index == 0 ? 0 : (UINT32)-1;
        assert(mQseecom.QseecomStartApp(&mQseecom, "keymaster", &Handle) == EFI_SUCCESS);
        CheckSend(mStartHandle, GBL_KM_CMD_SET_VBH, GBL_KM_LEN_SET_VBH, FALSE);
    }

    mStartHandle = 7;
    assert(mQseecom.QseecomStartApp(&mQseecom, "keymaster", &Handle) == EFI_SUCCESS);
    for (UINTN Index = 0; Index < sizeof(Commands) / sizeof(Commands[0]); Index++) {
        CheckSend(9, Commands[Index], Lengths[Index], FALSE);
        CheckSend(7, Commands[Index], Lengths[Index], TRUE);
        CheckSend(7, Commands[Index], Lengths[Index] - 1, FALSE);
    }

    mStartHandle = 9;
    assert(mQseecom.QseecomStartApp(&mQseecom, "other-ta", &Handle) == EFI_SUCCESS);
    CheckSend(9, GBL_KM_CMD_SET_BOOT_STATE, GBL_KM_LEN_SET_BOOT_STATE, FALSE);
    mStartStatus = EFI_DEVICE_ERROR;
    assert(mQseecom.QseecomStartApp(&mQseecom, "keymaster64", &Handle) == EFI_DEVICE_ERROR);
    CheckSend(9, GBL_KM_CMD_SET_BOOT_STATE, GBL_KM_LEN_SET_BOOT_STATE, FALSE);
    CheckSend(7, GBL_KM_CMD_SET_BOOT_STATE, GBL_KM_LEN_SET_BOOT_STATE, TRUE);
    mStartStatus = EFI_SUCCESS;
    assert(mQseecom.QseecomStartApp(&mQseecom, "keymaster64", &Handle) == EFI_SUCCESS);
    CheckSend(9, GBL_KM_CMD_SET_BOOT_STATE, GBL_KM_LEN_SET_BOOT_STATE, TRUE);
    CheckSend(7, GBL_KM_CMD_SET_BOOT_STATE, GBL_KM_LEN_SET_BOOT_STATE, FALSE);
    gManifest.WantProfileSpoof = FALSE;
    CheckSend(9, GBL_KM_CMD_SET_BOOT_STATE, GBL_KM_LEN_SET_BOOT_STATE, FALSE);
    gManifest.WantProfileSpoof = TRUE;
    ProfileOverlay_ClearProfile();
    CheckSend(9, GBL_KM_CMD_SET_BOOT_STATE, GBL_KM_LEN_SET_BOOT_STATE, FALSE);
    UINT8 SpssBuffer[GBL_SPSS_INFO_LEN] = {0};
    assert(!ProfileOverlay_RewriteSpss(SpssBuffer, sizeof(SpssBuffer)));
}

int main(void) {
    mProfile.version = 1;
    memset(mProfile.rot_digest, 0x11, sizeof(mProfile.rot_digest));
    memset(mProfile.pubkey_digest, 0x22, sizeof(mProfile.pubkey_digest));
    memset(mProfile.vbh, 0x33, sizeof(mProfile.vbh));
    mBootServices.LoadImage = MockLoadImage;
    mBootServices.StartImage = MockStartImage;
    mBootServices.UnloadImage = MockUnloadImage;
    mBootServices.LocateProtocol = MockLocateProtocol;
    TestBootFlow();
    TestQseecom();
    puts("PASS: mode-2 profile failure, lifecycle, and TA isolation");
    return 0;
}
