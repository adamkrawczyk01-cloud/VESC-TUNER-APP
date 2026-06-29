// ============================================================================
//  Helmet HUD — AtomS3R receiver  (helmet HUD)
//
//  ESP-NOW RX -> 2x M5 Puzzle 8x8 STACKED (8 wide x 16 tall) = 4 vertical bars
//  (speed / duty / battery / motor temp). AtomS3R 0.85" screen = big SPEED number.
//
//  The AtomS3R LCD is driven by a HAND-BUILT LGFX panel (GC9107 on SPI3) + LP5562
//  backlight over I2C, because M5GFX's panel-id autodetect fails on core 3.3.7/IDF5
//  (M5.Display comes up 0x0). M5.begin() is still used for board power + internal I2C.
//
//  Wiring: Puzzle#1 IN -> AtomS3R Grove (data G2=GPIO2); Puzzle#1 OUT -> Puzzle#2 IN.
//  Board:  M5AtomS3R   Flash: FQBN=m5stack:esp32:m5stack_atoms3r cardputer/flash.sh helmet/atoms3r_hud
// ============================================================================
#include <M5Unified.h>
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_GC9A01.hpp>   // exposes lgfx::Panel_GC9107 (not public via M5GFX.h)
#include <WiFi.h>
#include <esp_now.h>
#include <Adafruit_NeoPixel.h>

#define LED_PIN      2
#define NUM_LEDS     128
#define PANEL_TOP    0
#define PANEL_BOTTOM 1
#define PKT_MAGIC    0xBE
#define SCREEN_ROT   3     // USB-C on the right, content upright
#define HUD_NAME     "VHUD"   // boot branding

// ESP-NOW packet (defined up here so Arduino's auto-prototypes see the type)
typedef struct __attribute__((packed)) {
  uint8_t  magic, ver, board_id, flags;      // flags bit0=braking (reserved: rear light)
  uint8_t  batt_pct, duty_limit, motor_temp;
  uint8_t  bright;
  int16_t  speed_x10, duty_x10;
  uint8_t  seq;
} hud_pkt_t;

// ---- hand-built AtomS3R LCD (GC9107 on SPI3); backlight done separately ----
class LGFX_AtomS3R : public lgfx::LGFX_Device {
  lgfx::Panel_GC9107 _gc;
  lgfx::Bus_SPI      _spibus;
public:
  LGFX_AtomS3R(){
    { auto c = _spibus.config(); c.spi_host=SPI3_HOST; c.spi_mode=0;
      c.freq_write=40000000; c.freq_read=16000000; c.spi_3wire=true;
      c.pin_sclk=15; c.pin_mosi=21; c.pin_miso=-1; c.pin_dc=42;
      _spibus.config(c); _gc.setBus(&_spibus); }
    { auto c = _gc.config(); c.pin_cs=14; c.pin_rst=48;
      c.panel_width=128; c.panel_height=128; c.offset_y=32;
      c.readable=false; c.bus_shared=false; c.invert=true; _gc.config(c); }
    setPanel(&_gc);
  }
};
static LGFX_AtomS3R lcd;
// LP5562 backlight via M5Unified's internal I2C (addr 0x30)
static void lcdBacklight(uint8_t b){
  M5.In_I2C.writeRegister8(0x30, 0x00, 0x40, 400000);
  M5.In_I2C.writeRegister8(0x30, 0x08, 0x01, 400000);
  M5.In_I2C.writeRegister8(0x30, 0x70, 0x00, 400000);
  M5.In_I2C.writeRegister8(0x30, 0x0e, b,    400000);
}

