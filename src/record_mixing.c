#include "global.h"
#include "malloc.h"
#include "random.h"
#include "constants/items.h"
#include "text.h"
#include "item.h"
#include "task.h"
#include "save.h"
#include "load_save.h"
#include "pokemon.h"
#include "cable_club.h"
#include "link.h"
#include "link_rfu.h"
#include "battle_tower.h"
#include "window.h"
#include "mystery_event_script.h"
#include "mauville_old_man.h"
#include "sound.h"
#include "constants/songs.h"
#include "menu.h"
#include "overworld.h"
#include "field_screen_effect.h"
#include "fldeff_misc.h"
#include "script.h"
#include "event_data.h"
#include "lilycove_lady.h"
#include "strings.h"
#include "string_util.h"
#include "record_mixing.h"
#include "new_game.h"
#include "daycare.h"
#include "international_string_util.h"
#include "constants/battle_frontier.h"
#include "dewford_trend.h"

// Number of bytes of the record transferred at a time
#define BUFFER_CHUNK_SIZE 200

#define NUM_SWAP_COMBOS 3

// Used by several tasks in this file
#define tState        data[0]

struct RecordMixingHallRecords
{
    struct RankingHall1P hallRecords1P[HALL_FACILITIES_COUNT][FRONTIER_LVL_MODE_COUNT][HALL_RECORDS_COUNT * 2];
    struct RankingHall2P hallRecords2P[FRONTIER_LVL_MODE_COUNT][HALL_RECORDS_COUNT * 2];
};

struct PlayerRecordRS
{
    OldMan oldMan;
    struct DewfordTrend dewfordTrends[SAVED_TRENDS_COUNT];
    struct RSBattleTowerRecord battleTowerRecord;
    u16 giftItem;
};

struct PlayerRecordEmerald
{
    /* 0x1044 */ OldMan oldMan;
    /* 0x1084 */ struct DewfordTrend dewfordTrends[SAVED_TRENDS_COUNT];
    /* 0x1124 */ struct EmeraldBattleTowerRecord battleTowerRecord;
    /* 0x1210 */ u16 giftItem;
    /* 0x1214 */ LilycoveLady lilycoveLady;
    /* 0x1254 */ struct Apprentice apprentices[2];
    /* 0x12DC */ struct PlayerHallRecords hallRecords;
}; // 0x1444

union PlayerRecord
{
    struct PlayerRecordRS ruby;
    struct PlayerRecordEmerald emerald;
};

static bool8 sReadyToReceive;
static OldMan *sOldManSave;
static struct DewfordTrend *sDewfordTrendsSave;
static void *sBattleTowerSave;
static LilycoveLady *sLilycoveLadySave;
static void *sApprenticesSave;
static void *sBattleTowerSave_Duplicate;
static u32 sRecordStructSize;
#if FREE_RECORD_MIXING_HALL_RECORDS == FALSE
static struct PlayerHallRecords *sPartnerHallRecords[HALL_RECORDS_COUNT];
#endif //FREE_RECORD_MIXING_HALL_RECORDS

static EWRAM_DATA union PlayerRecord *sReceivedRecords = NULL;
static EWRAM_DATA union PlayerRecord *sSentRecord = NULL;

static void Task_RecordMixing_Main(u8);
static void Task_MixingRecordsRecv(u8);
static void Task_SendPacket(u8);
static void Task_CopyReceiveBuffer(u8);
static void Task_SendPacket_SwitchToReceive(u8);
static void *LoadPtrFromTaskData(const u16 *);
static void StorePtrInTaskData(void *, u16 *);
static u8 GetMultiplayerId_(void);
static void *GetPlayerRecvBuffer(u8);
static void ReceiveOldManData(OldMan *, size_t, u8);
static void ReceiveBattleTowerData(void *, size_t, u8);
static void ReceiveLilycoveLadyData(LilycoveLady *, size_t, u8);
static void ReceiveGiftItem(u16 *, u8 );
static void Task_DoRecordMixing(u8);
static void GetSavedApprentices(struct Apprentice *, struct Apprentice *);
static void ReceiveApprenticeData(struct Apprentice *, size_t, u32);
static void ReceiveRankingHallRecords(struct PlayerHallRecords *, size_t, u32);
static void SanitizeEmeraldBattleTowerRecord(struct EmeraldBattleTowerRecord *);
static void SanitizeRubyBattleTowerRecord(struct RSBattleTowerRecord *);

static const u8 sPlayerIdxOrders_2Player[] = {1, 0};

static const u8 sPlayerIdxOrders_3Player[][3] =
{
    {1, 2, 0},
    {2, 0, 1},
};

static const u8 sPlayerIdxOrders_4Player[][4] =
{
    {1, 0, 3, 2},
    {3, 0, 1, 2},
    {2, 0, 3, 1},
    {1, 3, 0, 2},
    {2, 3, 0, 1},
    {3, 2, 0, 1},
    {1, 2, 3, 0},
    {2, 3, 1, 0},
    {3, 2, 1, 0},
};

void RecordMixingPlayerSpotTriggered(void)
{
    CreateTask_EnterCableClubSeat(Task_RecordMixing_Main);
}

