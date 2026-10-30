/*
 * OpenEar Bluetooth LE - see oe_ble.h.
 *
 * Stack bring-up is ADI's BLE_dats example (main.c / stack_dats.c) with the
 * terminal UART, passkey pairing, out-of-band pairing, privacy and button
 * handling removed: OpenEar owns the console UART and the buttons.
 *
 * Portions Copyright (C) 2022-2024 Analog Devices, Inc., Apache-2.0.
 * OpenEar changes: MIT.
 */
#include "oe_ble.h"

#include <stdio.h>
#include <string.h>

#include "wsf_types.h"
#include "wsf_assert.h"
#include "wsf_buf.h"
#include "wsf_cs.h"
#include "wsf_heap.h"
#include "wsf_msg.h"
#include "wsf_nvm.h"
#include "wsf_os.h"
#include "wsf_timer.h"
#include "wsf_trace.h"
#include "util/bstream.h"

#include "att_api.h"
#include "att_handler.h"
#include "app_api.h"
#include "app_cfg.h"
#include "dm_api.h"
#include "dm_handler.h"
#include "gatt/gatt_api.h"
#include "hci_core.h"
#include "hci_defs.h"
#include "hci_handler.h"
#include "l2c_api.h"
#include "l2c_handler.h"
#include "sec_api.h"
#include "smp_api.h"
#include "smp_handler.h"
#include "svc_core.h"
#include "svc_wp.h"

#include "ll_init_api.h"
#include "pal_bb.h"
#include "pal_cfg.h"
#include "pal_led.h"
#include "mxc_device.h"
#include "rtc.h"
#include "trimsir_regs.h"
#include "wut.h"
#include "oe_time.h"

#define DEFAULT_TX_POWER 0 /* dBm */
#define TRIM_TIMER_EVT 0x99
#define TRIM_TIMER_PERIOD_MS 60000

/* ------------------------------------------------------------------------ */
/* configuration (values from ADI's BLE_dats example unless noted)          */
/* ------------------------------------------------------------------------ */
static const appAdvCfg_t advCfg = {
    { 0, 0, 0 },     /* advertise forever */
    { 160, 800, 0 }, /* 100 ms advertising interval (duration 0 = stay at it, as in ADI's example) */
};
static const appSlaveCfg_t slaveCfg = { 1 };
/* OpenEar: no bonding, no MITM - a phone connects with one tap */
static const appSecCfg_t secCfg = { 0, 0, 0, FALSE, FALSE };
static const smpCfg_t smpCfg = { 500, SMP_IO_NO_IN_NO_OUT, 7, 16, 1, 0, 64000, 64000, 2 };
static const appUpdateCfg_t updateCfg = { 0, (15 * 8 / 1.25), (15 * 12 / 1.25), 0, 600, 5 };
static const attCfg_t attCfg = { 15, 241, ATT_MAX_TRANS_TIMEOUT, 4 };

/* advertising: flags + the OpenEar (Cordio WP) service UUID */
static const uint8_t advData[] = {
    2, DM_ADV_TYPE_FLAGS, DM_FLAG_LE_GENERAL_DISC | DM_FLAG_LE_BREDR_NOT_SUP,
    17, DM_ADV_TYPE_128_UUID, ATT_UUID_P1_SERVICE,
};
static const uint8_t scanData[] = { 8, DM_ADV_TYPE_LOCAL_NAME, 'O', 'p', 'e', 'n', 'E', 'a', 'r' };

enum { CCC_GATT_SC, CCC_WP_DAT, CCC_NUM };
static const attsCccSet_t cccSet[CCC_NUM] = {
    { GATT_SC_CH_CCC_HDL, ATT_CLIENT_CFG_INDICATE, DM_SEC_LEVEL_NONE },
    { WP_DAT_CH_CCC_HDL, ATT_CLIENT_CFG_NOTIFY, DM_SEC_LEVEL_NONE },
};

