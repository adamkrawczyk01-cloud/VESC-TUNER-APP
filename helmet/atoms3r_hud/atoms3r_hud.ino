// ============================================================================
//  Helmet / wrist HUD — AtomS3R receiver  (dual-source: ESP-NOW or direct BLE)
//
//  Data source is auto-selected:
//   • Cardputer present  → receive telemetry over ESP-NOW (Cardputer holds the
//     VESC BLE link + logs to SD + GPS). Preferred whenever it's broadcasting.
//   • No Cardputer       → HUD connects DIRECTLY to the board over BLE (NimBLE
//     central, same CAN-wrapped protocol the Cardputer uses), picks/​remembers a
//     board, and renders a glanceable HUD. No SD log / GPS in this mode.
//
//  Only ONE BLE link to the VESC Express is possible → the HUD yields its link
//  (disconnects) whenever the Cardputer starts broadcasting again. READ-ONLY:
//  the HUD only ever sends GET_*/PING_CAN/FW_VERSION — never a SET command.
//
//  Button (M5Unified multi-click):
//   • RUN:    single-click = cycle page · long-press = brightness ·
//             triple-click (only in BLE mode & speed 0) = re-pick board
//   • SELECT: single-click = next board · long-press = confirm + connect
//
//  LCD = hand-built LGFX (ST7735S on SPI3) + LP5562 backlight (M5GFX autodetect
//  fails on core 3.3.7/IDF5). 2× M5 Puzzle 8×8 = 4 vertical bars over ESP-NOW-
//  shaped hud_pkt_t (filled from ESP-NOW OR from the local BLE parse).
//
//  Board: M5AtomS3R.  Flash (needs the big app partition for NimBLE+WiFi):
//    FQBN=m5stack:esp32:m5stack_atoms3r:PartitionScheme=default_8MB \
//      cardputer/flash.sh helmet/atoms3r_hud /dev/cu.usbmodemXXXX
// ============================================================================
#include <M5Unified.h>
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_ST7735.hpp>   // lgfx::Panel_ST7735S (AtomS3R panel; not public via M5GFX.h)
#include <WiFi.h>
#include <esp_now.h>
#include <Adafruit_NeoPixel.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#define LED_PIN      2
#define NUM_LEDS     128
#define PANEL_TOP    0
#define PANEL_BOTTOM 1
#define PKT_MAGIC    0xBE
#define SCREEN_ROT   1     // USB-C on the right, content upright (ST7735S offset_rotation=2)
#define HUD_NAME     "VHUD"   // boot branding

// ── VESC BLE protocol (same as the Cardputer dashboard) ──────────────────────
#define NUS_SVC_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CMD_FW_VERSION       0
#define CMD_GET_VALUES       4
#define CMD_FORWARD_CAN     34   // 0x22 — wrap [34,canId,cmd] to reach the motor over CAN
#define CMD_PING_CAN        62   // 0x3E — ESP32 bridge enumerates CAN device ids
#define CMD_BMS_GET_VALUES  96   // 0x60 — Smart BMS values (cells + temps)
#define CMD_CUSTOM_APP_DATA 36   // 0x24 — Refloat package commands
#define REFLOAT_MAGIC      101
#define REFLOAT_ALLDATA     10   // mode 1 (35 B) carries the footpad ADCs

// Compiled profile — the HUD has no SD/mcconf, so speed & SOC use these constants.
// Defaults match the Cardputer profile (GAD / ADV2 board). A different board would
// need these retuned for an accurate speed scale.
#define POLE_PAIRS_DEF   15      // pole pairs (30-magnet / 30-pole motor) — standard
#define WHEEL_MM_DEF    234      // standard 11" board, GPS-calibrated effective diameter
#define N_CELLS_DEF      20      // 20S battery fallback

// ESP-NOW / display packet (shared render input for both sources)
typedef struct __attribute__((packed)) {
  // flags: bit0 braking · bit1 footpad LEFT · bit2 footpad RIGHT · bit3 board link.
  // bit3 is what separates "Cardputer is here but has no board" from "no Cardputer"
  // — it broadcasts either way, so without the flag the wrist cannot tell them apart.
  uint8_t  magic, ver, board_id, flags;
  uint8_t  batt_pct, duty_limit, motor_temp, batt_temp, fet_temp, gps_sats, cells, bright;
  int16_t  speed_x10, duty_x10;
  uint16_t pack_v_x10;
  uint8_t  alert, seq;
} hud_pkt_t;

// 16x8 LANDSCAPE icons (8 rows x 16 cols, '#'=lit). Mapped to the panel like the bars.
static const char* IC_TEMP[8] = {     // °C  (overtemp)
  "##..............","##....#####.....",".....##...##....","....##..........",
  "....##..........","....##..........",".....##...##....","......#####....."};
static const char* IC_BOLT[8] = {     // lightning (electrical)
  "..........###...",".......#####....","....#####.......",".###########....",
  "....###########.","........#####...",".......###......","......##........"};
static const char* IC_BATT[8] = {     // battery (low)
  "................",".############...",".#..........###.",".###........#.#.",
  ".###........#.#.",".#..........###.",".############...","................"};
static const char* IC_WARN[8] = {     // warning triangle (generic fault)
  ".......##.......","......####......",".....##.###.....","....##.##.##....",
  "...##..##..##...","..##...##...##..",".##############.","................"};
static const char* const* ICONS[4] = { IC_TEMP, IC_BOLT, IC_BATT, IC_WARN };

struct AlertDef { uint8_t icon; bool red; const char* name; };  // icon: 0=°C,1=bolt,2=batt,3=warn
static const AlertDef ALERTS[] = {
  {3,true,""}, {1,true,"OVERCURR"}, {1,true,"OVER-VOLT"}, {1,true,"LOW VOLT"}, {1,true,"DRV"},
  {0,false,"FET WARM"}, {0,true,"FET HOT"}, {0,false,"MOT WARM"}, {0,true,"MOTOR HOT"},
  {0,true,"BATT HOT"}, {2,false,"LOW BATT"}, {2,true,"BATT CRIT"}, {3,true,"FAULT"} };
static const int ALERT_N = sizeof(ALERTS)/sizeof(ALERTS[0]);
#define ICON_FLIP_X 0
#define ICON_FLIP_Y 1     // un-mirror glyphs (landscape mapping transposes = mirror)

