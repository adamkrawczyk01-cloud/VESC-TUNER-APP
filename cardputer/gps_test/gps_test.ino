// ─────────────────────────────────────────────────────────────────────────────
//  GPS TEST — does the Cardputer see the M5 GPS Unit on the Grove port?
//
//  Auto-probes every plausible wiring: G1/G2 as RX × {9600, 38400} baud, counts
//  bytes + looks for NMEA ('$'). Whatever it finds is shown ON SCREEN (and on
//  serial @115200). Tells you exactly which pins/baud to bake into the firmware.
//
//  Flash:  cardputer/flash.sh cardputer/gps_test
//  Back:   cardputer/flash.sh
// ─────────────────────────────────────────────────────────────────────────────
#include <M5Cardputer.h>

static M5Canvas canvas(&M5Cardputer.Display);
#define DW 240
#define DH 135

struct Combo { int rx, tx, baud; };
static Combo COMBOS[] = { {1,2,9600}, {1,2,38400}, {1,2,57600}, {1,2,115200} };
static const int NCOMBO = 4;

static int   gBytes[NCOMBO] = {0};
static bool  gNmea[NCOMBO]  = {false};
static int   gWinner = -1;
static char  gLastLine[96] = "";
static int   gSats = 0;

static void show(const char* status) {
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(TFT_CYAN); canvas.setTextSize(2);
    canvas.drawString("GPS TEST", 6, 4);
    canvas.setTextSize(1); canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString(status, 6, 26);
    for (int i = 0; i < NCOMBO; i++) {
        char b[48];
        snprintf(b, sizeof(b), "G%d/G%d @%-5d : %4d B %s",
                 COMBOS[i].rx, COMBOS[i].tx, COMBOS[i].baud, gBytes[i],
                 gNmea[i] ? "NMEA!" : (gBytes[i] ? "bytes" : "--"));
        canvas.setTextColor(gNmea[i] ? TFT_GREEN : (gBytes[i] ? TFT_YELLOW : TFT_DARKGREY));
        canvas.drawString(b, 6, 42 + i * 13);
    }
    if (gWinner >= 0) {
        char b[64];
        canvas.setTextColor(TFT_GREEN);
        snprintf(b, sizeof(b), "USE: G%d=RX G%d=TX @%d  sats=%d",
                 COMBOS[gWinner].rx, COMBOS[gWinner].tx, COMBOS[gWinner].baud, gSats);
        canvas.drawString(b, 6, 100);
        canvas.setTextColor(TFT_WHITE);
        canvas.drawString(gLastLine, 6, 116);
    }
    canvas.pushSprite(0, 0);
}

// listen on one combo for `ms`, return bytes seen, set sawNmea
static int probe(Combo c, int ms, bool& sawNmea) {
    HardwareSerial s(1);
    s.begin(c.baud, SERIAL_8N1, c.rx, c.tx);
    int n = 0; sawNmea = false;
    uint32_t t0 = millis();
    while (millis() - t0 < (uint32_t)ms) {
        while (s.available()) { char ch = s.read(); n++; if (ch == '$') sawNmea = true; }
        delay(2);
    }
    s.end();
    return n;
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(120);
    canvas.createSprite(DW, DH);
    Serial.begin(115200); delay(400);
    Serial.println("\n[GPS-TEST] probing Grove for NMEA...");

    show("probing... (give it ~12s)");
    for (int i = 0; i < NCOMBO; i++) {
        bool nm = false;
        gBytes[i] = probe(COMBOS[i], 3000, nm);
        gNmea[i]  = nm;
        Serial.printf("[GPS-TEST] G%d/G%d @%d -> %d bytes, NMEA=%d\n",
                      COMBOS[i].rx, COMBOS[i].tx, COMBOS[i].baud, gBytes[i], nm);
        show("probing...");
    }
    // winner: prefer one that saw NMEA, else most bytes
    for (int i = 0; i < NCOMBO; i++) if (gNmea[i]) { gWinner = i; break; }
    if (gWinner < 0) { int best = 0; for (int i = 1; i < NCOMBO; i++) if (gBytes[i] > gBytes[best]) best = i;
                       if (gBytes[best] > 0) gWinner = best; }
    show(gWinner >= 0 ? "FOUND — live feed below" : "NO DATA on any combo");
}

void loop() {
    M5Cardputer.update();
    if (gWinner < 0) { delay(200); return; }   // nothing to read

    static HardwareSerial* s = nullptr;
    static char line[96]; static int len = 0;
    if (!s) { s = new HardwareSerial(1); s->begin(COMBOS[gWinner].baud, SERIAL_8N1, COMBOS[gWinner].rx, COMBOS[gWinner].tx); }

    while (s->available()) {
        char c = s->read();
        if (c == '\n') {
            line[len] = 0;
            if (len > 5 && line[0] == '$') {
                strncpy(gLastLine, line, sizeof(gLastLine) - 1);
                Serial.println(line);
                // sats from GGA field 7
                if (strstr(line, "GGA")) {
                    int comma = 0; for (char* p = line; *p; p++) { if (*p == ',') { comma++; if (comma == 7) { gSats = atoi(p + 1); break; } } }
                }
            }
            len = 0;
        } else if (c != '\r' && len < (int)sizeof(line) - 1) line[len++] = c;
    }
    static uint32_t last = 0;
    if (millis() - last > 500) { last = millis(); show("FOUND — live feed below"); }
}
