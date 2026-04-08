#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <TinyGPSPlus.h>
#include "config.h"
#include "secrets.h"

// GPS module on UART0 (GP0=TX, GP1=RX)
#define GPS_SERIAL Serial1
#define GPS_BAUD 9600

// WiFi credentials (from build flags)
#ifndef WIFI_SSID
#define WIFI_SSID "your_wifi_ssid"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "your_wifi_password"
#endif

// ========== Configurable cycle frequency ==========
// How often to wake, connect, get a fix, and publish.
// Examples: 60000UL = 1 min (testing), 3600000UL = 1 hour, 21600000UL = 6 hours
#define PUBLISH_INTERVAL_MS     300000UL

// Per-cycle timeouts
#define WIFI_CONNECT_TIMEOUT_MS 20000UL  // Max time to wait for WiFi association
#define GPS_FIX_WAIT_MS         60000UL  // Max time to wait for a fresh GPS fix

// TinyGPS++ object
TinyGPSPlus gps;

// WiFi and MQTT clients
WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

// Cycle timing
uint32_t lastCycle = 0;

// Connection state
bool tlsConfigured = false;

// System identifier (8-digit hex from chip ID)
char system_id[9] = "813EA37B";  // 8 hex chars + null terminator

// Function declarations
void runCycle();
void endCycle(const char* reason);
bool connectWiFi();
bool connectMQTT();
void publishGPSData();
void publishHeartbeat(const char* status);
void displayStatus();
void processGPS();

void setup() {
    // USB serial for debug output
    Serial.begin(115200);

    // Wait for USB serial connection (with timeout)
    uint32_t start = millis();
    while (!Serial && (millis() - start) < 3000) {
        delay(10);
    }

    Serial.println("\n=== GPS Tracker with AWS IoT Core ===");
    Serial.println("      (Resilient Connection Mode)");
    Serial.println();

    // Initialize GPS serial
    Serial.println("[GPS] Initializing UART0 (GP0/GP1)...");
    GPS_SERIAL.setRX(1);  // GP1
    GPS_SERIAL.setTX(0);  // GP0
    GPS_SERIAL.begin(GPS_BAUD);
    Serial.println("[GPS] Initialized at 9600 baud");

    // Configure TLS certificates
    Serial.println("[TLS] Configuring certificates...");
    wifiClient.setCACert(AWS_ROOT_CA);
    wifiClient.setCertificate(DEVICE_CERT);
    wifiClient.setPrivateKey(DEVICE_PRIVATE_KEY);
    tlsConfigured = true;
    Serial.println("[TLS] Certificates configured");

    // Configure MQTT
    mqttClient.setServer(AWS_IOT_ENDPOINT, 8883);
    mqttClient.setBufferSize(512);
    mqttClient.setKeepAlive(60);     // 60 second keepalive
    mqttClient.setSocketTimeout(30); // 30 s — covers cold TLS handshake on Pico W

    Serial.println();
    Serial.println("=== Setup Complete ===");
    Serial.print("GPS tracker active. Cycle interval: ");
    Serial.print(PUBLISH_INTERVAL_MS / 1000);
    Serial.println(" seconds.");
    Serial.println("First cycle will run immediately.");
    Serial.println();

    // Force first cycle to run immediately on entering loop()
    lastCycle = millis() - PUBLISH_INTERVAL_MS;
}

void loop() {
    // Always drain GPS serial so the parser stays current and the UART
    // buffer never overflows between cycles.
    processGPS();

    // Keep MQTT keepalive happy if we happen to still be connected.
    if (mqttClient.connected()) {
        mqttClient.loop();
    }

    // Run a full publish cycle once per interval.
    uint32_t now = millis();
    if (now - lastCycle >= PUBLISH_INTERVAL_MS) {
        lastCycle = now;
        runCycle();
    }
}