// ---- hand-built AtomS3R LCD (ST7735S on SPI3); backlight done separately ----
class LGFX_AtomS3R : public lgfx::LGFX_Device {
  lgfx::Panel_ST7735S _lcd;
  lgfx::Bus_SPI       _spibus;
public:
  LGFX_AtomS3R(){
    { auto c = _spibus.config(); c.spi_host=SPI3_HOST; c.spi_mode=0;
      c.freq_write=40000000; c.freq_read=16000000; c.spi_3wire=true;
      c.pin_sclk=15; c.pin_mosi=21; c.pin_miso=-1; c.pin_dc=42;
      _spibus.config(c); _lcd.setBus(&_spibus); }
    { auto c = _lcd.config(); c.pin_cs=14; c.pin_rst=48;       // AtomS3R ST7735S params (from M5GFX)
      c.panel_width=128; c.panel_height=128;
      c.offset_x=2; c.offset_y=31; c.offset_rotation=2;
      c.readable=true; c.bus_shared=false; c.invert=true; _lcd.config(c); }
    setPanel(&_lcd);
  }
};
static LGFX_AtomS3R lcd;
static M5Canvas scr(&lcd);          // offscreen buffer — pushed rotated 180° when flipped
// LP5562 backlight via M5Unified's internal I2C (addr 0x30)
static void lcdBacklight(uint8_t b){
  M5.In_I2C.writeRegister8(0x30, 0x00, 0x40, 400000);
  M5.In_I2C.writeRegister8(0x30, 0x08, 0x01, 400000);
  M5.In_I2C.writeRegister8(0x30, 0x70, 0x00, 400000);
  M5.In_I2C.writeRegister8(0x30, 0x0e, b,    400000);
}

Adafruit_NeoPixel px(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// orientation (IMU auto-flip)
static bool gFlip = false;          // physical 180° flip detected (IMU gravity)
static bool gLedFlip = true;        // LED 180° flip (true = current base mounting)
static int  gScreenRot = SCREEN_ROT;

// ── ESP-NOW receive state ────────────────────────────────────────────────────
static volatile hud_pkt_t gPkt;
static volatile uint32_t  gLastRx = 0, gRxCount = 0;

// ═════════════════════════════════════════════════════════════════════════════
//  Direct-BLE state (ported from vesc_dashboard.ino)
// ═════════════════════════════════════════════════════════════════════════════
struct VescVals {
  float speed_kmh=0, duty_pct=0, temp_fet=0, temp_mot=0, temp_bat=0, voltage=0, curr_mot=0;
  float adc1=0, adc2=0;            // footpad pressure, from Refloat ALLDATA
  uint32_t allRx=0;                // millis() of the last ALLDATA reply
  uint8_t fault=0;
  bool valid=false, bms=false;
};
static VescVals gV;

struct BmsData {
  float vTot=0, iIn=0;
  float cell[36]; bool bal[36]; float temp[16];
  int cellNum=0, tempNum=0;
  bool valid=false;
};
static BmsData gBms;

static NimBLEClient*               gBleClient = nullptr;
static NimBLERemoteCharacteristic* gCharRx    = nullptr;  // write to VESC
static NimBLERemoteCharacteristic* gCharTx    = nullptr;  // notify from VESC
static uint8_t  gRxBuf[512];        // one reassembled frame (GET_VALUES ~75, BMS ~170)
static uint16_t gRxLen   = 0;
static volatile bool gRxReady  = false;
static uint8_t  gAcc[1024];         // raw BLE chunk accumulator (reassembly across MTU chunks)
static uint16_t gAccLen  = 0;
static int      gCanId   = -1;      // motor controller CAN id; -1 = direct/unknown
static volatile bool gCanDiscovered = false;
static uint16_t gNotifyCount = 0;
static uint16_t gTxCount     = 0;
static bool     gBleOk       = false;
static bool     gUseWriteNoResp = true;
static uint32_t gLastValRx   = 0;   // millis() of last fresh GET_VALUES parse

// Per-board speed calibration, selected by the connected board's advertised BLE
// name; falls back to the *_DEF constants. wheel_mm = GPS-calibrated effective
// rolling diameter (reads true ground speed, ~6% under the nominal tyre size).
// pole_pairs = VESC "Motor Poles" / 2.
struct BoardCal { const char* key; float wheel_mm; int pole_pairs; };
static const BoardCal BOARD_CAL[] = {
  {"adv", 240.f, 15},   // ADV2 — 11.25" tyre, 30 poles (GPS ≈ 240mm effective)
  {"gad", 234.f, 15},   // GAD  — 11" tyre,    30 poles
};
static float gWheelMm   = WHEEL_MM_DEF;
static int   gPolePairs = POLE_PAIRS_DEF;
static void applyBoardCal(const char* name){
  gWheelMm=WHEEL_MM_DEF; gPolePairs=POLE_PAIRS_DEF;
  if (!name){ Serial.println("[CAL] no name -> default"); return; }
  std::string lc=name; for (auto& c:lc) c=tolower(c);
  for (auto& b:BOARD_CAL)
    if (lc.find(b.key)!=std::string::npos){ gWheelMm=b.wheel_mm; gPolePairs=b.pole_pairs; break; }
  Serial.printf("[CAL] %s -> wheel=%.0f poles(pairs)=%d\n", name, gWheelMm, gPolePairs);
}

// ── VESC framing / codec (verbatim from the dashboard) ───────────────────────
static uint16_t crc16(const uint8_t* d, size_t n){
  uint16_t crc=0;
  for (size_t i=0;i<n;i++){ crc^=(uint16_t)d[i]<<8;
    for (int b=0;b<8;b++) crc=(crc&0x8000)?(crc<<1)^0x1021:crc<<1; }
  return crc;
}
static int32_t rdI32(const uint8_t* b,int o){
  return ((int32_t)b[o]<<24)|((int32_t)b[o+1]<<16)|((int32_t)b[o+2]<<8)|(int32_t)b[o+3];
}
static int16_t rdI16(const uint8_t* b,int o){ return ((int16_t)b[o]<<8)|(int16_t)b[o+1]; }

static int buildPkt(uint8_t* out,const uint8_t* payload,int plen){
  int i=0;
  if (plen<=255){ out[i++]=0x02; out[i++]=(uint8_t)plen; }
  else { out[i++]=0x03; out[i++]=(uint8_t)(plen>>8); out[i++]=(uint8_t)(plen&0xFF); }
  memcpy(&out[i],payload,plen); i+=plen;
  uint16_t crc=crc16(payload,plen);
  out[i++]=(uint8_t)(crc>>8); out[i++]=(uint8_t)(crc&0xFF); out[i++]=0x03;
  return i;
}
static bool unpackPkt(const uint8_t* raw,int rlen,const uint8_t** pay,int* plen){
  if (rlen<5) return false;
  int i=0;
  if (raw[i]==0x02){ i++; *plen=raw[i++]; }
  else if (raw[i]==0x03){ i++; *plen=((int)raw[i]<<8)|raw[i+1]; i+=2; }
  else return false;
  if (i+*plen+3>rlen) return false;
  *pay=&raw[i];
  uint16_t got=((uint16_t)raw[i+*plen]<<8)|raw[i+*plen+1];
  return got==crc16(*pay,*plen);
}
// send a single command; CAN-forward everything except FW_VERSION/PING_CAN on a bridge
static void vescSend(uint8_t cmd){
  if (!gCharRx || !gBleOk) return;
  uint8_t pkt[16], pay[4]; int plen;
  if (gCanId>=0 && cmd!=CMD_FW_VERSION && cmd!=CMD_PING_CAN){
    pay[0]=CMD_FORWARD_CAN; pay[1]=(uint8_t)gCanId; pay[2]=cmd; plen=3;
  } else { pay[0]=cmd; plen=1; }
  int len=buildPkt(pkt,pay,plen);
  gTxCount++;
  gCharRx->writeValue(pkt,len,!gUseWriteNoResp);
}
// single-byte command WITHOUT CAN-forward (straight to the ESP32 bridge)
static void vescSendRawCmd(uint8_t cmd){
  if (!gCharRx || !gBleOk) return;
  uint8_t pkt[12], pay[1]={cmd};
  int len=buildPkt(pkt,pay,1);
  gTxCount++;
  gCharRx->writeValue(pkt,len,!gUseWriteNoResp);
}

// reassemble complete VESC frames out of the BLE chunk accumulator
static void reassemblePump(){
  while (gAccLen>0){
    uint8_t start=gAcc[0];
    if (start!=0x02 && start!=0x03){ memmove(gAcc,gAcc+1,--gAccLen); continue; }
    int headerLen=(start==0x02)?2:3;
    if (gAccLen<headerLen) break;
    int plen=(start==0x02)?gAcc[1]:(((int)gAcc[1]<<8)|gAcc[2]);
    int total=headerLen+plen+3;
    if (total<=0 || total>(int)sizeof(gAcc)){ gAccLen=0; break; }
    if (gAccLen<total) break;
    if (gAcc[total-1]!=0x03){ memmove(gAcc,gAcc+1,--gAccLen); continue; }
    const uint8_t* payload=gAcc+headerLen;
    uint16_t calc=crc16(payload,plen);
    uint16_t got=((uint16_t)gAcc[headerLen+plen]<<8)|gAcc[headerLen+plen+1];
    if (calc==got){
      if (total<=(int)sizeof(gRxBuf)){ memcpy(gRxBuf,gAcc,total); gRxLen=(uint16_t)total; gRxReady=true; }
      gAccLen-=total; memmove(gAcc,gAcc+total,gAccLen);
    } else { memmove(gAcc,gAcc+1,--gAccLen); }
  }
}
static void notifyCallback(NimBLERemoteCharacteristic*,uint8_t* data,size_t len,bool){
  gNotifyCount++;
  if (gAccLen+len>sizeof(gAcc)) gAccLen=0;   // overflow guard
  memcpy(gAcc+gAccLen,data,len); gAccLen+=(uint16_t)len;
  reassemblePump();
}
struct ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*)         override { gBleOk=true; }
  void onDisconnect(NimBLEClient*,int)  override {
    gBleOk=false; gV.valid=false; gCanId=-1; gCanDiscovered=false; gAccLen=0; gRxReady=false;
  }
} gClientCB;