/* ------------------------------------------------------------------------ */
/* state                                                                    */
/* ------------------------------------------------------------------------ */
static wsfHandlerId_t appHandlerId;
static wsfTimer_t trimTimer;
static dmConnId_t connId = DM_CONN_ID_NONE;
static volatile int wutTrimComplete;
static uint32_t rx_lines, tx_bytes, tx_dropped;

static wsfBufPoolDesc_t poolDesc[] = { { 16, 8 }, { 32, 4 }, { 192, 8 }, { 256, 16 } };
static LlRtCfg_t llRtCfg;

/* console text waiting to go out as notifications */
#define TXQ_SIZE 2048
static char txq[TXQ_SIZE];
static uint32_t txq_head, txq_tail;
static int tx_busy;
static uint32_t tx_t0; /* timer ticks when the last notification went out */

/* incoming bytes until a newline */
static char rx_line[128];
static int rx_len;

/* ------------------------------------------------------------------------ */
/* TX: one notification in flight, the next goes on its confirmation        */
/* ------------------------------------------------------------------------ */
static void tx_pump(void)
{
    uint8_t chunk[240];
    if (tx_busy || connId == DM_CONN_ID_NONE || txq_head == txq_tail) {
        return;
    }
    if (!AttsCccEnabled(connId, CCC_WP_DAT)) {
        txq_tail = txq_head; /* phone isn't listening: drop */
        return;
    }
    uint16_t max = AttGetMtu(connId) - 3;
    if (max > sizeof(chunk)) {
        max = sizeof(chunk);
    }
    uint16_t n = 0;
    while (n < max && txq_tail != txq_head) {
        chunk[n++] = (uint8_t)txq[txq_tail++ % TXQ_SIZE];
    }
    AttsHandleValueNtf(connId, WP_DAT_HDL, n, chunk);
    tx_busy = 1;
    tx_t0 = oe_ticks();
    tx_bytes += n;
}

void oe_ble_write(const char *s, int len)
{
    if (connId == DM_CONN_ID_NONE) {
        return;
    }
    for (int i = 0; i < len; i++) {
        if (txq_head - txq_tail >= TXQ_SIZE) {
            tx_dropped += (uint32_t)(len - i);
            break;
        }
        txq[txq_head++ % TXQ_SIZE] = s[i];
    }
    tx_pump();
}

/* ------------------------------------------------------------------------ */
/* RX: the phone writes command text                                        */
/* ------------------------------------------------------------------------ */
static void rx_flush(void)
{
    if (rx_len) {
        rx_line[rx_len] = 0;
        rx_len = 0;
        rx_lines++;
        oe_ble_on_line(rx_line);
    }
}