void runCycle() {
    Serial.println();
    Serial.println("========== Cycle start ==========");
    displayStatus();

    // 1. Ensure WiFi (kept associated between cycles, just re-checked here)
    if (WiFi.status() != WL_CONNECTED) {
        if (!connectWiFi()) {
            endCycle("WiFi unavailable - aborting cycle");
            return;
        }
    }

    // 2. Fresh MQTT connection every cycle (wake-publish-sleep pattern)
    if (!connectMQTT()) {
        endCycle("MQTT unavailable - aborting cycle");
        return;
    }

    // 3. Publish heartbeat immediately on the fresh connection. This proves
    //    we can publish to MQTT_STATUS_TOPIC and gives an early liveness signal
    //    independent of whether we eventually get a GPS fix.
    publishHeartbeat("online");

    // 4. Wait for a GPS fix (drain serial + service MQTT during the wait).
    Serial.println("[CYCLE] Waiting for GPS fix...");
    uint32_t waitStart = millis();
    while (!gps.location.isValid() && (millis() - waitStart) < GPS_FIX_WAIT_MS) {
        processGPS();
        if (mqttClient.connected()) {
            mqttClient.loop();
        } else {
            Serial.println("[MQTT] Connection lost during GPS wait");
            break;
        }
        delay(10);
    }

    // 5. Publish GPS telemetry only if we have a valid fix
    if (gps.location.isValid()) {
        publishGPSData();
    } else {
        Serial.println("[CYCLE] No GPS fix - telemetry skipped");
    }

    endCycle(NULL);
}

void endCycle(const char* reason) {
    if (reason) {
        Serial.print("[CYCLE] ");
        Serial.println(reason);
    }
    // Tear down MQTT cleanly (suppresses LWT) and close the TLS socket so the
    // next cycle starts with a fresh handshake. WiFi stays associated.
    if (mqttClient.connected()) {
        mqttClient.disconnect();
    }
    wifiClient.stop();
    Serial.println("[CYCLE] Sleeping until next cycle");
    Serial.println("========== Cycle end ==========");
}

void processGPS() {
    // Read all available GPS data (non-blocking)
    while (GPS_SERIAL.available() > 0) {
        gps.encode(GPS_SERIAL.read());
    }
}

bool connectWiFi() {
    Serial.print("[WiFi] Connecting to ");
    Serial.print(WIFI_SSID);
    Serial.print("...");

    // Disconnect first if in a bad state
    WiFi.disconnect();
    delay(100);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // Wait for association up to WIFI_CONNECT_TIMEOUT_MS
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
        // Keep GPS parser fed during WiFi connect
        processGPS();
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(" Connected!");
        Serial.print("[WiFi] IP: ");
        Serial.println(WiFi.localIP());
        Serial.print("[WiFi] RSSI: ");
        Serial.print(WiFi.RSSI());
        Serial.println(" dBm");
        return true;
    } else {
        Serial.println(" Failed!");
        return false;
    }
}

bool connectMQTT() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[MQTT] WiFi not connected, skipping MQTT connect");
        return false;
    }

    if (mqttClient.connected()) {
        return true;
    }

    Serial.print("[MQTT] Connecting to AWS IoT Core...");

    // Use a unique client ID based on chip ID
    String clientId = "GPS_Tracker_";
    clientId += String(rp2040.getChipID(), HEX);

    // Register Last Will & Testament: broker auto-publishes "offline" if we
    // disconnect without sending a clean DISCONNECT (power loss, crash, etc.).
    // Note: willRetain=false because retained publishes are denied by this
    // account's IoT auth layer despite the policy granting iot:PublishRetain.
    char willPayload[96];
    snprintf(willPayload, sizeof(willPayload),
        "{\"system_id\":\"%s\",\"status\":\"offline\"}", system_id);

    // connect(clientID, user, pass, willTopic, willQos, willRetain, willMessage)
    if (mqttClient.connect(clientId.c_str(),
                           NULL, NULL,
                           MQTT_STATUS_TOPIC, 1, false, willPayload)) {
        Serial.println(" Connected!");
        Serial.print("[MQTT] Client ID: ");
        Serial.println(clientId);
        Serial.print("[MQTT] Telemetry topic: ");
        Serial.println(MQTT_TOPIC);
        Serial.print("[MQTT] Status topic:    ");
        Serial.println(MQTT_STATUS_TOPIC);

        // Publish an "online" marker on every fresh connect (non-retained).
        char onlinePayload[96];
        snprintf(onlinePayload, sizeof(onlinePayload),
            "{\"system_id\":\"%s\",\"status\":\"online\"}", system_id);
        mqttClient.publish(MQTT_STATUS_TOPIC, onlinePayload, false);

        return true;
    } else {
        Serial.print(" Failed! RC=");
        Serial.println(mqttClient.state());

        // Decode error for debugging
        switch (mqttClient.state()) {
            case -4: Serial.println("       (Connection timeout)"); break;
            case -3: Serial.println("       (Connection lost)"); break;
            case -2: Serial.println("       (Connect failed)"); break;
            case -1: Serial.println("       (Disconnected)"); break;
            case 1:  Serial.println("       (Bad protocol)"); break;
            case 2:  Serial.println("       (Bad client ID)"); break;
            case 3:  Serial.println("       (Unavailable)"); break;
            case 4:  Serial.println("       (Bad credentials)"); break;
            case 5:  Serial.println("       (Unauthorized)"); break;
        }
        return false;
    }
}

