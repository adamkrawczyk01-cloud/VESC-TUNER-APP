// ============================================================================
//  ESP-NOW TX test  (helmet HUD, Phase 0 — bench)
//
//  Broadcasts a FAKE telemetry packet (ramping speed/duty/batt + periodic brake)
//  so we can validate the helmet HUD link + Puzzle render + brake flash WITHOUT
//  touching the production dashboard firmware. Flash to the Cardputer, run the
//  AtomS3R receiver, watch the panel. Then re-flash the dashboard: cardputer/flash.sh
//
//  Flash:  cardputer/flash.sh cardputer/espnow_tx_test
// ============================================================================
#include <M5Cardputer.h>
#include <WiFi.h>
#include <esp_now.h>

#define PKT_MAGIC 0xBE
typedef struct __attribute__((packed)) {
  uint8_t  magic, ver, board_id, flags;
  uint8_t  batt_pct, duty_limit;
  int16_t  speed_x10, duty_x10;
  uint8_t  seq;
} hud_pkt_t;

static uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static uint8_t gSeq = 0;
static uint32_t gLastTx = 0;
static M5Canvas cv(&M5Cardputer.Display);

void setup(){
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  cv.createSprite(240,135);
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_now_init();
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = 0; peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void loop(){
  M5Cardputer.update();
  uint32_t now = millis();
  if (now - gLastTx >= 50){            // 20 Hz
    gLastTx = now;
    float t = now/1000.0f;
    hud_pkt_t p;
    p.magic = PKT_MAGIC; p.ver = 1; p.board_id = 1;
    p.batt_pct  = 50 + (int)(40*sinf(t*0.2f));            // 10..90 sweep
    p.duty_limit = 80;
    float spd = 15 + 12*sinf(t*0.5f);                     // 3..27 km/h sweep
    float duty = 40 + 45*sinf(t*0.7f);                    // -5..85 sweep
    p.speed_x10 = (int16_t)(fmaxf(0,spd)*10);
    p.duty_x10  = (int16_t)(fmaxf(0,duty)*10);
    p.flags = ((int)t % 6 < 2) ? 0x01 : 0x00;             // "braking" 2s every 6s
    p.seq = gSeq++;
    esp_now_send(BCAST, (uint8_t*)&p, sizeof(p));

    cv.fillScreen(TFT_BLACK);
    cv.setTextColor(TFT_CYAN); cv.setTextSize(2); cv.setTextDatum(TL_DATUM);
    cv.drawString("ESP-NOW TX test", 6, 6);
    cv.setTextSize(1); cv.setTextColor(TFT_WHITE);
    char b[48];
    snprintf(b,sizeof(b),"seq %u  spd %.0f  duty %.0f", gSeq, spd, duty); cv.drawString(b,6,40);
    snprintf(b,sizeof(b),"batt %d%%  brake %d", p.batt_pct, p.flags&1);   cv.drawString(b,6,56);
    cv.setTextColor(TFT_DARKGREY); cv.drawString("broadcasting to helmet HUD", 6, 116);
    cv.pushSprite(0,0);
  }
  delay(2);
}