Adafruit_NeoPixel px(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

static volatile hud_pkt_t gPkt;
static volatile uint32_t  gLastRx = 0, gRxCount = 0;

static uint16_t xy(int x, int y){              // x:0..7  y:0..15 (0=top)
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
// boot animation: gauge sweep on the bars + white flash (car-dash style)
static void bootAnim(){
  for (int h=0; h<=16; h++){
    px.clear();
    vbarC(0,1, h/16.0f, px.Color(0,150,255));
    vbarC(3,4, h/16.0f, px.Color(255,150,0));
    vbarC(6,6, h/16.0f, px.Color(0,200,0));
    vbarC(7,7, h/16.0f, px.Color(255,80,0));
    px.show(); delay(30);
  }
  for (int i=0;i<NUM_LEDS;i++) px.setPixelColor(i, px.Color(150,150,150));
  px.show(); delay(140);
  px.clear(); px.show();
}
static void onRecv(const esp_now_recv_info_t*, const uint8_t* data, int len){
  if (len == (int)sizeof(hud_pkt_t) && data[0] == PKT_MAGIC){
    memcpy((void*)&gPkt, data, sizeof(hud_pkt_t));
    gLastRx = millis(); gRxCount++;
  }
}

// screen pages cycled by the AtomS3R button
enum { PG_SPEED, PG_BATT, PG_TEMP, PG_DUTY, PG_COUNT };
static int gPage = PG_SPEED;
static long gLastKey = -1;

static void drawScreen(bool link, const hud_pkt_t& p){
  const char* lab; int val;
  float spd = p.speed_x10/10.0f, duty = p.duty_x10/10.0f;
  switch (gPage){
    case PG_BATT: lab="BATTERY %"; val=p.batt_pct; break;
    case PG_TEMP: lab="MOTOR \xB0""C"; val=p.motor_temp; break;
    case PG_DUTY: lab="DUTY %";    val=(int)roundf(duty); break;
    default:      lab="SPEED km/h"; val=(int)roundf(spd);
  }
  long key = (long)gPage*100000 + (link?1:0)*10000 + (link?val:-1);
  if (key == gLastKey) return;                 // redraw only on change
  gLastKey = key;

  const int W = lcd.width(), H = lcd.height(), BAR = 28;
  lcd.fillScreen(TFT_BLACK);                          // black background
  // top label "belka": white text + white divider line
  lcd.setTextDatum(middle_center);
  lcd.setFont(&fonts::Font0); lcd.setTextSize(2); lcd.setTextColor(TFT_WHITE);
  lcd.drawString(link?lab:"NO LINK", W/2, BAR/2 - 1);
  lcd.drawFastHLine(0, BAR-1, W, TFT_WHITE);
  // big WHITE number, auto-sized to fill the whole area below the bar
  char b[8]; snprintf(b,sizeof(b), link?"%d":"--", val);
  lcd.setFont(&fonts::Font7); lcd.setTextColor(TFT_WHITE);
  int s = 8;
  for (; s > 1; s--){ lcd.setTextSize(s); if (lcd.textWidth(b) <= W-6 && lcd.fontHeight() <= H-BAR-6) break; }
  lcd.setTextSize(s); lcd.setTextDatum(middle_center);
  lcd.drawString(b, W/2, BAR + (H-BAR)/2);
}

void setup(){
  auto cfg = M5.config();
  M5.begin(cfg);                               // board power + internal I2C
  lcd.init(); lcd.setRotation(SCREEN_ROT);      // USB-C on the right
  lcdBacklight(180);
  // boot splash: VHUD name
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextDatum(middle_center); lcd.setTextColor(TFT_WHITE);
  lcd.setFont(&fonts::Font0); lcd.setTextSize(3);
  lcd.drawString(HUD_NAME, lcd.width()/2, lcd.height()/2 - 8);
  lcd.setTextSize(1); lcd.setTextColor(lcd.color565(120,120,120));
  lcd.drawString("PEV heads-up", lcd.width()/2, lcd.height()/2 + 22);

  px.begin(); px.setBrightness(20); px.clear(); px.show();
  bootAnim();                                   // gauge sweep on the LEDs

  WiFi.mode(WIFI_STA); WiFi.disconnect();
  if (esp_now_init() == ESP_OK) esp_now_register_recv_cb(onRecv);
  delay(300);
}

void loop(){
  M5.update();
  if (M5.BtnA.wasPressed()){ gPage = (gPage+1) % PG_COUNT; gLastKey = -1; }  // cycle HUD value
  uint32_t now = millis();
  bool link = (now - gLastRx) < 1000;
  hud_pkt_t p; memcpy(&p, (const void*)&gPkt, sizeof(p));

  // ---- LED panel: 4 vertical bars ----
  px.setBrightness(link && p.bright ? p.bright : 20);
  px.clear();
  if (link){
    float spd = p.speed_x10/10.0f, duty = p.duty_x10/10.0f;
    float k = constrain((spd-25.0f)/15.0f, 0.f, 1.f);
    vbarC(0,1, spd/45.0f, px.Color((int)(k*255),(int)((1-k)*150),(int)((1-k)*255)));     // speed blue->red
    uint32_t dc = duty<70 ? px.Color(255,170,0) : duty<80 ? px.Color(255,90,0) : px.Color(255,0,0);
    vbarC(3,4, duty/95.0f, dc);                                                          // duty
    uint32_t bc = p.batt_pct<25 ? px.Color(255,0,0) : p.batt_pct<50 ? px.Color(255,150,0) : px.Color(0,200,0);
    vbarC(6,6, p.batt_pct/100.0f, bc);                                                   // battery 1px
    int mt = p.motor_temp;
    uint32_t tc = mt<55 ? px.Color(0,200,0) : mt<70 ? px.Color(200,200,0) : mt<82 ? px.Color(255,120,0) : px.Color(255,0,0);
    vbarC(7,7, mt/90.0f, tc);                                                            // motor temp 1px
  } else {
    for (int y=6;y<=9;y++) px.setPixelColor(xy(3,y), px.Color(60,0,0));
  }
  px.show();

  // ---- AtomS3R screen: selected metric (button cycles speed/batt/temp/duty) ----
  drawScreen(link, p);

  delay(8);
}
