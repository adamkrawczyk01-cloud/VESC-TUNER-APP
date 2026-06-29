// ============================================================================
//  Helmet HUD — AtomS3R receiver  (helmet HUD, Phase 0)
//
//  ESP-NOW RX -> 2x M5 Puzzle 8x8 STACKED (8 wide x 16 tall) as 3 VERTICAL bars
//  (speed / duty / battery) + AtomS3R screen shows speed and flashes RED on brake.
//
//  Wiring: Puzzle#1 IN -> AtomS3R Grove (data G2=GPIO2); Puzzle#1 OUT -> Puzzle#2 IN.
//  Board:  M5AtomS3R   Flash: FQBN=m5stack:esp32:m5stack_atoms3r cardputer/flash.sh helmet/atoms3r_hud
// ============================================================================
#include <M5Unified.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Adafruit_NeoPixel.h>

#define LED_PIN      2       // AtomS3R Grove G2 -> Puzzle data
#define NUM_LEDS     128     // 2x 8x8 stacked = 8 wide x 16 tall
#define SERPENTINE   0       // set 1 if a panel's rows zig-zag
#define PANEL_TOP    0       // which px-block is the TOP 8x8: 0 (px0-63) or 1 (px64-127)
#define PANEL_BOTTOM 1
#define PKT_MAGIC    0xBE

Adafruit_NeoPixel px(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

typedef struct __attribute__((packed)) {
  uint8_t  magic, ver, board_id, flags;     // flags bit0=braking, bit1=footpad on
  uint8_t  batt_pct, duty_limit, motor_temp; // motor_temp deg C
  uint8_t  bright;                           // LED panel brightness
  int16_t  speed_x10, duty_x10;
  uint8_t  seq;
} hud_pkt_t;

static volatile hud_pkt_t gPkt;
static volatile uint32_t  gLastRx = 0, gRxCount = 0;

// 8 wide x 16 tall, two stacked 8x8.  x:0..7  y:0..15 (0 = top)
static uint16_t xy(int x, int y){
  int panel = (y < 8) ? PANEL_TOP : PANEL_BOTTOM;
  int ry    = (y < 8) ? y : (y - 8);
  if (SERPENTINE && (ry & 1)) x = 7 - x;
  return panel*64 + ry*8 + x;
}
// solid vertical bar in cols [x0,x1], value 0..1, grows from BOTTOM up, one colour
static void vbarC(int x0,int x1,float frac,uint32_t c){
  int h = (int)roundf(constrain(frac,0.f,1.f)*16);
  for (int y=0;y<16;y++){
    int lvl = 15 - y;                         // 0=bottom .. 15=top
    uint32_t cc = (lvl < h) ? c : 0;
    for (int x=x0;x<=x1;x++) px.setPixelColor(xy(x,y), cc);
  }
}
static void onRecv(const esp_now_recv_info_t*, const uint8_t* data, int len){
  if (len == (int)sizeof(hud_pkt_t) && data[0] == PKT_MAGIC){
    memcpy((void*)&gPkt, data, sizeof(hud_pkt_t));
    gLastRx = millis(); gRxCount++;
  }
}

void setup(){
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setBrightness(255);              // backlight max
  M5.Display.setRotation(0);
  M5.Display.setTextDatum(middle_center);
  // boot splash — if you see this, the LCD works
  M5.Display.fillScreen(BLUE);
  M5.Display.setTextColor(WHITE); M5.Display.setTextSize(2);
  M5.Display.drawString("HUD", M5.Display.width()/2, M5.Display.height()/2);
  delay(900);

  px.begin(); px.setBrightness(45); px.clear(); px.show();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() == ESP_OK) esp_now_register_recv_cb(onRecv);
}

void loop(){
  M5.update();
  uint32_t now = millis();
  bool link = (now - gLastRx) < 1000;
  hud_pkt_t p; memcpy(&p, (const void*)&gPkt, sizeof(p));

  // ---- LED panel: 4 vertical bars (speed / duty / batt / motor temp) ----
  px.setBrightness(link && p.bright ? p.bright : 20);   // brightness from packet ('d' on the keyboard)
  px.clear();
  if (link){
    float spd = p.speed_x10/10.0f, duty = p.duty_x10/10.0f;
    // SPEED cols 0-1, scale 0..45, colour blue->red across 25..40 km/h
    float k = constrain((spd-25.0f)/15.0f, 0.f, 1.f);
    uint32_t sc = px.Color((int)(k*255), (int)((1-k)*150), (int)((1-k)*255));
    vbarC(0,1, spd/45.0f, sc);
    // DUTY cols 3-4, scale 0..95, amber<70 / orange 70-80 / red >80
    uint32_t dc = duty<70 ? px.Color(255,170,0) : duty<80 ? px.Color(255,90,0) : px.Color(255,0,0);
    vbarC(3,4, duty/95.0f, dc);
    // BATTERY col 6 (1px): red<25 / amber<50 / green
    uint32_t bc = p.batt_pct<25 ? px.Color(255,0,0) : p.batt_pct<50 ? px.Color(255,150,0) : px.Color(0,200,0);
    vbarC(6,6, p.batt_pct/100.0f, bc);
    // MOTOR TEMP col 7 (1px): FC-style green->red, scale 0..90 C
    int mt = p.motor_temp;
    uint32_t tc = mt<55 ? px.Color(0,200,0) : mt<70 ? px.Color(200,200,0) : mt<82 ? px.Color(255,120,0) : px.Color(255,0,0);
    vbarC(7,7, mt/90.0f, tc);
  } else {
    for (int y=6;y<=9;y++) px.setPixelColor(xy(3,y), px.Color(60,0,0));                  // dim red = no link
  }
  px.show();

  // ---- AtomS3R screen ----
  bool braking = link && (p.flags & 0x01);
  int W=M5.Display.width(), H=M5.Display.height();
  if (braking){
    bool on = (now/120)&1;                                 // ~4Hz flash
    M5.Display.fillScreen(on ? RED : (uint16_t)0x6000);
    M5.Display.setTextColor(WHITE); M5.Display.setTextSize(2);
    M5.Display.drawString("BRAKE", W/2, H/2);
  } else {
    M5.Display.fillScreen(link ? BLACK : (uint16_t)0x2000);
    M5.Display.setTextColor(link?GREEN:DARKGREY); M5.Display.setTextSize(5);
    char b[8]; snprintf(b,sizeof(b), link?"%d":"--", (int)roundf(p.speed_x10/10.0f));
    M5.Display.drawString(b, W/2, H/2 - 8);
    M5.Display.setTextSize(1); M5.Display.setTextColor(DARKGREY);
    char s[20]; snprintf(s,sizeof(s), link?"seq %u":"NO LINK", (unsigned)p.seq);
    M5.Display.drawString(s, W/2, H-10);
  }
  delay(8);
}