// these variables were const in R/S, but had to become changeable because of saveblocks changing RAM position
static void SetSrcLookupPointers(void)
{
    sOldManSave = &gSaveBlock1Ptr->oldMan;
    sDewfordTrendsSave = gSaveBlock1Ptr->dewfordTrends;
    sBattleTowerSave = &gSaveBlock2Ptr->frontier.towerPlayer;
    sLilycoveLadySave = &gSaveBlock1Ptr->lilycoveLady;
    sApprenticesSave = gSaveBlock2Ptr->apprentices;
    sBattleTowerSave_Duplicate = &gSaveBlock2Ptr->frontier.towerPlayer;
}

static void PrepareUnknownExchangePacket(struct PlayerRecordRS *dest)
{
    memcpy(&dest->oldMan, sOldManSave, sizeof(dest->oldMan));
    memcpy(dest->dewfordTrends, sDewfordTrendsSave, sizeof(dest->dewfordTrends));
    EmeraldBattleTowerRecordToRuby(sBattleTowerSave, &dest->battleTowerRecord);

    if (GetMultiplayerId() == 0)
        dest->giftItem = GetRecordMixingGift();
}

static void PrepareExchangePacketForRubySapphire(struct PlayerRecordRS *dest)
{
    memcpy(&dest->oldMan, sOldManSave, sizeof(dest->oldMan));
    SanitizeMauvilleOldManForRuby(&dest->oldMan);
    memcpy(dest->dewfordTrends, sDewfordTrendsSave, sizeof(dest->dewfordTrends));
    EmeraldBattleTowerRecordToRuby(sBattleTowerSave, &dest->battleTowerRecord);
    SanitizeRubyBattleTowerRecord(&dest->battleTowerRecord);

    if (GetMultiplayerId() == 0)
        dest->giftItem = GetRecordMixingGift();
}

static void PrepareExchangePacket(void)
{
    SetSrcLookupPointers();

    if (Link_AnyPartnersPlayingRubyOrSapphire())
    {
        if (LinkDummy_Return2() == 0)
            PrepareUnknownExchangePacket(&sSentRecord->ruby);
        else
            PrepareExchangePacketForRubySapphire(&sSentRecord->ruby);
    }
    else
    {
        memcpy(&sSentRecord->emerald.oldMan, sOldManSave, sizeof(sSentRecord->emerald.oldMan));
        memcpy(&sSentRecord->emerald.lilycoveLady, sLilycoveLadySave, sizeof(sSentRecord->emerald.lilycoveLady));
        memcpy(sSentRecord->emerald.dewfordTrends, sDewfordTrendsSave, sizeof(sSentRecord->emerald.dewfordTrends));
        memcpy(&sSentRecord->emerald.battleTowerRecord, sBattleTowerSave, sizeof(sSentRecord->emerald.battleTowerRecord));
        SanitizeEmeraldBattleTowerRecord(&sSentRecord->emerald.battleTowerRecord);

        if (GetMultiplayerId() == 0)
            sSentRecord->emerald.giftItem = GetRecordMixingGift();

        GetSavedApprentices(sSentRecord->emerald.apprentices, sApprenticesSave);
        GetPlayerHallRecords(&sSentRecord->emerald.hallRecords);
    }
}

static void ReceiveExchangePacket(u32 multiplayerId)
{
    if (Link_AnyPartnersPlayingRubyOrSapphire())
    {
        // Ruby/Sapphire
        ReceiveBattleTowerData(&sReceivedRecords->ruby.battleTowerRecord, sizeof(sReceivedRecords->ruby), multiplayerId);
        ReceiveOldManData(&sReceivedRecords->ruby.oldMan, sizeof(sReceivedRecords->ruby), multiplayerId);
        ReceiveDewfordTrendData(sReceivedRecords->ruby.dewfordTrends, sizeof(sReceivedRecords->ruby), multiplayerId);
        ReceiveGiftItem(&sReceivedRecords->ruby.giftItem, multiplayerId);
    }
    else
    {
        // Emerald
        ReceiveOldManData(&sReceivedRecords->emerald.oldMan, sizeof(sReceivedRecords->emerald), multiplayerId);
        ReceiveDewfordTrendData(sReceivedRecords->emerald.dewfordTrends, sizeof(sReceivedRecords->emerald), multiplayerId);
        ReceiveBattleTowerData(&sReceivedRecords->emerald.battleTowerRecord, sizeof(sReceivedRecords->emerald), multiplayerId);
        ReceiveGiftItem(&sReceivedRecords->emerald.giftItem, multiplayerId);
        ReceiveLilycoveLadyData(&sReceivedRecords->emerald.lilycoveLady, sizeof(sReceivedRecords->emerald), multiplayerId);
        ReceiveApprenticeData(sReceivedRecords->emerald.apprentices, sizeof(sReceivedRecords->emerald), (u8)multiplayerId);
        ReceiveRankingHallRecords(&sReceivedRecords->emerald.hallRecords, sizeof(sReceivedRecords->emerald), (u8)multiplayerId);
    }
}

static void PrintTextOnRecordMixing(const u8 *src)
{
    DrawDialogueFrame(0, FALSE);
    AddTextPrinterParameterized(0, FONT_NORMAL, src, 0, 1, 0, NULL);
    CopyWindowToVram(0, COPYWIN_FULL);
}

#define tCounter data[0]

static void Task_RecordMixing_SoundEffect(u8 taskId)
{
    if (++gTasks[taskId].tCounter == 50)
    {
        PlaySE(SE_M_ATTRACT);
        gTasks[taskId].tCounter = 0;
    }
}

#undef tCounter

