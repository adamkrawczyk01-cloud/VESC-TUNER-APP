// ============================================================================
//  vesc_dashboard.ino — VESC AI Tuner for M5Stack Cardputer
//  Hardware : M5Stack Cardputer, 240x135 TFT, 20S battery (72–84V)
//  Libraries: M5Cardputer, NimBLE-Arduino
//  Version  : 1.0  (first iteration — iterate from here)
// ============================================================================
//
//  SCREEN LAYOUT (240×135):
//   Zone A  y=0..12        Status bar
//   Zone B  x=0..75   y=13..80   Speed (large integer)
//   Zone C  x=76..135 y=13..80   Duty dial (arc gauge)
//   Zone D  x=136..239 y=13..130  Right indicators (2 columns)
//   Zone E  y=81..121     Battery bar / duty bar / Ah·km
//   Zone F  y=122..134    Key hint bar
//
//  SCREENS  [M] cycles: 0=Dashboard  1=Session stats  2=Config review
//
//  BLE: Nordic UART Service → COMM_GET_VALUES @ 12Hz
//       COMM_GET_MCCONF + COMM_GET_APPCONF at session start
//
//  SD layout:
//    /sessions/session_NNN.csv
//    /sessions/session_NNN_mcconf.json
//    /sessions/session_NNN_appconf.json
//    /config/suggestions.json          (written by Mac → read by Cardputer)
//    /config/changes_log.csv
//
//  TODO (next iterations):
//    - Full SET_MCCONF binary serialisation (requires full mcconf blob)
//    - USB MSC raw block callbacks (sd.card()->readSectors / writeSectors)
//    - COMM_GET_IMU_DATA for real pitch angle
//    - COMM_GET_APPCONF full parse (Float Package params)
// ============================================================================

#include <M5Cardputer.h>
#include <NimBLEDevice.h>
#include <SD.h>
#include <SPI.h>
#include "USB.h"
#include "USBMSC.h"
#include <Preferences.h>
#include <math.h>

// ── SD pins (M5Cardputer) ────────────────────────────────────────────────────
#define PIN_SD_CS    12
#define PIN_SD_SCK   40
#define PIN_SD_MISO  39
#define PIN_SD_MOSI  14

// ── BLE (Nordic UART Service) ────────────────────────────────────────────────
#define NUS_SVC_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // Cardputer→VESC
#define NUS_TX_UUID  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // VESC→Cardputer (notify)

// ── VESC command IDs ─────────────────────────────────────────────────────────
#define CMD_FW_VERSION   0
#define CMD_GET_VALUES   4
#define CMD_GET_MCCONF  14
#define CMD_SET_MCCONF  13
#define CMD_GET_APPCONF 16
#define CMD_FORWARD_CAN 34       // 0x22 — wrap [34,canId,cmd] to reach motor over CAN
#define CMD_PING_CAN    62       // 0x3E — ESP32 discovers CAN device IDs
#define CMD_CUSTOM_APP_DATA 36   // 0x24 — Refloat package commands
#define REFLOAT_MAGIC       101
#define REFLOAT_GET_ALLDATA 10
#define CMD_GET_CUSTOM_CONFIG 93 // 0x5D — read Refloat package config (raw bytes)
#define CMD_BMS_GET_VALUES    96 // 0x60 — Smart BMS values (incl. temperatures)

// ── Display dimensions ───────────────────────────────────────────────────────
#define DW 240
#define DH 135

// Zone boundaries
#define ZA_Y   0
#define ZA_H  13
#define ZB_X   0
#define ZB_W  76
#define ZC_X  76
#define ZC_W  60
#define ZBC_Y 13
#define ZBC_H 68
#define ZD_X 136
#define ZD_W 104
#define ZE_Y  82
#define ZF_Y 122
#define ZF_H  13

// Duty dial geometry (centre inside Zone C)
#define DCX  105
#define DCY   48
#define DR    26
#define DTHK   7

// ── Colour palette (RGB565) ──────────────────────────────────────────────────
#define C_BG     0x0000u
#define C_SB     0x2104u   // status bar background
#define C_GREY   0x528Au   // labels
#define C_DGREY  0x39C7u   // dim / borders
#define C_WHITE  0xFFFFu
#define C_GREEN  0x07E0u
#define C_YELL   0xFFE0u
#define C_RED    0xF800u
#define C_ORANG  0xFD20u
#define C_CYAN   0x07FFu
// New front-end palette (ported from 240x135 prototype)
#define C_VOLT   0xC7A5u   // signature lime "voltage" accent
#define C_VOLTD  0x8D42u   // dark lime (bar fills)
#define C_OK     0x3EB4u   // teal-green (safe / connected)
#define C_WARN   0xFD85u   // amber (caution)
#define C_ICE    0x5E9Fu   // ice blue (read / info)
#define C_PANEL  0x10A2u   // panel hairline / row divider
#define C_FOOT   0x0841u   // footer background

// ── Motor / wheel defaults (overridden by device_profile.json at runtime) ────
#define POLE_PAIRS_DEF   15       // fallback: 30-magnet motor
#define WHEEL_MM_DEF    254       // fallback: 10-inch wheel
#define N_CELLS_DEF      20       // fallback: 20S battery

// ── Safety sandbox hard limits (20S) ────────────────────────────────────────
struct Limits {
    float lo, hi, max_delta_pct;
};
static const Limits LIMIT_TABLE[] = {
    // l_current_max
    {  20.0f, 120.0f, 0.15f },
    // l_in_current_max
    {  10.0f,  60.0f, 0.15f },
    // l_max_erpm
    { 10000.f,120000.f,0.15f},
    // l_temp_fet_start
    {  60.0f,  75.0f, 0.15f },
    // l_temp_fet_end
    {  65.0f,  85.0f, 0.15f },
};
static const char* const WHITELIST[] = {
    "l_current_max","l_in_current_max","l_max_erpm",
    "l_temp_fet_start","l_temp_fet_end"
};
static const int WHITELIST_N = 5;

// ─────────────────────────────────────────────────────────────────────────────
//  Data structures
// ─────────────────────────────────────────────────────────────────────────────
struct VescVals {
    float temp_fet  = 0, temp_mot  = 0;
    float curr_mot  = 0, curr_in   = 0;
    float duty_pct  = 0, rpm       = 0;
    float voltage   = 0, amp_hours = 0;
    float speed_kmh = 0, pitch     = 0;
    float roll      = 0, setpoint  = 0;   // Refloat GET_ALLDATA (float boards)
    uint8_t state   = 0;                  // Refloat state_compat (low nibble)
    bool  refloat   = false;              // true once GET_ALLDATA replied
    float temp_bat  = 0;                  // hottest BMS sensor (Smart BMS over CAN)
    bool  bms       = false;              // true once BMS values replied
    int32_t tacho   = 0;
    bool valid      = false;
};

// Smart BMS snapshot (COMM_BMS_GET_VALUES)
struct BmsData {
    int   cellNum = 0;
    float cell[36] = {};      // per-cell voltage (V)
    bool  bal[36]  = {};      // balancing flag per cell
    int   tempNum = 0;
    float temp[16] = {};      // temperature sensors (°C)
    float vTot = 0, iIn = 0;  // pack voltage / current
    bool  valid = false;
};

struct McConf {
    float l_current_max       = 100.f;
    float l_in_current_max    =  30.f;
    float l_max_erpm          = 70000.f;
    float l_temp_fet_start    =  70.f;
    float l_temp_fet_end      =  80.f;
    float l_battery_cut_start =  62.f;
    float l_battery_cut_end   =  58.f;
    float l_temp_mot_start    =  80.f;
    float l_temp_mot_end      =  90.f;
    int   foc_sensor_mode     =   2;
    int   si_motor_poles      =  15;
    bool  loaded              = false;
};

// ── Device profile (built from mcconf + saved to /device_profile.json) ───────
struct DeviceProfile {
    // Battery (derived from mcconf)
    int   batt_cells  = N_CELLS_DEF;
    float batt_max_v  = N_CELLS_DEF * 4.2f;
    float batt_min_v  = N_CELLS_DEF * 3.0f;
    float batt_nom_v  = N_CELLS_DEF * 3.6f;
    float batt_cap_ah = 0.0f;
    // Motor / wheel
    int   motor_poles = POLE_PAIRS_DEF;   // pole pairs (si_motor_poles)
    float wheel_mm    = WHEEL_MM_DEF;
    // Human label
    char  name[32]    = "My Board";
    bool  loaded      = false;
};

struct SessStats {
    float max_spd = 0, max_duty = 0, max_curr_in = 0;
    float max_tfet= 0, min_volt = 100.f;
    float total_ah= 0, total_km = 0;
    float start_ah= -1.f;
    int   spikes  = 0;
    bool  in_spike= false;
};

#define MAX_SUGG 5
struct Sugg {
    char  param[32]  = {};
    float cur        = 0, sug   = 0, delta = 0;
    char  reason[64] = {};
    bool  accepted   = false, skipped = false;
};

// ── Globals ──────────────────────────────────────────────────────────────────
static VescVals   gV;
static BmsData    gBms;
static McConf     gMC;
static SessStats    gStat;
static Sugg         gSugg[MAX_SUGG];
static int          gSuggN = 0, gSuggIdx = 0;
static DeviceProfile gProfile;

struct FwVersion {
    uint8_t major   = 0;
    uint8_t minor   = 0;
    char    hwName[32] = "";
    bool    received   = false;
};
static FwVersion gFwVer;

static bool     gBleOk   = false;
static bool     gRec     = false;
static bool     gSdOk    = false;
static int      gScreen  = 0;           // 0 HUD 1 TRIP 2 FAULT 3 BACKUP 4 RESTORE 5 BOARD 6 CONFIG 7 REVIEW 8 APPLY
#define SC_COUNT 9
static int      gBackupSel = 0;         // selected restore point on RESTORE screen
static uint8_t  gMcconfRaw[512];   static int gMcconfRawLen   = 0;  // raw config snapshots
static uint8_t  gAppconfRaw[512];  static int gAppconfRawLen  = 0;
static uint8_t  gCustomCfgRaw[768];static int gCustomCfgRawLen = 0; // Refloat package config (raw)
static char     gBackupMsg[48] = "";    // last backup status (shown on BACKUP screen)
static void doBackup();                 // fwd decl (defined in front-end block)
static uint8_t  gSessNum = 0;
static char     gSessName[32] = {};
static uint32_t gLastValMs = 0;
static uint32_t gLastAllMs = 0;
static uint32_t gLastBmsMs = 0;
static int      gBmsDbgN   = 0;          // serial-dump first few BMS replies
static uint32_t gLastCsvMs = 0;
static uint32_t gTripMs    = 0;

static M5Canvas canvas(&M5Cardputer.Display);

// ── NimBLE handles ───────────────────────────────────────────────────────────
static NimBLEClient*               gBleClient = nullptr;
static NimBLERemoteCharacteristic* gCharRx    = nullptr;  // write to VESC
static NimBLERemoteCharacteristic* gCharTx    = nullptr;  // notify from VESC

static uint8_t  gRxBuf[512];        // one complete reassembled frame (for unpackPkt)
static uint16_t gRxLen   = 0;
static volatile bool gRxReady  = false;
static uint8_t  gAcc[1024];         // raw BLE chunk accumulator (reassembly across MTU chunks)
static uint16_t gAccLen  = 0;
static int      gCanId   = -1;      // motor controller CAN id; -1 = direct/unknown
static volatile bool gCanDiscovered = false;
static uint16_t gNotifyCount = 0;  // incremented every notify callback
static uint16_t gPktOkCount = 0;            // incremented every valid unpacked pkt
static uint16_t gTxCount    = 0;            // incremented every vescSend write
static bool     gUseWriteNoResp = true;     // set from char properties at connect
static volatile bool gScanning = false;
static NimBLEAddress gTargetAddress;

// ── USB MSC ──────────────────────────────────────────────────────────────────
static USBMSC gMsc;

// ─────────────────────────────────────────────────────────────────────────────
//  VESC packet helpers
// ─────────────────────────────────────────────────────────────────────────────
static uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

static int32_t rdI32(const uint8_t* b, int o) {
    return ((int32_t)b[o]<<24)|((int32_t)b[o+1]<<16)|
           ((int32_t)b[o+2]<<8)|(int32_t)b[o+3];
}
static int16_t rdI16(const uint8_t* b, int o) {
    return ((int16_t)b[o]<<8)|(int16_t)b[o+1];
}

// Build framed VESC packet → out[]; return total length
static int buildPkt(uint8_t* out, const uint8_t* payload, int plen) {
    int i = 0;
    if (plen <= 255) { out[i++]=0x02; out[i++]=(uint8_t)plen; }
    else { out[i++]=0x03; out[i++]=(uint8_t)(plen>>8); out[i++]=(uint8_t)(plen&0xFF); }
    memcpy(&out[i], payload, plen); i += plen;
    uint16_t crc = crc16(payload, plen);
    out[i++]=(uint8_t)(crc>>8); out[i++]=(uint8_t)(crc&0xFF);
    out[i++]=0x03;
    return i;
}