void publishHeartbeat(const char* status) {
    if (!mqttClient.connected()) {
        Serial.println("[MQTT] Not connected, cannot publish heartbeat");
        return;
    }

    // Liveness payload — independent of GPS fix. Includes diagnostics that
    // help distinguish "alive but no fix" from "alive but GPS broken".
    char payload[256];
    snprintf(payload, sizeof(payload),
        "{"
        "\"system_id\":\"%s\","
        "\"status\":\"%s\","
        "\"uptime_s\":%lu,"
        "\"wifi_rssi\":%d,"
        "\"fix_valid\":%s,"
        "\"satellites\":%d,"
        "\"hdop\":%.2f,"
        "\"chars_processed\":%lu"
        "}",
        system_id,
        status,
        (unsigned long)(millis() / 1000),
        WiFi.RSSI(),
        gps.location.isValid() ? "true" : "false",
        gps.satellites.isValid() ? gps.satellites.value() : 0,
        gps.hdop.isValid() ? gps.hdop.hdop() : 99.99,
        (unsigned long)gps.charsProcessed()
    );

    // Non-retained: this account's IoT auth layer denies retained publishes
    // even though the policy grants iot:PublishRetain. Subscribers should
    // judge liveness by heartbeat freshness rather than the retained value.
    if (mqttClient.publish(MQTT_STATUS_TOPIC, payload, false)) {
        Serial.println("[MQTT] Heartbeat published:");
        Serial.print("  ");
        Serial.println(payload);
    } else {
        Serial.println("[MQTT] Heartbeat publish failed!");
    }
}

void publishGPSData() {
    if (!mqttClient.connected()) {
        Serial.println("[MQTT] Not connected, cannot publish");
        return;
    }

    // Build JSON payload
    char payload[300];
    snprintf(payload, sizeof(payload),
        "{"
        "\"system_id\":\"%s\","
        "\"timestamp\":%lu,"
        "\"location\":{"
            "\"lat\":%.6f,"
            "\"lng\":%.6f,"
            "\"alt\":%.1f"
        "},"
        "\"satellites\":%d,"
        "\"hdop\":%.2f,"
        "\"wifi_rssi\":%d"
        "}",
        system_id,
        millis() / 1000,
        gps.location.lat(),
        gps.location.lng(),
        gps.altitude.isValid() ? gps.altitude.meters() : 0.0,
        gps.satellites.isValid() ? gps.satellites.value() : 0,
        gps.hdop.isValid() ? gps.hdop.hdop() : 99.99,
        WiFi.RSSI()
    );

    // Publish to AWS IoT
    if (mqttClient.publish(MQTT_TOPIC, payload)) {
        Serial.println("[MQTT] Published:");
        Serial.print("  ");
        Serial.println(payload);
    } else {
        Serial.println("[MQTT] Publish failed!");
    }
}

void displayStatus() {
    Serial.println("----------------------------------------");

    // Uptime
    uint32_t uptime = millis() / 1000;
    Serial.print("[SYS] Uptime: ");
    Serial.print(uptime / 3600); Serial.print("h ");
    Serial.print((uptime % 3600) / 60); Serial.print("m ");
    Serial.print(uptime % 60); Serial.println("s");

    // WiFi status
    Serial.print("[WiFi] ");
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("Connected (");
        Serial.print(WiFi.RSSI());
        Serial.println(" dBm)");
    } else {
        Serial.println("Disconnected");
    }

    // MQTT status
    Serial.print("[MQTT] ");
    Serial.println(mqttClient.connected() ? "Connected" : "Disconnected");

    // GPS status
    Serial.print("[GPS] Location: ");
    if (gps.location.isValid()) {
        Serial.print(gps.location.lat(), 6);
        Serial.print(", ");
        Serial.println(gps.location.lng(), 6);
    } else {
        Serial.println("Waiting for fix...");
    }

    Serial.print("[GPS] Satellites: ");
    Serial.print(gps.satellites.isValid() ? gps.satellites.value() : 0);
    Serial.print(" | HDOP: ");
    Serial.println(gps.hdop.isValid() ? gps.hdop.hdop() : 99.99);
}