#define tTimer       data[8]
#define tLinkTaskId  data[10]
#define tSoundTaskId data[15]

// Note: gSpecialVar_0x8005 here contains the player's spot id.
static void Task_RecordMixing_Main(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    switch (tState)
    {
    case 0: // init
        sSentRecord = Alloc(sizeof(*sSentRecord));
        sReceivedRecords = Alloc(sizeof(*sReceivedRecords) * MAX_LINK_PLAYERS);
        SetLocalLinkPlayerId(gSpecialVar_0x8005);
        VarSet(VAR_TEMP_MIXED_RECORDS, 1);
        sReadyToReceive = FALSE;
        PrepareExchangePacket();
        CreateRecordMixingLights();
        tState = 1;
        tLinkTaskId = CreateTask(Task_MixingRecordsRecv, 80);
        tSoundTaskId = CreateTask(Task_RecordMixing_SoundEffect, 81);
        break;
    case 1: // wait for Task_MixingRecordsRecv
        if (!gTasks[tLinkTaskId].isActive)
        {
            tState = 2;
            FlagSet(FLAG_SYS_MIX_RECORD);
            DestroyRecordMixingLights();
            DestroyTask(tSoundTaskId);
        }
        break;
    case 2:
        tLinkTaskId = CreateTask(Task_DoRecordMixing, 10);
        tState = 3;
        PlaySE(SE_M_BATON_PASS);
        break;
    case 3: // wait for Task_DoRecordMixing
        if (!gTasks[tLinkTaskId].isActive)
        {
            tState = 4;
            if (gWirelessCommType == 0)
                tLinkTaskId = CreateTask_ReestablishCableClubLink();

            PrintTextOnRecordMixing(gText_RecordMixingComplete);
            tTimer = 0;
        }
        break;
    case 4: // wait 60 frames
        if (++tTimer > 60)
            tState = 5;
        break;
    case 5: // Wait for the task created by CreateTask_ReestablishCableClubLink
        if (!gTasks[tLinkTaskId].isActive)
        {
            Free(sReceivedRecords);
            Free(sSentRecord);
            SetLinkWaitingForScript();
            if (gWirelessCommType != 0)
                CreateTask(Task_ReturnToFieldRecordMixing, 10);
            ClearDialogWindowAndFrame(0, TRUE);
            DestroyTask(taskId);
            ScriptContext_Enable();
        }
        break;
    }
}

#undef tTimer
#undef tLinkTaskId
#undef tSoundTaskId

// Task data for Task_MixingRecordsRecv and subsequent tasks
#define tSentRecord    data[2] // Used to store a ptr, so data[2] and data[3]
#define tNumChunksSent data[4]
#define tMultiplayerId data[5]
#define tCopyTaskId    data[10]

// Task data for Task_CopyReceiveBuffer
#define tParentTaskId     data[0]
#define tNumChunksRecv(i) data[1 + (i)] // Number of chunks of the record received per player
#define tRecvRecords      data[5] // Used to store a ptr, so data[5] and data[6]

static void Task_MixingRecordsRecv(u8 taskId)
{
    struct Task *task = &gTasks[taskId];

    switch (task->tState)
    {
    case 0:
        PrintTextOnRecordMixing(gText_MixingRecords);
        task->data[8] = 0x708;
        task->tState = 400;
        ClearLinkCallback_2();
        break;
    case 100: // wait 20 frames
        if (++task->data[12] > 20)
        {
            task->data[12] = 0;
            task->tState = 101;
        }
        break;
    case 101:
        {
            u8 players = GetLinkPlayerCount_2();
            if (IsLinkMaster() == TRUE)
            {
                if (players == GetSavedPlayerCount())
                {
                    PlaySE(SE_PIN);
                    task->tState = 201;
                    task->data[12] = 0;
                }
            }
            else
            {
                PlaySE(SE_BOO);
                task->tState = 301;
            }
        }
        break;
    case 201:
        // We're the link master. Delay for 30 frames per connected player.
        if (GetSavedPlayerCount() == GetLinkPlayerCount_2() && ++task->data[12] > (GetLinkPlayerCount_2() * 30))
        {
            CheckShouldAdvanceLinkState();
            task->tState = 1;
        }
        break;
    case 301:
        if (GetSavedPlayerCount() == GetLinkPlayerCount_2())
            task->tState = 1;
        break;
    case 400: // wait 20 frames
        if (++task->data[12] > 20)
        {
            task->tState = 1;
            task->data[12] = 0;
        }
        break;
    case 1: // wait for handshake
        if (gReceivedRemoteLinkPlayers)
        {
            ConvertIntToDecimalStringN(gStringVar1, GetMultiplayerId_(), STR_CONV_MODE_LEADING_ZEROS, 2);
            task->tState = 5;
        }
        break;
    case 2:
        {
            u8 subTaskId;

            task->data[6] = GetLinkPlayerCount_2();
            task->tState = 0;
            task->tMultiplayerId = GetMultiplayerId_();
            task->func = Task_SendPacket;
            if (Link_AnyPartnersPlayingRubyOrSapphire())
            {
                StorePtrInTaskData(sSentRecord, (u16*) &task->tSentRecord);
                subTaskId = CreateTask(Task_CopyReceiveBuffer, 80);
                task->tCopyTaskId = subTaskId;
                gTasks[subTaskId].tParentTaskId = taskId;
                StorePtrInTaskData(sReceivedRecords, (u16*) &gTasks[subTaskId].tRecvRecords);
                sRecordStructSize = sizeof(struct PlayerRecordRS);
            }
            else
            {
                StorePtrInTaskData(sSentRecord, (u16*)  &task->tSentRecord);
                subTaskId = CreateTask(Task_CopyReceiveBuffer, 80);
                task->tCopyTaskId = subTaskId;
                gTasks[subTaskId].tParentTaskId = taskId;
                StorePtrInTaskData(sReceivedRecords,(u16*) &gTasks[subTaskId].tRecvRecords);
                sRecordStructSize = sizeof(struct PlayerRecordEmerald);
            }
        }
        break;
    case 5: // wait 60 frames
        if (++task->data[10] > 60)
        {
            task->data[10] = 0;
            task->tState = 2;
        }
        break;
    }
}

