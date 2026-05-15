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
bool ledState = false;

// State Terakhir yang dikirim ke Mobil (Agar tidak spam/jitter)
String lastDriveState = "release_drive";
String lastSteerState = "release_steer";

// State Virtual dari Serial (HTML Simulator)
bool vFwd = false, vBwd = false, vLft = false, vRgt = false;

// ======================
// 3. STRUKTUR DEBOUNCE TOMBOL FISIK
// ======================
struct Button {
    uint8_t pin;
    bool state;           // State yang sudah stabil (ter-debounce)
    bool lastReading;     // State mentah sebelumnya
    unsigned long lastDebounceTime;
};

Button btnFwd = {BTN_FORWARD, false, false, 0};
Button btnBwd = {BTN_BACKWARD, false, false, 0};
Button btnLft = {BTN_LEFT, false, false, 0};
Button btnRgt = {BTN_RIGHT, false, false, 0};

// Fungsi pembacaan tombol anti-jitter (50ms debounce)
void readButton(Button &b, unsigned long currentMillis) {
    bool reading = (digitalRead(b.pin) == LOW); // LOW = Ditekan
    
    // Jika ada perubahan state mentah (getaran tombol)
    if (reading != b.lastReading) {
        b.lastDebounceTime = currentMillis;
    }
    
    // Jika state mentah sudah stabil melewati batas waktu debounce (50ms)
    if ((currentMillis - b.lastDebounceTime) > 50) {
        if (reading != b.state) {
            b.state = reading; // Update state yang sesungguhnya
        }
    }
    b.lastReading = reading;
}

// ======================
// 4. FUNGSI KIRIM COMMAND
// ======================
void sendCommand(String cmd) {
    if (connected && pRemoteChar) {
        String payload = cmd + "\n"; 
        // Menggunakan true (with response) agar stabil untuk STM32
        bool success = pRemoteChar->writeValue(payload.c_str(), payload.length(), true);
        
        if(success) {
            Serial.print("[TX SUCCESS] -> "); 
            Serial.println(cmd);
        }
    }
}

// ======================
// 5. CALLBACK & KONEKSI BLE
// ======================
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

// ======================
// SETUP
// ======================
void setup() {
    Serial.begin(115200);
    
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH); // Pastikan mati saat start (Active Low)

    pinMode(BTN_FORWARD, INPUT_PULLUP);
    pinMode(BTN_BACKWARD, INPUT_PULLUP);
    pinMode(BTN_LEFT, INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);

    NimBLEDevice::init("ESP32_RC_PRO");
    connectToServer();
}

// ======================
// MAIN LOOP
// ======================
void loop() {
    unsigned long currentMillis = millis();

    // --- 1. LOGIKA LED & RECONNECT ---
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
        digitalWrite(PIN_LED, LOW); // Nyala solid (Active Low)
    }

    // --- 2. LOGIKA UTAMA (Berjalan setiap 15ms agar STM32 tidak crash) ---
    if (currentMillis - lastLoopTime >= 15) {
        lastLoopTime = currentMillis;

        // A. BACA SERIAL (Virtual Buttons dari HTML)
        while (Serial.available() > 0) {
            char c = Serial.read();
            if (c == 'W') vFwd = true; else if (c == 'w') vFwd = false;
            if (c == 'S') vBwd = true; else if (c == 's') vBwd = false;
            if (c == 'A') vLft = true; else if (c == 'a') vLft = false;
            if (c == 'D') vRgt = true; else if (c == 'd') vRgt = false;
            if (c == 'x') { vFwd=vBwd=vLft=vRgt=false; } // Emergency stop
        }

        // B. BACA TOMBOL FISIK (Dengan Debounce)
        readButton(btnFwd, currentMillis);
        readButton(btnBwd, currentMillis);
        readButton(btnLft, currentMillis);
        readButton(btnRgt, currentMillis);

        // C. GABUNGKAN INPUT (Fisik + Virtual saling mendukung)
        bool isFwd = btnFwd.state || vFwd;
        bool isBwd = btnBwd.state || vBwd;
        bool isLft = btnLft.state || vLft;
        bool isRgt = btnRgt.state || vRgt;

        // ==========================================
        // D. 🛡️ OPPOSITE CANCEL MODE RESOLUTION 🛡️
        // ==========================================
        String targetDrive = "release_drive";
        String targetSteer = "release_steer";

        // Resolusi Drive (Maju/Mundur)
        if (isFwd && !isBwd) {
            targetDrive = "maju";
        } else if (isBwd && !isFwd) {
            targetDrive = "mundur";
        } // else (jika ditekan bersamaan atau dilepas semua) -> tetap "release_drive"

        // Resolusi Steer (Kiri/Kanan)
        if (isLft && !isRgt) {
            targetSteer = "kiri";
        } else if (isRgt && !isLft) {
            targetSteer = "kanan";
        } // else (jika ditekan bersamaan atau dilepas semua) -> tetap "release_steer"

        // ==========================================
        // E. KIRIM JIKA ADA PERUBAHAN (Anti Spam)
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
}
