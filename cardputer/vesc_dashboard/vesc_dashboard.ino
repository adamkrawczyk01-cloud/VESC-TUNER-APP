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
//  READ-ONLY BLACK BOX: only GET_* commands are ever sent to the VESC (read
//  telemetry + config) and logged to SD. There is NO SET/write command anywhere
//  — this firmware can never change a VESC parameter. Tune by hand in VESC Tool
//  after reviewing the data + suggestions in the web app.
//  TODO: USB MSC raw block callbacks (sd.card()->readSectors / writeSectors).
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
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <ESPmDNS.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

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
#define CMD_GET_APPCONF 16
#define CMD_FORWARD_CAN 34       // 0x22 — wrap [34,canId,cmd] to reach motor over CAN
#define CMD_PING_CAN    62       // 0x3E — ESP32 discovers CAN device IDs
#define CMD_CUSTOM_APP_DATA 36   // 0x24 — Refloat package commands
#define REFLOAT_MAGIC       101
#define REFLOAT_GET_ALLDATA 10
#define CMD_GET_CUSTOM_CONFIG 93 // 0x5D — read Refloat package config (raw bytes)
#define CMD_BMS_GET_VALUES    96 // 0x60 — Smart BMS values (incl. temperatures)
#define CMD_GET_VALUES_SETUP  47 // 0x2F — battery %, odometer, totals summed over CAN
#define CMD_GET_IMU_DATA      65 // 0x41 — raw IMU (accel/gyro/yaw)

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
    float adc1      = 0, adc2      = 0;   // footpad sensors (~0..1)
    float atr_set   = 0, torque_tilt = 0, turn_tilt = 0;  // tilt contributions (°)
    float brake_tilt = 0, remote_tilt = 0; // SP-BrkTlt, SP-Remote setpoint contributions (°)
    float req_amps  = 0;                  // balance/requested current from Refloat (A)
    float booster   = 0, foc_id    = 0;   // booster current (A), id current (A)
    float id        = 0, iq        = 0;   // FOC d/q axis currents (A)
    float watt_hours = 0, wh_charged = 0, ah_charged = 0;  // energy counters
    int32_t tacho_abs = 0;                // absolute tachometer
    uint8_t fault   = 0;                  // mc_fault_code (0 = none)
    float batt_pct  = 0, odo_km = 0, batt_wh = 0;          // GET_VALUES_SETUP (47)
    bool  setup     = false;
    float yaw       = 0;                                   // raw IMU (65)
    float acc_x = 0, acc_y = 0, acc_z = 0;
    float gyro_x = 0, gyro_y = 0, gyro_z = 0;
    bool  imu       = false;
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
    float tiltback_duty = 80.0f;   // duty-Geiger limit — set to your Refloat tiltback_duty
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
static bool     gAutoArmed = false;     // auto-start logging on next motion
static bool     gSdOk    = false;
static int      gScreen  = 0;           // 0 HUD 1 TRIP 2 FAULT 3 BACKUP 4 RESTORE 5 BOARD 6 CONFIG 7 REVIEW 8 APPLY 10 WIFI
#define SC_COUNT 11
static int      gBackupSel = 0;         // selected restore point on RESTORE screen
static uint8_t  gMcconfRaw[1024];  static int gMcconfRawLen   = 0;  // raw config snapshots
static uint8_t  gAppconfRaw[1024]; static int gAppconfRawLen  = 0;  // (1024 — full mcconf/appconf, no truncation)
static uint8_t  gCustomCfgRaw[768];static int gCustomCfgRawLen = 0; // Refloat package config (raw)
static char     gBackupMsg[48] = "";    // last backup status (shown on BACKUP screen)
static void doBackup();                 // fwd decl (defined in front-end block)
static void drawWifi();                  // fwd decl (WiFi screen, defined before setup)
static void renderScreen();              // fwd decl
static bool wifiConnect();               // fwd decl
static void wifiDisconnect();            // fwd decl
static uint8_t  gSessNum = 0;
static char     gSessName[32] = {};
static uint32_t gLastValMs = 0;
static uint32_t gLastAllMs = 0;
// full-charge chime: rings when the pack reaches ~4.2 V/cell (100%); any key off
static bool     gChargeAlarm = false;
static bool     gChargeDismissed = false;
static uint32_t gChargeBeepMs = 0;
static bool     gChargeBeep2 = false;
static int      gChargePingStep = -1;    // last 0.1V step pinged on the run-up to full
static uint8_t  gVolume = 160;           // speaker volume (Q quieter / W louder), saved to NVS
static uint32_t gLastBmsMs = 0;
static uint32_t gLastSetupMs = 0;
static uint32_t gLastImuMs = 0;
static uint32_t gLastValRx = 0;          // millis() of last FRESH GET_VALUES parse
static uint32_t gLastAllRx = 0;          // millis() of last FRESH GET_ALLDATA (footpads) parse
static int      gBmsDbgN   = 0;          // serial-dump first few BMS replies
static int      gBmsReplies = 0;         // total BMS responses seen (diagnostic)
static int      gSetupDbgN = 0, gImuDbgN = 0;
static uint32_t gLastCsvMs = 0;
static uint32_t gTripMs    = 0;

// ── GPS (M5Stack GPS Unit v1.1, NMEA over the Grove UART) ─────────────────────
//  Plug the unit into the Cardputer Grove port. Grove pins = G1 (GPIO1) /
//  G2 (GPIO2). No fix after ~1 min outdoors? swap GPS_RX/TX, or try 38400 baud.
#define GPS_RX_PIN 1          // Cardputer Grove G1  ← GPS unit TX  (verified)
#define GPS_TX_PIN 2          // Cardputer Grove G2  → GPS unit RX (we don't send)
#define GPS_BAUD   115200     // M5 GPS Unit v1.1 (AT6558) streams NMEA @115200, not 9600
static HardwareSerial gpsSerial(1);
struct GpsData {
    double   lat = 0, lon = 0;
    float    alt = 0, spd_kmh = 0;
    int      sats = 0;
    bool     fix = false;
    uint32_t lastFixMs = 0;
};
static GpsData gGps;
static char gNmea[100];
static int  gNmeaLen = 0;

// NMEA ddmm.mmmm + hemisphere → signed decimal degrees
static double nmeaToDeg(const char* v, const char* hemi) {
    if (!v || !v[0]) return 0;
    double raw = atof(v);
    int deg = (int)(raw / 100);
    double d = deg + (raw - deg * 100) / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W') d = -d;
    return d;
}
static void parseNmea(char* s) {
    char* f[20]; int n = 0; f[n++] = s;
    for (char* p = s; *p && n < 20; p++) if (*p == ',') { *p = 0; f[n++] = p + 1; }
    if (n < 1) return;
    if (strstr(f[0], "GGA") && n >= 10) {                 // fix quality, sats, altitude
        gGps.sats = atoi(f[7]);
        if (atoi(f[6]) > 0 && f[2][0]) {
            gGps.lat = nmeaToDeg(f[2], f[3]);
            gGps.lon = nmeaToDeg(f[4], f[5]);
            gGps.alt = atof(f[9]);
            gGps.fix = true; gGps.lastFixMs = millis();
        }
    } else if (strstr(f[0], "RMC") && n >= 8) {            // validity, position, ground speed
        if (f[2][0] == 'A' && f[3][0]) {
            gGps.lat = nmeaToDeg(f[3], f[4]);
            gGps.lon = nmeaToDeg(f[5], f[6]);
            gGps.spd_kmh = atof(f[7]) * 1.852f;            // knots → km/h
            gGps.fix = true; gGps.lastFixMs = millis();
        }
    }
}
static void gpsPoll() {
    while (gpsSerial.available()) {
        char c = gpsSerial.read();
        if (c == '\n') { gNmea[gNmeaLen] = 0; if (gNmeaLen > 5 && gNmea[0] == '$') parseNmea(gNmea + 1); gNmeaLen = 0; }
        else if (c != '\r' && gNmeaLen < (int)sizeof(gNmea) - 1) gNmea[gNmeaLen++] = c;
    }
    if (gGps.fix && millis() - gGps.lastFixMs > 3000) gGps.fix = false;   // stale → drop fix
}

static M5Canvas canvas(&M5Cardputer.Display);

// ── NimBLE handles ───────────────────────────────────────────────────────────
static NimBLEClient*               gBleClient = nullptr;
static NimBLERemoteCharacteristic* gCharRx    = nullptr;  // write to VESC
static NimBLERemoteCharacteristic* gCharTx    = nullptr;  // notify from VESC