static void Task_SendPacket(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    switch (task->tState)
    {
    case 0: // Copy record data chunk to send buffer
        {
            void *recordData = LoadPtrFromTaskData((u16*)&task->tSentRecord) + task->tNumChunksSent * BUFFER_CHUNK_SIZE;

            memcpy(gBlockSendBuffer, recordData, BUFFER_CHUNK_SIZE);
            task->tState++;
        }
        break;
    case 1:
        if (GetMultiplayerId() == 0)
            SendBlockRequest(BLOCK_REQ_SIZE_200);
        task->tState++;
        break;
    case 2:
        break;
    case 3:
        // If sent final chunk of record, move on to next state.
        // Otherwise return to first state and send next chunk.
        task->tNumChunksSent++;
        if (task->tNumChunksSent == sRecordStructSize / BUFFER_CHUNK_SIZE + 1)
            task->tState++;
        else
            task->tState = 0;
        break;
    case 4:
        if (!gTasks[task->tCopyTaskId].isActive)
            task->func = Task_SendPacket_SwitchToReceive;
        break;
    }
}

static void Task_CopyReceiveBuffer(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    u8 status = GetBlockReceivedStatus();
    u8 handledPlayers = 0;

    if (status == GetLinkPlayerCountAsBitFlags())
    {
        u8 i;
        for (i = 0; i < GetLinkPlayerCount(); i++)
        {
            if ((status >> i) & 1)
            {
                void *dest = LoadPtrFromTaskData((u16*) &task->tRecvRecords) + task->tNumChunksRecv(i) * BUFFER_CHUNK_SIZE + sRecordStructSize * i;
                void *src = GetPlayerRecvBuffer(i);
                if ((task->tNumChunksRecv(i) + 1) * BUFFER_CHUNK_SIZE > sRecordStructSize)
                    memcpy(dest, src, sRecordStructSize - task->tNumChunksRecv(i) * BUFFER_CHUNK_SIZE);
                else
                    memcpy(dest, src, BUFFER_CHUNK_SIZE);
                ResetBlockReceivedFlag(i);
                task->tNumChunksRecv(i)++;
                if (task->tNumChunksRecv(i) == sRecordStructSize / BUFFER_CHUNK_SIZE + 1)
                    handledPlayers++;
            }
        }
        gTasks[task->tParentTaskId].tState++;
    }

    if (handledPlayers == GetLinkPlayerCount())
        DestroyTask(taskId);
}

static void Task_WaitReceivePacket(u8 taskId)
{
    struct Task *task = &gTasks[taskId];

    // Wait for Task_CopyReceiveBuffer to finish
    if (!gTasks[task->tCopyTaskId].isActive)
        DestroyTask(taskId);
}

static void Task_ReceivePacket(u8 taskId)
{
    struct Task *task = &gTasks[taskId];

    task->func = Task_WaitReceivePacket;
    if (sReadyToReceive == TRUE)
        ReceiveExchangePacket(task->tMultiplayerId);
}

static void Task_SendPacket_SwitchToReceive(u8 taskId)
{
    gTasks[taskId].func = Task_ReceivePacket;
    sReadyToReceive = TRUE;
}

static void *LoadPtrFromTaskData(const u16 *asShort)
{
    return (void *)(asShort[0] | (asShort[1] << 16));
}

static void StorePtrInTaskData(void *records, u16 *asShort)
{
    asShort[0] = (u32)records;
    asShort[1] = ((u32)records >> 16);
}

static u8 GetMultiplayerId_(void)
{
    return GetMultiplayerId();
}

static void *GetPlayerRecvBuffer(u8 id)
{
    return gBlockRecvBuffer[id];
}

static void ShufflePlayerIndices(u32 *data)
{
    u32 i;
    u32 linkTrainerId;
    u32 players = GetLinkPlayerCount();

    switch (players)
    {
    case 2:
        for (i = 0; i < ARRAY_COUNT(sPlayerIdxOrders_2Player); i++)
            data[i] = sPlayerIdxOrders_2Player[i];
        break;
    case 3:
        linkTrainerId = GetLinkPlayerTrainerId(0) % ARRAY_COUNT(sPlayerIdxOrders_3Player);
        for (i = 0; i < ARRAY_COUNT(sPlayerIdxOrders_3Player[0]); i++)
            data[i] = sPlayerIdxOrders_3Player[linkTrainerId][i];
        break;
    case 4:
        linkTrainerId = GetLinkPlayerTrainerId(0) % ARRAY_COUNT(sPlayerIdxOrders_4Player);
        for (i = 0; i < ARRAY_COUNT(sPlayerIdxOrders_4Player[0]); i++)
            data[i] = sPlayerIdxOrders_4Player[linkTrainerId][i];
        break;
    }
}