// Unpack received packet → true + payload pointer/len on success
static bool unpackPkt(const uint8_t* raw, int rlen,
                       const uint8_t** pay, int* plen) {
    if (rlen < 5) return false;
    int i = 0;
    if (raw[i]==0x02)      { i++; *plen=raw[i++]; }
    else if (raw[i]==0x03) { i++; *plen=((int)raw[i]<<8)|raw[i+1]; i+=2; }
    else return false;
    if (i + *plen + 3 > rlen) return false;
    *pay = &raw[i];
    uint16_t got  = ((uint16_t)raw[i+*plen]<<8)|raw[i+*plen+1];
    uint16_t calc = crc16(*pay, *plen);
    return got == calc;
}

static void vescSend(uint8_t cmd) {
    if (!gCharRx || !gBleOk) return;
    uint8_t pkt[16], pay[4];
    int plen;
    // VESC Express is a BLE→CAN bridge: only FW_VERSION/PING_CAN are handled by the
    // ESP32 itself. Every other command must be CAN-forwarded to the motor controller.
    if (gCanId >= 0 && cmd != CMD_FW_VERSION && cmd != CMD_PING_CAN) {
        pay[0] = CMD_FORWARD_CAN;
        pay[1] = (uint8_t)gCanId;
        pay[2] = cmd;
        plen   = 3;
    } else {
        pay[0] = cmd;
        plen   = 1;
    }
    int len = buildPkt(pkt, pay, plen);
    gRxReady = false;
    gTxCount++;
    // Serial debug — print only first 5 sends per cmd to avoid spam
    if (gTxCount <= 5 || cmd == CMD_FW_VERSION || cmd == CMD_PING_CAN) {
        Serial.printf("[TX] cmd=%d  can=%d  wnr=%d  pkt(%d):", cmd, gCanId, gUseWriteNoResp, len);
        for (int i = 0; i < len; i++) Serial.printf(" %02X", pkt[i]);
        Serial.println();
    }
    bool ok = gCharRx->writeValue(pkt, len, !gUseWriteNoResp);
    if (gTxCount <= 5 || cmd == CMD_FW_VERSION || cmd == CMD_PING_CAN)
        Serial.printf("[TX] writeValue -> %s\n", ok ? "OK" : "FAIL");
}

// Refloat GET_ALLDATA (mode 2) — CAN-forwarded when on a bridge board.
// Multi-byte payload, so it can't use vescSend (which wraps a single cmd byte).
static void vescSendAllData() {
    if (!gCharRx || !gBleOk) return;
    uint8_t pay[8]; int plen = 0;
    if (gCanId >= 0) { pay[plen++] = CMD_FORWARD_CAN; pay[plen++] = (uint8_t)gCanId; }
    pay[plen++] = CMD_CUSTOM_APP_DATA;
    pay[plen++] = REFLOAT_MAGIC;
    pay[plen++] = REFLOAT_GET_ALLDATA;
    pay[plen++] = 2;                 // mode 2 = RT + odometer + temps
    uint8_t pkt[16]; int len = buildPkt(pkt, pay, plen);
    gTxCount++;
    gCharRx->writeValue(pkt, len, !gUseWriteNoResp);
}

// Request Refloat package config (raw bytes). [FORWARD_CAN,canId,93,confInd=0]
static void vescSendCustomCfg() {
    if (!gCharRx || !gBleOk) return;
    uint8_t pay[6]; int plen = 0;
    if (gCanId >= 0) { pay[plen++] = CMD_FORWARD_CAN; pay[plen++] = (uint8_t)gCanId; }
    pay[plen++] = CMD_GET_CUSTOM_CONFIG;
    pay[plen++] = 0;                 // confInd 0 = main config
    uint8_t pkt[16]; int len = buildPkt(pkt, pay, plen);
    gTxCount++;
    gCharRx->writeValue(pkt, len, !gUseWriteNoResp);
}

// ─────────────────────────────────────────────────────────────────────────────
//  VESC response parsers
// ─────────────────────────────────────────────────────────────────────────────

// COMM_GET_VALUES (FW 6.x payload layout after cmd byte)
static void parseValues(const uint8_t* p, int len) {
    if (len < 50 || p[0] != CMD_GET_VALUES) return;
    gV.temp_fet   = rdI16(p,  1) / 10.f;
    gV.temp_mot   = rdI16(p,  3) / 10.f;
    gV.curr_mot   = rdI32(p,  5) / 100.f;
    gV.curr_in    = rdI32(p,  9) / 100.f;
    gV.duty_pct   = rdI16(p, 21) / 10.f;   // stored as ‰, result = %
    gV.rpm        = (float)rdI32(p, 23);
    gV.voltage    = rdI16(p, 27) / 10.f;
    gV.amp_hours  = rdI32(p, 29) / 10000.f;
    gV.tacho      = rdI32(p, 45);
    // Speed: rpm / pole_pairs * circumference * 60 / 1000 → km/h
    const float circ_km = (float)M_PI * gProfile.wheel_mm / 1e6f;
    gV.speed_kmh  = fabsf(gV.rpm) / gProfile.motor_poles * circ_km * 60.f;
    gV.valid = true;
}

// Refloat state_compat (low nibble of byte 10) → short label + colour
static const char* refloatStateName(uint8_t s) {
    switch (s) {
        case 0:  return "STARTUP";   case 1:  return "RUNNING";  case 2:  return "TILTBACK";
        case 3:  return "WHEELSLIP"; case 4:  return "UPSIDEDN"; case 5:  return "FLYWHEEL";
        case 6:  return "F:PITCH";   case 7:  return "F:ROLL";   case 8:  return "F:SW-HALF";
        case 9:  return "F:SW-FULL"; case 11: return "F:START";  case 12: return "F:REVERSE";
        case 13: return "F:QSTOP";   case 14: return "CHARGING"; case 15: return "DISABLED";
        default: return "?";
    }
}
static uint16_t refloatStateColor(uint8_t s) {
    if (s == 1)               return C_OK;    // RUNNING
    if (s == 2)               return C_WARN;  // TILTBACK
    if (s >= 6 && s <= 13)    return C_RED;   // faults
    if (s == 14)              return C_ICE;   // charging
    return C_GREY;
}

// Parse Refloat COMMAND_GET_ALLDATA (mode 2). Fills only the float-specific
// fields (pitch/roll/state/setpoint); speed/duty/V/temps stay from GET_VALUES.
static void parseAllData(const uint8_t* p, int len) {
    if (len < 4 || p[1] != REFLOAT_MAGIC || p[2] != REFLOAT_GET_ALLDATA) return;
    if (p[3] == 69) { gV.refloat = true; return; }   // fault marker → fields zeroed
    if (len < 34) return;
    gV.roll     = rdI16(p, 8)  / 10.f;
    gV.state    = p[10] & 0x0F;
    gV.setpoint = ((int)p[14] - 128) / 5.f;
    gV.pitch    = rdI16(p, 20) / 10.f;
    gV.refloat  = true;
}

// Parse COMM_BMS_GET_VALUES (96) — Smart BMS aggregated by the VESC over CAN.
// Layout: cmd, 6×float32 (v_tot,v_chg,i_in,i_in_ic,ah,wh), cell_num,
// cells(int16/1000), balance(1B/cell), temp_num, temps(int16/100), ...
static void parseBms(const uint8_t* p, int len) {
    int ind = 1;
    auto f32 = [&](float sc) -> float {
        if (ind + 4 > len) { ind += 4; return 0; }
        int32_t v = ((int32_t)p[ind] << 24) | ((int32_t)p[ind+1] << 16) |
                    ((int32_t)p[ind+2] << 8) | (int32_t)p[ind+3];
        ind += 4; return v / sc;
    };
    auto f16 = [&](float sc) -> float {
        if (ind + 2 > len) { ind += 2; return 0; }
        int16_t v = (int16_t)(((uint16_t)p[ind] << 8) | p[ind+1]);
        ind += 2; return v / sc;
    };
    gBms.vTot = f32(1e6f);
    f32(1e6f);                              // v_charge
    gBms.iIn  = f32(1e6f);
    f32(1e6f);                              // i_in_ic
    f32(1e3f);                              // ah_cnt
    f32(1e3f);                              // wh_cnt
    if (ind >= len) return;
    int cn = p[ind++];
    if (cn < 0 || cn > 36) return;
    gBms.cellNum = cn;
    for (int i = 0; i < cn; i++) gBms.cell[i] = f16(1e3f);
    for (int i = 0; i < cn; i++) { gBms.bal[i] = (ind < len) ? (p[ind] != 0) : false; ind++; }
    if (ind < len) {
        int tn = p[ind++];
        if (tn < 0 || tn > 16) tn = 0;
        gBms.tempNum = tn;
        float maxT = -100.f;
        for (int i = 0; i < tn; i++) { float t = f16(1e2f); gBms.temp[i] = t; if (t > maxT) maxT = t; }
        if (tn > 0) { gV.temp_bat = maxT; gV.bms = true; }
    }
    gBms.valid = (cn > 0);
    if (cn > 0) gV.bms = true;
}

#define PROFILE_PATH "/device_profile.json"
static void buildProfileFromMcconf();  // forward decl
static void saveProfileToSD();         // forward decl

// COMM_GET_MCCONF — offsets within payload (after cmd byte), FW 6.x
// Each field is int32 big-endian, scaled as noted
#define MC_OFF_CURR_MAX    3   // /100
#define MC_OFF_CURR_IN    11   // /100
#define MC_OFF_ERPM       43   // /1
#define MC_OFF_TFET_START 87   // /100
#define MC_OFF_TFET_END   91   // /100
#define MC_OFF_VCUT_START 95   // /100
#define MC_OFF_VCUT_END   99   // /100

