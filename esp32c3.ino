#include <NimBLEDevice.h>

// ======================
// 1. KONFIGURASI
// ======================
static NimBLEAddress serverAddress("84:D3:B2:FA:9D:F7", BLE_ADDR_PUBLIC);

#define SERVICE_UUID        "0000ffe0-0000-1000-8000-00805f9b34fb"
#define CHARACTERISTIC_UUID "0000ffe1-0000-1000-8000-00805f9b34fb"

#define PIN_LED      8  
#define BTN_FORWARD  2
#define BTN_BACKWARD 3
#define BTN_LEFT     4
#define BTN_RIGHT    5

NimBLERemoteCharacteristic* pRemoteChar = nullptr;
NimBLEClient* pClient = nullptr;
bool connected = false;

// Timing Variable (Non-Blocking)
unsigned long lastReconTime = 0;
unsigned long lastBlinkTime = 0;
unsigned long lastLoopTime = 0; // Pengganti delay(15)
bool ledState = false;

// Fungsi Kirim Command
void sendCommand(String cmd) {
    if (connected && pRemoteChar) {
        String payload = cmd + "\n"; 
        // Menggunakan true (with response) agar stabil
        bool success = pRemoteChar->writeValue(payload.c_str(), payload.length(), true);
        
        if(success) {
            Serial.print("[TX SUCCESS] -> "); 
            Serial.println(cmd);
        }
    }
}

// Handler Koneksi
class MyClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) { 
        Serial.println(">>> Terkoneksi!"); 
    }
    void onDisconnect(NimBLEClient* pClient) {
        connected = false;
        digitalWrite(PIN_LED, HIGH); // MATI (Active Low)
        Serial.println(">>> Terputus!");
    }
};

bool connectToServer() {
    Serial.println(">>> Mencoba menyambung...");
    if (!pClient) {
        pClient = NimBLEDevice::createClient();
        pClient->setClientCallbacks(new MyClientCallbacks(), false);
    }

    if (!pClient->connect(serverAddress)) return false;

    NimBLERemoteService* pService = pClient->getService(SERVICE_UUID);
    if (!pService) { pClient->disconnect(); return false; }

    pRemoteChar = pService->getCharacteristic(CHARACTERISTIC_UUID);
    if (!pRemoteChar) { pClient->disconnect(); return false; }

    connected = true;
    digitalWrite(PIN_LED, LOW); // NYALA (Active Low)
    Serial.println(">>> SIAP!");
    return true;
}

void setup() {
    Serial.begin(115200);
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH); // Pastikan mati saat start (Active Low)

    pinMode(BTN_FORWARD, INPUT_PULLUP);
    pinMode(BTN_BACKWARD, INPUT_PULLUP);
    pinMode(BTN_LEFT, INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);

    NimBLEDevice::init("ESP32_Remote");
    connectToServer();
}

void loop() {
    unsigned long currentMillis = millis();

    // --- 1. LOGIKA LED & RECONNECT (NON-BLOCKING) ---
    if (!connected) {
        if (currentMillis - lastBlinkTime >= 400) {
            lastBlinkTime = currentMillis;
            ledState = !ledState;
            digitalWrite(PIN_LED, ledState ? LOW : HIGH); // Blink Active Low
        }

        if (currentMillis - lastReconTime >= 5000) {
            lastReconTime = currentMillis;
            connectToServer();
        }
    } else {
        digitalWrite(PIN_LED, LOW); // Nyala terus (Active Low)
    }

    // --- 2. LOGIKA PEMBATAS KECEPATAN LOOP (Ganti delay 15ms) ---
    // Program hanya akan membaca tombol & serial setiap 15ms
    if (currentMillis - lastLoopTime >= 15) {
        lastLoopTime = currentMillis;

        // --- SERIAL DEBUGGER ---
        if (Serial.available() > 0) {
            char c = Serial.read();
            if (c == 'w') sendCommand("maju");
            if (c == 's') sendCommand("mundur");
            if (c == 'a') sendCommand("kiri");
            if (c == 'd') sendCommand("kanan");
            if (c == 'x') { sendCommand("release_drive"); sendCommand("release_steer"); }
        }

        // --- TOMBOL FISIK ---
        static bool lF = false, lB = false, lL = false, lR = false;
        bool f = digitalRead(BTN_FORWARD) == LOW;
        bool b = digitalRead(BTN_BACKWARD) == LOW;
        bool l = digitalRead(BTN_LEFT) == LOW;
        bool r = digitalRead(BTN_RIGHT) == LOW;

        if (f != lF) { sendCommand(f ? "maju" : "release_drive"); lF = f; }
        if (b != lB) { sendCommand(b ? "mundur" : "release_drive"); lB = b; }
        if (l != lL) { sendCommand(l ? "kiri" : "release_steer"); lL = l; }
        if (r != lR) { sendCommand(r ? "kanan" : "release_steer"); lR = r; }
    }
}