static void ReceiveOldManData(OldMan *records, size_t recordSize, u8 multiplayerId)
{
    u8 version;
    u16 language;
    OldMan *oldMan;
    u32 mixIndices[MAX_LINK_PLAYERS];

    ShufflePlayerIndices(mixIndices);
    oldMan = (void *)records + recordSize * mixIndices[multiplayerId];
    version = gLinkPlayers[mixIndices[multiplayerId]].version;
    language = gLinkPlayers[mixIndices[multiplayerId]].language;

    if (Link_AnyPartnersPlayingRubyOrSapphire())
        SanitizeReceivedRubyOldMan(oldMan, version, language);
    else
        SanitizeReceivedEmeraldOldMan(oldMan, version, language);

    memcpy(sOldManSave, (void *)records + recordSize * mixIndices[multiplayerId], sizeof(OldMan));
    ResetMauvilleOldManFlag();
}

static void ReceiveBattleTowerData(void *records, size_t recordSize, u8 multiplayerId)
{
    struct EmeraldBattleTowerRecord *battleTowerRecord;
    struct BattleTowerPokemon *btPokemon;
    u32 mixIndices[MAX_LINK_PLAYERS];
    s32 i;

    ShufflePlayerIndices(mixIndices);
    if (Link_AnyPartnersPlayingRubyOrSapphire())
    {
        if (RubyBattleTowerRecordToEmerald((void *)records + recordSize * mixIndices[multiplayerId], (void *)records + recordSize * multiplayerId) == TRUE)
        {
            battleTowerRecord = (void *)records + recordSize * multiplayerId;
            battleTowerRecord->language = gLinkPlayers[mixIndices[multiplayerId]].language;
            CalcEmeraldBattleTowerChecksum(battleTowerRecord);
        }
    }
    else
    {
        memcpy((void *)records + recordSize * multiplayerId, (void *)records + recordSize * mixIndices[multiplayerId], sizeof(struct EmeraldBattleTowerRecord));
        battleTowerRecord = (void *)records + recordSize * multiplayerId;
        for (i = 0; i < MAX_FRONTIER_PARTY_SIZE; i++)
        {
            btPokemon = &battleTowerRecord->party[i];
            if (btPokemon->species != SPECIES_NONE && IsStringJapanese(btPokemon->nickname))
                ConvertInternationalString(btPokemon->nickname, LANGUAGE_JAPANESE);
        }
        CalcEmeraldBattleTowerChecksum(battleTowerRecord);
    }
    PutNewBattleTowerRecord((void *)records + recordSize * multiplayerId);
}

static void ReceiveLilycoveLadyData(LilycoveLady *records, size_t recordSize, u8 multiplayerId)
{
    LilycoveLady *lilycoveLady;
    u32 mixIndices[MAX_LINK_PLAYERS];

    ShufflePlayerIndices(mixIndices);
    memcpy((void *)records + recordSize * multiplayerId, sLilycoveLadySave, sizeof(LilycoveLady));

    if (GetLilycoveLadyId() == 0)
    {
        lilycoveLady = Alloc(sizeof(*lilycoveLady));
        if (lilycoveLady == NULL)
            return;

        memcpy(lilycoveLady, sLilycoveLadySave, sizeof(LilycoveLady));
    }
    else
    {
        lilycoveLady = NULL;
    }

    memcpy(sLilycoveLadySave, (void *)records + recordSize * mixIndices[multiplayerId], sizeof(LilycoveLady));
    ResetLilycoveLadyForRecordMix();
    if (lilycoveLady != NULL)
    {
        QuizLadyClearQuestionForRecordMix(lilycoveLady);
        Free(lilycoveLady);
    }
}

// Indexes for a 2 element array used to store the multiplayer id and daycare
// slot that correspond to a daycare Pokémon that can hold an item.
enum {
    MULTIPLAYER_ID,
    DAYCARE_SLOT,
};

static void ReceiveGiftItem(u16 *item, u8 multiplayerId)
{
    if (multiplayerId != 0 && *item != ITEM_NONE && GetPocketByItemId(*item) == POCKET_KEY_ITEMS)
    {
        if (!CheckBagHasItem(*item, 1) && !CheckPCHasItem(*item, 1) && AddBagItem(*item, 1))
        {
            VarSet(VAR_TEMP_RECORD_MIX_GIFT_ITEM, *item);
            StringCopy(gStringVar1, gLinkPlayers[0].name);
            if (*item == ITEM_EON_TICKET)
                FlagSet(FLAG_ENABLE_SHIP_SOUTHERN_ISLAND);
        }
        else
        {
            VarSet(VAR_TEMP_RECORD_MIX_GIFT_ITEM, ITEM_NONE);
        }
    }
}