// ── Scan results (filtered to VESC/NUS boards for the picker) ─────────────────
static const int MAX_SCAN_RESULTS = 6;
struct ScanEntry { NimBLEAddress addr; std::string name; bool hasNUS; };
static ScanEntry gScanList[MAX_SCAN_RESULTS];
static int       gScanCount = 0;
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    std::string nm=dev->getName();
    bool hasNUS=dev->isAdvertisingService(NimBLEUUID(NUS_SVC_UUID));
    std::string lc=nm; for (auto& ch:lc) ch=tolower(ch);
    bool isVesc = lc.find("vesc")!=std::string::npos || lc.find("float")!=std::string::npos ||
                  lc.find("adv")!=std::string::npos;
    if (!(hasNUS||isVesc)) return;                       // only real boards in the list
    for (int i=0;i<gScanCount;i++) if (gScanList[i].addr==dev->getAddress()) return;
    if (gScanCount<MAX_SCAN_RESULTS)
      gScanList[gScanCount++]={ dev->getAddress(), nm.empty()?"(no name)":nm, hasNUS };
  }
};
static ScanCB gScanCB;

// ── VESC response parsers (trimmed to what the HUD displays) ──────────────────
static void parseValues(const uint8_t* p,int len){
  if (len<50 || p[0]!=CMD_GET_VALUES) return;
  gLastValRx=millis();
  gV.temp_fet=rdI16(p,1)/10.f;
  gV.temp_mot=rdI16(p,3)/10.f;
  gV.curr_mot=rdI32(p,5)/100.f;
  gV.duty_pct=rdI16(p,21)/10.f;
  float rpm=(float)rdI32(p,23);
  gV.voltage=rdI16(p,27)/10.f;
  if (len>=54) gV.fault=p[53];
  const float circ_km=(float)M_PI*gWheelMm/1e6f;      // per-board (see applyBoardCal)
  gV.speed_kmh=fabsf(rpm)/gPolePairs*circ_km*60.f;
  gV.valid=true;
}
static void parseBms(const uint8_t* p,int len){
  int ind=1;
  auto f32=[&](float sc)->float{ if (ind+4>len){ ind+=4; return 0; }
    int32_t v=((int32_t)p[ind]<<24)|((int32_t)p[ind+1]<<16)|((int32_t)p[ind+2]<<8)|(int32_t)p[ind+3];
    ind+=4; return v/sc; };
  auto f16=[&](float sc)->float{ if (ind+2>len){ ind+=2; return 0; }
    int16_t v=(int16_t)(((uint16_t)p[ind]<<8)|p[ind+1]); ind+=2; return v/sc; };
  float vtot=f32(1e6f);
  if (vtot<20.f || vtot>130.f){ gBms.valid=false; return; }   // reject corrupt frame
  gBms.vTot=vtot;
  f32(1e6f);                        // v_charge
  gBms.iIn=f32(1e6f);
  f32(1e6f);                        // i_in_ic
  f32(1e3f);                        // ah_cnt
  f32(1e3f);                        // wh_cnt
  if (ind>=len) return;
  int cn=p[ind++];
  if (cn<0 || cn>36) return;
  gBms.cellNum=cn;
  for (int i=0;i<cn;i++) gBms.cell[i]=f16(1e3f);
  for (int i=0;i<cn;i++){ gBms.bal[i]=(ind<len)?(p[ind]!=0):false; ind++; }
  if (ind<len){
    int tn=p[ind++]; if (tn<0||tn>16) tn=0; gBms.tempNum=tn;
    float maxT=-1000.f;
    for (int i=0;i<tn;i++){ float t=f16(1e2f); gBms.temp[i]=t; if (t>maxT) maxT=t; }
    float ic=f16(1e2f); if (ic>0.f&&ic<120.f&&ic>maxT) maxT=ic;
    f16(1e2f); f16(1e2f);
    float mc=f16(1e2f); if (mc>0.f&&mc<120.f&&mc>maxT) maxT=mc;
    if (maxT>-100.f) gV.temp_bat=maxT;
  }
  gBms.valid=(cn>0);
  if (cn>0) gV.bms=true;
}