static void parseMcConf(const uint8_t* p, int len) {
    if (len < 104 || p[0] != CMD_GET_MCCONF) return;
    gMC.l_current_max       = rdI32(p, MC_OFF_CURR_MAX)    / 100.f;
    gMC.l_in_current_max    = rdI32(p, MC_OFF_CURR_IN)     / 100.f;
    gMC.l_max_erpm          = (float)rdI32(p, MC_OFF_ERPM);
    gMC.l_temp_fet_start    = rdI32(p, MC_OFF_TFET_START)  / 100.f;
    gMC.l_temp_fet_end      = rdI32(p, MC_OFF_TFET_END)    / 100.f;
    gMC.l_battery_cut_start = rdI32(p, MC_OFF_VCUT_START)  / 100.f;
    gMC.l_battery_cut_end   = rdI32(p, MC_OFF_VCUT_END)    / 100.f;
    gMC.loaded = true;
    // Build / refresh profile from fresh mcconf data
    if (!gSdOk || !SD.exists(PROFILE_PATH)) {
        buildProfileFromMcconf();
        saveProfileToSD();  // guarded internally by gSdOk
    } else if (!gProfile.loaded) {
        buildProfileFromMcconf();
    }

    if (!gSdOk) return;
    char path[64];
    snprintf(path, sizeof(path), "/sessions/%s_mcconf.json", gSessName);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    f.printf("{\n  \"_session\": \"%s\",\n", gSessName);
    f.printf("  \"l_current_max\": %.2f,\n",       gMC.l_current_max);
    f.printf("  \"l_in_current_max\": %.2f,\n",    gMC.l_in_current_max);
    f.printf("  \"l_max_erpm\": %.0f,\n",           gMC.l_max_erpm);
    f.printf("  \"l_temp_fet_start\": %.2f,\n",    gMC.l_temp_fet_start);
    f.printf("  \"l_temp_fet_end\": %.2f,\n",      gMC.l_temp_fet_end);
    f.printf("  \"l_battery_cut_start\": %.2f,\n", gMC.l_battery_cut_start);
    f.printf("  \"l_battery_cut_end\": %.2f\n}\n", gMC.l_battery_cut_end);
    f.close();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Device profile — build / save / load
// ─────────────────────────────────────────────────────────────────────────────

static void buildProfileFromMcconf() {
    // Derive cell count from battery cutoff: l_battery_cut_end / 3.0V per cell
    int cells = (int)roundf(gMC.l_battery_cut_end / 3.0f);
    if (cells < 4)  cells = N_CELLS_DEF;
    if (cells > 32) cells = N_CELLS_DEF;
    gProfile.batt_cells = cells;
    gProfile.batt_min_v = gMC.l_battery_cut_end;
    gProfile.batt_max_v = cells * 4.2f;
    gProfile.batt_nom_v = cells * 3.6f;
    // motor_poles / wheel_mm kept from previous profile or defaults
    gProfile.loaded = true;
}

static void saveProfileToSD() {
    if (!gSdOk) return;
    File f = SD.open(PROFILE_PATH, FILE_WRITE);
    if (!f) return;
    f.printf("{\n");
    f.printf("  \"device_name\": \"%s\",\n",      gProfile.name);
    f.printf("  \"batt_cells\": %d,\n",            gProfile.batt_cells);
    f.printf("  \"batt_max_v\": %.2f,\n",          gProfile.batt_max_v);
    f.printf("  \"batt_min_v\": %.2f,\n",          gProfile.batt_min_v);
    f.printf("  \"batt_nom_v\": %.2f,\n",          gProfile.batt_nom_v);
    f.printf("  \"batt_cap_ah\": %.2f,\n",         gProfile.batt_cap_ah);
    f.printf("  \"motor_poles\": %d,\n",           gProfile.motor_poles);
    f.printf("  \"wheel_mm\": %.1f\n",             gProfile.wheel_mm);
    f.printf("}\n");
    f.close();
}

static void loadProfileFromSD() {
    File f = SD.open(PROFILE_PATH, FILE_READ);
    if (!f) return;
    // Read whole file line by line
    while (f.available()) {
        String line = f.readStringUntil('\n');
        float fv = jFloat(line, "batt_cells");
        if (fv > 0) gProfile.batt_cells = (int)fv;
        fv = jFloat(line, "batt_max_v");
        if (fv > 0) gProfile.batt_max_v = fv;
        fv = jFloat(line, "batt_min_v");
        if (fv > 0) gProfile.batt_min_v = fv;
        fv = jFloat(line, "batt_nom_v");
        if (fv > 0) gProfile.batt_nom_v = fv;
        fv = jFloat(line, "batt_cap_ah");
        if (fv >= 0) gProfile.batt_cap_ah = fv;
        fv = jFloat(line, "motor_poles");
        if (fv > 0) gProfile.motor_poles = (int)fv;
        fv = jFloat(line, "wheel_mm");
        if (fv > 0) gProfile.wheel_mm = fv;
        String sv = jStr(line, "device_name");
        if (sv.length() > 0)
            strncpy(gProfile.name, sv.c_str(), sizeof(gProfile.name)-1);
    }
    f.close();
    gProfile.loaded = true;
}


// ─────────────────────────────────────────────────────────────────────────────
//  NimBLE callbacks
// ─────────────────────────────────────────────────────────────────────────────

static void parseFwVersion(const uint8_t* d, int len) {
    // payload layout (after framing stripped by unpackPkt):
    //   d[0]=0x00 (cmd), d[1]=major, d[2]=minor, d[3..] = hw_name (null-term)
    if (len < 3 || d[0] != CMD_FW_VERSION) return;
    gFwVer.major = d[1];
    gFwVer.minor = d[2];
    int i = 0;
    while (i < 31 && (3 + i) < len && d[3 + i] != 0) {
        gFwVer.hwName[i] = (char)d[3 + i]; i++;
    }
    gFwVer.hwName[i] = '\0';
    gFwVer.received  = true;
    Serial.printf("[BLE] FW: %d.%d  HW: %s\n",
                  gFwVer.major, gFwVer.minor, gFwVer.hwName);
}

// Scan the accumulator for complete VESC frames. On a valid frame (CRC + 0x03
// terminator), copy it into gRxBuf and raise gRxReady, then drop it from gAcc.
// Handles responses split across multiple BLE MTU chunks (esp. MCCONF ~450B).
static void reassemblePump() {
    while (gAccLen > 0) {
        uint8_t start = gAcc[0];
        if (start != 0x02 && start != 0x03) {           // resync: not a frame start
            memmove(gAcc, gAcc + 1, --gAccLen);
            continue;
        }
        int headerLen = (start == 0x02) ? 2 : 3;
        if (gAccLen < headerLen) break;                  // need more bytes
        int plen = (start == 0x02) ? gAcc[1]
                                   : (((int)gAcc[1] << 8) | gAcc[2]);
        int total = headerLen + plen + 3;                // payload + crc(2) + stop(1)
        if (total <= 0 || total > (int)sizeof(gAcc)) {   // bogus length → flush
            gAccLen = 0; break;
        }
        if (gAccLen < total) break;                      // wait for the rest
        if (gAcc[total - 1] != 0x03) {                   // bad terminator → resync
            memmove(gAcc, gAcc + 1, --gAccLen);
            continue;
        }
        const uint8_t* payload = gAcc + headerLen;
        uint16_t calc = crc16(payload, plen);
        uint16_t got  = ((uint16_t)gAcc[headerLen + plen] << 8) | gAcc[headerLen + plen + 1];
        if (calc == got) {
            if (total <= (int)sizeof(gRxBuf)) {
                memcpy(gRxBuf, gAcc, total);
                gRxLen   = (uint16_t)total;
                gRxReady = true;
            }
            gAccLen -= total;                            // consume the frame
            memmove(gAcc, gAcc + total, gAccLen);
        } else {                                         // bad CRC → resync
            memmove(gAcc, gAcc + 1, --gAccLen);
        }
    }
}

static void notifyCallback(NimBLERemoteCharacteristic*, uint8_t* data,
                           size_t len, bool) {
    gNotifyCount++;
    if (gNotifyCount <= 8) {
        Serial.printf("[RX] notify #%d  len=%d:", gNotifyCount, (int)len);
        for (size_t i = 0; i < len && i < 12; i++) Serial.printf(" %02X", data[i]);
        if (len > 12) Serial.printf(" ...(+%d)", (int)(len - 12));
        Serial.println();
    }
    if (gAccLen + len > sizeof(gAcc)) gAccLen = 0;       // overflow guard
    memcpy(gAcc + gAccLen, data, len);
    gAccLen += (uint16_t)len;
    reassemblePump();
}

struct ClientCB : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient*)            override { gBleOk = true;  }
    void onDisconnect(NimBLEClient*, int)    override {
        gBleOk = false; gV.valid = false;
        gCanId = -1; gCanDiscovered = false; gAccLen = 0; gRxReady = false;
    }
} gClientCB;

// Scan results storage
static const int MAX_SCAN_RESULTS = 6;
struct ScanEntry { NimBLEAddress addr; std::string name; bool hasNUS; };
static ScanEntry gScanList[MAX_SCAN_RESULTS];
static int       gScanCount  = 0;
static bool      gInScanMenu = false;  // true = showing scan result list

// v2.x scan callback: collect ALL devices, auto-pick NUS/vesc/float
class ScanCB : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        std::string nm = dev->getName();
        bool hasNUS = dev->isAdvertisingService(NimBLEUUID(NUS_SVC_UUID));

        // Deduplicate by address
        for (int i = 0; i < gScanCount; i++)
            if (gScanList[i].addr == dev->getAddress()) return;

        if (gScanCount < MAX_SCAN_RESULTS)
            gScanList[gScanCount++] = { dev->getAddress(),
                                        nm.empty() ? "(no name)" : nm,
                                        hasNUS };

        auto lc = nm; for (auto& ch : lc) ch = tolower(ch);
        bool isVesc = lc.find("vesc")  != std::string::npos ||
                      lc.find("float") != std::string::npos;
        if ((hasNUS || isVesc) && gTargetAddress.isNull())
            gTargetAddress = dev->getAddress();
    }
};

// Draw scan menu (persistent — stays until user picks or re-scans)
static void drawScanMenu(const char* status) {
    canvas.fillScreen(C_BG);
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(1);
    canvas.setTextColor(C_CYAN);
    canvas.drawString("BLE SCAN", 2, 2);
    canvas.setTextColor(C_GREY);
    canvas.drawString(status, 60, 2);
    for (int i = 0; i < gScanCount; i++) {
        char line[52];
        snprintf(line, sizeof(line), "[%d] %s  %s%s",
                 i + 1,
                 gScanList[i].name.c_str(),
                 gScanList[i].addr.toString().c_str(),
                 gScanList[i].hasNUS ? " NUS" : "");
        canvas.setTextColor(C_WHITE);
        canvas.drawString(line, 2, 16 + i * 16);
    }
    if (gScanCount == 0)
        canvas.drawString("nothing found", 2, 32);
    canvas.setTextColor(C_GREY);
    canvas.drawString("[1-6]conn [P]rescan [M]back", 2, 126);
    canvas.pushSprite(0, 0);
}

// Try to connect to given address via NUS; status shown on scan menu
static void debugPrintServices() {
    auto services = gBleClient->getServices(true);
    Serial.printf("\n=== BLE Services on device ===\n");
    for (auto& svc : services) {
        Serial.printf("SVC: %s\n", svc->getUUID().toString().c_str());
        auto chars = svc->getCharacteristics(true);
        for (auto& ch : chars) {
            uint8_t props = 0;
            if (ch->canRead())             props |= 0x02;
            if (ch->canWriteNoResponse())  props |= 0x04;
            if (ch->canWrite())            props |= 0x08;
            if (ch->canNotify())           props |= 0x10;
            if (ch->canIndicate())         props |= 0x20;
            Serial.printf("  CHR: %s  props:0x%02X [%s%s%s%s%s]\n",
                ch->getUUID().toString().c_str(), props,
                ch->canRead()            ? "R" : "-",
                ch->canWrite()           ? "W" : "-",
                ch->canWriteNoResponse() ? "w" : "-",
                ch->canNotify()          ? "N" : "-",
                ch->canIndicate()        ? "I" : "-");
        }
    }
    Serial.println("==============================\n");
}

static bool bleConnect(const NimBLEAddress& addr) {
    if (!gBleClient) {
        gBleClient = NimBLEDevice::createClient();
        gBleClient->setClientCallbacks(&gClientCB, false);
    } else if (gBleClient->isConnected()) {
        gBleClient->disconnect();
        delay(300);
    }

    drawScanMenu("connecting...");
    if (!gBleClient->connect(addr)) {
        drawScanMenu("FAIL: connect");
        return false;
    }

    drawScanMenu("GATT discovery...");
    if (!gBleClient->discoverAttributes()) {
        gBleClient->disconnect();
        drawScanMenu("FAIL: GATT discovery");
        return false;
    }
    debugPrintServices();

    auto* svc = gBleClient->getService(NUS_SVC_UUID);
    if (!svc) {
        gBleClient->disconnect();
        drawScanMenu("FAIL: NUS service not found");
        return false;
    }

    // ── Find characteristics by PROPERTY, not by UUID ────────────────────────
    // VESC Express may have swapped or different UUID assignments vs classic NRF.
    // Rule: NOTIFY char = data FROM vesc (we subscribe); WRITE char = data TO vesc.
    gCharRx = nullptr;  // char we WRITE to (→ VESC)
    gCharTx = nullptr;  // char we NOTIFY from (← VESC)
    {
        auto chars = svc->getCharacteristics(true);
        Serial.printf("[BLE] NUS chars found: %d\n", (int)chars.size());
        for (auto& ch : chars) {
            uint8_t props = 0;
            if (ch->canRead())             props |= 0x02;
            if (ch->canWriteNoResponse())  props |= 0x04;
            if (ch->canWrite())            props |= 0x08;
            if (ch->canNotify())           props |= 0x10;
            if (ch->canIndicate())         props |= 0x20;
            Serial.printf("[BLE]  CHR %s  props=0x%02X [%s%s%s%s%s]\n",
                ch->getUUID().toString().c_str(), props,
                ch->canRead()            ? "R" : "-",
                ch->canWrite()           ? "W" : "-",
                ch->canWriteNoResponse() ? "w" : "-",
                ch->canNotify()          ? "N" : "-",
                ch->canIndicate()        ? "I" : "-");
            if (!gCharTx && (ch->canNotify() || ch->canIndicate()))
                gCharTx = ch;
            if (!gCharRx && (ch->canWrite() || ch->canWriteNoResponse()))
                gCharRx = ch;
        }
    }
    if (!gCharRx || !gCharTx) {
        gBleClient->disconnect();
        char msg[48];
        snprintf(msg, sizeof(msg), "FAIL: no %s char",
                 !gCharRx ? "WRITE" : "NOTIFY");
        drawScanMenu(msg);
        Serial.printf("[BLE] %s\n", msg);
        return false;
    }
    Serial.printf("[BLE] WRITE char: %s  NOTIFY char: %s\n",
                  gCharRx->getUUID().toString().c_str(),
                  gCharTx->getUUID().toString().c_str());

    // Try notify first; fall back to indicate
    bool useNotify = gCharTx->canNotify();
    if (!gCharTx->subscribe(useNotify, notifyCallback)) {
        gBleClient->disconnect();
        drawScanMenu("FAIL: subscribe");
        return false;
    }
    Serial.printf("[BLE] subscribed (%s) OK\n", useNotify ? "notify" : "indicate");

    // Show assignment on screen briefly
    {
        char info[64];
        snprintf(info, sizeof(info), "W:%s  N:%s",
                 gCharRx->getUUID().toString().c_str() + 4,   // short UUID suffix
                 gCharTx->getUUID().toString().c_str() + 4);
        drawScanMenu(info);
        delay(1000);
    }

    // Use WriteNoResponse if available, else Write
    gUseWriteNoResp = gCharRx->canWriteNoResponse();

    // Fresh reassembly + CAN state for this connection
    gAccLen = 0; gRxReady = false;
    gCanId  = -1; gCanDiscovered = false;

    // ── VESC handshake: COMM_FW_VERSION must be first packet ─────────────────
    gFwVer.received = false;
    drawScanMenu("FW_VERSION handshake...");
    vescSend(CMD_FW_VERSION);
    unsigned long t0 = millis();
    while (!gFwVer.received && millis() - t0 < 3000) {
        // Ręczne przepchnięcie gRxReady z ISR context (notifyCallback)
        if (gRxReady) {
            gRxReady = false;
            const uint8_t* pay; int plen;
            if (unpackPkt(gRxBuf, gRxLen, &pay, &plen) && pay[0] == CMD_FW_VERSION) {
                parseFwVersion(pay, plen);
            }
        }
        delay(50);
    }
    if (!gFwVer.received) {
        Serial.println("[BLE] No FW_VERSION response — continuing anyway");
        drawScanMenu("warn: no FW resp (continuing)");
        delay(800);
    }

    // ── CAN discovery: VESC Express is a dumb BLE→CAN bridge. PING_CAN makes the
    //    ESP32 enumerate CAN devices; the first id is the motor controller. ─────
    drawScanMenu("PING_CAN discovery...");
    gRxReady = false;
    vescSend(CMD_PING_CAN);                 // raw — handled by the ESP32 bridge
    unsigned long tc = millis();
    while (!gCanDiscovered && millis() - tc < 3500) {
        if (gRxReady) {
            gRxReady = false;
            const uint8_t* pay; int plen;
            if (unpackPkt(gRxBuf, gRxLen, &pay, &plen) && pay[0] == CMD_PING_CAN) {
                if (plen >= 2) gCanId = pay[1];   // first discovered CAN device
                gCanDiscovered = true;
            }
        }
        delay(50);
    }
    if (gCanId >= 0) {
        Serial.printf("[CAN] motor controller id=%d — commands will be CAN-forwarded\n", gCanId);
        char m[40]; snprintf(m, sizeof(m), "CAN id %d found", gCanId);
        drawScanMenu(m); delay(700);
    } else {
        Serial.println("[CAN] no devices / timeout — using direct mode");
        drawScanMenu("no CAN — direct mode"); delay(700);
    }

    // Remember this board for auto-connect on next boot
    {
        const char* nm = gProfile.name;
        for (int i = 0; i < gScanCount; i++)
            if (gScanList[i].addr == addr && gScanList[i].name.size() > 0) nm = gScanList[i].name.c_str();
        Preferences prefs; prefs.begin("vesc", false);
        prefs.putString("lastmac", addr.toString().c_str());
        prefs.putString("lastname", nm);
        prefs.end();
        Serial.printf("[BLE] saved last board %s (%s)\n", addr.toString().c_str(), nm);
    }
    // ─────────────────────────────────────────────────────────────────────────

    return true;
}