static void Task_DoRecordMixing(u8 taskId)
{
    struct Task *task = &gTasks[taskId];

    switch (task->tState)
    {
    case 0:
        task->tState++;
        break;
    case 1:
        if (Link_AnyPartnersPlayingRubyOrSapphire())
            task->tState++;
        else
            task->tState = 6;
        break;
    case 2:
        // Mixing Ruby/Sapphire records.
        SetContinueGameWarpStatusToDynamicWarp();
        WriteSaveBlock2();
        task->tState++;
        break;
    case 3:
        if (WriteSaveBlock1Sector())
        {
            ClearContinueGameWarpStatus2();
            task->tState = 4;
            task->data[1] = 0;
        }
        break;
    case 4: // Wait 10 frames
        if (++task->data[1] > 10)
        {
            SetCloseLinkCallback();
            task->tState++;
        }
        break;
    case 5:
        // Finish mixing Ruby/Sapphire records
        if (gReceivedRemoteLinkPlayers == FALSE)
            DestroyTask(taskId);
        break;

    // Mixing Emerald records.
    case 6:
        if (!Rfu_SetLinkRecovery(FALSE))
        {
            CreateTask(Task_LinkFullSave, 5);
            task->tState++;
        }
        break;
    case 7: // wait for Task_LinkFullSave to finish.
        if (!FuncIsActiveTask(Task_LinkFullSave))
        {
            if (gWirelessCommType)
            {
                Rfu_SetLinkRecovery(TRUE);
                task->tState = 8;
            }
            else
            {
                task->tState = 4;
            }
        }
        break;
    case 8:
        SetLinkStandbyCallback();
        task->tState++;
        break;
    case 9:
        if (IsLinkTaskFinished())
            DestroyTask(taskId);
        break;
    }
}

static void GetSavedApprentices(struct Apprentice *dst, struct Apprentice *src)
{
    s32 i, id;
    s32 apprenticeSaveId, oldPlayerApprenticeSaveId;
    s32 numOldPlayerApprentices, numMixApprentices;

    dst[0].playerName[0] = EOS;
    dst[1].playerName[0] = EOS;

    dst[0] = src[0];

    oldPlayerApprenticeSaveId = 0;
    numOldPlayerApprentices = 0;
    apprenticeSaveId = 0;
    numMixApprentices = 0;
    for (i = 0; i < 2; i++)
    {
        id = (i + gSaveBlock2Ptr->playerApprentice.saveId) % (APPRENTICE_COUNT - 1) + 1;
        if (src[id].playerName[0] != EOS)
        {
            if (GetTrainerId(src[id].playerId) != GetTrainerId(gSaveBlock2Ptr->playerTrainerId))
            {
                numMixApprentices++;
                apprenticeSaveId = id;
            }
            if (GetTrainerId(src[id].playerId) == GetTrainerId(gSaveBlock2Ptr->playerTrainerId))
            {
                numOldPlayerApprentices++;
                oldPlayerApprenticeSaveId = id;
            }
        }
    }

    // Prefer passing on other mixed Apprentices rather than old player's Apprentices
    if (numMixApprentices == 0 && numOldPlayerApprentices != 0)
    {
        numMixApprentices = numOldPlayerApprentices;
        apprenticeSaveId = oldPlayerApprenticeSaveId;
    }

    switch (numMixApprentices)
    {
    case 1:
        dst[1] = src[apprenticeSaveId];
        break;
    case 2:
        if (Random2() > 0x3333)
            dst[1] = src[gSaveBlock2Ptr->playerApprentice.saveId + 1];
        else
            dst[1] = src[((gSaveBlock2Ptr->playerApprentice.saveId + 1) % (APPRENTICE_COUNT - 1) + 1)];
        break;
    }
}

void GetPlayerHallRecords(struct PlayerHallRecords *dst)
{
    s32 i, j;

    for (i = 0; i < HALL_FACILITIES_COUNT; i++)
    {
        for (j = 0; j < FRONTIER_LVL_MODE_COUNT; j++)
        {
            CopyTrainerId(dst->onePlayer[i][j].id, gSaveBlock2Ptr->playerTrainerId);
            dst->onePlayer[i][j].language = GAME_LANGUAGE;
            StringCopy(dst->onePlayer[i][j].name, gSaveBlock2Ptr->playerName);
        }
    }

    for (j = 0; j < FRONTIER_LVL_MODE_COUNT; j++)
    {
        dst->twoPlayers[j].language = GAME_LANGUAGE;
        CopyTrainerId(dst->twoPlayers[j].id1, gSaveBlock2Ptr->playerTrainerId);
        CopyTrainerId(dst->twoPlayers[j].id2, gSaveBlock2Ptr->frontier.opponentTrainerIds[j]);
        StringCopy(dst->twoPlayers[j].name1, gSaveBlock2Ptr->playerName);
        StringCopy(dst->twoPlayers[j].name2, gSaveBlock2Ptr->frontier.opponentNames[j]);
    }

    for (i = 0; i < FRONTIER_LVL_MODE_COUNT; i++)
    {
        dst->onePlayer[RANKING_HALL_TOWER_SINGLES][i].winStreak = gSaveBlock2Ptr->frontier.towerRecordWinStreaks[FRONTIER_MODE_SINGLES][i];
        dst->onePlayer[RANKING_HALL_TOWER_DOUBLES][i].winStreak = gSaveBlock2Ptr->frontier.towerRecordWinStreaks[FRONTIER_MODE_DOUBLES][i];
        dst->onePlayer[RANKING_HALL_TOWER_MULTIS][i].winStreak = gSaveBlock2Ptr->frontier.towerRecordWinStreaks[FRONTIER_MODE_MULTIS][i];
        dst->onePlayer[RANKING_HALL_DOME][i].winStreak = gSaveBlock2Ptr->frontier.domeRecordWinStreaks[FRONTIER_MODE_SINGLES][i];
        dst->onePlayer[RANKING_HALL_PALACE][i].winStreak = gSaveBlock2Ptr->frontier.palaceRecordWinStreaks[FRONTIER_MODE_SINGLES][i];
        dst->onePlayer[RANKING_HALL_ARENA][i].winStreak = gSaveBlock2Ptr->frontier.arenaRecordStreaks[i];
        dst->onePlayer[RANKING_HALL_FACTORY][i].winStreak = gSaveBlock2Ptr->frontier.factoryRecordWinStreaks[FRONTIER_MODE_SINGLES][i];
        dst->onePlayer[RANKING_HALL_PIKE][i].winStreak = gSaveBlock2Ptr->frontier.pikeRecordStreaks[i];
        dst->onePlayer[RANKING_HALL_PYRAMID][i].winStreak = gSaveBlock2Ptr->frontier.pyramidRecordStreaks[i];

        dst->twoPlayers[i].winStreak = gSaveBlock2Ptr->frontier.towerRecordWinStreaks[FRONTIER_MODE_LINK_MULTIS][i];
    }
}