static uint8_t wpWriteCback(dmConnId_t cid, uint16_t handle, uint8_t operation, uint16_t offset,
                            uint16_t len, uint8_t *pValue, attsAttr_t *pAttr)
{
    (void)cid;
    (void)handle;
    (void)offset;
    (void)pAttr;
    int had_newline = 0;
    /* A write longer than MTU-3 arrives as prepared writes: Cordio calls us
     * once per fragment while queueing (ignore those) and again per fragment
     * on execute. Fragments only end a line at a newline. */
    if (operation == ATT_PDU_PREP_WRITE_REQ) {
        return ATT_SUCCESS;
    }
    for (uint16_t i = 0; i < len; i++) {
        char c = (char)pValue[i];
        if (c == '\n' || c == '\r') {
            had_newline = 1;
            rx_flush();
        } else if (rx_len < (int)sizeof(rx_line) - 1) {
            rx_line[rx_len++] = c;
        }
    }
    /* terminal apps often send a command without a newline: one (unfragmented)
     * write = one line */
    if (!had_newline && operation != ATT_PDU_EXEC_WRITE_REQ) {
        rx_flush();
    }
    return ATT_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* stack callbacks                                                          */
/* ------------------------------------------------------------------------ */
static void dmCback(dmEvt_t *pDmEvt)
{
    if (pDmEvt->hdr.event == DM_SEC_ECC_KEY_IND) {
        DmSecSetEccKey(&pDmEvt->eccMsg.data.key);
        return;
    }
    uint16_t len = DmSizeOfEvt(pDmEvt);
    dmEvt_t *pMsg = WsfMsgAlloc(len);
    if (pMsg) {
        memcpy(pMsg, pDmEvt, len);
        WsfMsgSend(appHandlerId, pMsg);
    }
}

static void attCback(attEvt_t *pEvt)
{
    attEvt_t *pMsg = WsfMsgAlloc(sizeof(attEvt_t) + pEvt->valueLen);
    if (pMsg) {
        memcpy(pMsg, pEvt, sizeof(attEvt_t));
        pMsg->pValue = (uint8_t *)(pMsg + 1);
        memcpy(pMsg->pValue, pEvt->pValue, pEvt->valueLen);
        WsfMsgSend(appHandlerId, pMsg);
    }
}

static void cccCback(attsCccEvt_t *pEvt)
{
    (void)pEvt;
}

static void wutTrimCb(int err)
{
    (void)err;
    wutTrimComplete = 1;
}

static void setup_advertising(void)
{
    AppAdvSetData(APP_ADV_DATA_DISCOVERABLE, sizeof(advData), (uint8_t *)advData);
    AppAdvSetData(APP_SCAN_DATA_DISCOVERABLE, sizeof(scanData), (uint8_t *)scanData);
    AppAdvSetData(APP_ADV_DATA_CONNECTABLE, sizeof(advData), (uint8_t *)advData);
    AppAdvSetData(APP_SCAN_DATA_CONNECTABLE, sizeof(scanData), (uint8_t *)scanData);
    AppAdvStart(APP_MODE_AUTO_INIT);
}

static void procMsg(dmEvt_t *pMsg)
{
    switch (pMsg->hdr.event) {
    case DM_RESET_CMPL_IND:
        AttsCalculateDbHash();
        DmSecGenerateEccKeyReq();
        LlSetAdvTxPower(DEFAULT_TX_POWER);
        setup_advertising();
        break;
    case DM_ADV_START_IND:
        WsfTimerStartMs(&trimTimer, TRIM_TIMER_PERIOD_MS);
        break;
    case DM_ADV_STOP_IND:
        WsfTimerStop(&trimTimer);
        break;
    case DM_CONN_OPEN_IND:
        connId = (dmConnId_t)pMsg->hdr.param;
        txq_head = txq_tail = 0;
        tx_busy = 0;
        rx_len = 0; /* drop any half-received command from an earlier link */
        printf("\n[ble] phone connected\n> ");
        break;
    case DM_CONN_CLOSE_IND:
        connId = DM_CONN_ID_NONE;
        tx_busy = 0;
        rx_len = 0;
        WsfTimerStop(&trimTimer);
        printf("\n[ble] disconnected (reason 0x%02x)\n> ", pMsg->connClose.reason);
        break;
    case DM_SEC_PAIR_CMPL_IND:
    case DM_SEC_PAIR_FAIL_IND:
        DmSecGenerateEccKeyReq();
        break;
    case ATTS_HANDLE_VALUE_CNF:
        /* ignore the confirmation of the Service Changed indication */
        if (((attEvt_t *)pMsg)->handle == WP_DAT_HDL) {
            tx_busy = 0;
            tx_pump();
        }
        break;
    case TRIM_TIMER_EVT:
        MXC_WUT_TrimCrystalAsync(MXC_WUT0, wutTrimCb);
        WsfTimerStartMs(&trimTimer, TRIM_TIMER_PERIOD_MS);
        break;
    default:
        break;
    }
}

static void appHandler(wsfEventMask_t event, wsfMsgHdr_t *pMsg)
{
    (void)event;
    if (!pMsg) {
        return;
    }
    if (pMsg->event >= ATT_CBACK_START && pMsg->event <= ATT_CBACK_END) {
        AppServerProcAttMsg(pMsg);
    } else if (pMsg->event >= DM_CBACK_START && pMsg->event <= DM_CBACK_END) {
        AppSlaveProcDmMsg((dmEvt_t *)pMsg);
        AppSlaveSecProcDmMsg((dmEvt_t *)pMsg);
    }
    procMsg((dmEvt_t *)pMsg);
}

static void appHandlerInit(wsfHandlerId_t id)
{
    appHandlerId = id;
    pAppSlaveCfg = (appSlaveCfg_t *)&slaveCfg;
    pAppAdvCfg = (appAdvCfg_t *)&advCfg;
    pAppSecCfg = (appSecCfg_t *)&secCfg;
    pAppUpdateCfg = (appUpdateCfg_t *)&updateCfg;
    pSmpCfg = (smpCfg_t *)&smpCfg;
    pAttCfg = (attCfg_t *)&attCfg;
    AppSlaveInit();
    AppServerInit();
    trimTimer.handlerId = id;
    trimTimer.msg.event = TRIM_TIMER_EVT;
}

/* ------------------------------------------------------------------------ */
/* stack init: ADI's StackInitDats() + main() sequence                      */
/* ------------------------------------------------------------------------ */
static void stack_init(void)
{
    wsfHandlerId_t h;

    SecInit();
    SecAesInit();
    SecCmacInit();
    SecEccInit();

    h = WsfOsSetNextHandler(HciHandler);
    HciHandlerInit(h);

    h = WsfOsSetNextHandler(DmHandler);
    DmDevVsInit(0);
    DmConnInit();
    DmAdvInit();
    DmConnSlaveInit();
    DmSecInit();
    DmSecLescInit();
    DmPrivInit();
    DmHandlerInit(h);

    h = WsfOsSetNextHandler(L2cSlaveHandler);
    L2cSlaveHandlerInit(h);
    L2cInit();
    L2cSlaveInit();

    h = WsfOsSetNextHandler(AttHandler);
    AttHandlerInit(h);
    AttsInit();
    AttsIndInit();

    h = WsfOsSetNextHandler(SmpHandler);
    SmpHandlerInit(h);
    SmprInit();
    SmprScInit();
    HciSetMaxRxAclLen(256);

    h = WsfOsSetNextHandler(AppHandler);
    AppHandlerInit(h);

    h = WsfOsSetNextHandler(appHandler);
    appHandlerInit(h);
}

static void app_start(void)
{
    DmRegister(dmCback);
    DmConnRegister(DM_CLIENT_ID_APP, dmCback);
    AttRegister(attCback);
    AttConnRegister(AppServerConnCback);
    AttsCccRegister(CCC_NUM, (attsCccSet_t *)cccSet, cccCback);

    SvcCoreGattCbackRegister(GattReadCback, GattWriteCback);
    SvcCoreAddGroup();
    SvcWpCbackRegister(NULL, wpWriteCback);
    SvcWpAddGroup();
    GattSetSvcChangedIdx(CCC_GATT_SC);

    DmPhyInit();
    WsfNvmInit();
    DmDevReset();
}

void oe_ble_init(void)
{
    static BbRtCfg_t bbRtCfg;

    PalBbLoadCfg((PalBbCfg_t *)&bbRtCfg);
    LlGetDefaultRunTimeCfg(&llRtCfg);
    llRtCfg.btVer = LL_VER_BT_CORE_SPEC_5_0;
    PalCfgLoadData(PAL_CFG_ID_LL_PARAM, &llRtCfg.maxAdvSets, sizeof(LlRtCfg_t) - 9);
    PalCfgLoadData(PAL_CFG_ID_BLE_PHY, (uint8_t *)&llRtCfg.phy2mSup, 4);
    bbRtCfg.clkPpm = 20;
    llRtCfg.defTxPwrLvl = DEFAULT_TX_POWER;

    /* WSF buffers (sizes adjusted to the LL config, as in ADI's example) */
    poolDesc[2].len = 12 + 2 + 255;
    poolDesc[2].num = llRtCfg.maxAdvReports;
    poolDesc[3].len = 12 + llRtCfg.maxAclLen + 4 + BB_DATA_PDU_TAILROOM;
    poolDesc[3].num = llRtCfg.numTxBufs + llRtCfg.numRxBufs;
    const uint8_t numPools = sizeof(poolDesc) / sizeof(poolDesc[0]);
    WsfCsEnter();
    uint16_t memUsed = WsfBufCalcSize(numPools, poolDesc);
    WsfHeapAlloc(memUsed);
    WsfBufInit(numPools, poolDesc);
    WsfCsExit();
    WsfOsInit();
    WsfTimerInit();

    /* link layer memory */
    WsfCsEnter();
    LlInitRtCfg_t llCfg = { .pBbRtCfg = &bbRtCfg,
                            .wlSizeCfg = 4,
                            .rlSizeCfg = 4,
                            .plSizeCfg = 4,
                            .pLlRtCfg = &llRtCfg,
                            .pFreeMem = WsfHeapGetFreeStartAddress(),
                            .freeMemAvail = WsfHeapCountAvailable() };
    uint32_t llmemUsed = LlInitSetRtCfg(&llCfg);
    WsfHeapAlloc(llmemUsed);
    llCfg.pFreeMem = WsfHeapGetFreeStartAddress();
    llCfg.freeMemAvail = WsfHeapCountAvailable();
    if (llmemUsed != LlInit(&llCfg)) {
        WSF_ASSERT(0);
    }
    WsfCsExit();

    bdAddr_t bdAddr;
    PalCfgLoadData(PAL_CFG_ID_BD_ADDR, bdAddr, sizeof(bdAddr_t));
    LlSetBdAddr((uint8_t *)&bdAddr);

    /* trim the 32 kHz crystal against the 32 MHz one */
    PalBbEnable();
    wutTrimComplete = 0;
    MXC_WUT_TrimCrystalAsync(MXC_WUT0, wutTrimCb);
    /* ADI's example waits for ever; give up after 2 s so a crystal problem
     * can't stop the console (and audio) from coming up */
    uint32_t t0 = oe_ticks(), spins = 0;
    while (!wutTrimComplete && (oe_ticks() - t0) < oe_ticks_per_ms() * 2000 && ++spins < 400000000u) {
    }
    if (!wutTrimComplete) {
        printf("[ble] 32 kHz crystal trim did not finish (continuing)\n");
    }
    PalBbDisable();

    stack_init();
    app_start();

    /* the BLE platform layer drives LED 1 (our green LED) as a CPU-activity
     * light from its interrupts; OpenEar owns the LEDs */
    PalLedDeInit();
}

void oe_ble_poll(void)
{
    /* WsfOsEnterMainLoop() without the sleep: the audio needs the CPU awake */
    WsfTimerSleepUpdate();
    wsfOsDispatcher();
    /* if a notification's confirmation never arrives (e.g. a buffer
     * allocation failed inside the stack), don't stall the console for ever */
    if (tx_busy && (oe_ticks() - tx_t0) > oe_ticks_per_ms() * 200) {
        tx_busy = 0;
        tx_pump();
    }
}

int oe_ble_connected(void)
{
    return connId != DM_CONN_ID_NONE;
}

void oe_ble_status(char *buf, int len)
{
    snprintf(buf, len, "%s, rx %lu lines, tx %lu bytes, dropped %lu",
             connId != DM_CONN_ID_NONE ? "connected" : "advertising as OpenEar",
             (unsigned long)rx_lines, (unsigned long)tx_bytes, (unsigned long)tx_dropped);
}
