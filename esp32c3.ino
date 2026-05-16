#include <NimBLEDevice.h>

// ======================
// 1. KONFIGURASI BLE
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

// ======================
// 2. VARIABEL TIMING & STATE
// ======================
unsigned long lastReconTime = 0;
unsigned long lastBlinkTime = 0;
unsigned long lastLoopTime = 0;
unsigned long lastPingTime = 0; // Timer untuk Keep-Alive
bool ledState = false;

// State awal sekarang menggunakan protokol huruf tunggal (S = Stop Drive, C = Center Steer)
String lastDriveState = "S"; 
String lastSteerState = "C";

bool vFwd = false, vBwd = false, vLft = false, vRgt = false;

// ======================
// 3. DEBOUNCE TOMBOL
// ======================
struct Button {
    uint8_t pin;
    bool state;
    bool lastReading;
    unsigned long lastDebounceTime;
};

Button btnFwd = {BTN_FORWARD, false, false, 0};
Button btnBwd = {BTN_BACKWARD, false, false, 0};
Button btnLft = {BTN_LEFT, false, false, 0};
Button btnRgt = {BTN_RIGHT, false, false, 0};

void readButton(Button &b, unsigned long currentMillis) {
    bool reading = (digitalRead(b.pin) == LOW);
    if (reading != b.lastReading) b.lastDebounceTime = currentMillis;
    if ((currentMillis - b.lastDebounceTime) > 50) {
        if (reading != b.state) b.state = reading;
    }
    b.lastReading = reading;
}

// ======================
// 4. FUNGSI KIRIM
// ======================
void sendCommand(String cmd) {
    if (connected && pRemoteChar) {
        String payload = cmd + "\n"; 
        bool success = pRemoteChar->writeValue(payload.c_str(), payload.length(), true);
        
        // Sembunyikan log T (Telemetry) dan P (Ping) agar Serial Monitor tidak penuh
        if(success && cmd != "T" && cmd != "P") { 
            Serial.print("[TX] -> "); 
            Serial.println(cmd);
        }
    }
}

// ======================
// 5. CALLBACK & KONEKSI
// ======================
class MyClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) { Serial.println(">>> Terkoneksi!"); }
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
    Serial.println(">>> BOOT!");
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH); // Mati di awal

    pinMode(BTN_FORWARD, INPUT_PULLUP);
    pinMode(BTN_BACKWARD, INPUT_PULLUP);
    pinMode(BTN_LEFT, INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);

    NimBLEDevice::init("ESP32_RC_PRO");
    connectToServer();
}

void loop() {
    unsigned long currentMillis = millis();

    // --- 1. LOGIKA LED & RECONNECT ---
    if (!connected) {
        if (currentMillis - lastBlinkTime >= 400) {
            lastBlinkTime = currentMillis;
            ledState = !ledState;
            digitalWrite(PIN_LED, ledState ? LOW : HIGH);
        }
        if (currentMillis - lastReconTime >= 5000) {
            lastReconTime = currentMillis;
            connectToServer();
        }
    } else {
        digitalWrite(PIN_LED, LOW); // Nyala Solid
    }

    // --- 2. LOGIKA KONTROL UTAMA (15ms) ---
    if (currentMillis - lastLoopTime >= 15) {
        lastLoopTime = currentMillis;

        // Baca Input Virtual dari Serial Debugger
        while (Serial.available() > 0) {
            char c = Serial.read();
            if (c == 'W') vFwd = true; else if (c == 'w') vFwd = false;
            if (c == 'S') vBwd = true; else if (c == 's') vBwd = false;
            if (c == 'A') vLft = true; else if (c == 'a') vLft = false;
            if (c == 'D') vRgt = true; else if (c == 'd') vRgt = false;
            if (c == 'x') { vFwd=vBwd=vLft=vRgt=false; }
        }

        // Baca Input Tombol Fisik (Debounced)
        readButton(btnFwd, currentMillis);
        readButton(btnBwd, currentMillis);
        readButton(btnLft, currentMillis);
        readButton(btnRgt, currentMillis);

        bool isFwd = btnFwd.state || vFwd;
        bool isBwd = btnBwd.state || vBwd;
        bool isLft = btnLft.state || vLft;
        bool isRgt = btnRgt.state || vRgt;

        // ==========================================
        // 🛡️ OPPOSITE CANCEL MODE RESOLUTION
        // ==========================================
        // Default adalah S (Stop) dan C (Center)
        String targetDrive = "S"; 
        String targetSteer = "C";

        // Filter Drive
        if (isFwd && !isBwd) targetDrive = "F";      // Maju
        else if (isBwd && !isFwd) targetDrive = "B"; // Mundur

        // Filter Steer
        if (isLft && !isRgt) targetSteer = "L";      // Kiri
        else if (isRgt && !isLft) targetSteer = "R"; // Kanan

        // ==========================================
        // KIRIM HANYA JIKA BERUBAH (Anti-Spam)
        // ==========================================
        if (targetDrive != lastDriveState) {
            sendCommand(targetDrive);
            lastDriveState = targetDrive;
        }

        if (targetSteer != lastSteerState) {
            sendCommand(targetSteer);
            lastSteerState = targetSteer;
        }
    }

    // --- 3. KEEP ALIVE / PING (Mencegah Stall Timeout STM32) ---
    // Di STM32 kamu, "T" akan membalas telemetry, atau "P" hanya untuk reset timer timeout.
    // Kita gunakan "T" agar sejalan dengan STM32-mu.
    if (connected && (currentMillis - lastPingTime >= 300)) {
        lastPingTime = currentMillis;
        sendCommand("T"); 
    }
}