static bool32 IsApprenticeAlreadySaved(struct Apprentice *mixApprentice, struct Apprentice *apprentices)
{
    s32 i;

    for (i = 0; i < APPRENTICE_COUNT; i++)
    {
        if (GetTrainerId(mixApprentice->playerId) == GetTrainerId(apprentices[i].playerId)
            && mixApprentice->number == apprentices[i].number)
            return TRUE;
    }

    return FALSE;
}

static void ReceiveApprenticeData(struct Apprentice *records, size_t recordSize, u32 multiplayerId)
{
    s32 i, numApprentices, apprenticeId;
    struct Apprentice *mixApprentice;
    u32 mixIndices[MAX_LINK_PLAYERS];
    u32 apprenticeSaveId;

    ShufflePlayerIndices(mixIndices);
    mixApprentice = (void *)records + (recordSize * mixIndices[multiplayerId]);
    numApprentices = 0;
    apprenticeId = 0;
    for (i = 0; i < 2; i++)
    {
        if (mixApprentice[i].playerName[0] != EOS && !IsApprenticeAlreadySaved(&mixApprentice[i], &gSaveBlock2Ptr->apprentices[0]))
        {
            numApprentices++;
            apprenticeId = i;
        }
    }

    switch (numApprentices)
    {
    case 1:
        apprenticeSaveId = gSaveBlock2Ptr->playerApprentice.saveId + 1;
        gSaveBlock2Ptr->apprentices[apprenticeSaveId] = mixApprentice[apprenticeId];
        gSaveBlock2Ptr->playerApprentice.saveId = (gSaveBlock2Ptr->playerApprentice.saveId + 1) % (APPRENTICE_COUNT - 1);
        break;
    case 2:
        for (i = 0; i < 2; i++)
        {
            apprenticeSaveId = ((i ^ 1) + gSaveBlock2Ptr->playerApprentice.saveId) % (APPRENTICE_COUNT - 1) + 1;
            gSaveBlock2Ptr->apprentices[apprenticeSaveId] = mixApprentice[i];
        }
        gSaveBlock2Ptr->playerApprentice.saveId = (gSaveBlock2Ptr->playerApprentice.saveId + 2) % (APPRENTICE_COUNT - 1);
        break;
    }
}

#if FREE_RECORD_MIXING_HALL_RECORDS == FALSE
static void GetNewHallRecords(struct RecordMixingHallRecords *dst, void *records, size_t recordSize, u32 multiplayerId, s32 linkPlayerCount)
{
    s32 i, j, k, l;
    s32 repeatTrainers;

    // Load sPartnerHallRecords with link partners' hall records
    k = 0;
    for (i = 0; i < linkPlayerCount; i++)
    {
        if (i != multiplayerId)
            sPartnerHallRecords[k++] = records;
        if (k == HALL_RECORDS_COUNT)
            break;
        records += recordSize;
    }

    // Get improved 1P hall records
    for (i = 0; i < HALL_FACILITIES_COUNT; i++)
    {
        for (j = 0; j < FRONTIER_LVL_MODE_COUNT; j++)
        {
            // First get the existing saved records
            for (k = 0; k < HALL_RECORDS_COUNT; k++)
                dst->hallRecords1P[i][j][k] = gSaveBlock2Ptr->hallRecords1P[i][j][k];

            // Then read the new mixed records
            for (k = 0; k < linkPlayerCount - 1; k++)
            {
                repeatTrainers = 0;
                for (l = 0; l < HALL_RECORDS_COUNT; l++)
                {
                    // If the new trainer is already in the existing saved records, only
                    // use the new one if the win streak is better
                    if (GetTrainerId(dst->hallRecords1P[i][j][l].id) == GetTrainerId(sPartnerHallRecords[k]->onePlayer[i][j].id))
                    {
                        repeatTrainers++;
                        if (dst->hallRecords1P[i][j][l].winStreak < sPartnerHallRecords[k]->onePlayer[i][j].winStreak)
                            dst->hallRecords1P[i][j][l] = sPartnerHallRecords[k]->onePlayer[i][j];
                    }
                }

                // If all of the mixed records are new trainers, just save them
                if (repeatTrainers == 0)
                    dst->hallRecords1P[i][j][k + HALL_RECORDS_COUNT] = sPartnerHallRecords[k]->onePlayer[i][j];
            }
        }
    }

    // Get improved 2P hall records
    for (j = 0; j < FRONTIER_LVL_MODE_COUNT; j++)
    {
        // First get the existing saved records
        for (k = 0; k < HALL_RECORDS_COUNT; k++)
            dst->hallRecords2P[j][k] = gSaveBlock2Ptr->hallRecords2P[j][k];

        // Then read the new mixed records
        for (k = 0; k < linkPlayerCount - 1; k++)
        {
            repeatTrainers = 0;
            for (l = 0; l < HALL_RECORDS_COUNT; l++)
            {
                // If the new trainer pair is already in the existing saved records, only
                // use the new pair if the win streak is better
                if (GetTrainerId(dst->hallRecords2P[j][l].id1) == GetTrainerId(sPartnerHallRecords[k]->twoPlayers[j].id1)
                 && GetTrainerId(dst->hallRecords2P[j][l].id2) == GetTrainerId(sPartnerHallRecords[k]->twoPlayers[j].id2))
                {
                    repeatTrainers++;
                    if (dst->hallRecords2P[j][l].winStreak < sPartnerHallRecords[k]->twoPlayers[j].winStreak)
                        dst->hallRecords2P[j][l] = sPartnerHallRecords[k]->twoPlayers[j];
                }
            }

            // If all of the mixed records are new trainer pairs, just save them
            if (repeatTrainers == 0)
                dst->hallRecords2P[j][k + HALL_RECORDS_COUNT] = sPartnerHallRecords[k]->twoPlayers[j];
        }
    }
}

