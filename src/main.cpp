#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <TinyGPSPlus.h>
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

// Timing intervals (milliseconds)
#define PUBLISH_INTERVAL        10000   // Publish GPS every 10 seconds
#define WIFI_CHECK_INTERVAL     5000    // Check WiFi every 5 seconds
#define MQTT_CHECK_INTERVAL     5000    // Check MQTT every 5 seconds
#define WIFI_RECONNECT_DELAY    5000    // Wait between WiFi reconnect attempts
#define MQTT_RECONNECT_DELAY    5000    // Wait between MQTT reconnect attempts
#define MAX_RECONNECT_DELAY     60000   // Max backoff delay (1 minute)

// TinyGPS++ object
TinyGPSPlus gps;

// WiFi and MQTT clients
WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

// Timing state
uint32_t lastPublish = 0;
uint32_t lastWiFiCheck = 0;
uint32_t lastMQTTCheck = 0;
uint32_t lastWiFiReconnectAttempt = 0;
uint32_t lastMQTTReconnectAttempt = 0;

// Reconnection backoff state
uint32_t wifiReconnectDelay = WIFI_RECONNECT_DELAY;
uint32_t mqttReconnectDelay = MQTT_RECONNECT_DELAY;
uint32_t wifiReconnectCount = 0;
uint32_t mqttReconnectCount = 0;

// Connection state
bool tlsConfigured = false;

// System identifier (8-digit hex from chip ID)
char system_id[9] = "813EA37B";  // 8 hex chars + null terminator

// Function declarations
void checkWiFiConnection();
void checkMQTTConnection();
bool connectWiFi();
bool connectMQTT();
void publishGPSData();
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
    mqttClient.setKeepAlive(60);  // 60 second keepalive

    // Initial connection attempts
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) {
        connectMQTT();
    }

    Serial.println();
    Serial.println("=== Setup Complete ===");
    Serial.println("GPS tracking active. Connections will auto-recover.");
    Serial.println();
}

void loop() {
    uint32_t now = millis();

    // Always process GPS data (non-blocking)
    processGPS();

    // Periodically check WiFi connection
    if (now - lastWiFiCheck >= WIFI_CHECK_INTERVAL) {
        lastWiFiCheck = now;
        checkWiFiConnection();
    }

    // Periodically check MQTT connection (only if WiFi is up)
    if (WiFi.status() == WL_CONNECTED && (now - lastMQTTCheck >= MQTT_CHECK_INTERVAL)) {
        lastMQTTCheck = now;
        checkMQTTConnection();
    }

    // Process MQTT messages (non-blocking)
    if (mqttClient.connected()) {
        mqttClient.loop();
    }

    // Publish GPS data at interval
    if (now - lastPublish >= PUBLISH_INTERVAL) {
        lastPublish = now;
        displayStatus();

        if (gps.location.isValid() && mqttClient.connected()) {
            publishGPSData();
        } else if (!gps.location.isValid()) {
            Serial.println("[GPS] No fix yet - not publishing");
        } else if (!mqttClient.connected()) {
            Serial.println("[MQTT] Not connected - data queued in GPS buffer");
        }
    }
}

void processGPS() {
    // Read all available GPS data (non-blocking)
    while (GPS_SERIAL.available() > 0) {
        gps.encode(GPS_SERIAL.read());
    }
}

void checkWiFiConnection() {
    if (WiFi.status() == WL_CONNECTED) {
        // Connected - reset backoff
        if (wifiReconnectCount > 0) {
            Serial.println("[WiFi] Connection restored!");
            wifiReconnectCount = 0;
            wifiReconnectDelay = WIFI_RECONNECT_DELAY;
        }
        return;
    }

    // Not connected - attempt reconnect with backoff
    uint32_t now = millis();
    if (now - lastWiFiReconnectAttempt >= wifiReconnectDelay) {
        lastWiFiReconnectAttempt = now;

        wifiReconnectCount++;
        Serial.print("[WiFi] Connection lost. Reconnect attempt #");
        Serial.println(wifiReconnectCount);

        if (connectWiFi()) {
            // Success - reset backoff
            wifiReconnectDelay = WIFI_RECONNECT_DELAY;
        } else {
            // Failed - increase backoff (exponential with cap)
            wifiReconnectDelay = min(wifiReconnectDelay * 2, MAX_RECONNECT_DELAY);
            Serial.print("[WiFi] Next retry in ");
            Serial.print(wifiReconnectDelay / 1000);
            Serial.println(" seconds");
        }
    }
}

void checkMQTTConnection() {
    if (mqttClient.connected()) {
        // Connected - reset backoff
        if (mqttReconnectCount > 0) {
            Serial.println("[MQTT] Connection restored!");
            mqttReconnectCount = 0;
            mqttReconnectDelay = MQTT_RECONNECT_DELAY;
        }
        return;
    }

    // Not connected - attempt reconnect with backoff
    uint32_t now = millis();
    if (now - lastMQTTReconnectAttempt >= mqttReconnectDelay) {
        lastMQTTReconnectAttempt = now;

        mqttReconnectCount++;
        Serial.print("[MQTT] Connection lost. Reconnect attempt #");
        Serial.println(mqttReconnectCount);

        if (connectMQTT()) {
            // Success - reset backoff
            mqttReconnectDelay = MQTT_RECONNECT_DELAY;
        } else {
            // Failed - increase backoff (exponential with cap)
            mqttReconnectDelay = min(mqttReconnectDelay * 2, MAX_RECONNECT_DELAY);
            Serial.print("[MQTT] Next retry in ");
            Serial.print(mqttReconnectDelay / 1000);
            Serial.println(" seconds");
        }
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

    // Wait for connection with timeout (non-blocking friendly)
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;

        // Keep GPS running during WiFi connect
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

    // Single connection attempt (backoff handled by caller)
    if (mqttClient.connect(clientId.c_str())) {
        Serial.println(" Connected!");
        Serial.print("[MQTT] Client ID: ");
        Serial.println(clientId);
        Serial.print("[MQTT] Topic: ");
        Serial.println(MQTT_TOPIC);
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
        Serial.print(" dBm)");
        if (wifiReconnectCount > 0) {
            Serial.print(" [recovered after ");
            Serial.print(wifiReconnectCount);
            Serial.print(" attempts]");
        }
        Serial.println();
    } else {
        Serial.print("Disconnected (retry #");
        Serial.print(wifiReconnectCount);
        Serial.println(")");
    }

    // MQTT status
    Serial.print("[MQTT] ");
    if (mqttClient.connected()) {
        Serial.print("Connected");
        if (mqttReconnectCount > 0) {
            Serial.print(" [recovered after ");
            Serial.print(mqttReconnectCount);
            Serial.print(" attempts]");
        }
        Serial.println();
    } else {
        Serial.print("Disconnected (retry #");
        Serial.print(mqttReconnectCount);
        Serial.println(")");
    }

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