static uint8_t  gRxBuf[1024];       // one complete reassembled frame (1024 — full mcconf/appconf)
static uint16_t gRxLen   = 0;
static volatile bool gRxReady  = false;
static uint8_t  gAcc[2048];         // raw BLE chunk accumulator (reassembly across MTU chunks)
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
static float rdF32be(const uint8_t* b, int o) {   // IEEE-754 float, big-endian
    uint32_t u = ((uint32_t)b[o]<<24)|((uint32_t)b[o+1]<<16)|((uint32_t)b[o+2]<<8)|(uint32_t)b[o+3];
    float f; memcpy(&f, &u, 4); return f;
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
    gLastValRx = millis();               // fresh values arrived
    gV.temp_fet   = rdI16(p,  1) / 10.f;
    gV.temp_mot   = rdI16(p,  3) / 10.f;
    gV.curr_mot   = rdI32(p,  5) / 100.f;
    gV.curr_in    = rdI32(p,  9) / 100.f;
    gV.duty_pct   = rdI16(p, 21) / 10.f;   // stored as ‰, result = %
    gV.rpm        = (float)rdI32(p, 23);
    gV.voltage    = rdI16(p, 27) / 10.f;
    gV.amp_hours  = rdI32(p, 29) / 10000.f;
    gV.id         = rdI32(p, 13) / 100.f;
    gV.iq         = rdI32(p, 17) / 100.f;
    if (len >= 37) gV.ah_charged = rdI32(p, 33) / 10000.f;
    if (len >= 41) gV.watt_hours = rdI32(p, 37) / 10000.f;
    if (len >= 45) gV.wh_charged = rdI32(p, 41) / 10000.f;
    gV.tacho      = rdI32(p, 45);
    if (len >= 53) gV.tacho_abs = rdI32(p, 49);
    if (len >= 54) gV.fault     = p[53];
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
    gV.roll        = rdI16(p, 8)  / 10.f;
    gV.state       = p[10] & 0x0F;
    gV.adc1        = p[12] / 50.f;
    gV.adc2        = p[13] / 50.f;
    gV.req_amps    = rdI16(p, 4) / 10.f;   // balance_current = requested amps
    gV.setpoint    = ((int)p[14] - 128) / 5.f;
    gV.atr_set     = ((int)p[15] - 128) / 5.f;
    gV.brake_tilt  = ((int)p[16] - 128) / 5.f;
    gV.torque_tilt = ((int)p[17] - 128) / 5.f;
    gV.turn_tilt   = ((int)p[18] - 128) / 5.f;
    gV.remote_tilt = ((int)p[19] - 128) / 5.f;
    gV.pitch       = rdI16(p, 20) / 10.f;
    gV.booster     = (int)p[22] - 128;
    if (len > 34) { int f = p[34]; gV.foc_id = (f == 222) ? 0 : f / 3.f; }
    gV.refloat     = true;
    gLastAllRx     = millis();           // fresh footpad/Refloat data arrived
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
    // reject a corrupt/misaligned BMS frame at the source: an out-of-range pack
    // voltage means the whole packet is garbage (we've seen vTot=1695V, cells
    // -29V, temp 258°C). Drop it → keep last-good values, don't poison the SOC,
    // the charge alarm or the logged CSV.
    float vtot = f32(1e6f);
    if (vtot < 20.f || vtot > 130.f) { gBms.valid = false; return; }
    gBms.vTot = vtot;
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
        float maxT = -1000.f;
        for (int i = 0; i < tn; i++) { float t = f16(1e2f); gBms.temp[i] = t; if (t > maxT) maxT = t; }
        // Trailing fields: temp_ic, temp_hum, humidity, temp_max_cell (all f16/100).
        // Many packs report 0 ADC sensors and carry the temperature in temp_ic /
        // temp_max_cell instead. Read them and keep the hottest plausible value.
        float ic = f16(1e2f);                          // temp_ic
        if (ic > 0.f && ic < 120.f && ic > maxT) maxT = ic;
        f16(1e2f);                                     // temp_hum (skip)
        f16(1e2f);                                     // humidity (skip — not a temp)
        float mc = f16(1e2f);                          // temp_max_cell
        if (mc > 0.f && mc < 120.f && mc > maxT) maxT = mc;
        if (maxT > -100.f) gV.temp_bat = maxT;
    }
    gBms.valid = (cn > 0);
    if (cn > 0) gV.bms = true;
}

// Send a single-byte command WITHOUT CAN-forward (straight to the ESP32 bridge).
static void vescSendRawCmd(uint8_t cmd) {
    if (!gCharRx || !gBleOk) return;
    uint8_t pkt[12], pay[1] = { cmd };
    int len = buildPkt(pkt, pay, 1);
    gTxCount++;
    gCharRx->writeValue(pkt, len, !gUseWriteNoResp);
}

// Request raw IMU (mask 0x01FF = roll,pitch,yaw,acc xyz,gyro xyz).
static void vescSendImu() {
    if (!gCharRx || !gBleOk) return;
    uint8_t pay[6]; int plen = 0;
    if (gCanId >= 0) { pay[plen++] = CMD_FORWARD_CAN; pay[plen++] = (uint8_t)gCanId; }
    pay[plen++] = CMD_GET_IMU_DATA;
    pay[plen++] = 0x01; pay[plen++] = 0xFF;          // field mask
    uint8_t pkt[16]; int len = buildPkt(pkt, pay, plen);
    gTxCount++;
    gCharRx->writeValue(pkt, len, !gUseWriteNoResp);
}

// COMM_GET_VALUES_SETUP (47) — battery %, odometer, totals summed over CAN.
static void parseSetup(const uint8_t* p, int len) {
    if (len < 23) return;
    gV.batt_pct = rdI16(p, 21) / 1000.f * 100.f;     // battery_level 0..1 → %
    if (len >= 57) {
        gV.batt_wh = rdI32(p, 49) / 1000.f;          // battery Wh left
        uint32_t odo = ((uint32_t)p[53]<<24)|((uint32_t)p[54]<<16)|((uint32_t)p[55]<<8)|p[56];
        gV.odo_km = odo / 1000.f;                    // odometer metres → km
    }
    gV.setup = true;
}

// COMM_GET_IMU_DATA (65) — [cmd][mask:2][float32 × set-bits]. Mask 0x01FF order:
// roll, pitch, yaw, acc x/y/z, gyro x/y/z.
static void parseImu(const uint8_t* p, int len) {
    int ind = 3;                                      // skip cmd + mask(2)
    if (ind + 9 * 4 > len) return;
    ind += 8;                                         // roll + pitch (have from Refloat)
    gV.yaw    = rdF32be(p, ind); ind += 4;
    gV.acc_x  = rdF32be(p, ind); ind += 4;
    gV.acc_y  = rdF32be(p, ind); ind += 4;
    gV.acc_z  = rdF32be(p, ind); ind += 4;
    gV.gyro_x = rdF32be(p, ind); ind += 4;
    gV.gyro_y = rdF32be(p, ind); ind += 4;
    gV.gyro_z = rdF32be(p, ind); ind += 4;
    gV.imu = true;
}

#define PROFILE_PATH "/device_profile.json"
static void buildProfileFromMcconf();  // forward decl
static void saveProfileToSD();         // forward decl

// COMM_GET_MCCONF payload: [cmd 0x0e][signature:4][4 enum bytes][float32_auto…].
// VESC float32_auto == IEEE-754 float32 BE, so read with rdF32be (NOT int32/100 —
// that was the old bug, and offset 3 even landed inside the signature → garbage
// like l_battery_cut_end=1587). The limits section starts at offset 9 (1 cmd +
// 4 sig + 4 enums). Offsets below verified against the GAD board's mcconf blob:
//   9=l_current_max(150) 13=l_current_min(-150) 17=l_in_current_max(70)
//   21=l_in_current_min(-45) 29=l_abs_current_max(225).
#define MC_OFF_CURR_MAX    9
#define MC_OFF_CURR_MIN   13
#define MC_OFF_CURR_IN    17
#define MC_OFF_CURR_IN_MIN 21
#define MC_OFF_ABS_CURR   29

