// ============================================================================
//  Helmet HUD — AtomS3R receiver  (helmet HUD, Phase 0)
//
//  Receives telemetry over ESP-NOW (broadcast from the board unit) and renders
//  it on 2x M5 Puzzle 8x8 (16x8 WS2812) + flashes the AtomS3R screen RED while
//  braking. Shows NO-LINK if packets stop.
//
//  Wiring: Puzzle #1 Grove IN -> AtomS3R Grove (data = G2 = GPIO2).
//          Puzzle #1 OUT -> Puzzle #2 IN (chain). 5V/GND via Grove.
//  Board:  M5AtomS3R (m5stack:esp32:m5stack_atoms3r)
//  Flash:  cardputer/flash.sh helmet/atoms3r_hud
// ============================================================================
#include <M5Unified.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Adafruit_NeoPixel.h>

#define LED_PIN    2         // AtomS3R Grove G2 -> Puzzle data
#define NUM_LEDS   128       // 2x 8x8
#define SERPENTINE 0         // set 1 if rows zig-zag (adjust after seeing hardware)
#define PKT_MAGIC  0xBE

Adafruit_NeoPixel px(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// shared ESP-NOW packet (must match the board TX)
typedef struct __attribute__((packed)) {
  uint8_t  magic, ver, board_id, flags;   // flags bit0=braking, bit1=footpad on
  uint8_t  batt_pct, duty_limit;
  int16_t  speed_x10, duty_x10;
  uint8_t  seq;
} hud_pkt_t;

static volatile hud_pkt_t gPkt;
static volatile uint32_t  gLastRx = 0, gRxCount = 0;

// 16x8: left 8x8 = px 0..63, right 8x8 = 64..127, row-major per panel
static uint16_t xy(int x, int y){
  int panel = (x < 8) ? 0 : 1, lx = x - panel*8;
  if (SERPENTINE && (y & 1)) lx = 7 - lx;
  return panel*64 + y*8 + lx;
}
static void bar(int y0, int y1, float frac, uint32_t base, uint32_t red, float redFrac){
  int on = (int)roundf(constrain(frac,0.f,1.f) * 16);
  for (int x = 0; x < 16; x++){
    uint32_t c = (x < on) ? ((x >= redFrac*16) ? red : base) : 0;
    for (int y = y0; y <= y1; y++) px.setPixelColor(xy(x,y), c);
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
  M5.Display.setRotation(0);
  M5.Display.setTextDatum(middle_center);
  px.begin(); px.setBrightness(40); px.clear(); px.show();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() == ESP_OK) esp_now_register_recv_cb(onRecv);
}

void loop(){
  M5.update();
  uint32_t now = millis();
  bool link = (now - gLastRx) < 1000;
  hud_pkt_t p; memcpy(&p, (const void*)&gPkt, sizeof(p));

  // --- LED panel ---
  px.clear();
  if (link){
    float spd = p.speed_x10/10.0f, duty = p.duty_x10/10.0f;
    float dlim = p.duty_limit > 0 ? p.duty_limit : 80;
    bar(0,1, spd/35.0f,        px.Color(0,80,160), px.Color(0,160,255), 1.1f);            // speed (no red)
    bar(3,4, duty/100.0f,      px.Color(0,160,0),  px.Color(255,0,0),  (dlim-10)/100.0f); // duty zones
    // battery: low % = red, mid = amber, high = green
    int on = (int)roundf(constrain(p.batt_pct/100.0f,0.f,1.f)*16);
    uint32_t bc = p.batt_pct<25 ? px.Color(255,0,0) : p.batt_pct<50 ? px.Color(255,150,0) : px.Color(0,200,0);
    for(int x=0;x<16;x++){ uint32_t c=(x<on)?bc:0; for(int y=6;y<=7;y++) px.setPixelColor(xy(x,y),c); }
  } else {
    for(int x=0;x<16;x+=2) px.setPixelColor(xy(x,3), px.Color(60,0,0));   // dim red dashes = no link
  }
  px.show();

  // --- AtomS3R screen: RED flash while braking, else speed + link ---
  bool braking = link && (p.flags & 0x01);
  if (braking){
    bool on = (now/120)&1;                                  // ~4Hz flash
    M5.Display.fillScreen(on ? RED : (uint16_t)0x6000);
    M5.Display.setTextColor(WHITE); M5.Display.setTextSize(3);
    M5.Display.drawString("BRAKE", M5.Display.width()/2, M5.Display.height()/2);
  } else {
    M5.Display.fillScreen(link ? BLACK : (uint16_t)0x2000);
    M5.Display.setTextColor(link?GREEN:DARKGREY); M5.Display.setTextSize(4);
    char b[8]; snprintf(b,sizeof(b), link?"%d":"--", (int)roundf(p.speed_x10/10.0f));
    M5.Display.drawString(b, M5.Display.width()/2, M5.Display.height()/2 - 10);
    M5.Display.setTextSize(1); M5.Display.setTextColor(DARKGREY);
    M5.Display.drawString(link?"km/h":"NO LINK", M5.Display.width()/2, M5.Display.height()-12);
  }
  delay(8);
}
