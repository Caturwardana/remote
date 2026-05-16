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
unsigned long lastPingTime = 0; 
bool ledState = false;

String lastDriveState = "S"; 
String lastSteerState = "C";

bool vFwd = false, vBwd = false, vLft = false, vRgt = false;

// 🔥 VARIABEL BARU UNTUK AUTO-RECONNECT
int pingFailCount = 0; 

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
// 4. FUNGSI KIRIM (DENGAN WATCHDOG)
// ======================
void sendCommand(String cmd) {
    if (connected && pRemoteChar) {
        String payload = cmd + "\n"; 
        
        // Wajib 'true' agar kita tau pesannya sampai atau tidak
        bool success = pRemoteChar->writeValue(payload.c_str(), payload.length(), true);
        
        if (!success) {
            // Jika gagal kirim, catat kesalahannya
            pingFailCount++;
            if (pingFailCount >= 3) {
                Serial.println("[SYSTEM] Mobil tidak merespon (Mati/Putus). Memaksa Reconnect!");
                connected = false;
                if (pClient) pClient->disconnect(); // Paksa putus dari sisi ESP32
                pingFailCount = 0; // Reset counter
            }
        } else {
            // Jika sukses kirim, reset counter karena koneksi berarti sehat
            pingFailCount = 0; 
            
            // Print log hanya untuk perintah selain Ping agar rapi
            if(cmd != "P") { 
                Serial.print("[TX] -> "); 
                Serial.println(cmd);
            }
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
        pingFailCount = 0;
        digitalWrite(PIN_LED, HIGH); // MATI (Active Low)
        Serial.println(">>> Terputus (Callback)!");
    }
};

bool connectToServer() {
    Serial.println(">>> Mencoba menyambung...");
    if (!pClient) {
        pClient = NimBLEDevice::createClient();
        pClient->setClientCallbacks(new MyClientCallbacks(), false);
    }
    
    // Pastikan status client bersih sebelum menyambung ulang
    if (pClient->isConnected()) pClient->disconnect();
    
    if (!pClient->connect(serverAddress)) {
        Serial.println("--- Gagal. Mencari ulang dalam 5 detik ---");
        return false;
    }
    
    NimBLERemoteService* pService = pClient->getService(SERVICE_UUID);
    if (!pService) { pClient->disconnect(); return false; }
    
    pRemoteChar = pService->getCharacteristic(CHARACTERISTIC_UUID);
    if (!pRemoteChar) { pClient->disconnect(); return false; }

    connected = true;
    pingFailCount = 0; // Reset watchdog saat sukses konek
    digitalWrite(PIN_LED, LOW); // NYALA (Active Low)
    Serial.println(">>> SIAP! (Ketik w, a, s, d, atau x untuk REM/STOP)");
    return true;
}

void setup() {
    Serial.begin(115200);
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);

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
        // Kedip cepat menandakan sedang mencari koneksi
        if (currentMillis - lastBlinkTime >= 200) {
            lastBlinkTime = currentMillis;
            ledState = !ledState;
            digitalWrite(PIN_LED, ledState ? LOW : HIGH);
        }
        // Coba konek ulang setiap 5 detik
        if (currentMillis - lastReconTime >= 5000) {
            lastReconTime = currentMillis;
            connectToServer();
        }
    } else {
        digitalWrite(PIN_LED, LOW); // Nyala Solid kalau konek
    }

    // --- 2. LOGIKA KONTROL (Setiap 15ms) ---
    if (currentMillis - lastLoopTime >= 15) {
        lastLoopTime = currentMillis;

        while (Serial.available() > 0) {
            char c = Serial.read();
            if (c == 'W') { vFwd = true; vBwd = false; } 
            else if (c == 'w') { vFwd = false; }
            if (c == 'S') { vBwd = true; vFwd = false; } 
            else if (c == 's') { vBwd = false; }
            if (c == 'A') { vLft = true; vRgt = false; } 
            else if (c == 'a') { vLft = false; }
            if (c == 'D') { vRgt = true; vLft = false; } 
            else if (c == 'd') { vRgt = false; }
            if (c == 'x' || c == 'X') { 
                vFwd=vBwd=vLft=vRgt=false; 
                Serial.println("[SYSTEM] X ditekan -> REM DARURAT!");
            } 
        }

        readButton(btnFwd, currentMillis);
        readButton(btnBwd, currentMillis);
        readButton(btnLft, currentMillis);
        readButton(btnRgt, currentMillis);

        bool isFwd = btnFwd.state || vFwd;
        bool isBwd = btnBwd.state || vBwd;
        bool isLft = btnLft.state || vLft;
        bool isRgt = btnRgt.state || vRgt;

        String targetDrive = "S"; 
        String targetSteer = "C"; 

        if (isFwd && !isBwd) targetDrive = "F";      
        else if (isBwd && !isFwd) targetDrive = "B"; 

        if (isLft && !isRgt) targetSteer = "L";      
        else if (isRgt && !isLft) targetSteer = "R"; 

        if (targetDrive != lastDriveState) {
            sendCommand(targetDrive);
            lastDriveState = targetDrive;
        }

        if (targetSteer != lastSteerState) {
            sendCommand(targetSteer);
            lastSteerState = targetSteer;
        }
    }

    // --- 3. HEARTBEAT / WATCHDOG (Setiap 1.5 detik) ---
    if (connected && (currentMillis - lastPingTime >= 1500)) {
        lastPingTime = currentMillis;
        sendCommand("P"); 
    }
}
