#include <NimBLEDevice.h>

// ======================
// 1. KONFIGURASI BLE
// ======================
static NimBLEAddress serverAddress("84:D3:B2:FA:9D:F7", BLE_ADDR_PUBLIC);

#define SERVICE_UUID        "0000ffe0-0000-1000-8000-00805f9b34fb"
#define CHARACTERISTIC_UUID "0000ffe1-0000-1000-8000-00805f9b34fb"

#define PIN_LED      2    // LED onboard ESP32 biasa
#define BTN_FORWARD  25
#define BTN_BACKWARD 26
#define BTN_LEFT     27
#define BTN_RIGHT    14

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
int pingFailCount = 0; 

// 🔥 VARIABEL MODE SETTING
bool isSettingMode = false;
unsigned long allBtnHoldTimer = 0;
bool allBtnTimingActive = false;

int valSpm = 80;  // Akan di-override oleh get_params dari STM32
int valBel = 150; // Akan di-override oleh get_params dari STM32

bool lastBtnFwd = false;
bool lastBtnBwd = false;
bool lastBtnLft = false;
bool lastBtnRgt = false;

// Buffer untuk menerima data panjang dari STM32 via BLE
String bleBuffer = "";

// ======================
// 3. DEBOUNCE TOMBOL & SERIAL PRINT
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

// Tambah parameter btnName untuk print ke Serial Monitor
void readButton(Button &b, unsigned long currentMillis, const char* btnName) {
    // Membaca tombol (LOW = ditekan karena pakai INPUT_PULLUP)
    bool reading = (digitalRead(b.pin) == LOW); 
    
    if (reading != b.lastReading) {
        b.lastDebounceTime = currentMillis;
    }
    
    if ((currentMillis - b.lastDebounceTime) > 50) {
        if (reading != b.state) {
            b.state = reading;
            
            // --- OUTPUT SERIAL SETIAP ADA PERUBAHAN TOMBOL ---
            if (b.state) {
                Serial.print("[TOMBOL] ");
                Serial.print(btnName);
                Serial.println(" Ditekan! (Pin LOW)");
            } else {
                Serial.print("[TOMBOL] ");
                Serial.print(btnName);
                Serial.println(" Dilepas (Pin HIGH)");
            }
        }
    }
    b.lastReading = reading;
}

// ======================
// 4. FUNGSI KIRIM (DENGAN WATCHDOG)
// ======================
void sendCommand(String cmd) {
    if (connected && pRemoteChar) {
        String payload = cmd + "\n"; 
        bool success = pRemoteChar->writeValue(payload.c_str(), payload.length(), true);
        
        if (!success) {
            pingFailCount++;
            if (pingFailCount >= 3) {
                Serial.println("[SYSTEM] Mobil tidak merespon (Mati/Putus). Memaksa Reconnect!");
                connected = false;
                if (pClient) pClient->disconnect();
                pingFailCount = 0; 
            }
        } else {
            pingFailCount = 0; 
            if(cmd != "P") { 
                Serial.print("[TX] -> "); 
                Serial.println(cmd);
            }
        }
    }
}

// ======================
// 5. CALLBACK TERIMA DATA & KONEKSI
// ======================
void notifyCallback(NimBLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    for (int i = 0; i < length; i++) {
        char c = (char)pData[i];
        if (c == '\n' || c == '\r') {
            if (bleBuffer.length() > 0) {
                int idxSpm = bleBuffer.indexOf("\"spm\":");
                if (idxSpm != -1) {
                    int endSpm = bleBuffer.indexOf(',', idxSpm);
                    valSpm = bleBuffer.substring(idxSpm + 6, endSpm).toInt();
                }

                int idxBel = bleBuffer.indexOf("\"bel\":");
                if (idxBel != -1) {
                    int endBel = bleBuffer.indexOf(',', idxBel);
                    valBel = bleBuffer.substring(idxBel + 6, endBel).toInt();
                }

                if (idxSpm != -1 || idxBel != -1) {
                    Serial.println("\n[BLE] Sync Parameter dari STM32 BERHASIL!");
                    Serial.println(">>> Start Drive PWM (SPM): " + String(valSpm));
                    Serial.println(">>> Start Belok PWM (BEL): " + String(valBel) + "\n");
                }
                
                bleBuffer = ""; 
            }
        } else {
            bleBuffer += c;
        }
    }
}

class MyClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) { Serial.println(">>> Terkoneksi!"); }
    void onDisconnect(NimBLEClient* pClient) {
        connected = false;
        pingFailCount = 0;
        isSettingMode = false;
        digitalWrite(PIN_LED, LOW); // MATI (Active High)
        Serial.println(">>> Terputus (Callback)!");
    }
};

bool connectToServer() {
    Serial.println(">>> Mencoba menyambung...");
    if (!pClient) {
        pClient = NimBLEDevice::createClient();
        pClient->setClientCallbacks(new MyClientCallbacks(), false);
    }
    
    if (pClient->isConnected()) pClient->disconnect();
    
    if (!pClient->connect(serverAddress)) {
        Serial.println("--- Gagal. Mencari ulang dalam 5 detik ---");
        return false;
    }
    
    NimBLERemoteService* pService = pClient->getService(SERVICE_UUID);
    if (!pService) { pClient->disconnect(); return false; }
    
    pRemoteChar = pService->getCharacteristic(CHARACTERISTIC_UUID);
    if (!pRemoteChar) { pClient->disconnect(); return false; }

    if(pRemoteChar->canNotify()) {
        pRemoteChar->subscribe(true, notifyCallback);
    }

    connected = true;
    pingFailCount = 0; 
    digitalWrite(PIN_LED, HIGH); // NYALA (Active High)
    Serial.println(">>> SIAP! (Ketik w, a, s, d, atau x untuk REM/STOP)");
    return true;
}