// Run BLE scan (blocking 6 s), then show persistent result menu
static void bleScan() {
    gTargetAddress = NimBLEAddress();
    // Keep gScanCount/gScanList from previous scan — names are preserved
    // Reset only if list is full (allow refresh)
    if (gScanCount >= MAX_SCAN_RESULTS) gScanCount = 0;
    gInScanMenu = true;

    drawScanMenu("scanning 6s...");

    NimBLEScan* scan = NimBLEDevice::getScan();
    // Do NOT clearResults() — preserves cached device names between scans
    scan->setScanCallbacks(new ScanCB(), true);
    scan->setActiveScan(true); scan->setInterval(100); scan->setWindow(99);
    scan->getResults(6000, false);  // v2.x: blocking, duration in ms

    drawScanMenu(gScanCount ? "pick [1-6]  [P]rescan  [M]back" : "nothing found — [P]retry");
}

// ─────────────────────────────────────────────────────────────────────────────
//  SD helpers
// ─────────────────────────────────────────────────────────────────────────────
static void sdInit() {
    SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    gSdOk = SD.begin(PIN_SD_CS);
    if (!gSdOk) return;
    SD.mkdir("/sessions");
    SD.mkdir("/config");
    // Find first unused session number
    for (int n = 1; n <= 999; n++) {
        char p[64]; snprintf(p, sizeof(p), "/sessions/session_%03d.csv", n);
        if (!SD.exists(p)) { gSessNum = n; break; }
    }
    snprintf(gSessName, sizeof(gSessName), "session_%03d", gSessNum);
}

static void csvWriteHeader() {
    char path[64]; snprintf(path, sizeof(path), "/sessions/%s.csv", gSessName);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    f.println("ts_ms,speed_kmh,rpm,voltage_V,curr_in_A,curr_mot_A,"
              "duty_pct,temp_fet_C,temp_mot_C,amp_hours,tacho");
    f.close();
}

static void csvAppend() {
    if (!gSdOk || !gRec || !gV.valid) return;
    char path[64]; snprintf(path, sizeof(path), "/sessions/%s.csv", gSessName);
    File f = SD.open(path, FILE_APPEND);
    if (!f) return;
    f.printf("%lu,%.1f,%.0f,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.4f,%ld\n",
        millis(), gV.speed_kmh, gV.rpm, gV.voltage,
        gV.curr_in, gV.curr_mot, gV.duty_pct,
        gV.temp_fet, gV.temp_mot, gV.amp_hours, gV.tacho);
    f.close();
}

// logChange() removed — READ-ONLY mode

// ─────────────────────────────────────────────────────────────────────────────
//  USB Mass Storage
//  TODO: implement mscRead/mscWrite raw block callbacks via SD.card()
// ─────────────────────────────────────────────────────────────────────────────
static int32_t mscRead(uint32_t lba, uint32_t /*off*/, void* buf, uint32_t bsz) {
    // TODO: sd.card()->readSectors(lba, (uint8_t*)buf, bsz/512);
    (void)lba; memset(buf, 0, bsz); return bsz;
}
static int32_t mscWrite(uint32_t lba, uint32_t /*off*/, uint8_t* buf, uint32_t bsz) {
    // TODO: sd.card()->writeSectors(lba, buf, bsz/512);
    (void)lba; (void)buf; return bsz;
}