// ── Direct-BLE SOC / alert / packet build (mirrors the dashboard) ─────────────
static float packVoltageBle(){
  return (gBms.valid && gBms.vTot>20.f && gBms.vTot<130.f) ? gBms.vTot : gV.voltage;
}
static float batterySocBle(){   // 0..1, or -1 if no reading — voltage-based (see dashboard)
  int cells=(gBms.valid && gBms.cellNum>=4 && gBms.cellNum<=32)?gBms.cellNum:N_CELLS_DEF;
  float vmin=cells*3.0f, vmax=cells*4.2f;
  float pv=packVoltageBle();
  if ((gV.valid||gBms.valid) && pv>1.f) return constrain((pv-vmin)/(vmax-vmin),0.f,1.f);
  return -1.f;
}
static uint8_t computeAlertBle(){
  switch (gV.fault){
    case 1: return 2; case 2: return 3; case 3: return 4; case 4: return 1;
    case 5: return 6; case 6: return 8; default: if (gV.fault!=0) return 12;
  }
  if (gV.temp_fet>=85) return 6;  if (gV.temp_fet>=72) return 5;
  if (gV.temp_mot>=90) return 8;  if (gV.temp_mot>=78) return 7;
  if (gV.bms && gV.temp_bat>=55) return 9;
  int cells=(gBms.valid && gBms.cellNum>=4 && gBms.cellNum<=32)?gBms.cellNum:N_CELLS_DEF;
  float vc=packVoltageBle()/cells;
  if (vc>2.5f && vc<3.00f) return 11;
  if (vc>2.5f && vc<3.25f) return 10;
  return 0;
}
// Refloat ALLDATA mode 1 — the only place the footpad ADCs are published.
// GET_VALUES does not carry them, which is why the direct-BLE path had no footpad
// state at all until now. Mode 1 is 35 B; mode 2 only adds distance and temps we
// already read from GET_VALUES.
static void vescSendAllData(){
  if (!gCharRx || !gBleOk) return;
  uint8_t pay[8]; int plen=0;
  if (gCanId>=0){ pay[plen++]=CMD_FORWARD_CAN; pay[plen++]=(uint8_t)gCanId; }
  pay[plen++]=CMD_CUSTOM_APP_DATA; pay[plen++]=REFLOAT_MAGIC;
  pay[plen++]=REFLOAT_ALLDATA;     pay[plen++]=1;
  uint8_t pkt[16]; int len=buildPkt(pkt,pay,plen);
  gCharRx->writeValue(pkt,len,!gUseWriteNoResp);
}
// Offsets verified against refloat/src/main.c cmd_send_all_data(): the reply is
// wrapped as COMM_CUSTOM_APP_DATA, so every index is that function's buffer
// position + 1. p[3]==69 marks a fault frame with the fields zeroed.
static void parseAllData(const uint8_t* p, int len){
  if (len < 35 || p[1]!=REFLOAT_MAGIC || p[2]!=REFLOAT_ALLDATA) return;
  if (p[3]==69) return;
  gV.adc1 = p[12]/50.0f;
  gV.adc2 = p[13]/50.0f;
  gV.allRx = millis();
}