void setup() {
    setCpuFrequencyMhz(80);
    Serial.begin(115200);
    
    // --- LED SETUP (ACTIVE HIGH) ---
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW); // Awal mula matikan LED

    pinMode(BTN_FORWARD, INPUT_PULLUP);
    pinMode(BTN_BACKWARD, INPUT_PULLUP);
    pinMode(BTN_LEFT, INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);

    NimBLEDevice::init("ESP32_RC_PRO");
    connectToServer();
}

void loop() {
    unsigned long currentMillis = millis();

    // --- 1. LOGIKA LED INDIKATOR (ACTIVE HIGH) ---
    if (!connected) {
        // Kedip normal (200ms) cari koneksi
        if (currentMillis - lastBlinkTime >= 200) {
            lastBlinkTime = currentMillis;
            ledState = !ledState;
            digitalWrite(PIN_LED, ledState ? HIGH : LOW);
        }
        if (currentMillis - lastReconTime >= 5000) {
            lastReconTime = currentMillis;
            connectToServer();
        }
    } else if (isSettingMode) {
        // Kedip CEPAT (80ms) Mode Setting
        if (currentMillis - lastBlinkTime >= 80) {
            lastBlinkTime = currentMillis;
            ledState = !ledState;
            digitalWrite(PIN_LED, ledState ? HIGH : LOW);
        }
    } else {
        // Mode normal terkoneksi menyala solid HIGH
        digitalWrite(PIN_LED, HIGH); 
    }

    // --- 2. LOGIKA KONTROL (Setiap 15ms) ---
    if (connected && (currentMillis - lastLoopTime >= 15)) {
        lastLoopTime = currentMillis;

        // Cek Serial Monitor WASD
        while (Serial.available() > 0) {
            char c = Serial.read();
            if (c == 'W') { vFwd = true; vBwd = false; } else if (c == 'w') { vFwd = false; }
            if (c == 'S') { vBwd = true; vFwd = false; } else if (c == 's') { vBwd = false; }
            if (c == 'A') { vLft = true; vRgt = false; } else if (c == 'a') { vLft = false; }
            if (c == 'D') { vRgt = true; vLft = false; } else if (c == 'd') { vRgt = false; }
            if (c == 'x' || c == 'X') { 
                vFwd=vBwd=vLft=vRgt=false; 
                Serial.println("[SYSTEM] X ditekan -> REM DARURAT!");
            } 
        }

        // BACA TOMBOL DENGAN NAMA (Untuk Serial Monitor)
        readButton(btnFwd, currentMillis, "MAJU");
        readButton(btnBwd, currentMillis, "MUNDUR");
        readButton(btnLft, currentMillis, "KIRI");
        readButton(btnRgt, currentMillis, "KANAN");

        // --- DETEKSI 4 TOMBOL DITEKAN BERSAMAAN ---
        if (btnFwd.state && btnBwd.state && btnLft.state && btnRgt.state) {
            if (!allBtnTimingActive) {
                allBtnTimingActive = true;
                allBtnHoldTimer = currentMillis;
            } else if (currentMillis - allBtnHoldTimer >= 2000) {
                isSettingMode = !isSettingMode; 
                allBtnTimingActive = false;
                
                if (isSettingMode) {
                    Serial.println("\n[SYSTEM] MASUK MODE SETTING!");
                    // Paksa berhenti dulu
                    sendCommand("S");
                    sendCommand("C");
                    lastDriveState = "S";
                    lastSteerState = "C";
                    
                    // Request parameter saat ini ke STM32
                    sendCommand("get_params"); 
                } else {
                    Serial.println("\n[SYSTEM] KELUAR MODE SETTING. Kembali ke Remote Biasa.");
                    digitalWrite(PIN_LED, HIGH); 
                }
                
                delay(500); // Debounce pas ganti mode
            }
        } else {
            allBtnTimingActive = false;
        }

        // --- CABANG LOGIKA: SETTING VS NORMAL ---
        if (isSettingMode) {
            // Mode Setting: Deteksi klik (Edge Detection)
            if (btnFwd.state && !lastBtnFwd) { 
                valSpm += 10; if(valSpm > 250) valSpm = 250;
                sendCommand("set_params:spm=" + String(valSpm)); 
                Serial.println("[SETTING] Drive PWM +10  -> SPM: " + String(valSpm));
            }
            if (btnBwd.state && !lastBtnBwd) { 
                valSpm -= 10; if(valSpm < 40) valSpm = 40;
                sendCommand("set_params:spm=" + String(valSpm)); 
                Serial.println("[SETTING] Drive PWM -10  -> SPM: " + String(valSpm));
            }
            if (btnRgt.state && !lastBtnRgt) { 
                valBel += 10; if(valBel > 250) valBel = 250;
                sendCommand("set_params:bel=" + String(valBel)); 
                Serial.println("[SETTING] Belok PWM +10 -> BEL: " + String(valBel));
            }
            if (btnLft.state && !lastBtnLft) { 
                valBel -= 10; if(valBel < 30) valBel = 30;
                sendCommand("set_params:bel=" + String(valBel)); 
                Serial.println("[SETTING] Belok PWM -10 -> BEL: " + String(valBel));
            }
        } else {
            // Mode Normal
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

            if (!allBtnTimingActive) {
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

        lastBtnFwd = btnFwd.state;
        lastBtnBwd = btnBwd.state;
        lastBtnLft = btnLft.state;
        lastBtnRgt = btnRgt.state;
    }

    // --- 3. HEARTBEAT / WATCHDOG (Setiap 1.5 detik) ---
    if (connected && (currentMillis - lastPingTime >= 1500)) {
        lastPingTime = currentMillis;
        sendCommand("P"); 
    }
}