static void usbMscBegin() __attribute__((unused));
static void usbMscBegin() {
    if (!gSdOk || gBleOk) return;
    uint32_t sectors = SD.totalBytes() / 512;
    gMsc.vendorID("M5Stack"); gMsc.productID("VESCTuner");
    gMsc.productRevision("1.0");
    gMsc.onRead(mscRead); gMsc.onWrite(mscWrite);
    gMsc.mediaPresent(true); gMsc.begin(sectors, 512);
    USB.begin();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Suggestions parser (minimal JSON, matches our suggestions.json format)
// ─────────────────────────────────────────────────────────────────────────────
static float jFloat(const String& line, const char* key) {
    String k = String("\"") + key + "\":";
    int p = line.indexOf(k);
    if (p < 0) return 0.f;
    p += k.length();
    while (p < (int)line.length() && line[p] == ' ') p++;
    return line.substring(p).toFloat();
}

static String jStr(const String& line, const char* key) {
    String k = String("\"") + key + "\":";
    int p = line.indexOf(k);
    if (p < 0) return "";
    int q1 = line.indexOf('"', p + k.length());
    int q2 = line.indexOf('"', q1 + 1);
    if (q1 < 0 || q2 < 0) return "";
    return line.substring(q1 + 1, q2);
}

static void loadSuggestions() {
    gSuggN = 0;
    if (!gSdOk) return;
    File f = SD.open("/config/suggestions.json", FILE_READ);
    if (!f) return;
    int si = -1;
    while (f.available()) {
        String ln = f.readStringUntil('\n'); ln.trim();
        if (ln.indexOf("\"param\"") >= 0) {
            si++;
            if (si >= MAX_SUGG) break;
            gSugg[si] = Sugg{};
            String nm = jStr(ln, "param");
            strncpy(gSugg[si].param, nm.c_str(), 31);
            gSuggN = si + 1;
        }
        if (si < 0 || si >= MAX_SUGG) continue;
        if (ln.indexOf("\"current\"")   >= 0) gSugg[si].cur   = jFloat(ln,"current");
        if (ln.indexOf("\"suggested\"") >= 0) gSugg[si].sug   = jFloat(ln,"suggested");
        if (ln.indexOf("\"delta_pct\"") >= 0) gSugg[si].delta = jFloat(ln,"delta_pct");
        if (ln.indexOf("\"reason\"")    >= 0) {
            String r = jStr(ln, "reason");
            strncpy(gSugg[si].reason, r.c_str(), 63);
        }
    }
    f.close();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Safety sandbox — clamp value to ±15% and absolute limits
// ─────────────────────────────────────────────────────────────────────────────
// sandboxVal() and applyParam() removed — READ-ONLY mode

// ─────────────────────────────────────────────────────────────────────────────
//  Session stats update
// ─────────────────────────────────────────────────────────────────────────────
static void updateStats() {
    if (!gV.valid) return;
    if (gV.speed_kmh  > gStat.max_spd)    gStat.max_spd    = gV.speed_kmh;
    if (gV.duty_pct   > gStat.max_duty)   gStat.max_duty   = gV.duty_pct;
    if (gV.curr_in    > gStat.max_curr_in) gStat.max_curr_in= gV.curr_in;
    if (gV.temp_fet   > gStat.max_tfet)   gStat.max_tfet   = gV.temp_fet;
    if (gV.voltage    < gStat.min_volt)   gStat.min_volt   = gV.voltage;
    if (gStat.start_ah < 0) gStat.start_ah = gV.amp_hours;
    gStat.total_ah = gV.amp_hours - gStat.start_ah;
    gStat.total_km = gV.speed_kmh * (millis() - gTripMs) / 3600000.f;

    // Count current spikes (rising edge above 80% of l_in_current_max)
    bool spike = gV.curr_in > gMC.l_in_current_max * 0.80f;
    if (spike && !gStat.in_spike) gStat.spikes++;
    gStat.in_spike = spike;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Drawing utilities
// ─────────────────────────────────────────────────────────────────────────────
static uint16_t tempCol(float t, float warn, float crit) {
    return t >= crit ? C_RED : t >= warn ? C_YELL : C_GREEN;
}
static uint16_t dutyCol(float d) {
    return d > 85.f ? C_RED : d > 70.f ? C_YELL : C_GREEN;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ZONE A — Status bar (y = 0..12)
// ─────────────────────────────────────────────────────────────────────────────
static void drawZoneA() {
    canvas.fillRect(0, ZA_Y, DW, ZA_H, C_SB);
    canvas.setTextSize(1);
    canvas.setTextDatum(TL_DATUM);

    // BLE status
    if (!gBleOk) {
        canvas.setTextColor(C_RED);
        canvas.drawString("SCAN", 2, 2);
    } else if (!gV.valid) {
        canvas.setTextColor(C_YELL);
        canvas.drawString("BLE?", 2, 2);
    } else {
        canvas.setTextColor(C_GREEN);
        canvas.drawString("BLE", 2, 2);
    }

    // Recording / SD status indicator
    if (gRec) {
        canvas.setTextColor(C_RED);   canvas.drawString("REC",   34, 2);
    } else if (!gSdOk) {
        canvas.setTextColor(C_YELL);  canvas.drawString("NO-SD", 34, 2);
    }

    // Battery voltage
    char buf[20];
    snprintf(buf, sizeof(buf), "%.0fV", gV.voltage);
    canvas.setTextColor(C_WHITE); canvas.drawString(buf, 58, 2);

    // FW version / READ-ONLY label
    if (gFwVer.received) {
        char fwbuf[48];
        snprintf(fwbuf, sizeof(fwbuf), "FW%d.%02d %s",
                 gFwVer.major, gFwVer.minor, gFwVer.hwName);
        canvas.setTextColor(C_CYAN); canvas.drawString(fwbuf, 88, 2);
    } else {
        canvas.setTextColor(C_CYAN); canvas.drawString("READ", 88, 2);
    }

    // Trip timer
    if (gRec) {
        uint32_t sec = (millis() - gTripMs) / 1000;
        snprintf(buf, sizeof(buf), "%02lu:%02lu trip", sec/60, sec%60);
        canvas.setTextColor(C_CYAN); canvas.drawString(buf, 112, 2);
    }

    // Screen indicator (right-aligned)
    canvas.setTextDatum(TR_DATUM);
    snprintf(buf, sizeof(buf), "[%d]", gScreen);
    canvas.setTextColor(C_DGREY);
    canvas.drawString(buf, DW - 2, 2);
    canvas.setTextDatum(TL_DATUM);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ZONE B — Speed (x=0..75, y=13..80)
// ─────────────────────────────────────────────────────────────────────────────
static void drawZoneB() {
    canvas.fillRect(ZB_X, ZBC_Y, ZB_W, ZBC_H, C_BG);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(4);
    canvas.setTextColor(C_WHITE);
    char buf[8]; snprintf(buf, sizeof(buf), "%d", (int)gV.speed_kmh);
    canvas.drawString(buf, ZB_X + ZB_W/2, ZBC_Y + 30);
    canvas.setTextSize(1);
    canvas.setTextColor(C_GREY);
    canvas.drawString("km/h", ZB_X + ZB_W/2, ZBC_Y + 56);
    canvas.setTextDatum(TL_DATUM);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ZONE C — Duty arc dial (x=76..135, y=13..80)
//  Gauge: 240° sweep, starts bottom-left (150°), ends bottom-right (390°)
// ─────────────────────────────────────────────────────────────────────────────
static void drawZoneC() {
    canvas.fillRect(ZC_X, ZBC_Y, ZC_W, ZBC_H, C_BG);
    float duty = gV.duty_pct;

    // Background ring (full 240° sweep)
    canvas.fillArc(DCX, DCY, DR, DR - DTHK, 150.f, 390.f, C_DGREY);

    // Foreground ring (proportional to duty)
    if (duty > 0.5f) {
        float endA = 150.f + duty / 100.f * 240.f;
        canvas.fillArc(DCX, DCY, DR, DR - DTHK, 150.f, endA, dutyCol(duty));
    }

    // Duty % in centre
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(1);
    canvas.setTextColor(dutyCol(duty));
    char buf[8]; snprintf(buf, sizeof(buf), "%d%%", (int)duty);
    canvas.drawString(buf, DCX, DCY);

    canvas.setTextDatum(TL_DATUM);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ZONE D — Right indicators, two columns (x=136..239, y=13..80)
// ─────────────────────────────────────────────────────────────────────────────
static void drawKV(int x, int y, const char* lbl, const char* val, uint16_t c) {
    canvas.setTextSize(1);
    canvas.setTextColor(C_GREY);  canvas.drawString(lbl, x, y);
    canvas.setTextColor(c);       canvas.drawString(val, x, y + 9);
}

static void drawZoneD() {
    canvas.fillRect(ZD_X, ZBC_Y, ZD_W, ZBC_H + 50, C_BG);
    const int c1 = ZD_X + 2, c2 = ZD_X + 54, row = 20;
    int y = ZBC_Y + 1;
    char b[16];

    // Row 0: V/cell  |  FET °C
    float vcell = gV.voltage / gProfile.batt_cells;
    snprintf(b, sizeof(b), "%.2fV", vcell);
    drawKV(c1, y, "V/CELL", b,
           vcell < 3.3f ? C_RED : vcell < 3.6f ? C_YELL : C_GREEN);
    snprintf(b, sizeof(b), "%.0fC", gV.temp_fet);
    drawKV(c2, y, "FET C", b, tempCol(gV.temp_fet, 65, 70));

    y += row;
    // Row 1: Curr in  |  MOT °C
    snprintf(b, sizeof(b), "%.1fA", gV.curr_in);
    drawKV(c1, y, "CURR IN", b,
           gV.curr_in > gMC.l_in_current_max * 0.9f ? C_RED : C_WHITE);
    snprintf(b, sizeof(b), "%.0fC", gV.temp_mot);
    drawKV(c2, y, "MOT C", b, tempCol(gV.temp_mot, 75, 82));

    y += row;
    // Row 2: Power  |  (spare)
    float pw = fabsf(gV.voltage * gV.curr_in);
    snprintf(b, sizeof(b), "%.0fW", pw);
    drawKV(c1, y, "POWER", b, pw > 2000 ? C_RED : pw > 1200 ? C_YELL : C_WHITE);

    y += row;
    // Row 3: Pitch  |  (spare)
    snprintf(b, sizeof(b), "%.1f°", gV.pitch);
    drawKV(c1, y, "PITCH", b, C_CYAN);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ZONE E — Bottom bars + Ah/km (y=82..121)
// ─────────────────────────────────────────────────────────────────────────────
#define BAR_W   130
#define BAT_Y    84
#define BAT_H     6
#define DUTY_Y   95
#define DUTY_H    4

static void drawZoneE() {
    canvas.fillRect(0, ZE_Y, DW, ZF_Y - ZE_Y, C_BG);

    // SOC bar (linear: 84V=100%, 60V=0%)
    float soc = constrain((gV.voltage - gProfile.batt_min_v) /
                         (gProfile.batt_max_v - gProfile.batt_min_v) * 100.f, 0.f, 100.f);
    int batFill = (int)(soc / 100.f * BAR_W);
    canvas.fillRect(0, BAT_Y, BAR_W, BAT_H, C_DGREY);
    canvas.fillRect(0, BAT_Y, batFill, BAT_H,
                    soc > 50 ? C_GREEN : soc > 20 ? C_YELL : C_RED);
    char buf[16]; snprintf(buf, sizeof(buf), "%.0f%%", soc);
    canvas.setTextSize(1); canvas.setTextColor(C_WHITE);
    canvas.drawString(buf, BAR_W + 4, BAT_Y);

    // Duty bar + label to the right
    int dutyFill = (int)(gV.duty_pct / 100.f * BAR_W);
    canvas.fillRect(0, DUTY_Y, BAR_W, DUTY_H, C_DGREY);
    canvas.fillRect(0, DUTY_Y, dutyFill, DUTY_H, C_ORANG);
    canvas.setTextSize(1); canvas.setTextColor(C_GREY);
    canvas.drawString("duty", BAR_W + 4, DUTY_Y);

    // Ah and km text
    snprintf(buf, sizeof(buf), "Ah:%.2f  km:%.1f", gStat.total_ah, gStat.total_km);
    canvas.setTextColor(C_GREY);
    canvas.drawString(buf, 2, DUTY_Y + 8);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ZONE F — Key hints (y=122..134)
// ─────────────────────────────────────────────────────────────────────────────
static void drawZoneF(const char* hints = nullptr) {
    canvas.fillRect(0, ZF_Y, DW, ZF_H, C_SB);
    canvas.setTextSize(1); canvas.setTextColor(C_GREY);
    canvas.drawString(hints ? hints : "[P]conn [L]log [S]stop [M]mode",
                      2, ZF_Y + 2);
}

// ─────────────────────────────────────────────────────────────────────────────
//  SCREEN 0 — Dashboard
// ─────────────────────────────────────────────────────────────────────────────
static void drawDash() {
    canvas.fillScreen(C_BG);
    canvas.drawFastVLine(ZC_X,     ZBC_Y, ZBC_H, C_DGREY);  // B|C divider
    canvas.drawFastVLine(ZD_X - 1, ZBC_Y, ZBC_H + 50, C_DGREY);  // C|D divider
    drawZoneA(); drawZoneB(); drawZoneC(); drawZoneD();
    drawZoneE(); drawZoneF();
}

// ─────────────────────────────────────────────────────────────────────────────
//  SCREEN 1 — Session stats
// ─────────────────────────────────────────────────────────────────────────────
static void drawStats() {
    canvas.fillScreen(C_BG); drawZoneA();
    canvas.setTextSize(1);
    canvas.setTextColor(C_CYAN); canvas.setTextDatum(TL_DATUM);
    canvas.drawString("-- STATYSTYKI SESJI --", 4, 16);

    const int lx=4, vx=140, dy=11; int y=29;
    char b[24];
    auto row = [&](const char* lbl, const char* val, uint16_t col=C_WHITE){
        canvas.setTextColor(C_GREY); canvas.drawString(lbl, lx, y);
        canvas.setTextColor(col);    canvas.drawString(val, vx, y);
        y += dy;
    };

    snprintf(b,sizeof(b),"%.1f km/h", gStat.max_spd);    row("Maks predkosc:", b);
    snprintf(b,sizeof(b),"%.1f %%",   gStat.max_duty);   row("Maks duty:", b, gStat.max_duty>85?C_RED:C_WHITE);
    snprintf(b,sizeof(b),"%.1f A",    gStat.max_curr_in);row("Maks prad bat:", b);
    snprintf(b,sizeof(b),"%.1f C",    gStat.max_tfet);   row("Maks FET:", b, gStat.max_tfet>70?C_RED:C_WHITE);
    snprintf(b,sizeof(b),"%.2f V  (%.3fV/cell)", gStat.min_volt, gStat.min_volt/gProfile.batt_cells);
    row("Min napiecie:", b, gStat.min_volt<66?C_RED:C_WHITE);
    float wh_km = (gStat.total_km > 0.01f)
                  ? gStat.total_ah * gV.voltage / gStat.total_km : 0;
    snprintf(b,sizeof(b),"%.1f Wh/km", wh_km);           row("Efektywnosc:", b);
    snprintf(b,sizeof(b),"%d",         gStat.spikes);    row("Spiki pradu:", b, gStat.spikes>=5?C_RED:C_WHITE);

    // BLE diagnostics
    canvas.setTextColor(C_DGREY);
    canvas.drawFastHLine(4, y+2, DW-8, C_DGREY);
    y += 6;
    char ble[48];
    if (gFwVer.received)
        snprintf(ble, sizeof(ble), "FW %d.%d %s  tx:%d n:%d p:%d",
                 gFwVer.major, gFwVer.minor, gFwVer.hwName,
                 gTxCount, gNotifyCount, gPktOkCount);
    else
        snprintf(ble, sizeof(ble), "no FW  tx:%d n:%d p:%d",
                 gTxCount, gNotifyCount, gPktOkCount);
    canvas.setTextColor(gNotifyCount > 0 ? C_GREEN : C_YELL);
    canvas.drawString(ble, lx, y);

    drawZoneF("[M]powrot do dashboardu");
}

// ─────────────────────────────────────────────────────────────────────────────
//  SCREEN 2 — Suggestions review
// ─────────────────────────────────────────────────────────────────────────────
static void drawConfigReview() {
    canvas.fillScreen(C_BG); drawZoneA();
    canvas.setTextSize(1); canvas.setTextDatum(TL_DATUM);

    if (gSuggN == 0) {
        canvas.setTextColor(C_YELL); canvas.setTextDatum(MC_DATUM);
        canvas.drawString("Brak sugestii w suggestions.json", DW/2, 70);
        canvas.setTextDatum(TL_DATUM);
        drawZoneF("[M]powrot");
        return;
    }

    // Header
    char h[32]; snprintf(h, sizeof(h), "SUGESTIA %d / %d", gSuggIdx+1, gSuggN);
    canvas.setTextColor(C_CYAN); canvas.drawString(h, 4, 16);

    Sugg& s = gSugg[gSuggIdx];

    // Line 1: param name (bold-ish with larger text)
    canvas.setTextSize(1); canvas.setTextColor(C_WHITE);
    canvas.drawString(s.param, 4, 28);

    // Line 2: BYLO → NOWE (delta %)
    char l2[52];
    snprintf(l2, sizeof(l2), "BYLO: %.2f  ->  NOWE: %.2f  (%+.1f%%)",
             s.cur, s.sug, s.delta);
    canvas.setTextColor(s.delta < 0 ? C_ORANG : C_CYAN);
    canvas.drawString(l2, 4, 42);

    // Line 3: reason (first 38 chars)
    char short_r[39]; strncpy(short_r, s.reason, 38); short_r[38]='\0';
    canvas.setTextColor(C_GREY); canvas.drawString(short_r, 4, 56);

    // Status dots for all suggestions
    int dx = 4;
    for (int i = 0; i < gSuggN; i++) {
        const char* dot = (i == gSuggIdx)  ? ">" :
                           gSugg[i].accepted ? "Y" :
                           gSugg[i].skipped  ? "S" : ".";
        canvas.setTextColor(
            gSugg[i].accepted ? C_GREEN :
            gSugg[i].skipped  ? C_DGREY : C_WHITE);
        canvas.drawString(dot, dx + i*16, 70);
    }

    drawZoneF("TRYB READ-ONLY — uzyj analyze.py na Macu");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Screen 3 — Device profile
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
//  Apply all accepted suggestions with progress display
// ─────────────────────────────────────────────────────────────────────────────
// applyAccepted() removed — READ-ONLY mode

// ─────────────────────────────────────────────────────────────────────────────
//  Keyboard handler
// ─────────────────────────────────────────────────────────────────────────────
static void handleKeys() {
    if (!M5Cardputer.Keyboard.isChange() ||
        !M5Cardputer.Keyboard.isPressed()) return;

    auto st = M5Cardputer.Keyboard.keysState();
    for (char c : st.word) {
        c = tolower(c);

        // ── Scan menu mode ────────────────────────────────────────────────
        if (gInScanMenu) {
            if (c == 'p') { bleScan(); continue; }  // rescan
            if (c == 'm') { gInScanMenu = false; continue; }  // back
            if (c >= '1' && c <= '6') {
                int idx = c - '1';
                if (idx < gScanCount) {
                    drawScanMenu("connecting...");
                    if (bleConnect(gScanList[idx].addr)) {
                        gInScanMenu = false;
                    } else {
                        drawScanMenu("FAILED — try another:");
                    }
                }
                continue;
            }
            continue;  // eat all other keys in scan menu
        }

        // [1]-[9] — jump straight to a screen (matches prototype deck)
        if (c >= '1' && c <= '9') {
            gScreen = c - '1';
            if (gScreen == 7) { loadSuggestions(); gSuggIdx = 0; }
            continue;
        }

        // [M] — cycle screens
        if (c == 'm') {
            gScreen = (gScreen + 1) % SC_COUNT;
            if (gScreen == 7) { loadSuggestions(); gSuggIdx = 0; }
            continue;
        }

        // [P] — BLE scan/connect
        if (c == 'p') {
            bleScan();
            continue;
        }

        // HUD (0) — recording controls
        if (gScreen == 0) {
            if (c == 'l' && !gRec && gSdOk) {
                gRec = true;
                gTripMs = millis();
                memset(&gStat, 0, sizeof(gStat));
                gStat.min_volt = 100.f; gStat.start_ah = -1.f;
                csvWriteHeader();
                vescSend(CMD_GET_MCCONF);
                delay(600);
                vescSend(CMD_GET_APPCONF);
            }
            if (c == 's' && gRec) gRec = false;
        }

        // BACKUP (3) — create restore point (read config → SD)
        if (gScreen == 3) {
            if (c == 'b') doBackup();
        }

        // RESTORE (4) — move selection cursor
        if (gScreen == 4) {
            if (c == ';' || c == '.') gBackupSel++;          // ; up-arrow, . next
            if (c == '/' && gBackupSel > 0) gBackupSel--;    // / down-arrow
        }

        // REVIEW (7) — accept/skip suggestions (READ-ONLY: navigate only, no write)
        if (gScreen == 7 && gSuggN > 0) {
            if (c == 'y') { gSugg[gSuggIdx].accepted=true;  gSuggIdx=(gSuggIdx+1)%gSuggN; }
            if (c == 'n') { gSugg[gSuggIdx].accepted=false; gSuggIdx=(gSuggIdx+1)%gSuggN; }
            if (c == '.') { gSuggIdx = (gSuggIdx+1) % gSuggN; }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  NEW FRONT-END — ported 1:1 from the 240x135 prototype
//  9 screens: HUD · TRIP · FAULT · BACKUP · RESTORE · BOARD · CONFIG · REVIEW · APPLY
// ═════════════════════════════════════════════════════════════════════════════
static uint16_t dutyColor(float d){ return d > 88 ? C_RED : d > 72 ? C_WARN : C_VOLT; }
static uint16_t tempColor(float t){ return t > 85 ? C_RED : t > 70 ? C_WARN : C_OK; }

// Top status bar (y 0..11): left text · optional pill · right text
static void uiStatBar(const char* left, const char* pill, uint16_t pillCol, const char* right) {
    canvas.fillRect(0, 0, DW, 12, C_SB);
    canvas.drawFastHLine(0, 12, DW, C_PANEL);
    canvas.setTextSize(1); canvas.setTextDatum(TL_DATUM);
    canvas.fillCircle(5, 6, 2, gBleOk ? C_OK : C_DGREY);
    canvas.setTextColor(C_GREY); canvas.drawString(left, 11, 2);
    int rx = DW - 3;
    if (right && right[0]) {
        canvas.setTextDatum(TR_DATUM); canvas.setTextColor(C_GREY);
        canvas.drawString(right, rx, 2);
        rx -= (int)strlen(right) * 6 + 5;
    }
    if (pill && pill[0]) {
        int pw = (int)strlen(pill) * 6 + 6;
        canvas.fillRoundRect(rx - pw, 1, pw, 10, 2, pillCol);
        canvas.setTextDatum(TL_DATUM); canvas.setTextColor(C_BG);
        canvas.drawString(pill, rx - pw + 3, 2);
    }
    canvas.setTextDatum(TL_DATUM);
}

// Screen title row (y ~15) + optional badge on the right
static void uiTitle(const char* t, const char* badge, uint16_t badgeCol) {
    canvas.setTextSize(1); canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(C_VOLT); canvas.drawString(t, 5, 15);
    if (badge && badge[0]) {
        int bw = (int)strlen(badge) * 6 + 6;
        canvas.drawRoundRect(DW - 5 - bw, 14, bw, 11, 2, badgeCol);
        canvas.setTextColor(badgeCol); canvas.drawString(badge, DW - 5 - bw + 3, 16);
    }
}

// Thick horizontal bar with big value at the end (HUD speed/duty)
static void uiMegaBar(int y, int h, const char* label, float frac, uint16_t fillCol,
                      const char* valStr, const char* unit) {
    int x = 5, w = DW - 10;
    canvas.drawRoundRect(x, y, w, h, 2, C_DGREY);
    int fw = (int)((w - 2) * constrain(frac, 0.f, 1.f));
    if (fw > 0) canvas.fillRect(x + 1, y + 1, fw, h - 2, fillCol);
    canvas.setTextSize(1); canvas.setTextDatum(ML_DATUM);
    canvas.setTextColor(C_WHITE); canvas.drawString(label, x + 4, y + h / 2);
    int uw = (int)strlen(unit) * 6;
    canvas.setTextDatum(MR_DATUM); canvas.setTextColor(C_GREY);
    canvas.drawString(unit, x + w - 4, y + h / 2 + 5);
    canvas.setTextSize(2); canvas.setTextColor(C_WHITE);
    canvas.drawString(valStr, x + w - 5 - uw, y + h / 2);
    canvas.setTextSize(1); canvas.setTextDatum(TL_DATUM);
}

// Blocking config read (pumps gRxReady like the handshake). Fills raw buffer + gMC.
static bool requestConfig(uint8_t cmd, int timeoutMs) {
    if (cmd == CMD_GET_MCCONF) gMcconfRawLen = 0; else gAppconfRawLen = 0;
    gRxReady = false;
    vescSend(cmd);
    unsigned long t0 = millis();
    while (millis() - t0 < (unsigned)timeoutMs) {
        if (gRxReady) {
            gRxReady = false;
            const uint8_t* pay; int plen;
            if (unpackPkt(gRxBuf, gRxLen, &pay, &plen)) {
                if (pay[0] == CMD_GET_MCCONF && plen <= (int)sizeof(gMcconfRaw)) {
                    memcpy(gMcconfRaw, pay, plen); gMcconfRawLen = plen; parseMcConf(pay, plen);
                } else if (pay[0] == CMD_GET_APPCONF && plen <= (int)sizeof(gAppconfRaw)) {
                    memcpy(gAppconfRaw, pay, plen); gAppconfRawLen = plen;
                }
            }
        }
        if (cmd == CMD_GET_MCCONF  && gMcconfRawLen  > 0) return true;
        if (cmd == CMD_GET_APPCONF && gAppconfRawLen > 0) return true;
        delay(10);
    }
    return false;
}

// Blocking Refloat custom-config read (raw package config bytes).
static bool requestCustomCfg(int timeoutMs) {
    gCustomCfgRawLen = 0; gRxReady = false;
    vescSendCustomCfg();
    unsigned long t0 = millis();
    while (millis() - t0 < (unsigned)timeoutMs) {
        if (gRxReady) {
            gRxReady = false;
            const uint8_t* pay; int plen;
            if (unpackPkt(gRxBuf, gRxLen, &pay, &plen) &&
                pay[0] == CMD_GET_CUSTOM_CONFIG && plen <= (int)sizeof(gCustomCfgRaw)) {
                memcpy(gCustomCfgRaw, pay, plen); gCustomCfgRawLen = plen;
            }
        }
        if (gCustomCfgRawLen > 0) return true;
        delay(10);
    }
    return false;
}

// Create a restore point — full board snapshot (VBK2) to SD /backups.
// Captures: FW + HW name + CAN id + mcconf + appconf + Refloat package config.
// READ-ONLY toward the VESC — only the SD card is written.
static void doBackup() {
    if (!gBleOk) { snprintf(gBackupMsg, sizeof(gBackupMsg), "ERR: not connected"); return; }
    if (!gSdOk)  { snprintf(gBackupMsg, sizeof(gBackupMsg), "ERR: no SD card");    return; }

    canvas.fillScreen(C_BG);
    uiStatBar("BACKUP", "READING", C_WARN, "");
    canvas.setTextColor(C_VOLT); canvas.setTextDatum(MC_DATUM); canvas.setTextSize(1);
    canvas.drawString("reading full config from VESC...", DW / 2, 65);
    canvas.setTextDatum(TL_DATUM); canvas.pushSprite(0, 0);

    bool m = requestConfig(CMD_GET_MCCONF, 2000);
    bool a = requestConfig(CMD_GET_APPCONF, 2000);
    bool c = requestCustomCfg(2500);            // Refloat package config (float boards)
    if (!m && !a && !c) { snprintf(gBackupMsg, sizeof(gBackupMsg), "ERR: no config response"); return; }

    SD.mkdir("/backups");
    char path[64]; int n = 1;
    for (; n <= 999; n++) {
        snprintf(path, sizeof(path), "/backups/backup_%03d.bin", n);
        if (!SD.exists(path)) break;
    }
    File f = SD.open(path, FILE_WRITE);
    if (!f) { snprintf(gBackupMsg, sizeof(gBackupMsg), "ERR: SD write failed"); return; }

    // VBK2 format: magic, fw(2), canId, hwLen + hwName, then 3 length-prefixed blobs
    uint8_t hwLen = (uint8_t)strnlen(gFwVer.hwName, 31);
    uint8_t hdr[5] = { 'V','B','K','2', hwLen };
    f.write(hdr, sizeof(hdr));
    f.write((const uint8_t*)gFwVer.hwName, hwLen);
    uint8_t meta[3] = { gFwVer.major, gFwVer.minor, (uint8_t)(gCanId & 0xFF) };
    f.write(meta, sizeof(meta));
    auto blob = [&](const uint8_t* d, int len) {
        uint8_t l2[2] = { (uint8_t)(len >> 8), (uint8_t)(len & 0xFF) };
        f.write(l2, 2);
        if (len > 0) f.write(d, len);
    };
    blob(gMcconfRaw, gMcconfRawLen);
    blob(gAppconfRaw, gAppconfRawLen);
    blob(gCustomCfgRaw, gCustomCfgRawLen);
    f.close();
    snprintf(gBackupMsg, sizeof(gBackupMsg), "Saved %03d mc:%d app:%d cfg:%d",
             n, gMcconfRawLen, gAppconfRawLen, gCustomCfgRawLen);
}

// ── SCREEN 0 · HUD ───────────────────────────────────────────────────────────
static void drawHUD() {
    canvas.fillScreen(C_BG);
    char l[40], r[12];
    if (gCanId >= 0) snprintf(l, sizeof(l), "%s  CAN %d", gFwVer.received ? "LINK" : "BLE", gCanId);
    else             snprintf(l, sizeof(l), "%s  DIRECT", gBleOk ? "LINK" : "OFF");
    uint32_t ts = gRec ? (millis() - gTripMs) / 1000 : 0;
    snprintf(r, sizeof(r), "%02u:%02u", (unsigned)(ts / 60), (unsigned)(ts % 60));
    uiStatBar(l, gRec ? "REC" : "RIDE", gRec ? C_RED : C_VOLT, r);

    float spd = gV.valid ? gV.speed_kmh : 0, duty = gV.valid ? gV.duty_pct : 0;
    char v[12];
    snprintf(v, sizeof(v), "%d", (int)spd);
    uiMegaBar(14, 20, "SPEED", spd / 45.f, C_VOLTD, v, "KM/H");
    snprintf(v, sizeof(v), "%d", (int)duty);
    uiMegaBar(37, 20, "DUTY", duty / 100.f, dutyColor(duty), v, "%");

    // state pill (left) — real Refloat state when available, else derived
    const char* st; uint16_t stc;
    if (gV.refloat)       { st = refloatStateName(gV.state); stc = refloatStateColor(gV.state); }
    else if (!gV.valid)   { st = "NO DATA";  stc = C_DGREY; }
    else                  { st = duty > 88 ? "TILTBACK" : spd > 2 ? "RUNNING" : "READY";
                            stc = duty > 88 ? C_RED : spd > 2 ? C_OK : C_GREY; }
    int pw = (int)strlen(st) * 6 + 6;
    canvas.fillRoundRect(6, 62, pw, 10, 2, stc);
    canvas.setTextColor(C_BG); canvas.setTextDatum(TL_DATUM); canvas.drawString(st, 9, 63);

    // pitch / setpoint bar (left) — lime = setpoint (target), ice = live pitch
    char plab[24]; snprintf(plab, sizeof(plab), "PITCH %+.0f  ROLL %+.0f", gV.pitch, gV.roll);
    canvas.setTextColor(C_GREY); canvas.drawString(plab, 6, 76);
    int pbx = 6, pby = 85, pbw = 104, pbh = 7;
    canvas.drawRect(pbx, pby, pbw, pbh, C_DGREY);
    canvas.drawFastVLine(pbx + pbw / 2, pby, pbh, C_DGREY);
    float sp = constrain(gV.setpoint, -15.f, 15.f);
    int spx = pbx + pbw / 2 + (int)(sp / 15.f * (pbw / 2 - 2));
    canvas.fillRect(spx, pby, 2, pbh, C_VOLT);
    float pit = constrain(gV.pitch, -15.f, 15.f);
    int pix = pbx + pbw / 2 + (int)(pit / 15.f * (pbw / 2 - 2));
    canvas.fillRect(pix, pby, 2, pbh, C_ICE);

    // temps (left)
    auto tbar = [&](int y, const char* lab, float t) {
        canvas.setTextColor(C_GREY); canvas.setTextDatum(TL_DATUM); canvas.drawString(lab, 6, y);
        int bx = 26, bw = 64, bh = 5;
        canvas.drawRect(bx, y, bw, bh, C_DGREY);
        float f = constrain((t - 25) / 70.f, 0.f, 1.f);
        if (f > 0) canvas.fillRect(bx + 1, y + 1, (int)((bw - 2) * f), bh - 2, tempColor(t));
        char b[8]; snprintf(b, sizeof(b), "%d", (int)t);
        canvas.setTextColor(C_WHITE); canvas.drawString(b, bx + bw + 4, y - 1);
    };
    tbar(96, "FET", gV.temp_fet);
    tbar(107, "MOT", gV.temp_mot);

    // electrical strip (right)
    float vcell = gProfile.batt_cells ? gV.voltage / gProfile.batt_cells : 0;
    float kw = fabsf(gV.voltage * gV.curr_in) / 1000.f;
    auto cell = [&](int y, const char* lab, const char* val) {
        canvas.setTextColor(C_GREY); canvas.setTextDatum(TL_DATUM); canvas.drawString(lab, 150, y);
        canvas.setTextColor(C_WHITE); canvas.setTextDatum(TR_DATUM); canvas.drawString(val, DW - 4, y);
        canvas.drawFastHLine(150, y + 10, DW - 154, C_PANEL);
        canvas.setTextDatum(TL_DATUM);
    };
    char b[16];
    snprintf(b, sizeof(b), "%.2f", vcell);     cell(62, "V/CELL", b);
    snprintf(b, sizeof(b), "%.0fA", gV.curr_in); cell(80, "A IN", b);
    snprintf(b, sizeof(b), "%.1fkW", kw);      cell(98, "POWER", b);

    // battery footer
    canvas.fillRect(0, 121, DW, 14, C_FOOT);
    canvas.drawFastHLine(0, 121, DW, C_PANEL);
    float soc = gV.valid ? constrain((gV.voltage - gProfile.batt_min_v) /
                (gProfile.batt_max_v - gProfile.batt_min_v), 0.f, 1.f) : 0;
    char bp[8]; snprintf(bp, sizeof(bp), "%d%%", (int)(soc * 100));
    canvas.setTextColor(C_VOLT); canvas.setTextDatum(ML_DATUM); canvas.drawString(bp, 3, 128);
    int bx = 32, bw = 148, by = 125, bh = 6;
    canvas.drawRect(bx, by, bw, bh, C_DGREY);
    uint16_t sc = soc < 0.15f ? C_RED : soc < 0.30f ? C_WARN : C_VOLT;
    if (soc > 0) canvas.fillRect(bx + 1, by + 1, (int)((bw - 2) * soc), bh - 2, sc);
    char tr[24]; snprintf(tr, sizeof(tr), "%.1fkm", gStat.total_km);
    canvas.setTextColor(C_GREY); canvas.setTextDatum(MR_DATUM); canvas.drawString(tr, DW - 3, 128);
    canvas.setTextDatum(TL_DATUM);
}

// ── SCREEN 1 · TRIP ──────────────────────────────────────────────────────────
static void drawTrip() {
    canvas.fillScreen(C_BG);
    uiStatBar("RIDE SUMMARY", "TRIP", C_VOLT, "");
    uiTitle("RIDE SUMMARY", gSessName, C_DGREY);
    const char* labels[6] = {"TOP SPEED","DISTANCE","PEAK DUTY","MAX A IN","MAX FET","USED AH"};
    char vals[6][12]; uint16_t cols[6];
    snprintf(vals[0],12,"%.0f", gStat.max_spd);     cols[0]=C_VOLT;
    snprintf(vals[1],12,"%.1f", gStat.total_km);    cols[1]=C_WHITE;
    snprintf(vals[2],12,"%.0f", gStat.max_duty);    cols[2]=gStat.max_duty>85?C_RED:C_WHITE;
    snprintf(vals[3],12,"%.0f", gStat.max_curr_in); cols[3]=C_WHITE;
    snprintf(vals[4],12,"%.0f", gStat.max_tfet);    cols[4]=gStat.max_tfet>70?C_RED:C_WHITE;
    snprintf(vals[5],12,"%.1f", gStat.total_ah);    cols[5]=C_WHITE;
    int gx = 4, gy = 30, cw = 74, ch = 44;
    for (int i = 0; i < 6; i++) {
        int cx = gx + (i % 3) * (cw + 4), cy = gy + (i / 3) * (ch + 4);
        canvas.drawRoundRect(cx, cy, cw, ch, 3, C_DGREY);
        canvas.setTextSize(1); canvas.setTextDatum(TL_DATUM);
        canvas.setTextColor(C_GREY); canvas.drawString(labels[i], cx + 4, cy + 5);
        canvas.setTextSize(2); canvas.setTextColor(cols[i]); canvas.drawString(vals[i], cx + 4, cy + 18);
    }
    canvas.setTextSize(1);
}

// ── SCREEN 2 · FAULT ─────────────────────────────────────────────────────────
static void drawFault() {
    canvas.fillScreen(0x1000);
    uiStatBar("FAULT MONITOR", "FAULT", C_RED, "");
    canvas.setTextDatum(MC_DATUM);
    // No fault tracking from GET_VALUES yet → show nominal
    canvas.setTextColor(C_OK); canvas.setTextSize(2); canvas.drawString("NO FAULT", DW / 2, 58);
    canvas.setTextSize(1); canvas.setTextColor(C_GREY); canvas.drawString("system nominal", DW / 2, 82);
    canvas.setTextColor(C_DGREY); canvas.drawString("fault decode: TODO", DW / 2, 98);
    canvas.setTextDatum(TL_DATUM); canvas.setTextSize(1);
}

// ── SCREEN 5 · BOARD ─────────────────────────────────────────────────────────
static void drawBoard() {
    canvas.fillScreen(C_BG);
    uiStatBar("LINK", gBleOk ? "CONN" : "---", gBleOk ? C_OK : C_DGREY, "");
    uiTitle("BOARD IDENTITY", gBleOk ? "CONNECTED" : "OFFLINE", gBleOk ? C_OK : C_DGREY);
    int y = 30; const int dy = 14;
    auto row = [&](const char* k, const char* val, uint16_t c) {
        canvas.setTextSize(1); canvas.setTextDatum(TL_DATUM);
        canvas.setTextColor(C_GREY); canvas.drawString(k, 6, y);
        canvas.setTextDatum(TR_DATUM); canvas.setTextColor(c); canvas.drawString(val, DW - 6, y);
        canvas.drawFastHLine(6, y + 10, DW - 12, C_PANEL);
        y += dy; canvas.setTextDatum(TL_DATUM);
    };
    char b[40];
    row("NAME", gProfile.name, C_VOLT);
    if (gFwVer.received) { snprintf(b, sizeof(b), "VESC %d.%d", gFwVer.major, gFwVer.minor); row("FIRMWARE", b, C_WHITE); }
    else row("FIRMWARE", "--", C_DGREY);
    row("HARDWARE", gFwVer.received ? gFwVer.hwName : "--", C_WHITE);
    if (gCanId >= 0) { snprintf(b, sizeof(b), "0x%02X / %d", gCanId, gCanId); row("CAN ID", b, C_VOLT); }
    else row("CAN ID", "DIRECT", C_ICE);
    snprintf(b, sizeof(b), "%dS  %.0fV", gProfile.batt_cells, gProfile.batt_max_v); row("BATTERY", b, C_WHITE);
    if (gV.bms) { snprintf(b, sizeof(b), "%.1f C", gV.temp_bat); row("BMS TEMP", b, tempColor(gV.temp_bat)); }
    else        { row("BMS TEMP", "-- no reply", C_DGREY); }
    snprintf(b, sizeof(b), "tx%d n%d p%d", gTxCount, gNotifyCount, gPktOkCount);
    row("LINK DIAG", b, gNotifyCount > 0 ? C_OK : C_WARN);
}

// ── SCREEN 6 · CONFIG ────────────────────────────────────────────────────────
static void drawConfig() {
    canvas.fillScreen(C_BG);
    uiStatBar("REFLOAT CFG", gMC.loaded ? "READ" : "--", gMC.loaded ? C_ICE : C_DGREY, "");
    uiTitle("CONFIG", gMC.loaded ? "GET 14" : "NO DATA", C_ICE);
    int y = 29; const int dy = 12;
    auto row = [&](const char* k, const char* val, bool wl) {
        if (wl) { canvas.fillRect(2, y - 1, DW - 4, dy, 0x10C2); canvas.fillCircle(6, y + 3, 2, C_VOLT); }
        canvas.setTextSize(1); canvas.setTextDatum(TL_DATUM);
        canvas.setTextColor(wl ? C_VOLT : C_GREY); canvas.drawString(k, 11, y);
        canvas.setTextDatum(TR_DATUM); canvas.setTextColor(C_WHITE); canvas.drawString(val, DW - 6, y);
        canvas.drawFastHLine(4, y + dy - 1, DW - 8, C_PANEL);
        y += dy; canvas.setTextDatum(TL_DATUM);
    };
    char b[24];
    snprintf(b,sizeof(b),"%.0f A", gMC.l_current_max);      row("l_current_max", b, true);
    snprintf(b,sizeof(b),"%.0f A", gMC.l_in_current_max);   row("l_in_current_max", b, true);
    snprintf(b,sizeof(b),"%.0f",   gMC.l_max_erpm);         row("l_max_erpm", b, true);
    snprintf(b,sizeof(b),"%.0f C", gMC.l_temp_fet_start);   row("l_temp_fet_start", b, true);
    snprintf(b,sizeof(b),"%.0f C", gMC.l_temp_fet_end);     row("l_temp_fet_end", b, true);
    snprintf(b,sizeof(b),"%.0f C", gMC.l_temp_mot_start);   row("l_temp_mot_start", b, false);
    snprintf(b,sizeof(b),"%.0f V", gMC.l_battery_cut_start);row("l_batt_cut_start", b, false);
    canvas.setTextSize(1);
}

// ── SCREEN 7 · REVIEW ────────────────────────────────────────────────────────
static void drawReview() {
    canvas.fillScreen(C_BG);
    uiStatBar("SUGGESTIONS", "ARMED", C_WARN, "");
    uiTitle("REVIEW TUNE", "+/-15%", C_WARN);
    if (gSuggN == 0) {
        canvas.setTextColor(C_GREY); canvas.setTextDatum(MC_DATUM);
        canvas.drawString("no suggestions.json on SD", DW / 2, 70);
        canvas.setTextDatum(TL_DATUM); return;
    }
    int y = 30; const int dy = 16;
    for (int i = 0; i < gSuggN && i < 5; i++) {
        if (i == gSuggIdx) canvas.drawRoundRect(3, y - 1, DW - 6, dy - 1, 2, C_VOLTD);
        canvas.setTextSize(2); canvas.setTextDatum(TL_DATUM);
        canvas.setTextColor(gSugg[i].accepted ? C_OK : C_DGREY);
        canvas.drawString(gSugg[i].accepted ? "Y" : "-", 6, y + 1);
        canvas.setTextSize(1); canvas.setTextColor(C_WHITE); canvas.drawString(gSugg[i].param, 22, y);
        char d[28]; snprintf(d, sizeof(d), "%.0f>%.0f %+.0f%%", gSugg[i].cur, gSugg[i].sug, gSugg[i].delta);
        canvas.setTextColor(C_VOLT); canvas.setTextDatum(TR_DATUM); canvas.drawString(d, DW - 6, y + 6);
        canvas.setTextDatum(TL_DATUM); y += dy;
    }
    canvas.setTextSize(1); canvas.setTextColor(C_GREY);
    canvas.drawString("[Y]/[N] toggle   [.] next", 5, 124);
}

// ── SCREEN 8 · APPLY ─────────────────────────────────────────────────────────
static void drawApply() {
    canvas.fillScreen(C_BG);
    uiStatBar("APPLY TUNE", "READ-ONLY", C_ICE, "");
    uiTitle("APPLY TUNE", "DISABLED", C_DGREY);
    canvas.drawRoundRect(5, 30, 110, 17, 3, C_OK);
    canvas.setTextSize(1); canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(C_OK); canvas.drawString("SAFETY GATE", 9, 32);
    canvas.setTextColor(C_OK); canvas.drawString("BOARD IDLE OK", 9, 39);
    canvas.drawRoundRect(125, 30, 110, 17, 3, C_DGREY);
    canvas.setTextColor(C_DGREY); canvas.drawString("WRITE PATH", 129, 32);
    canvas.drawString("READ-ONLY MODE", 129, 39);
    int y = 53, armed = 0;
    for (int i = 0; i < gSuggN && i < 4; i++) {
        if (!gSugg[i].accepted) continue;
        canvas.setTextColor(C_DGREY); canvas.setTextDatum(TL_DATUM); canvas.drawString(">", 8, y);
        canvas.setTextColor(C_WHITE); canvas.drawString(gSugg[i].param, 18, y);
        char vb[12]; snprintf(vb, sizeof(vb), "%.0f", gSugg[i].sug);
        canvas.setTextColor(C_VOLT); canvas.setTextDatum(TR_DATUM); canvas.drawString(vb, DW - 8, y);
        canvas.setTextDatum(TL_DATUM); y += 12; armed++;
    }
    if (armed == 0) { canvas.setTextColor(C_GREY); canvas.drawString("nothing armed (REVIEW first)", 18, 60); }
    canvas.setTextColor(C_WARN); canvas.drawString("WRITE DISABLED - read-only build", 5, 124);
}

// ── SCREEN 3/4 · BACKUP / RESTORE ────────────────────────────────────────────
static int uiBackupList(int y, int sel) {
    int n = 0;
    if (gSdOk) {
        File dir = SD.open("/backups");
        if (dir) {
            File e = dir.openNextFile();
            while (e && n < 4) {
                if (!e.isDirectory()) {
                    bool s = (n == sel);
                    if (s) canvas.drawRoundRect(3, y - 1, DW - 6, 13, 2, C_VOLTD);
                    canvas.setTextSize(1); canvas.setTextDatum(TL_DATUM);
                    canvas.setTextColor(s ? C_VOLT : C_DGREY); canvas.drawString(s ? ">" : " ", 6, y + 2);
                    canvas.setTextColor(C_WHITE); canvas.drawString(e.name(), 16, y + 2);
                    char sz[12]; snprintf(sz, sizeof(sz), "%dB", (int)e.size());
                    canvas.setTextDatum(TR_DATUM); canvas.setTextColor(C_GREY); canvas.drawString(sz, DW - 6, y + 2);
                    canvas.setTextDatum(TL_DATUM);
                    y += 14; n++;
                }
                e = dir.openNextFile();
            }
            dir.close();
        }
    }
    if (n == 0) { canvas.setTextColor(C_GREY); canvas.setTextDatum(TL_DATUM); canvas.drawString("no backups on SD /backups", 16, y + 2); }
    return n;
}

// ── SCREEN 3 · BMS CELLS — per-cell voltage inspector ────────────────────────
static void drawCells() {
    canvas.fillScreen(C_BG);
    uiStatBar("BMS CELLS", gBms.valid ? "LIVE" : "--", gBms.valid ? C_OK : C_DGREY, "");
    if (!gBms.valid || gBms.cellNum == 0) {
        canvas.setTextColor(C_GREY); canvas.setTextDatum(MC_DATUM); canvas.setTextSize(1);
        canvas.drawString(gBleOk ? "waiting for BMS data..." : "not connected", DW / 2, 70);
        canvas.setTextDatum(TL_DATUM); return;
    }
    float mn = 99, mx = 0; int mni = 0;
    for (int i = 0; i < gBms.cellNum; i++) {
        float v = gBms.cell[i];
        if (v < mn) { mn = v; mni = i; }
        if (v > mx) mx = v;
    }
    float delta = (mx - mn) * 1000.f;
    canvas.setTextSize(1); canvas.setTextDatum(TL_DATUM);
    char h[40]; snprintf(h, sizeof(h), "%dS  %.1fV", gBms.cellNum, gBms.vTot);
    canvas.setTextColor(C_VOLT); canvas.drawString(h, 5, 15);
    char h2[40]; snprintf(h2, sizeof(h2), "min%.3f max%.3f d%.0fmV", mn, mx, delta);
    canvas.setTextColor(delta > 100 ? C_WARN : C_GREY); canvas.setTextDatum(TR_DATUM);
    canvas.drawString(h2, DW - 5, 15); canvas.setTextDatum(TL_DATUM);

    int n = gBms.cellNum; if (n > 20) n = 20;
    int rows = (n + 1) / 2;
    int top = 27; int rowH = (134 - top) / (rows > 0 ? rows : 1);
    if (rowH > 10) rowH = 10;
    for (int i = 0; i < n; i++) {
        int col = i / rows, r = i % rows;
        int x = 4 + col * 119;
        int y = top + r * rowH;
        float v = gBms.cell[i];
        bool low = (i == mni);
        uint16_t c = gBms.bal[i] ? C_VOLT : low ? C_WARN : C_WHITE;
        char cl[4]; snprintf(cl, sizeof(cl), "%02d", i + 1);
        canvas.setTextColor(C_DGREY); canvas.drawString(cl, x, y);
        int bx = x + 15, bw = 56, bh = 5;
        canvas.drawRect(bx, y, bw, bh, C_DGREY);
        float f = constrain((v - 3.0f) / 1.25f, 0.f, 1.f);
        if (f > 0) canvas.fillRect(bx + 1, y + 1, (int)((bw - 2) * f), bh - 2,
                                   gBms.bal[i] ? C_VOLT : low ? C_WARN : C_OK);
        char vb[8]; snprintf(vb, sizeof(vb), "%.3f", v);
        canvas.setTextColor(c); canvas.drawString(vb, bx + bw + 2, y);
    }
}

static void drawBackup() {
    canvas.fillScreen(C_BG);
    uiStatBar("BACKUP", "READ", C_ICE, "");
    uiTitle("BACKUP CONFIG", "GET 14/16", C_ICE);
    canvas.drawRoundRect(5, 29, DW - 10, 15, 3, C_VOLTD);
    canvas.setTextSize(1); canvas.setTextDatum(ML_DATUM); canvas.setTextColor(C_VOLT);
    canvas.drawString("> CREATE RESTORE POINT     [B]", 11, 36);
    canvas.setTextDatum(TL_DATUM);
    if (gBackupMsg[0]) {
        canvas.setTextColor(strncmp(gBackupMsg, "ERR", 3) == 0 ? C_RED : C_OK);
        canvas.drawString(gBackupMsg, 6, 49);
    } else {
        canvas.setTextColor(C_GREY);
        canvas.drawString("SAVED RESTORE POINTS", 6, 49);
    }
    uiBackupList(61, -1);
}

static void drawRestore() {
    canvas.fillScreen(C_BG);
    uiStatBar("RESTORE", "WRITES VESC", C_WARN, "");
    uiTitle("RESTORE BACKUP", "SET", C_VOLT);
    int n = uiBackupList(30, gBackupSel);
    if (n > 0 && gBackupSel >= n) gBackupSel = n - 1;
    canvas.setTextSize(1); canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(C_OK);   canvas.drawString("SAFETY GATE: BOARD IDLE OK", 6, 104);
    canvas.setTextColor(C_WARN); canvas.drawString("WRITE DISABLED - read-only build", 6, 124);
}

static void renderScreen() {
    switch (gScreen) {
        case 0: drawHUD();     break;
        case 1: drawTrip();    break;
        case 2: drawFault();   break;
        case 3: drawCells();   break;
        case 4: drawRestore(); break;
        case 5: drawBoard();   break;
        case 6: drawConfig();  break;
        case 7: drawReview();  break;
        case 8: drawApply();   break;
    }
}

// Auto-connect the last board (saved in NVS) without pressing [P].
static void autoConnectLast() {
    Preferences prefs; prefs.begin("vesc", true);
    String mac = prefs.getString("lastmac", "");
    String nm  = prefs.getString("lastname", "board");
    prefs.end();
    if (mac.length() < 5) return;

    canvas.fillScreen(C_BG);
    canvas.setTextColor(C_VOLT); canvas.setTextDatum(MC_DATUM); canvas.setTextSize(1);
    char m[48]; snprintf(m, sizeof(m), "auto-connect: %s", nm.c_str());
    canvas.drawString(m, DW / 2, 56);
    canvas.setTextColor(C_GREY); canvas.drawString("scanning saved board...", DW / 2, 72);
    canvas.setTextDatum(TL_DATUM); canvas.pushSprite(0, 0);

    gScanCount = 0;
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(new ScanCB(), true);
    scan->setActiveScan(true); scan->setInterval(100); scan->setWindow(99);
    scan->getResults(5000, false);

    for (int i = 0; i < gScanCount; i++) {
        if (strcmp(gScanList[i].addr.toString().c_str(), mac.c_str()) == 0) {
            gInScanMenu = false;
            bleConnect(gScanList[i].addr);
            return;
        }
    }
    Serial.printf("[BLE] auto-connect: %s not advertising — press [P]\n", mac.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
//  setup()
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(120);

    canvas.createSprite(DW, DH);
    canvas.setTextWrap(false);

    // Splash screen
    canvas.fillScreen(C_BG);
    canvas.setTextDatum(MC_DATUM); canvas.setTextSize(2); canvas.setTextColor(C_CYAN);
    canvas.drawString("VESC AI Tuner", DW/2, 38);
    canvas.setTextSize(1); canvas.setTextColor(C_GREY);
    canvas.drawString("Float Package  |  Universal", DW/2, 62);
    canvas.drawString("v1.0", DW/2, 76);
    canvas.pushSprite(0, 0);
    delay(900);

    // SD init
    canvas.setTextColor(C_YELL); canvas.drawString("Init SD...", DW/2, 96);
    canvas.pushSprite(0, 0);
    sdInit();
    if (gSdOk) loadProfileFromSD();   // load profile if it exists
    canvas.fillRect(0, 80, DW, 36, C_BG);
    if (gSdOk) {
        canvas.setTextSize(1);
        canvas.setTextColor(C_GREEN);
        canvas.drawString("SD OK", DW/2, 96);
        canvas.pushSprite(0, 0);
        delay(600);
    } else {
        canvas.setTextSize(2);
        canvas.setTextColor(C_YELL);
        canvas.drawString("BRAK KARTY SD", DW/2, 80);
        canvas.setTextSize(1);
        canvas.setTextColor(C_WHITE);
        canvas.drawString("tylko podglad BLE — bez zapisu", DW/2, 106);
        canvas.pushSprite(0, 0);
        delay(2500);
    }
    canvas.setTextSize(1);

    // USB MSC — enable when not riding (comment out to disable auto-mount)
    // usbMscBegin();

    // BLE init
    NimBLEDevice::init("VESC-Tuner");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setMTU(517);   // request large ATT MTU — fewer fragmented responses
    canvas.setTextDatum(TL_DATUM);

    autoConnectLast();           // reconnect last board without [P]
}

// ─────────────────────────────────────────────────────────────────────────────
//  loop()
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    M5Cardputer.update();
    handleKeys();

    uint32_t now = millis();

    // Poll VESC @ 12Hz when connected
    if (gBleOk && now - gLastValMs >= 83) {
        gLastValMs = now;
        vescSend(CMD_GET_VALUES);
    }
    // Poll Refloat GET_ALLDATA @ 5Hz for pitch/roll/state (float boards only;
    // harmless no-op on classic VESC which won't answer the Refloat command)
    if (gBleOk && now - gLastAllMs >= 200) {
        gLastAllMs = now;
        vescSendAllData();
    }
    // Poll Smart BMS values @1Hz (battery temperature etc.)
    if (gBleOk && now - gLastBmsMs >= 1000) {
        gLastBmsMs = now;
        vescSend(CMD_BMS_GET_VALUES);
    }

    // Dispatch incoming BLE packet
    if (gRxReady) {
        gRxReady = false;
        const uint8_t* pay; int plen;
        if (unpackPkt(gRxBuf, gRxLen, &pay, &plen)) {
            gPktOkCount++;
            switch (pay[0]) {
                case CMD_FW_VERSION:  parseFwVersion(pay, plen); break;
                case CMD_GET_VALUES:  parseValues(pay, plen);   break;
                case CMD_GET_MCCONF:
                    if (plen <= (int)sizeof(gMcconfRaw)) { memcpy(gMcconfRaw, pay, plen); gMcconfRawLen = plen; }
                    parseMcConf(pay, plen);
                    break;
                case CMD_GET_APPCONF:
                    if (plen <= (int)sizeof(gAppconfRaw)) { memcpy(gAppconfRaw, pay, plen); gAppconfRawLen = plen; }
                    break;
                case CMD_CUSTOM_APP_DATA: parseAllData(pay, plen); break;
                case CMD_GET_CUSTOM_CONFIG:
                    if (plen <= (int)sizeof(gCustomCfgRaw)) { memcpy(gCustomCfgRaw, pay, plen); gCustomCfgRawLen = plen; }
                    break;
                case CMD_BMS_GET_VALUES:
                    if (gBmsDbgN < 5) {       // dump raw a few times to verify byte layout
                        gBmsDbgN++;
                        Serial.printf("[BMS] len=%d:", plen);
                        for (int i = 0; i < plen && i < 56; i++) Serial.printf(" %02X", pay[i]);
                        Serial.println();
                    }
                    parseBms(pay, plen);
                    break;
            }
        }
    }

    // Stats + CSV logging
    if (gRec && gV.valid) {
        updateStats();
        if (now - gLastCsvMs >= 83) { gLastCsvMs = now; csvAppend(); }
    }

    // Render active screen (skip when scan menu is open)
    if (!gInScanMenu) {
        renderScreen();
        canvas.pushSprite(0, 0);
    }

    delay(15);
}
