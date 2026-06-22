// ─────────────────────────────────────────────────────────────────────────────
//  BLE RELAY — feasibility spike (TODO: BLE proxy, Phase 0)
//
//  Cardputer = BLE central (to the VESC) + BLE peripheral (fake VESC). A phone
//  running Float Control connects to the Cardputer; every byte is shuttled raw
//  in both directions (NO parsing, NO SET originated here — pure passthrough).
//
//  GOAL of this spike: does Float Control connect to the Cardputer-as-VESC while
//  the Cardputer holds the real VESC link, and what THROUGHPUT do we get?
//
//  Chain:  Phone (Float Control) <-BLE-> Cardputer <-BLE-> VESC Express <-CAN-> motor
//
//  Flash this SEPARATE sketch (cardputer/ble_relay/). The main dashboard firmware
//  is untouched. Screen shows VESC / PHONE link + up/down B/s. Serial @115200.
// ─────────────────────────────────────────────────────────────────────────────
#include <M5Cardputer.h>
#include <NimBLEDevice.h>

#define NUS_SVC "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"   // central writes here  (Cardputer/phone -> VESC)
#define NUS_TX  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"   // notify               (VESC -> Cardputer/phone)

// ---- peripheral (the phone connects to this) ----
static NimBLECharacteristic* gPhoneTx = nullptr;   // notify to phone
static volatile bool gPhoneOk = false;

// ---- central (we connect to the real VESC) ----
static NimBLEClient*               gVescClient = nullptr;
static NimBLERemoteCharacteristic* gVescRx     = nullptr;   // write to VESC
static NimBLERemoteCharacteristic* gVescTx     = nullptr;   // notify from VESC
static NimBLEAddress               gVescAddr;
static volatile bool gVescOk = false;
static bool gHaveTarget = false;

// ---- throughput counters ----
static volatile uint32_t gUp = 0, gDown = 0;       // bytes since last sample
static uint32_t gUpRate = 0, gDownRate = 0, gRateMs = 0;

static M5Canvas canvas(&M5Cardputer.Display);
#define DW 240
#define DH 135

// VESC notify -> forward raw to the phone
static void onVescNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    if (gPhoneOk && gPhoneTx) { gPhoneTx->setValue(data, len); gPhoneTx->notify(); gDown += len; }
}

// phone writes -> forward raw to the VESC
class PhoneRxCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        if (gVescOk && gVescRx) {
            std::string v = c->getValue();
            gVescRx->writeValue((uint8_t*)v.data(), v.size(), false);   // no-response for speed
            gUp += v.size();
        }
    }
};

class ServerCB : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo&) override { gPhoneOk = true; }
    void onDisconnect(NimBLEServer* s, NimBLEConnInfo&, int) override {
        gPhoneOk = false; NimBLEDevice::getAdvertising()->start();   // re-advertise
    }
};

class VescCB : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient*) override { gVescOk = true; }
    void onDisconnect(NimBLEClient*, int) override { gVescOk = false; gVescRx = gVescTx = nullptr; }
};

class ScanCB : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* d) override {
        if (!gHaveTarget && d->isAdvertisingService(NimBLEUUID(NUS_SVC))) {
            gVescAddr = d->getAddress(); gHaveTarget = true;
            NimBLEDevice::getScan()->stop();
        }
    }
};

static bool connectVesc() {
    if (!gHaveTarget) return false;
    if (!gVescClient) { gVescClient = NimBLEDevice::createClient(); gVescClient->setClientCallbacks(new VescCB(), false); }
    if (!gVescClient->connect(gVescAddr)) return false;
    gVescClient->exchangeMTU();
    NimBLERemoteService* svc = gVescClient->getService(NUS_SVC);
    if (!svc) { gVescClient->disconnect(); return false; }
    gVescRx = svc->getCharacteristic(NUS_RX);
    gVescTx = svc->getCharacteristic(NUS_TX);
    if (!gVescRx || !gVescTx) { gVescClient->disconnect(); return false; }
    gVescTx->subscribe(true, onVescNotify);
    gVescOk = true;
    return true;
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(120);
    canvas.createSprite(DW, DH);
    Serial.begin(115200); delay(200);
    Serial.println("\n[RELAY] boot");

    NimBLEDevice::init("VESC-Tuner");          // name Float Control will see
    NimBLEDevice::setMTU(517);

    // peripheral: NUS server (the phone connects here)
    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCB());
    NimBLEService* svc = server->createService(NUS_SVC);
    NimBLECharacteristic* rx = svc->createCharacteristic(
        NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new PhoneRxCB());
    gPhoneTx = svc->createCharacteristic(NUS_TX, NIMBLE_PROPERTY::NOTIFY);
    svc->start();
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SVC);
    adv->enableScanResponse(true);
    adv->start();
    Serial.println("[RELAY] advertising NUS (phone can connect)");

    // central: find + connect to the real VESC
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(new ScanCB(), false);
    scan->setActiveScan(true);
    scan->getResults(6000, false);
    if (connectVesc()) Serial.println("[RELAY] VESC connected");
    else               Serial.println("[RELAY] VESC not found — retrying in loop");
}

static void draw() {
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(TFT_CYAN);  canvas.setTextSize(2);
    canvas.drawString("BLE RELAY", 6, 6);
    canvas.setTextSize(1);
    canvas.setTextColor(gVescOk  ? TFT_GREEN : TFT_RED);
    canvas.drawString(gVescOk  ? "VESC   connected" : "VESC   --", 6, 36);
    canvas.setTextColor(gPhoneOk ? TFT_GREEN : TFT_DARKGREY);
    canvas.drawString(gPhoneOk ? "PHONE  connected" : "PHONE  waiting...", 6, 50);
    canvas.setTextColor(TFT_WHITE); canvas.setTextSize(2);
    char b[40];
    snprintf(b, sizeof(b), "down %4u B/s", (unsigned)gDownRate); canvas.drawString(b, 6, 76);
    snprintf(b, sizeof(b), "up   %4u B/s", (unsigned)gUpRate);   canvas.drawString(b, 6, 98);
    canvas.setTextSize(1); canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString("Float Control -> VESC-Tuner", 6, 122);
    canvas.pushSprite(0, 0);
}

void loop() {
    M5Cardputer.update();
    uint32_t now = millis();
    if (now - gRateMs >= 1000) {
        gUpRate = gUp; gDownRate = gDown; gUp = 0; gDown = 0; gRateMs = now;
        Serial.printf("[RELAY] vesc=%d phone=%d  down=%u up=%u B/s\n", gVescOk, gPhoneOk, gDownRate, gUpRate);
        if (!gVescOk) { if (!gHaveTarget) { NimBLEDevice::getScan()->getResults(2000, false); } connectVesc(); }
        draw();
    }
    delay(5);
}