static void packFromBle(hud_pkt_t& p){
  p.magic=PKT_MAGIC; p.ver=1; p.board_id=1;
  uint8_t fl=0;
  if (gV.curr_mot<-8.f) fl|=0x01;
  if (gV.adc1>0.25f)    fl|=0x02;
  if (gV.adc2>0.25f)    fl|=0x04;
  if (gBleOk && gV.valid) fl|=0x08;      // the HUD itself holds the board link
  p.flags=fl;
  float soc=batterySocBle();
  p.batt_pct=(uint8_t)(soc<0.f?0:constrain((int)(soc*100.f),0,100));
  p.duty_limit=80;
  p.motor_temp=(uint8_t)constrain((int)gV.temp_mot,0,200);
  p.batt_temp =(uint8_t)constrain((int)(gV.bms?gV.temp_bat:0.f),0,200);
  p.fet_temp  =(uint8_t)constrain((int)gV.temp_fet,0,200);
  p.gps_sats=0;                        // GPS lives on the Cardputer — not available direct
  p.cells=(uint8_t)((gBms.valid&&gBms.cellNum>0)?gBms.cellNum:N_CELLS_DEF);
  p.bright=40;
  p.speed_x10=(int16_t)(gV.speed_kmh*10);
  p.duty_x10 =(int16_t)(gV.duty_pct*10);
  p.pack_v_x10=(uint16_t)constrain((int)(packVoltageBle()*10),0,65535);
  p.alert=gV.valid?computeAlertBle():0;
  p.seq=0;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Source / UI state machine
// ═════════════════════════════════════════════════════════════════════════════
enum Source { SRC_ESPNOW, SRC_BLE };
enum Ui     { UI_SELECT, UI_CONNECTING, UI_RUN };
static Source gSrc = SRC_ESPNOW;
static Ui     gUi  = UI_RUN;
static int    gSelIdx = 0;
static uint32_t gLastValMs = 0, gLastBmsMs = 0, gLastAllMs = 0;

// screen pages: short-click cycles; long-press cycles brightness (6 levels)
enum { PG_SPEED, PG_BATT, PG_BATTV, PG_CELLV, PG_TEMP, PG_BTEMP, PG_FTEMP, PG_DUTY, PG_GPS, PG_COUNT };
static int gPage = PG_SPEED;
static const uint8_t PX_LEVELS[6]  = {  3, 10, 25, 55, 110, 200 };   // LED panel
static const uint8_t LCD_LEVELS[6] = { 20, 50, 90,140, 200, 255 };   // screen backlight
static int gBrightIdx = 2;

static char gLastStr[12] = ""; static int gLastPg = -1; static bool gLastLink = false; static bool gLastPushFlip = false;
static long gLastLedSig = -999;   // LED panel state hash — redraw only on change

// ── LED panel drawing ────────────────────────────────────────────────────────
static uint16_t xy(int x, int y){              // x:0..7  y:0..15 (0=top)
  if (gLedFlip){ x = 7 - x; y = 15 - y; }
  int panel = (y < 8) ? PANEL_TOP : PANEL_BOTTOM;
  int ry    = (y < 8) ? y : (y - 8);
  return panel*64 + ry*8 + x;
}
static void vbarC(int x0,int x1,float frac,uint32_t c){
  int h = (int)roundf(constrain(frac,0.f,1.f)*16);
  for (int y=0;y<16;y++){
    int lvl = 15 - y; uint32_t cc = (lvl < h) ? c : 0;
    for (int x=x0;x<=x1;x++) px.setPixelColor(xy(x,y), cc);
  }
}
static void drawIcon(const char* const* ic, uint32_t c){
  for (int r=0;r<8;r++) for (int col=0;col<16;col++)
    if (ic[r][col]=='#'){
      int cx = ICON_FLIP_X ? 7-r   : r;
      int cy = ICON_FLIP_Y ? 15-col: col;
      px.setPixelColor(xy(cx,cy), c);
    }
}
// Searching animation — the SAME four bars that later show speed, duty and
// battery, running a travelling wave. There is no separate boot intro: the only
// time the HUD holds you up is while it is actually looking for a board, and this
// is what "looking" looks like. The instant it connects, the bars become the
// gauge, so nothing on the panel is decoration.
static void searchAnim(uint32_t t){
  const int x0[4]={0,3,6,7}, x1[4]={1,4,6,7};
  const uint32_t col[4]={ px.Color(0,150,255), px.Color(255,150,0),
                          px.Color(0,200,0),   px.Color(255,80,0) };
  px.clear();
  for (int b=0;b<4;b++){
    // phase-shifted sine per bar → the wave visibly travels across the panel
    float ph = (t/90.0f) - b*0.7f;
    float f  = 0.20f + 0.80f * (0.5f * (1.0f + sinf(ph)));
    vbarC(x0[b], x1[b], f, col[b]);
  }
  px.show();
}
static void onRecv(const esp_now_recv_info_t*, const uint8_t* data, int len){
  if (len == (int)sizeof(hud_pkt_t) && data[0] == PKT_MAGIC){
    memcpy((void*)&gPkt, data, sizeof(hud_pkt_t));
    gLastRx = millis(); gRxCount++;
  }
}

// ── LCD screens ──────────────────────────────────────────────────────────────
static void pushScreen(){                         // blit buffer to LCD, rotated 180° when flipped
  if (gFlip) scr.pushRotated(180);
  else       scr.pushSprite(0, 0);
}
static void drawScreen(bool link, const hud_pkt_t& p){
  const char* lab; char v[12];
  float spd = p.speed_x10/10.0f, duty = p.duty_x10/10.0f, packv = p.pack_v_x10/10.0f;
  switch (gPage){
    case PG_BATT:  lab="BATTERY %"; snprintf(v,sizeof(v),"%d", p.batt_pct); break;
    case PG_BATTV: lab="BATT V";    snprintf(v,sizeof(v),"%.1f", packv); break;
    case PG_CELLV: lab="CELL V";    snprintf(v,sizeof(v),"%.2f", p.cells? packv/p.cells : 0.f); break;
    case PG_TEMP:  lab="MOTOR \xB0""C"; snprintf(v,sizeof(v),"%d", p.motor_temp); break;
    case PG_BTEMP: lab="BATT \xB0""C";  snprintf(v,sizeof(v),"%d", p.batt_temp); break;
    case PG_FTEMP: lab="CTRL \xB0""C";  snprintf(v,sizeof(v),"%d", p.fet_temp); break;
    case PG_DUTY:  lab="DUTY %";    snprintf(v,sizeof(v),"%d", (int)roundf(duty)); break;
    case PG_GPS:   lab="GPS sat";   snprintf(v,sizeof(v),"%d", p.gps_sats); break;
    default:       lab="SPEED km/h"; snprintf(v,sizeof(v),"%d", (int)roundf(spd));
  }
  if (!link) strcpy(v, "--");

  // Three status dots, drawn on EVERY page. They answer the two questions you
  // cannot afford to guess at while riding: where is my data coming from, and are
  // both pads reading. Folded into the redraw guard below — the screen only
  // repaints on change, so without this the dots would freeze until the number
  // happened to move.
  //   dot 1  source: GREEN Cardputer has the board · YELLOW Cardputer here but no
  //          board · BLUE HUD holds the board itself · RED nothing
  //   dot 2/3  footpads left/right: GREEN pressed, RED released
  uint16_t dotSrc;
  if (gSrc == SRC_BLE)              dotSrc = (link ? TFT_BLUE : TFT_RED);
  else if (!link)                   dotSrc = TFT_RED;          // no fresh ESP-NOW
  else if (p.flags & 0x08)          dotSrc = TFT_GREEN;        // Cardputer + board
  else                              dotSrc = TFT_YELLOW;       // Cardputer, no board
  bool fp1 = link && (p.flags & 0x02), fp2 = link && (p.flags & 0x04);
  uint8_t dotSig = (uint8_t)((dotSrc==TFT_GREEN?1:dotSrc==TFT_YELLOW?2:dotSrc==TFT_BLUE?3:0)
                             | (fp1?0x10:0) | (fp2?0x20:0));

  static uint8_t gLastDotSig = 0xFF;
  if (gPage==gLastPg && link==gLastLink && dotSig==gLastDotSig && strcmp(v,gLastStr)==0){
    if (gFlip != gLastPushFlip){ pushScreen(); gLastPushFlip=gFlip; } return; }
  gLastPg=gPage; gLastLink=link; gLastDotSig=dotSig; strncpy(gLastStr, v, sizeof(gLastStr)-1);

  const int W = scr.width(), H = scr.height(), BAR = 28;
  scr.fillScreen(TFT_BLACK);
  scr.setTextDatum(middle_center);
  scr.setFont(&fonts::Font0); scr.setTextSize(2); scr.setTextColor(TFT_WHITE);
  scr.drawString(link?lab:"NO LINK", W/2, BAR/2 - 1);
  scr.drawFastHLine(0, BAR-1, W, TFT_WHITE);
  scr.setFont(&fonts::Font7); scr.setTextColor(TFT_WHITE);
  int s = 8;
  for (; s > 1; s--){ scr.setTextSize(s); if (scr.textWidth(v) <= W-6 && scr.fontHeight() <= H-BAR-6) break; }
  scr.setTextSize(s); scr.setTextDatum(middle_center);
  scr.drawString(v, W/2, BAR + (H-BAR)/2);

  // Bottom edge, centred: source · pad L · pad R. Small enough to stay out of the
  // way of the big number, low enough to sit in peripheral vision rather than
  // competing with the reading you actually came to the screen for.
  const int DR = 4, GAP = 14, DY = H - DR - 3;
  scr.fillCircle(W/2 - GAP, DY, DR, dotSrc);
  scr.fillCircle(W/2,       DY, DR, fp1 ? TFT_GREEN : TFT_RED);
  scr.fillCircle(W/2 + GAP, DY, DR, fp2 ? TFT_GREEN : TFT_RED);

  pushScreen(); gLastPushFlip = gFlip;
}
static void drawAlertScreen(const AlertDef& ad){
  uint16_t col = ad.red ? TFT_RED : (uint16_t)scr.color565(255,150,0);
  scr.fillScreen(TFT_BLACK);
  scr.setTextDatum(middle_center); scr.setTextColor(col);
  scr.setFont(&fonts::Font0); scr.setTextSize(2);
  scr.drawString("! ALERT", scr.width()/2, 20);
  scr.setTextSize(3);
  if (scr.textWidth(ad.name) > scr.width()-4) scr.setTextSize(2);
  scr.drawString(ad.name, scr.width()/2, scr.height()/2 + 10);
  pushScreen();
}
static void drawBanner(const char* msg, uint16_t col){
  scr.fillScreen(TFT_BLACK);
  scr.setTextDatum(middle_center); scr.setTextColor(col);
  scr.setFont(&fonts::Font0); scr.setTextSize(2);
  scr.drawString(msg, scr.width()/2, scr.height()/2);
  pushScreen();
}
static void drawSelect(){
  scr.fillScreen(TFT_BLACK);
  scr.setTextDatum(top_left); scr.setFont(&fonts::Font0); scr.setTextSize(1);
  scr.setTextColor(TFT_CYAN); scr.drawString("PICK BOARD", 2, 2);
  if (gScanCount==0){
    scr.setTextColor(scr.color565(255,150,0));
    scr.drawString("none found", 2, 40); scr.drawString("click=rescan", 2, 54);
  }
  for (int i=0;i<gScanCount;i++){
    char line[24];
    snprintf(line,sizeof(line),"%c%.13s", i==gSelIdx?'>':' ', gScanList[i].name.c_str());
    scr.setTextColor(i==gSelIdx?TFT_GREEN:TFT_WHITE);
    scr.drawString(line, 2, 18+i*13);
  }
  scr.setTextColor(scr.color565(120,120,120));
  scr.drawString("clk=next", 2, 108);
  scr.drawString("hold=OK",  2, 118);
  pushScreen();
}

// ═════════════════════════════════════════════════════════════════════════════
//  BLE connect / scan / auto-connect (ported + trimmed from the dashboard)
// ═════════════════════════════════════════════════════════════════════════════
static bool bleConnect(const NimBLEAddress& addr){
  if (!gBleClient){ gBleClient=NimBLEDevice::createClient(); gBleClient->setClientCallbacks(&gClientCB,false); }
  else if (gBleClient->isConnected()){ gBleClient->disconnect(); delay(300); }

  drawBanner("CONNECT", TFT_WHITE);
  if (!gBleClient->connect(addr)){ Serial.println("[BLE] connect fail"); return false; }
  gBleClient->updateConnParams(6,12,0,400);              // fast interval → low-latency telemetry
  if (!gBleClient->discoverAttributes()){ gBleClient->disconnect(); return false; }
  auto* svc=gBleClient->getService(NUS_SVC_UUID);
  if (!svc){ gBleClient->disconnect(); Serial.println("[BLE] no NUS"); return false; }

  gCharRx=nullptr; gCharTx=nullptr;                      // find chars by PROPERTY, not UUID
  for (auto& ch : svc->getCharacteristics(true)){
    if (!gCharTx && (ch->canNotify()||ch->canIndicate())) gCharTx=ch;
    if (!gCharRx && (ch->canWrite()||ch->canWriteNoResponse())) gCharRx=ch;
  }
  if (!gCharRx || !gCharTx){ gBleClient->disconnect(); Serial.println("[BLE] no RX/TX char"); return false; }
  bool useNotify=gCharTx->canNotify();
  if (!gCharTx->subscribe(useNotify,notifyCallback)){ gBleClient->disconnect(); return false; }
  gUseWriteNoResp=gCharRx->canWriteNoResponse();

  gAccLen=0; gRxReady=false; gCanId=-1; gCanDiscovered=false; gBleOk=true;

  // FW_VERSION handshake first (required by the VESC Express bridge)
  vescSend(CMD_FW_VERSION);
  uint32_t t0=millis(); bool fw=false;
  while (!fw && millis()-t0<3000){
    if (gRxReady){ gRxReady=false; const uint8_t* pay; int pl;
      if (unpackPkt(gRxBuf,gRxLen,&pay,&pl) && pay[0]==CMD_FW_VERSION) fw=true; }
    delay(30);
  }
  // PING_CAN discovery → wrap all further commands to the motor controller id
  gRxReady=false; vescSend(CMD_PING_CAN);
  uint32_t tc=millis();
  while (!gCanDiscovered && millis()-tc<3500){
    if (gRxReady){ gRxReady=false; const uint8_t* pay; int pl;
      if (unpackPkt(gRxBuf,gRxLen,&pay,&pl) && pay[0]==CMD_PING_CAN){
        if (pl>=2) gCanId=pay[1]; gCanDiscovered=true; } }
    delay(30);
  }
  Serial.printf("[BLE] connected, can=%d\n", gCanId);

  // remember this board for next boot
  const char* nm="board";
  for (int i=0;i<gScanCount;i++) if (gScanList[i].addr==addr && gScanList[i].name.size()>0) nm=gScanList[i].name.c_str();
  applyBoardCal(nm);                                   // pick wheel/poles for this board
  { Preferences pr; pr.begin("vesc",false);
    pr.putString("lastmac",addr.toString().c_str()); pr.putString("lastname",nm); pr.end(); }

  gLastValMs=0; gLastBmsMs=0; gLastAllMs=0; gLastPg=-1;
  return true;
}
// Scan in short slices so the bars keep moving. getResults() blocks for its whole
// timeout, so one 5 s call would freeze the panel exactly when the rider is
// waiting and wondering whether the thing is alive. Slices continue the same scan
// (is_continue=true), so results accumulate as if it were one call.
static void scanSliced(int totalMs){
  NimBLEScan* scan=NimBLEDevice::getScan();
  scan->setScanCallbacks(&gScanCB,true);
  scan->setActiveScan(true); scan->setInterval(100); scan->setWindow(99);
  uint32_t t0=millis(); bool cont=false;
  while ((int)(millis()-t0) < totalMs){
    scan->getResults(150, cont); cont=true;
    searchAnim(millis());
  }
}
static void bleScan(){                          // populate the filtered picker list
  gScanCount=0; gSelIdx=0;
  drawBanner("SCAN", TFT_CYAN);
  scanSliced(5000);
}
static bool bleTryConnectSaved(){               // scan for the remembered board and connect
  Preferences pr; pr.begin("vesc",true); String mac=pr.getString("lastmac",""); pr.end();
  if (mac.length()<5) return false;
  drawBanner("FIND BRD", TFT_WHITE);
  gScanCount=0;
  // Stop the moment the remembered board shows up — no reason to sit out the full
  // window once we have what we came for.
  NimBLEScan* scan=NimBLEDevice::getScan();
  scan->setScanCallbacks(&gScanCB,true);
  scan->setActiveScan(true); scan->setInterval(100); scan->setWindow(99);
  uint32_t t0=millis(); bool cont=false;
  while (millis()-t0 < 5000){
    scan->getResults(150, cont); cont=true;
    searchAnim(millis());
    for (int i=0;i<gScanCount;i++)
      if (strcmp(gScanList[i].addr.toString().c_str(),mac.c_str())==0){
        scan->stop();
        return bleConnect(gScanList[i].addr);
      }
  }
  return false;
}
static void enterSelect(){ gSrc=SRC_BLE; gUi=UI_SELECT; bleScan(); drawSelect(); }
static void startBleMode(){                      // called when there's no Cardputer
  gSrc=SRC_BLE; gBleOk=false;
  if (bleTryConnectSaved()){ gUi=UI_RUN; return; }
  enterSelect();
}

// ═════════════════════════════════════════════════════════════════════════════
void setup(){
  Serial.begin(115200);
  auto cfg = M5.config();
  M5.begin(cfg);                               // board power + internal I2C
  M5.Imu.begin();                              // IMU for auto-orient (needs explicit begin here)
  lcd.init(); lcd.setRotation(SCREEN_ROT);
  lcd.setPivot(lcd.width()/2.0f, lcd.height()/2.0f);
  scr.createSprite(lcd.width(), lcd.height());
  scr.setPivot(scr.width()/2.0f, scr.height()/2.0f);
  lcdBacklight(LCD_LEVELS[gBrightIdx]);
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextDatum(middle_center); lcd.setTextColor(TFT_WHITE);
  lcd.setFont(&fonts::Font0); lcd.setTextSize(3);
  lcd.drawString(HUD_NAME, lcd.width()/2, lcd.height()/2 - 8);
  lcd.setTextSize(1); lcd.setTextColor(lcd.color565(120,120,120));
  lcd.drawString("PEV heads-up", lcd.width()/2, lcd.height()/2 + 22);

  px.begin(); px.setBrightness(20); px.clear(); px.show();

  // Radios: WiFi STA + ESP-NOW RX (Cardputer link) AND NimBLE central (direct link).
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  if (esp_now_init() == ESP_OK) esp_now_register_recv_cb(onRecv);
  NimBLEDevice::init(HUD_NAME);

  // No boot intro. The old 2.4 s animation was also doing real work — it was the
  // window in which a Cardputer broadcast could arrive — so it cannot simply be
  // deleted, only shortened to what the job actually needs. The Cardputer sends at
  // 30 Hz, so a packet is due every 33 ms; 400 ms is ~12 chances to hear one.
  // Exit early the moment one lands, which is the common case.
  uint32_t t0 = millis();
  while (millis() - t0 < 400){
    if (gLastRx) break;                         // Cardputer heard — stop waiting
    delay(10);
  }

  // Source decision: Cardputer broadcasting → ESP-NOW; else go direct BLE.
  if (gLastRx && millis() - gLastRx < 1500){
    gSrc=SRC_ESPNOW; gUi=UI_RUN; Serial.println("[SRC] ESP-NOW (Cardputer present)");
  } else { Serial.println("[SRC] no Cardputer -> direct BLE"); startBleMode(); }
}

// ── contextual button ────────────────────────────────────────────────────────
static void handleButton(float speed_now){
  if (gUi==UI_SELECT){
    if (M5.BtnA.wasHold()){                                  // confirm selection
      if (gScanCount>0){
        gUi=UI_CONNECTING;
        if (bleConnect(gScanList[gSelIdx].addr)){ gSrc=SRC_BLE; gUi=UI_RUN; }
        else enterSelect();
      } else enterSelect();                                  // empty → rescan
    } else if (M5.BtnA.wasSingleClicked()){
      if (gScanCount>0){ gSelIdx=(gSelIdx+1)%gScanCount; drawSelect(); }
      else enterSelect();                                    // empty → rescan
    }
    return;
  }
  // UI_RUN / UI_CONNECTING
  if (M5.BtnA.wasHold()){                                    // brightness
    gBrightIdx=(gBrightIdx+1)%6; lcdBacklight(LCD_LEVELS[gBrightIdx]);
  } else if (M5.BtnA.wasSingleClicked()){                    // cycle page
    gPage=(gPage+1)%PG_COUNT; gLastPg=-1;
  } else if (M5.BtnA.wasDecideClickCount() && M5.BtnA.getClickCount()==3){
    if (gSrc==SRC_BLE && speed_now < 0.5f){                  // re-pick board, only stopped
      if (gBleClient && gBleClient->isConnected()) gBleClient->disconnect();
      gBleOk=false; enterSelect();
    }
  }
}

void loop(){
  M5.update();

  // auto-orient: physical 180° flip → gravity moves from ay≈+1 to ≈-1 (filter + hysteresis)
  { static uint32_t t=0; static float ayf=0.98f;
    if (millis()-t > 150){ t=millis(); float ax,ay,az; M5.Imu.getAccel(&ax,&ay,&az);
      ayf = ayf*0.85f + ay*0.15f;
      bool nf = (ayf < -0.35f) ? true : (ayf > 0.35f) ? false : gFlip;
      if (nf != gFlip){ gFlip=nf; gLedFlip=!gFlip; }
    } }

  bool espFresh = (millis() - gLastRx < 1000);

  // ── source arbitration ──────────────────────────────────────────────────
  // Prefer the Cardputer whenever it's broadcasting: yield the single BLE link.
  if (espFresh && gSrc != SRC_ESPNOW){
    if (gBleClient && gBleClient->isConnected()) gBleClient->disconnect();
    gBleOk=false; gSrc=SRC_ESPNOW; gUi=UI_RUN; gLastPg=-1;
    Serial.println("[SRC] -> ESP-NOW (Cardputer back)");
  }
  // Cardputer gone for >3s → fall back to direct BLE.
  if (gSrc==SRC_ESPNOW && millis()-gLastRx > 3000){
    Serial.println("[SRC] Cardputer lost -> direct BLE");
    startBleMode();
  }
  // Direct-BLE link dropped mid-ride → retry the saved board (throttled).
  if (gSrc==SRC_BLE && gUi==UI_RUN && !gBleOk && !espFresh){
    static uint32_t lastRetry=0;
    if (millis()-lastRetry > 4000){ lastRetry=millis();
      gUi=UI_CONNECTING;
      if (bleTryConnectSaved()) gUi=UI_RUN; else enterSelect();
    }
  }

  float speed_now = (gSrc==SRC_BLE) ? gV.speed_kmh : gPkt.speed_x10/10.0f;
  handleButton(speed_now);

  // ── board picker screen ─────────────────────────────────────────────────
  if (gUi==UI_SELECT){
    static uint32_t sledsig=-1;                 // clear LEDs once on entry
    if (sledsig != 1){ sledsig=1; gLastLedSig=-999; px.clear(); px.show(); }
    delay(10);
    return;
  }

  // ── direct-BLE polling + dispatch ───────────────────────────────────────
  if (gSrc==SRC_BLE && gUi==UI_RUN && gBleOk){
    uint32_t now=millis();
    if (now-gLastValMs >= 40){ gLastValMs=now; vescSend(CMD_GET_VALUES); }
    if (now-gLastBmsMs >= 300){ gLastBmsMs=now; vescSend(CMD_BMS_GET_VALUES); vescSendRawCmd(CMD_BMS_GET_VALUES); }
    if (now-gLastAllMs >= 100){ gLastAllMs=now; vescSendAllData(); }   // footpads
  }
  if (gRxReady){
    gRxReady=false; const uint8_t* pay; int plen;
    if (unpackPkt(gRxBuf,gRxLen,&pay,&plen)){
      switch (pay[0]){
        case CMD_GET_VALUES:     parseValues(pay,plen); break;
        case CMD_BMS_GET_VALUES: parseBms(pay,plen);    break;
        case CMD_CUSTOM_APP_DATA: parseAllData(pay,plen); break;
      }
    }
  }

  // ── build the render packet + link status for the current source ─────────
  hud_pkt_t p;
  bool link;
  if (gSrc==SRC_BLE){
    packFromBle(p);
    link = gBleOk && gV.valid && (millis()-gLastValRx < 1500);
    if (gUi==UI_CONNECTING){ drawBanner("CONNECT", TFT_WHITE); delay(10); return; }
  } else {
    memcpy(&p, (const void*)&gPkt, sizeof(p));
    link = espFresh;
  }

  uint32_t now = millis();

  // ── alert handling: show icon+name 4s, repeat every 10s while it persists ──
  static uint8_t gAlertCode = 0; static uint32_t gAlertStart = 0; static uint8_t gAlarmShown = 0;
  uint8_t a = link ? p.alert : 0;
  if (a >= ALERT_N) a = 12;
  if (a == 0) gAlertCode = 0;
  else if (a != gAlertCode){ gAlertCode = a; gAlertStart = now; }
  else if (now - gAlertStart >= 10000){ gAlertStart = now; }
  bool alarm = gAlertCode && (now - gAlertStart < 4000);

  // ── LED panel: alert icon OR 4 vertical bars — redraw only when it changes ──
  bool blinkPhase = (now / 150) & 1;
  float spd = p.speed_x10/10.0f, duty = p.duty_x10/10.0f;
  bool dutyBlink = (duty >= 90.0f) && blinkPhase;
  long ledSig;
  if (alarm)      ledSig = 1000000L + gAlertCode * 10 + gBrightIdx + gFlip * 100;
  else if (link){
    int sh = (int)roundf(constrain(spd/45.0f,0.f,1.f)*16);
    int dh = dutyBlink ? 0 : (int)roundf(constrain(duty/95.0f,0.f,1.f)*16);
    int bh = (int)roundf(constrain(p.batt_pct/100.0f,0.f,1.f)*16);
    int th = (int)roundf(constrain(p.motor_temp/90.0f,0.f,1.f)*16);
    ledSig = sh + dh*17L + bh*289L + th*4913L + (long)gBrightIdx*83521L + (long)gFlip*400000L;
  } else ledSig = -2 - gFlip;

  if (ledSig != gLastLedSig){
    gLastLedSig = ledSig;
    px.setBrightness(PX_LEVELS[gBrightIdx]);
    px.clear();
    if (alarm){
      const AlertDef& ad = ALERTS[gAlertCode];
      drawIcon(ICONS[ad.icon], ad.red ? px.Color(255,0,0) : px.Color(255,150,0));
    } else if (link){
      float k = constrain((spd-25.0f)/15.0f, 0.f, 1.f);
      vbarC(0,1, spd/45.0f, px.Color((int)(k*255),(int)((1-k)*150),(int)((1-k)*255)));
      uint32_t dc = duty<70 ? px.Color(255,170,0) : duty<80 ? px.Color(255,90,0) : px.Color(255,0,0);
      vbarC(3,4, dutyBlink ? 0.0f : duty/95.0f, dc);
      uint32_t bc = p.batt_pct<25 ? px.Color(255,0,0) : p.batt_pct<50 ? px.Color(255,150,0) : px.Color(0,200,0);
      vbarC(6,6, p.batt_pct/100.0f, bc);
      int mt = p.motor_temp;
      uint32_t tc = mt<55 ? px.Color(0,200,0) : mt<70 ? px.Color(200,200,0) : mt<82 ? px.Color(255,120,0) : px.Color(255,0,0);
      vbarC(7,7, mt/90.0f, tc);
    } else {
      for (int y=6;y<=9;y++) px.setPixelColor(xy(3,y), px.Color(60,0,0));
    }
    px.show();
  }

  // ── AtomS3R screen ──
  if (alarm){
    if (gAlertCode != gAlarmShown){ drawAlertScreen(ALERTS[gAlertCode]); gAlarmShown = gAlertCode; }
  } else {
    if (gAlarmShown){ gLastPg = -1; gAlarmShown = 0; }
    drawScreen(link, p);
  }

  delay(2);
}