static void FillWinStreakRecords1P(struct RankingHall1P *playerRecords, struct RankingHall1P *mixRecords)
{
    s32 i, j;

    // Fill the player's 1P records with the highest win streaks from the mixed records
    for (i = 0; i < HALL_RECORDS_COUNT; i++)
    {
        // Get the highest remaining win streak in the mixed hall records
        s32 highestWinStreak = 0;
        s32 highestId = -1;
        for (j = 0; j < HALL_RECORDS_COUNT * 2; j++)
        {
            if (mixRecords[j].winStreak > highestWinStreak)
            {
                highestId = j;
                highestWinStreak = mixRecords[j].winStreak;
            }
        }

        // Save the win streak to the player's records, then clear it from the mixed records
        if (highestId >= 0)
        {
            playerRecords[i] = mixRecords[highestId];
            mixRecords[highestId].winStreak = 0;
        }
    }
}

static void FillWinStreakRecords2P(struct RankingHall2P *playerRecords, struct RankingHall2P *mixRecords)
{
    s32 i, j;

    // Fill the player's 2P records with the highest win streaks from the mixed records
    for (i = 0; i < HALL_RECORDS_COUNT; i++)
    {
        // Get the highest remaining win streak in the mixed hall records
        s32 highestWinStreak = 0;
        s32 highestId = -1;
        for (j = 0; j < HALL_RECORDS_COUNT * 2; j++)
        {
            if (mixRecords[j].winStreak > highestWinStreak)
            {
                highestId = j;
                highestWinStreak = mixRecords[j].winStreak;
            }
        }

        // Save the win streak to the player's records, then clear it from the mixed records
        if (highestId >= 0)
        {
            playerRecords[i] = mixRecords[highestId];
            mixRecords[highestId].winStreak = 0;
        }
    }
}

static void SaveHighestWinStreakRecords(struct RecordMixingHallRecords *mixHallRecords)
{
    s32 i, j;

    for (i = 0; i < HALL_FACILITIES_COUNT; i++)
    {
        for (j = 0; j < FRONTIER_LVL_MODE_COUNT; j++)
            FillWinStreakRecords1P(gSaveBlock2Ptr->hallRecords1P[i][j], mixHallRecords->hallRecords1P[i][j]);
    }

    for (j = 0; j < FRONTIER_LVL_MODE_COUNT; j++)
        FillWinStreakRecords2P(gSaveBlock2Ptr->hallRecords2P[j], mixHallRecords->hallRecords2P[j]);
}
#endif //FREE_RECORD_MIXING_HALL_RECORDS

static void ReceiveRankingHallRecords(struct PlayerHallRecords *records, size_t recordSize, u32 multiplayerId)
{
#if FREE_RECORD_MIXING_HALL_RECORDS == FALSE
    u8 linkPlayerCount = GetLinkPlayerCount();
    struct RecordMixingHallRecords *mixHallRecords = AllocZeroed(sizeof(*mixHallRecords));

    GetNewHallRecords(mixHallRecords, records, recordSize, multiplayerId, linkPlayerCount);
    SaveHighestWinStreakRecords(mixHallRecords);

    Free(mixHallRecords);
#endif //FREE_RECORD_MIXING_HALL_RECORDS
}

static void SanitizeRubyBattleTowerRecord(struct RSBattleTowerRecord *src)
{

}

static void SanitizeEmeraldBattleTowerRecord(struct EmeraldBattleTowerRecord *dst)
{
    s32 i;

    for (i = 0; i < MAX_FRONTIER_PARTY_SIZE; i++)
    {
        struct BattleTowerPokemon *towerMon = &dst->party[i];
        if (towerMon->species != SPECIES_NONE)
            StripExtCtrlCodes(towerMon->nickname);
    }

    CalcEmeraldBattleTowerChecksum(dst);
}