static void parseMcConf(const uint8_t* p, int len) {
    if (len < 33 || p[0] != CMD_GET_MCCONF) return;
    gMC.l_current_max       = rdF32be(p, MC_OFF_CURR_MAX);
    gMC.l_in_current_max    = rdF32be(p, MC_OFF_CURR_IN);
    // l_max_erpm / l_temp_fet_* / l_battery_cut_* sit deeper in the blob and their
    // exact offsets depend on the confgenerator layout for this fw signature
    // (0x2efd0142) — not yet confirmed, so they keep their safe struct defaults
    // rather than logging garbage. buildProfileFromMcconf derives cells from
    // measured voltage, so it no longer needs l_battery_cut_end.
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
    // cut_end is the per-pack low cutoff; reject implausible values (bad mcconf
    // parse) so we never persist garbage like 1587V into the profile.
    float cutEnd = gMC.l_battery_cut_end;
    if (cutEnd < cells * 2.5f || cutEnd > cells * 3.6f) cutEnd = cells * 3.0f;
    gProfile.batt_min_v = cutEnd;
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
    f.printf("  \"tiltback_duty\": %.1f,\n",       gProfile.tiltback_duty);
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
        fv = jFloat(line, "tiltback_duty");
        if (fv > 0) gProfile.tiltback_duty = fv;
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
        gRec = false;            // board off → stop & finalize the log
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
    gAutoArmed = true;           // arm auto-logging for this fresh connection
    requestConfig(CMD_GET_MCCONF, 1500);   // motor config snapshot for this connection
    requestConfig(CMD_GET_APPCONF, 1500);  // app config too — capture everything
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

// Pick the next free session_NNN name (a new file per ride / power-cycle)
static void nextSession() {
    for (int n = 1; n <= 999; n++) {
        char p[64]; snprintf(p, sizeof(p), "/sessions/session_%03d.csv", n);
        if (!SD.exists(p)) { gSessNum = n; break; }
    }
    snprintf(gSessName, sizeof(gSessName), "session_%03d", gSessNum);
}

// Full telemetry header — log EVERYTHING (SD is roomy). Fixed 20 cell columns.
static void csvWriteHeader() {
    char path[64]; snprintf(path, sizeof(path), "/sessions/%s.csv", gSessName);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    f.print("ts_ms,speed_kmh,rpm,voltage_V,vcell_V,curr_in_A,curr_mot_A,power_W,duty_pct,");
    f.print("temp_fet_C,temp_mot_C,temp_bat_C,pitch_deg,roll_deg,setpoint_deg,atr_deg,torquetilt_deg,turntilt_deg,braketilt_deg,remotetilt_deg,");
    f.print("adc1,adc2,booster_A,foc_id_A,req_amps_A,state,amp_hours,tacho,");
    f.print("id_A,iq_A,watt_hours,wh_charged,ah_charged,tacho_abs,fault,");
    f.print("batt_pct,odo_km,batt_wh,yaw,acc_x,acc_y,acc_z,gyro_x,gyro_y,gyro_z,");
    f.print("bms_v_tot,bms_i_in,cell_min,cell_max,cell_delta_mV");
    for (int i = 1; i <= 20; i++) f.printf(",cell_%02d", i);
    for (int i = 1; i <= 6;  i++) f.printf(",bms_temp_%02d", i);
    f.print(",gps_lat,gps_lon,altitude_m,gps_spd_kmh,gps_sats");
    f.print(",val_age_ms,fp_age_ms");   // ms since last FRESH values / footpad data (staleness)
    f.println();
    f.close();
}

static void csvAppend() {
    if (!gSdOk || !gRec || !gV.valid) return;
    char path[64]; snprintf(path, sizeof(path), "/sessions/%s.csv", gSessName);
    File f = SD.open(path, FILE_APPEND);
    if (!f) return;
    float vcell = gProfile.batt_cells ? gV.voltage / gProfile.batt_cells : 0;
    float power = gV.voltage * gV.curr_in;
    float cmn = 0, cmx = 0, cd = 0;
    if (gBms.cellNum > 0) {
        cmn = 99; cmx = 0;
        for (int i = 0; i < gBms.cellNum; i++) { float v = gBms.cell[i]; if (v < cmn) cmn = v; if (v > cmx) cmx = v; }
        cd = (cmx - cmn) * 1000.f;
    }
    f.printf("%lu,%.2f,%.0f,%.2f,%.3f,%.2f,%.2f,%.0f,%.1f,",
             (unsigned long)millis(), gV.speed_kmh, gV.rpm, gV.voltage, vcell,
             gV.curr_in, gV.curr_mot, power, gV.duty_pct);
    f.printf("%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,",
             gV.temp_fet, gV.temp_mot, gV.bms ? gV.temp_bat : 0.f,
             gV.pitch, gV.roll, gV.setpoint, gV.atr_set, gV.torque_tilt, gV.turn_tilt,
             gV.brake_tilt, gV.remote_tilt);
    f.printf("%.3f,%.3f,%.1f,%.1f,%.2f,%d,%.4f,%ld,",
             gV.adc1, gV.adc2, gV.booster, gV.foc_id, gV.req_amps, gV.state, gV.amp_hours, (long)gV.tacho);
    f.printf("%.2f,%.2f,%.4f,%.4f,%.4f,%ld,%d,",
             gV.id, gV.iq, gV.watt_hours, gV.wh_charged, gV.ah_charged, (long)gV.tacho_abs, gV.fault);
    f.printf("%.1f,%.2f,%.0f,%.1f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,",
             gV.batt_pct, gV.odo_km, gV.batt_wh, gV.yaw,
             gV.acc_x, gV.acc_y, gV.acc_z, gV.gyro_x, gV.gyro_y, gV.gyro_z);
    f.printf("%.2f,%.2f,%.3f,%.3f,%.0f",
             gBms.vTot, gBms.iIn, cmn, cmx, cd);
    for (int i = 0; i < 20; i++) {
        if (i < gBms.cellNum) f.printf(",%.3f", gBms.cell[i]); else f.print(",");
    }
    for (int i = 0; i < 6; i++) {
        if (i < gBms.tempNum) f.printf(",%.1f", gBms.temp[i]); else f.print(",");
    }
    f.printf(",%.6f,%.6f,%.1f,%.2f,%d", gGps.lat, gGps.lon, gGps.alt, gGps.spd_kmh, gGps.sats);
    uint32_t va = gLastValRx ? (millis() - gLastValRx) : 9999;
    uint32_t fa = gLastAllRx ? (millis() - gLastAllRx) : 9999;
    if (va > 9999) va = 9999;  if (fa > 9999) fa = 9999;
    f.printf(",%lu,%lu", (unsigned long)va, (unsigned long)fa);   // staleness markers
    f.println();
    f.close();
}

// Recording session control (manual [R] + auto-start on motion)
static void startSession() {
    if (gRec || !gSdOk) return;
    nextSession();
    memset(&gStat, 0, sizeof(gStat));
    gStat.min_volt = 100.f; gStat.start_ah = -1.f;
    gTripMs = millis();
    csvWriteHeader();
    // raw config snapshots for this session (captured at connect) — the full
    // untruncated blobs hold every mcconf/appconf field for later decoding.
    if (gMcconfRawLen > 0) {
        char mp[64]; snprintf(mp, sizeof(mp), "/sessions/%s_mcconf.bin", gSessName);
        File mf = SD.open(mp, FILE_WRITE);
        if (mf) { mf.write(gMcconfRaw, gMcconfRawLen); mf.close(); }
    }
    if (gAppconfRawLen > 0) {
        char ap[64]; snprintf(ap, sizeof(ap), "/sessions/%s_appconf.bin", gSessName);
        File af = SD.open(ap, FILE_WRITE);
        if (af) { af.write(gAppconfRaw, gAppconfRawLen); af.close(); }
    }
    gRec = true;
    Serial.printf("[LOG] start %s\n", gSessName);
}
static void stopSession() {
    if (!gRec) return;
    gRec = false;
    Serial.printf("[LOG] stop %s\n", gSessName);
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
// Pack voltage for SOC / charge logic: prefer the BMS cell-sum (true cell state)
// over the controller v_in, which the charger inflates while charging.
static float packVoltage() {
    return (gBms.valid && gBms.vTot > 20.f && gBms.vTot < 130.f) ? gBms.vTot : gV.voltage;
}

// Seed the system clock from the build date/time (the Cardputer has no RTC that
// survives power-off). Makes SD files get a sane date (~flash date) instead of
// 1980/2033. Re-applied every boot.
static void setClockFromBuild() {
    struct tm t = {0};
    char mon[8] = {0}; int day = 1, year = 2025, hh = 0, mm = 0, ss = 0;
    sscanf(__DATE__, "%7s %d %d", mon, &day, &year);   // e.g. "Jun 22 2026"
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    static const char* M = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* p = strstr(M, mon);
    t.tm_mon  = p ? (int)((p - M) / 3) : 0;
    t.tm_mday = day; t.tm_year = year - 1900;
    t.tm_hour = hh; t.tm_min = mm; t.tm_sec = ss;
    time_t epoch = mktime(&t);
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
}

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

    // SOC bar (linear: 84V=100%, 60V=0%) — uses BMS cell voltage when available
    float soc = constrain((packVoltage() - gProfile.batt_min_v) /
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

    // any key silences the full-charge chime (and is consumed for that)
    if (gChargeAlarm && !gChargeDismissed) {
        gChargeDismissed = true; gChargeAlarm = false;
        M5Cardputer.Speaker.stop();
        return;
    }

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

        // [1]-[9],[0] — jump straight to a screen (1->RIDE ... 9->REVIEW, 0->APPLY)
        if (c >= '1' && c <= '9') {
            gScreen = c - '1';
            if (gScreen == 8) { loadSuggestions(); gSuggIdx = 0; }
            continue;
        }
        if (c == '0') { gScreen = 9; continue; }

        // [M] — cycle screens
        if (c == 'm') {
            gScreen = (gScreen + 1) % SC_COUNT;
            if (gScreen == 8) { loadSuggestions(); gSuggIdx = 0; }
            continue;
        }

        // [P] — BLE scan/connect
        if (c == 'p') {
            bleScan();
            continue;
        }

        // [Q] quieter / [W] louder — speaker volume (saved to NVS, audible feedback)
        if (c == 'q' || c == 'w') {
            int v = (int)gVolume + (c == 'w' ? 20 : -20);
            gVolume = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            M5Cardputer.Speaker.setVolume(gVolume);
            M5Cardputer.Speaker.tone(3000, 80);        // feedback beep at the new level
            Preferences p; p.begin("vesc", false); p.putUChar("vol", gVolume); p.end();
            continue;
        }
        // [I] — jump to WiFi screen and connect to the saved home network (was [W])
        if (c == 'i') {
            gScreen = 10;
            renderScreen(); canvas.pushSprite(0, 0);  // show "connecting" before the blocking connect
            wifiConnect();
            continue;
        }
        // [X] on WIFI screen — turn the radio + server off
        if (gScreen == 10 && c == 'x') {
            wifiDisconnect();
            continue;
        }

        // RIDE (0) — [R] toggles logging. Manual stop disarms auto-start
        // until the board reconnects (power-cycle = fresh log).
        if (gScreen == 0 && c == 'r') {
            if (gRec) { stopSession(); gAutoArmed = false; }
            else      { startSession(); }
        }

        // BACKUP/RESTORE (5) — [B] create backup, cursor to pick a restore point
        if (gScreen == 5) {
            if (c == 'b') doBackup();
            if (c == ';' || c == '.') gBackupSel++;          // ; up-arrow, . next
            if (c == '/' && gBackupSel > 0) gBackupSel--;    // / down-arrow
        }

        // REVIEW (8) — accept/skip suggestions (READ-ONLY: navigate only, no write)
        if (gScreen == 8 && gSuggN > 0) {
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

// ── SCREEN 0 · RIDE — glance: speed · duty · pitch · 3 temps · footpads ──────
static void drawRide() {
    canvas.fillScreen(C_BG);

    // status bar with footpad F1/F2 dots
    canvas.fillRect(0, 0, DW, 12, C_SB);
    canvas.drawFastHLine(0, 12, DW, C_PANEL);
    canvas.setTextSize(1); canvas.setTextDatum(TL_DATUM);
    canvas.fillCircle(5, 6, 2, gBleOk ? C_OK : C_DGREY);
    char lb[20];
    if (gCanId >= 0) snprintf(lb, sizeof(lb), "ADV CAN%d", gCanId);
    else             snprintf(lb, sizeof(lb), gBleOk ? "DIRECT" : "OFFLINE");
    canvas.setTextColor(C_GREY); canvas.drawString(lb, 11, 2);
    int fx = 11 + (int)strlen(lb) * 6 + 8;
    canvas.setTextColor(C_DGREY); canvas.drawString("F1", fx, 2);
    canvas.fillCircle(fx + 15, 6, 2, gV.adc1 > 0.25f ? C_OK : C_DGREY);
    canvas.setTextColor(C_DGREY); canvas.drawString("F2", fx + 22, 2);
    canvas.fillCircle(fx + 37, 6, 2, gV.adc2 > 0.25f ? C_OK : C_DGREY);
    // GPS: green "GPS" + sat count when we have a fix
    canvas.setTextColor(gGps.fix ? C_OK : C_DGREY); canvas.drawString("GPS", 120, 2);
    char gsb[6]; snprintf(gsb, sizeof(gsb), "%d", gGps.sats);
    canvas.setTextColor(C_GREY); canvas.drawString(gsb, 140, 2);
    uint32_t ts = gRec ? (millis() - gTripMs) / 1000 : 0;
    char rc[8]; snprintf(rc, sizeof(rc), "%02u:%02u", (unsigned)(ts/60), (unsigned)(ts%60));
    canvas.setTextDatum(TR_DATUM); canvas.setTextColor(C_GREY); canvas.drawString(rc, DW - 3, 2);
    if (gRec) canvas.fillCircle(DW - 36, 6, 2, C_RED);
    canvas.fillRoundRect(DW - 68, 1, 28, 10, 2, gRec ? C_RED : C_VOLT);
    canvas.setTextDatum(TL_DATUM); canvas.setTextColor(C_BG); canvas.drawString(gRec ? "REC" : "RIDE", DW - 65, 2);

    float spd = gV.valid ? gV.speed_kmh : 0, duty = gV.valid ? gV.duty_pct : 0;
    char v[12];
    snprintf(v, sizeof(v), "%d", (int)spd);
    uiMegaBar(14, 20, "SPEED", spd / 45.f, C_VOLTD, v, "KM/H");
    snprintf(v, sizeof(v), "%d", (int)duty);
    uiMegaBar(37, 20, "DUTY", duty / 100.f, dutyColor(duty), v, "%");

    // PITCH bar (3rd) — needles: lime = setpoint, ice = live pitch
    {
        int x = 5, y = 60, w = DW - 10, h = 20;
        canvas.drawRoundRect(x, y, w, h, 2, C_DGREY);
        int midx = x + w / 2;
        canvas.drawFastVLine(midx, y + 3, h - 6, C_VOLTD);
        float sp  = constrain(gV.setpoint, -15.f, 15.f);
        float pit = constrain(gV.pitch, -15.f, 15.f);
        int spx = midx + (int)(sp  / 15.f * (w / 2 - 5));
        int pix = midx + (int)(pit / 15.f * (w / 2 - 5));
        int lo = min(spx, pix), hi = max(spx, pix);
        if (hi > lo) canvas.fillRect(lo, y + 5, hi - lo, h - 10, C_PANEL);
        canvas.fillRect(spx - 1, y + 2, 2, h - 4, C_VOLT);
        canvas.fillRect(pix - 1, y + 2, 2, h - 4, C_ICE);
        canvas.setTextSize(1); canvas.setTextDatum(ML_DATUM); canvas.setTextColor(C_WHITE);
        canvas.drawString("PITCH", x + 4, y + h / 2);
        char pv[10]; snprintf(pv, sizeof(pv), "%+.0f", gV.pitch);
        canvas.setTextDatum(MR_DATUM); canvas.setTextColor(C_ICE);
        canvas.drawString(pv, x + w - 32, y + h / 2);
        char sv[12]; snprintf(sv, sizeof(sv), "SP%+.0f", gV.setpoint);
        canvas.setTextColor(C_VOLT); canvas.drawString(sv, x + w - 4, y + h / 2);
        canvas.setTextDatum(TL_DATUM);
    }

    // 3 temperature tiles: CTRL · MOTOR · BAT
    auto temptile = [&](int x, int w, const char* lab, float t, bool has) {
        canvas.fillRoundRect(x, 82, w, 26, 3, C_FOOT);
        canvas.drawRoundRect(x, 82, w, 26, 3, C_DGREY);
        canvas.setTextSize(1); canvas.setTextDatum(TC_DATUM); canvas.setTextColor(C_GREY);
        canvas.drawString(lab, x + w / 2, 85);
        char b[8]; if (has) snprintf(b, sizeof(b), "%d", (int)t); else snprintf(b, sizeof(b), "--");
        canvas.setTextSize(2); canvas.setTextColor(has ? C_WHITE : C_DGREY);   // white = readable on the dark tile
        canvas.drawString(b, x + w / 2, 94);
        canvas.setTextSize(1); canvas.setTextDatum(TL_DATUM);
    };
    int tw = (DW - 10 - 10) / 3;
    temptile(5,                 tw, "CTRL",  gV.temp_fet, gV.valid);
    temptile(5 + tw + 5,        tw, "MOTOR", gV.temp_mot, gV.valid);
    temptile(5 + 2 * (tw + 5),  tw, "BAT",   gV.temp_bat, gV.bms);

    // battery footer
    canvas.fillRect(0, 121, DW, 14, C_FOOT);
    canvas.drawFastHLine(0, 121, DW, C_PANEL);
    // SOC from measured pack voltage (profile range). The VESC's battery_level
    // (gV.batt_pct) is unreliable on this setup — a wrong cell count in the VESC
    // config saturates it at 100% (e.g. 72.9V reported as full) — so trust the
    // measured voltage; fall back to battery_level only if voltage is invalid.
    // Sane voltage window from cell count (3.0–4.2 V/cell). The saved profile
    // can be corrupt (we've seen batt_min_v=1587V from a bad mcconf/SD profile),
    // so only trust profile min/max when they're physically plausible.
    int   cells = (gProfile.batt_cells >= 4 && gProfile.batt_cells <= 32) ? gProfile.batt_cells : N_CELLS_DEF;
    float vmin = cells * 3.0f, vmax = cells * 4.2f;
    if (gProfile.batt_min_v > 10.f && gProfile.batt_min_v < vmax &&
        gProfile.batt_max_v > gProfile.batt_min_v && gProfile.batt_max_v < 200.f) {
        vmin = gProfile.batt_min_v; vmax = gProfile.batt_max_v;
    }
    // soc < 0 = no reading yet (not connected to a VESC) → show "--" not "0%".
    // packVoltage() prefers the BMS cell-sum (true SoC) over charger-inflated v_in.
    float pv = packVoltage();
    float soc = -1.f;
    if ((gV.valid || gBms.valid) && pv > 1.f)
        soc = constrain((pv - vmin) / (vmax - vmin), 0.f, 1.f);
    else if (gV.setup && gV.batt_pct > 0)
        soc = constrain(gV.batt_pct / 100.f, 0.f, 1.f);
    char bp[8];
    if (soc < 0.f) snprintf(bp, sizeof(bp), "--");
    else           snprintf(bp, sizeof(bp), "%d%%", (int)(soc * 100));
    canvas.setTextColor(C_VOLT); canvas.setTextDatum(ML_DATUM); canvas.drawString(bp, 3, 128);
    int bx = 32, bw = 148, by = 125, bh = 6;
    canvas.drawRect(bx, by, bw, bh, C_DGREY);
    uint16_t sc = soc < 0.15f ? C_RED : soc < 0.30f ? C_WARN : C_VOLT;
    if (soc > 0.f) canvas.fillRect(bx + 1, by + 1, (int)((bw - 2) * soc), bh - 2, sc);
    char tr[24]; snprintf(tr, sizeof(tr), "%.1fkm", gStat.total_km);
    canvas.setTextColor(C_GREY); canvas.setTextDatum(MR_DATUM); canvas.drawString(tr, DW - 3, 128);
    canvas.setTextDatum(TL_DATUM);
    // full-charge banner (blinking) overrides the footer while the chime rings
    if (gChargeAlarm && !gChargeDismissed) {
        canvas.fillRect(0, 121, DW, 14, C_BG);
        canvas.setTextColor((millis() / 400) % 2 ? C_GREEN : C_VOLT);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString("FULL 100% - any key", DW / 2, 128);
        canvas.setTextDatum(TL_DATUM);
    }
}

// ── SCREEN 1 · DETAIL — full data grid (review on Mac later) ─────────────────
static void drawDetail() {
    canvas.fillScreen(C_BG);
    uiStatBar("DETAIL", "ALL", C_ICE, "");
    float vcell = gProfile.batt_cells ? gV.voltage / gProfile.batt_cells : 0;
    float kw = fabsf(gV.voltage * gV.curr_in) / 1000.f;
    struct { const char* k; char v[12]; uint16_t c; } rows[16];
    int n = 0;
    auto add = [&](const char* k, const char* fmt, float val, uint16_t c) {
        rows[n].k = k; snprintf(rows[n].v, 12, fmt, val); rows[n].c = c; n++;
    };
    add("V/CELL", "%.2f",  vcell,          C_VOLT);
    add("PACK",   "%.1f",  gV.voltage,     C_WHITE);
    add("A IN",   "%.0f",  gV.curr_in,     C_WHITE);
    add("A MOT",  "%.0f",  gV.curr_mot,    C_WHITE);
    add("POWER",  "%.1f",  kw,             C_WHITE);
    add("PITCH",  "%+.1f", gV.pitch,       C_ICE);
    add("ROLL",   "%+.1f", gV.roll,        C_WHITE);
    add("SETPT",  "%+.1f", gV.setpoint,    C_VOLT);
    add("ATR",    "%+.1f", gV.atr_set,     C_WHITE);
    add("TQTILT", "%+.1f", gV.torque_tilt, C_WHITE);
    add("TURN",   "%+.1f", gV.turn_tilt,   C_WHITE);
    add("ADC1",   "%.2f",  gV.adc1,        C_WHITE);
    add("ADC2",   "%.2f",  gV.adc2,        C_WHITE);
    add("BOOST",  "%.0f",  gV.booster,     C_WHITE);
    add("FOC ID", "%.1f",  gV.foc_id,      C_WHITE);
    rows[n].k = "STATE"; rows[n].c = C_VOLT;
    snprintf(rows[n].v, 12, "%s", gV.refloat ? refloatStateName(gV.state) : (gV.valid ? "RUN" : "--")); n++;

    int top = 16, dy = 14, colW = DW / 2, half = (n + 1) / 2;
    for (int i = 0; i < n; i++) {
        int col = i / half, r = i % half;
        int x = col * colW + 5, y = top + r * dy;
        canvas.setTextSize(1); canvas.setTextDatum(TL_DATUM);
        canvas.setTextColor(C_GREY); canvas.drawString(rows[i].k, x, y);
        canvas.setTextColor(rows[i].c); canvas.setTextDatum(TR_DATUM);
        canvas.drawString(rows[i].v, col * colW + colW - 6, y);
        canvas.drawFastHLine(x, y + 10, colW - 11, C_PANEL);
        canvas.setTextDatum(TL_DATUM);
    }
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
static const char* faultName(uint8_t f) {
    switch (f) {
        case 0:  return "NONE";            case 1:  return "OVER VOLTAGE";
        case 2:  return "UNDER VOLTAGE";   case 3:  return "DRV";
        case 4:  return "ABS OVERCURRENT"; case 5:  return "OVER TEMP FET";
        case 6:  return "OVER TEMP MOTOR"; case 7:  return "GATE DRV OV";
        case 8:  return "GATE DRV UV";     case 9:  return "MCU UNDER VOLT";
        case 10: return "WATCHDOG RESET";  case 11: return "ENCODER SPI";
        case 12: return "ENCODER MIN";     case 13: return "ENCODER MAX";
        case 14: return "FLASH CORRUPT";   default: return "FAULT";
    }
}

static void drawFault() {
    bool f = (gV.fault != 0);
    canvas.fillScreen(f ? 0x2000 : C_BG);
    uiStatBar("FAULT MONITOR", f ? "FAULT" : "OK", f ? C_RED : C_OK, "");
    canvas.setTextDatum(MC_DATUM);
    if (f) {
        canvas.setTextColor(C_RED); canvas.setTextSize(3); canvas.drawString("!", DW / 2, 40);
        canvas.setTextSize(2); canvas.setTextColor(C_WHITE); canvas.drawString(faultName(gV.fault), DW / 2, 72);
        char c[20]; snprintf(c, sizeof(c), "MC FAULT  CODE %d", gV.fault);
        canvas.setTextSize(1); canvas.setTextColor(C_RED); canvas.drawString(c, DW / 2, 92);
        canvas.setTextColor(C_DGREY); canvas.drawString("recorded to session log", DW / 2, 108);
    } else {
        canvas.setTextColor(C_OK); canvas.setTextSize(2); canvas.drawString("NO FAULT", DW / 2, 55);
        canvas.setTextSize(1); canvas.setTextColor(C_GREY); canvas.drawString("system nominal", DW / 2, 80);
        if (gV.refloat && gV.state >= 6 && gV.state <= 13) {
            canvas.setTextColor(C_WARN); canvas.drawString(refloatStateName(gV.state), DW / 2, 98);
        }
    }
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
        if (!gBleOk) {
            canvas.drawString("not connected", DW / 2, 70);
        } else {
            char m[44]; snprintf(m, sizeof(m), "no cell data  (BMS replies: %d)", gBmsReplies);
            canvas.drawString(m, DW / 2, 64);
            canvas.setTextColor(C_DGREY);
            canvas.drawString(gBmsReplies > 0 ? "BMS answers but no cells in packet"
                                              : "BMS not answering cmd 96", DW / 2, 80);
        }
        canvas.setTextDatum(TL_DATUM); canvas.setTextSize(1); return;
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

// Combined BACKUP / RESTORE management screen.
static void drawRestore() {
    canvas.fillScreen(C_BG);
    uiStatBar("BACKUP / RESTORE", gBleOk ? "READY" : "--", gBleOk ? C_OK : C_DGREY, "");
    uiTitle("BACKUP / RESTORE", "GET 14/16/93", C_ICE);
    // create-backup action
    canvas.drawRoundRect(5, 28, DW - 10, 14, 3, C_VOLTD);
    canvas.setTextSize(1); canvas.setTextDatum(ML_DATUM); canvas.setTextColor(C_VOLT);
    canvas.drawString("> CREATE BACKUP    press [B]", 10, 35);
    canvas.setTextDatum(TL_DATUM);
    if (gBackupMsg[0]) {
        canvas.setTextColor(strncmp(gBackupMsg, "ERR", 3) == 0 ? C_RED : C_OK);
        canvas.drawString(gBackupMsg, 6, 46);
    } else {
        canvas.setTextColor(C_GREY); canvas.drawString("RESTORE POINTS ON SD:", 6, 46);
    }
    int n = uiBackupList(57, gBackupSel);
    if (n > 0 && gBackupSel >= n) gBackupSel = n - 1;
    canvas.setTextColor(C_WARN); canvas.setTextDatum(TL_DATUM);
    canvas.drawString("restore write: disabled (read-only)", 6, 124);
}

static void renderScreen() {
    switch (gScreen) {
        case 0: drawRide();    break;
        case 1: drawDetail();  break;
        case 2: drawTrip();    break;
        case 3: drawFault();   break;
        case 4: drawCells();   break;
        case 5: drawRestore(); break;
        case 6: drawBoard();   break;
        case 7: drawConfig();  break;
        case 8: drawReview();  break;
        case 9: drawApply();   break;
        case 10: drawWifi();   break;
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

// ═════════════════════════════════════════════════════════════════════════════
//  WiFi — home-network LAN server (read-only file download + live telemetry)
//  Creds: SD /wifi.txt (line1 SSID, line2 password) → cached in NVS.
//  URLs:  http://vesctuner.local/            built-in browse + live page
//         GET /api/sessions                  JSON list of /sessions files
//         GET /sd?f=session_001.csv          stream/download one file
//         GET /api/live                      current telemetry JSON (poll)
//         GET /api/info                      device + connection status
// ═════════════════════════════════════════════════════════════════════════════
static WebServer gWeb(80);
static bool      gWifiOn   = false;
static String    gWifiIp   = "";
static String    gWifiSsid = "";

static bool wifiReadCreds(String& ssid, String& pass) {
    if (gSdOk && SD.exists("/wifi.txt")) {
        File f = SD.open("/wifi.txt", FILE_READ);
        if (f) {
            ssid = f.readStringUntil('\n'); pass = f.readStringUntil('\n'); f.close();
            ssid.trim(); pass.trim();
            if (ssid.length()) {   // cache to NVS so it survives without the card
                Preferences p; p.begin("vesc", false);
                p.putString("wifi_ssid", ssid); p.putString("wifi_pass", pass); p.end();
                return true;
            }
        }
    }
    Preferences p; p.begin("vesc", true);
    ssid = p.getString("wifi_ssid", ""); pass = p.getString("wifi_pass", ""); p.end();
    return ssid.length() > 0;
}

// JSON-escape a filename (filenames are simple, but be safe)
static String jsonEsc(const String& s) {
    String o; for (size_t i = 0; i < s.length(); i++) { char c = s[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; } else o += c; } return o;
}

static void wifiCors() { gWeb.sendHeader("Access-Control-Allow-Origin", "*"); }

static void handleSessions() {
    wifiCors();
    String out = "[";
    File dir = SD.open("/sessions");
    bool first = true;
    if (dir) {
        for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
            if (e.isDirectory()) { e.close(); continue; }
            String nm = e.name(); int sl = nm.lastIndexOf('/'); if (sl >= 0) nm = nm.substring(sl + 1);
            if (!first) out += ",";
            out += "{\"name\":\"" + jsonEsc(nm) + "\",\"size\":" + String((uint32_t)e.size()) + "}";
            first = false; e.close();
        }
        dir.close();
    }
    out += "]";
    gWeb.send(200, "application/json", out);
}

static void handleDownload() {
    wifiCors();
    String f = gWeb.arg("f");
    if (f.length() == 0 || f.indexOf("..") >= 0 || f.indexOf('/') >= 0) { gWeb.send(400, "text/plain", "bad file"); return; }
    String path = "/sessions/" + f;
    if (!SD.exists(path)) { gWeb.send(404, "text/plain", "not found"); return; }
    File file = SD.open(path, FILE_READ);
    if (!file) { gWeb.send(500, "text/plain", "open failed"); return; }
    String ct = f.endsWith(".json") ? "application/json" : f.endsWith(".csv") ? "text/csv" : "application/octet-stream";
    gWeb.sendHeader("Content-Disposition", "attachment; filename=\"" + f + "\"");
    gWeb.streamFile(file, ct);
    file.close();
}

static void handleLive() {
    wifiCors();
    char b[420];
    float power = gV.voltage * gV.curr_in;
    snprintf(b, sizeof(b),
        "{\"t\":%lu,\"ble\":%d,\"rec\":%d,\"valid\":%d,"
        "\"speed\":%.2f,\"duty\":%.1f,\"vin\":%.2f,\"ain\":%.2f,\"amot\":%.2f,"
        "\"power\":%.0f,\"fet\":%.1f,\"mot\":%.1f,\"batt\":%.0f,\"pitch\":%.2f,"
        "\"rpm\":%.0f,\"wh\":%.1f,\"odo\":%.2f,\"cells\":%d}",
        (unsigned long)millis(), gBleOk ? 1 : 0, gRec ? 1 : 0, gV.valid ? 1 : 0,
        gV.speed_kmh, gV.duty_pct, gV.voltage, gV.curr_in, gV.curr_mot,
        power, gV.temp_fet, gV.temp_mot, gV.batt_pct, gV.pitch,
        gV.rpm, gV.watt_hours, gV.odo_km, gBms.cellNum);
    gWeb.send(200, "application/json", b);
}

static void handleInfo() {
    wifiCors();
    char b[200];
    snprintf(b, sizeof(b),
        "{\"name\":\"vesc-tuner\",\"ssid\":\"%s\",\"ip\":\"%s\",\"ble\":%d,\"canid\":%d,\"sd\":%d}",
        gWifiSsid.c_str(), gWifiIp.c_str(), gBleOk ? 1 : 0, gCanId, gSdOk ? 1 : 0);
    gWeb.send(200, "application/json", b);
}

// Compact self-contained browse + live page (no external assets except uPlot CDN).
static const char ROOT_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>VESC Tuner — device</title><style>
body{background:#0f172a;color:#e2e8f0;font:14px -apple-system,system-ui,sans-serif;margin:0;padding:18px}
h1{font-size:17px;margin:0 0 2px}h1 b{color:#06b6d4}.sub{color:#64748b;font-size:12px;margin-bottom:16px}
.card{background:#1e293b;border:1px solid #334155;border-radius:10px;padding:14px;margin-bottom:14px}
.g{display:grid;grid-template-columns:repeat(auto-fill,minmax(110px,1fr));gap:10px}
.kv{background:#0f172a;border:1px solid #334155;border-radius:8px;padding:9px 11px}
.kv .k{font-size:10px;color:#64748b;text-transform:uppercase;letter-spacing:.5px}
.kv .v{font-size:21px;font-weight:800;font-variant-numeric:tabular-nums;margin-top:3px}
table{width:100%;border-collapse:collapse;font-size:13px}td,th{text-align:left;padding:7px 9px;border-bottom:1px solid #334155}
th{color:#64748b;font-size:11px;text-transform:uppercase}a{color:#38bdf8;text-decoration:none}a:hover{text-decoration:underline}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
</style></head><body>
<h1>VESC <b>TUNER</b> · device</h1><div class=sub id=info>on your home WiFi · read-only</div>
<div class=card><div style="font-size:11px;color:#64748b;text-transform:uppercase;margin-bottom:10px">Live <span id=lstat></span></div>
<div class=g id=live></div></div>
<div class=card><div style="font-size:11px;color:#64748b;text-transform:uppercase;margin-bottom:10px">Sessions on SD</div>
<table id=tbl><tr><th>file</th><th>size</th><th></th></tr></table></div>
<script>
const F=[['speed','km/h'],['duty','%'],['vin','V'],['ain','A in'],['amot','A mot'],['power','W'],['fet','FET°'],['mot','MOT°'],['batt','batt%'],['pitch','pitch°']];
const live=document.getElementById('live');
F.forEach(([k,l])=>{live.insertAdjacentHTML('beforeend',`<div class=kv><div class=k>${l}</div><div class=v id=v_${k}>–</div></div>`)});
function fmt(n){return (n==null||isNaN(n))?'–':(+n).toFixed(1)}
async function tick(){try{const r=await fetch('/api/live');const d=await r.json();
 F.forEach(([k])=>document.getElementById('v_'+k).textContent=fmt(d[k]));
 document.getElementById('lstat').innerHTML=`<span class=dot style="background:${d.ble?'#22c55e':'#ef4444'}"></span>${d.ble?'BLE connected':'no BLE'}${d.rec?' · REC':''}`;
}catch(e){document.getElementById('lstat').textContent='offline'}}
async function listing(){try{const r=await fetch('/api/sessions');const a=await r.json();
 const t=document.getElementById('tbl');t.innerHTML='<tr><th>file</th><th>size</th><th></th></tr>';
 a.sort((x,y)=>y.name.localeCompare(x.name));
 a.forEach(s=>{const kb=(s.size/1024).toFixed(0)+' KB';
  t.insertAdjacentHTML('beforeend',`<tr><td>${s.name}</td><td>${kb}</td><td><a href="/sd?f=${encodeURIComponent(s.name)}">download</a></td></tr>`)});
}catch(e){}}
async function info(){try{const d=await(await fetch('/api/info')).json();
 document.getElementById('info').textContent=`${d.ip} · ssid ${d.ssid} · CAN ${d.canid} · read-only`;}catch(e){}}
info();listing();tick();setInterval(tick,300);setInterval(listing,5000);
</script></body></html>)HTML";

static void handleRoot() { gWeb.send_P(200, "text/html", ROOT_HTML); }

static void wifiRoutes() {
    gWeb.on("/",             handleRoot);
    gWeb.on("/api/sessions", handleSessions);
    gWeb.on("/sd",           handleDownload);
    gWeb.on("/api/live",     handleLive);
    gWeb.on("/api/info",     handleInfo);
    gWeb.onNotFound([]() { wifiCors(); gWeb.send(404, "text/plain", "not found"); });
}

// Connect to the saved home network and start the LAN server. User-triggered ([W]).
static bool wifiConnect() {
    String ssid, pass;
    if (!wifiReadCreds(ssid, pass)) { Serial.println("[WiFi] no creds — put SSID/pass in SD /wifi.txt"); return false; }
    gWifiSsid = ssid;
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("[WiFi] connecting to %s ...\n", ssid.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) { delay(200); }
    if (WiFi.status() != WL_CONNECTED) { Serial.println("[WiFi] connect timeout"); gWifiOn = false; return false; }
    gWifiIp = WiFi.localIP().toString();
    if (MDNS.begin("vesctuner")) MDNS.addService("http", "tcp", 80);
    wifiRoutes();
    gWeb.begin();
    gWifiOn = true;
    Serial.printf("[WiFi] up: http://%s/  (http://vesctuner.local/)\n", gWifiIp.c_str());
    return true;
}

static void wifiDisconnect() {
    if (!gWifiOn) return;
    gWeb.stop(); MDNS.end(); WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    gWifiOn = false; gWifiIp = "";
    Serial.println("[WiFi] off");
}

// WIFI screen (gScreen 10) — status + how-to
static void drawWifi() {
    canvas.fillScreen(C_BG);
    uiStatBar("WIFI", gWifiOn ? "ON" : "OFF", gWifiOn ? C_OK : C_DGREY, gWifiIp.c_str());
    uiTitle("WiFi LAN server", gWifiOn ? "LIVE" : nullptr, C_OK);
    canvas.setTextSize(1); canvas.setTextDatum(TL_DATUM);
    int y = 32;
    if (gWifiOn) {
        canvas.setTextColor(C_OK);    canvas.drawString("Connected", 6, y); y += 14;
        char l[48];
        canvas.setTextColor(C_GREY);  snprintf(l, sizeof(l), "SSID: %s", gWifiSsid.c_str()); canvas.drawString(l, 6, y); y += 12;
        canvas.setTextColor(C_VOLT);  snprintf(l, sizeof(l), "http://%s/", gWifiIp.c_str());  canvas.drawString(l, 6, y); y += 12;
        canvas.setTextColor(C_CYAN);  canvas.drawString("http://vesctuner.local/", 6, y);     y += 16;
        canvas.setTextColor(C_GREY);  canvas.drawString("[W] reconnect   [X] turn off", 6, y);
    } else {
        canvas.setTextColor(C_GREY);
        canvas.drawString("Put credentials on SD card:", 6, y); y += 14;
        canvas.setTextColor(C_WHITE);
        canvas.drawString("/wifi.txt  line1=SSID  line2=pass", 6, y); y += 16;
        canvas.setTextColor(C_VOLT);
        canvas.drawString("[W] connect to home WiFi", 6, y); y += 14;
        canvas.setTextColor(C_DGREY);
        canvas.drawString("then browse vesctuner.local", 6, y);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Helmet HUD broadcast (ESP-NOW) — read-only: just mirrors telemetry to the HUD.
//  Coexists with BLE; uses WiFi STA (no AP). If the WiFi-LAN download feature is
//  toggled it changes channel/mode and ESP-NOW pauses until reboot.
// ─────────────────────────────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t magic, ver, board_id, flags;       // flags bit0=braking, bit1=footpad on
    uint8_t batt_pct, duty_limit, motor_temp, bright, gps_sats;
    int16_t speed_x10, duty_x10;
    uint8_t seq;
} hud_pkt_t;
static uint8_t  HUD_BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static bool     gEspNowOk = false;
static uint32_t gLastEspMs = 0;
static uint8_t  gEspSeq = 0;

static void espnowInit() {
    if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
    if (esp_now_init() != ESP_OK) { gEspNowOk = false; return; }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, HUD_BCAST, 6);
    peer.channel = 0; peer.encrypt = false;
    esp_now_add_peer(&peer);
    gEspNowOk = true;
    Serial.println("[ESP-NOW] HUD broadcast ready");
}
static void espnowSend() {
    if (!gEspNowOk || !gV.valid) return;
    hud_pkt_t p;
    p.magic = 0xBE; p.ver = 1; p.board_id = 1;
    uint8_t fl = 0;
    if (gV.curr_mot < -8.f)                 fl |= 0x01;   // regen / braking
    if (gV.adc1 > 0.25f || gV.adc2 > 0.25f) fl |= 0x02;   // footpad engaged
    p.flags = fl;
    int batt = (int)gV.batt_pct;
    if (batt <= 0 && gProfile.batt_cells > 0) {
        float vc = packVoltage() / gProfile.batt_cells;
        batt = (int)((vc - 3.0f) / 1.2f * 100.f);
    }
    p.batt_pct   = (uint8_t)constrain(batt, 0, 100);
    p.duty_limit = (uint8_t)(gProfile.tiltback_duty > 0 ? gProfile.tiltback_duty : 80);
    p.motor_temp = (uint8_t)constrain((int)gV.temp_mot, 0, 200);
    p.bright     = 40;
    p.gps_sats   = (uint8_t)constrain(gGps.sats, 0, 99);
    p.speed_x10  = (int16_t)(gV.speed_kmh * 10);
    p.duty_x10   = (int16_t)(gV.duty_pct * 10);
    p.seq        = gEspSeq++;
    esp_now_send(HUD_BCAST, (uint8_t*)&p, sizeof(p));
}

// ─────────────────────────────────────────────────────────────────────────────
//  setup()
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[BOOT] setup() start");
    setClockFromBuild();         // sane date for SD files (no hardware RTC)
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(120);
    { Preferences p; p.begin("vesc", true); gVolume = p.getUChar("vol", 160); p.end(); }
    M5Cardputer.Speaker.setVolume(gVolume);   // restored volume (Q/W adjust)
    gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);   // GPS Unit on Grove
    espnowInit();                                                    // helmet HUD broadcast
    Serial.printf("[BOOT] M5 begin ok, disp %dx%d\n",
                  (int)M5Cardputer.Display.width(), (int)M5Cardputer.Display.height());

    void* sb = canvas.createSprite(DW, DH);
    canvas.setTextWrap(false);
    Serial.printf("[BOOT] sprite buf=%p %dx%d\n", sb, (int)canvas.width(), (int)canvas.height());

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
    Serial.printf("[BOOT] SD=%d, init BLE...\n", (int)gSdOk);
    NimBLEDevice::init("VESC-Tuner");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setMTU(517);   // request large ATT MTU — fewer fragmented responses
    canvas.setTextDatum(TL_DATUM);
    Serial.println("[BOOT] BLE init done, autoConnectLast()...");

    autoConnectLast();           // reconnect last board without [P]
    Serial.println("[BOOT] setup() done");
}

// Full-charge chime: rings a two-note bell every ~2.5s once the pack hits
// ~4.2 V/cell (100%). Any keypress silences it (see handleKeys); it re-arms
// after the voltage drops ~1V (so the next full charge alerts again).
static void checkChargeAlarm() {
    if (!(gV.valid || gBms.valid)) { gChargeAlarm = false; return; }
    // only meaningful when parked/charging — never while riding (regen near full
    // would false-fire). Riding = any real speed.
    if (fabsf(gV.speed_kmh) > 1.5f) { gChargeAlarm = false; gChargePingStep = -1; return; }
    float v = packVoltage();             // BMS cell-sum (true), not charger-inflated v_in
    if (v < 1.f) { gChargeAlarm = false; return; }
    int cells = (gProfile.batt_cells >= 4 && gProfile.batt_cells <= 32) ? gProfile.batt_cells : N_CELLS_DEF;
    float full    = cells * 4.2f;        // 100%  (84.0 V on 20S)
    float pingLow = full - 1.0f;         // start of the run-up window (83.0 V)

    // re-arm everything once we drop back below the window (next charge alerts again)
    if (v < pingLow - 0.1f) {
        gChargePingStep = -1; gChargeAlarm = false; gChargeDismissed = false;
    }

    // run-up pings: one short "ping" per ascending 0.1V step, 83.1 … 83.9 V
    if (v >= pingLow && v < full - 0.05f) {
        int step = (int)floorf((v - pingLow) / 0.1f + 0.001f);            // 0..9
        if (step > gChargePingStep) {
            gChargePingStep = step;
            if (step >= 1) M5Cardputer.Speaker.tone(3136, 70);            // single ping (G7)
        }
    }

    // full (~100%): continuous two-note bell + banner until any key dismisses it
    if (v >= full - 0.05f) {
        if (!gChargeDismissed) gChargeAlarm = true;
        if (gChargePingStep < 10) gChargePingStep = 10;
    }
    if (gChargeAlarm && !gChargeDismissed) {
        uint32_t now = millis(), dt = now - gChargeBeepMs;
        if (dt > 2500) { gChargeBeepMs = now; M5Cardputer.Speaker.tone(2093, 120); gChargeBeep2 = true; }
        else if (gChargeBeep2 && dt > 160) { gChargeBeep2 = false; M5Cardputer.Speaker.tone(2794, 150); }
    }
}

// Duty-cycle Geiger counter: from 10% below the configured tiltback_duty up to
// it, tick faster the closer you get — audible "you're pushing duty" warning.
// Limit comes from the SD profile (gProfile.tiltback_duty) — set it to match
// your Refloat tiltback_duty; defaults to 80%.
static uint32_t gDutyTickMs = 0;
static void checkDutyAlarm() {
    if (!gV.valid) return;
    const float hi = gProfile.tiltback_duty > 0 ? gProfile.tiltback_duty : 80.0f;
    const float lo = hi - 10.0f;
    float d = fabsf(gV.duty_pct);
    if (d < lo) return;                                     // below window → silent
    float frac = (d - lo) / (hi - lo);
    if (frac > 1.f) frac = 1.f;                             // at/over limit → fastest
    uint32_t interval = (uint32_t)(650.f - frac * (650.f - 55.f));   // 650ms@70% → 55ms@80%
    uint32_t now = millis();
    if (now - gDutyTickMs >= interval) {
        gDutyTickMs = now;
        M5Cardputer.Speaker.tone(4186, 20);                 // short Geiger "tick" (C8)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  loop()
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    M5Cardputer.update();
    handleKeys();
    checkChargeAlarm();
    checkDutyAlarm();

    if (gWifiOn) gWeb.handleClient();   // serve LAN requests (download / live)
    gpsPoll();                          // drain GPS NMEA every loop
    if (millis() - gLastEspMs >= 50) { gLastEspMs = millis(); espnowSend(); }  // HUD @20Hz

    uint32_t now = millis();

    // Poll VESC @ 12Hz when connected
    if (gBleOk && now - gLastValMs >= 83) {
        gLastValMs = now;
        vescSend(CMD_GET_VALUES);
    }
    // Poll Refloat GET_ALLDATA @ 12Hz for pitch/roll/state + FOOTPADS (float boards
    // only; harmless no-op on classic VESC). Was 5Hz — bumped so footpads aren't
    // stale during fast events (the burnout case where adc looked frozen).
    if (gBleOk && now - gLastAllMs >= 83) {
        gLastAllMs = now;
        vescSendAllData();
    }
    // Poll Smart BMS values @1Hz — try both the CAN-forwarded motor controller
    // and the ESP32 bridge directly (depending on which one aggregates the BMS).
    if (gBleOk && now - gLastBmsMs >= 1000) {
        gLastBmsMs = now;
        vescSend(CMD_BMS_GET_VALUES);        // CAN-forward to motor controller
        vescSendRawCmd(CMD_BMS_GET_VALUES);  // direct to ESP32 bridge
    }
    // Poll setup values @1Hz (battery %, odometer) and raw IMU @5Hz (accel/gyro)
    if (gBleOk && now - gLastSetupMs >= 1000) {
        gLastSetupMs = now;
        vescSend(CMD_GET_VALUES_SETUP);
    }
    if (gBleOk && now - gLastImuMs >= 200) {
        gLastImuMs = now;
        vescSendImu();
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
                    gBmsReplies++;
                    parseBms(pay, plen);
                    if (gBmsDbgN < 5) {       // dump FULL packet to verify temp section
                        gBmsDbgN++;
                        Serial.printf("[BMS] len=%d cells=%d temps=%d tbat=%.1f:",
                                      plen, gBms.cellNum, gBms.tempNum, gV.temp_bat);
                        for (int i = 0; i < plen && i < 128; i++) Serial.printf(" %02X", pay[i]);
                        Serial.println();
                    }
                    break;
                case CMD_GET_VALUES_SETUP:
                    if (gSetupDbgN < 3) { gSetupDbgN++;
                        Serial.printf("[SETUP] len=%d:", plen);
                        for (int i = 0; i < plen && i < 60; i++) Serial.printf(" %02X", pay[i]);
                        Serial.println();
                    }
                    parseSetup(pay, plen);
                    break;
                case CMD_GET_IMU_DATA:
                    if (gImuDbgN < 3) { gImuDbgN++;
                        Serial.printf("[IMU] len=%d:", plen);
                        for (int i = 0; i < plen && i < 44; i++) Serial.printf(" %02X", pay[i]);
                        Serial.println();
                    }
                    parseImu(pay, plen);
                    break;
            }
        }
    }

    // Auto-start logging when the board starts moving (RUNNING / speed > 2 km/h)
    if (gBleOk && gAutoArmed && !gRec && gSdOk && gV.valid &&
        (gV.speed_kmh > 2.f || (gV.refloat && gV.state == 1))) {
        startSession();
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
