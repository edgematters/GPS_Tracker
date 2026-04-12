#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <TinyGPSPlus.h>
#include <EEPROM.h>
#include "config.h"
#include "secrets.h"

// NVM layout for persisted settings (EEPROM-emulated flash).
// A magic word validates the stored data; on first boot or after a
// flash erase the magic won't match and we fall back to defaults.
//
//   Offset  Size  Field
//   0       4     magic word (0x47505332 = "GPS2")
//   4       4     publishInterval (uint32_t, ms)
//   8       8     system_id (8 ASCII hex chars, no null)
//
#define NVM_SIZE            16
#define NVM_MAGIC           0x47505332  // "GPS2" — bumped to invalidate old layout
#define NVM_ADDR_MAGIC      0
#define NVM_ADDR_INTERVAL   4
#define NVM_ADDR_SYSTEM_ID  8
#define SYSTEM_ID_LEN       8

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
// How often to wake, connect, get a fix, and publish. This is the boot default;
// it can be changed at runtime via the "set_interval" downlink command.
// Examples: 60000 = 1 min (testing), 3600000 = 1 hour, 21600000 = 6 hours
#define DEFAULT_PUBLISH_INTERVAL_MS  300000UL
#define MIN_PUBLISH_INTERVAL_MS       10000UL  // 10 s floor
#define MAX_PUBLISH_INTERVAL_MS    86400000UL  // 24 h ceiling

// Per-cycle timeouts
#define WIFI_CONNECT_TIMEOUT_MS 20000UL  // Max time to wait for WiFi association
#define GPS_FIX_WAIT_MS         60000UL  // Max time to wait for a fresh GPS fix

// TinyGPS++ object
TinyGPSPlus gps;

// WiFi and MQTT clients
WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

// Cycle timing
uint32_t publishInterval = DEFAULT_PUBLISH_INTERVAL_MS;
uint32_t lastCycle = 0;

// Connection state
bool tlsConfigured = false;

// System identifier (8-digit hex). Default is compiled-in; NVM overrides it.
#define DEFAULT_SYSTEM_ID "813EA37B"
char system_id[SYSTEM_ID_LEN + 1] = DEFAULT_SYSTEM_ID;

// Resolved MQTT topics. The macros in config.h use '+' as a placeholder for
// the system_id; resolveTopics() expands them once at boot into these buffers.
char mqtt_topic[80];
char mqtt_status_topic[80];
char mqtt_downlink_topic[80];

// Function declarations
void runCycle();
void endCycle(const char* reason);
bool connectWiFi();
bool connectMQTT();
void publishGPSData();
void publishHeartbeat(const char* status);
void displayStatus();
void processGPS();
void resolveTopic(char* out, size_t outLen, const char* tmpl, const char* id);
uint32_t gpsUnixSeconds();
void mqttCallback(char* topic, byte* payload, unsigned int length);
uint32_t nvmReadInterval();
void nvmWriteInterval(uint32_t interval);
void nvmReadSystemId(char* out);
void nvmWriteSystemId(const char* id);

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

    // Load persisted settings from NVM before resolving topics, since
    // system_id determines the topic paths.
    EEPROM.begin(NVM_SIZE);
    nvmReadSystemId(system_id);
    publishInterval = nvmReadInterval();
    Serial.print("[SYS] System ID: ");
    Serial.println(system_id);

    // Resolve topic templates from config.h: substitute '+' with system_id.
    resolveTopic(mqtt_topic,          sizeof(mqtt_topic),          MQTT_TOPIC,          system_id);
    resolveTopic(mqtt_status_topic,   sizeof(mqtt_status_topic),   MQTT_STATUS_TOPIC,   system_id);
    resolveTopic(mqtt_downlink_topic, sizeof(mqtt_downlink_topic), MQTT_DOWNLINK_TOPIC, system_id);
    Serial.print("[MQTT] Telemetry topic resolved: ");
    Serial.println(mqtt_topic);
    Serial.print("[MQTT] Status topic resolved:    ");
    Serial.println(mqtt_status_topic);
    Serial.print("[MQTT] Downlink topic resolved:  ");
    Serial.println(mqtt_downlink_topic);

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
    mqttClient.setCallback(mqttCallback);

    Serial.println();
    Serial.println("=== Setup Complete ===");
    Serial.print("GPS tracker active. Cycle interval: ");
    Serial.print(publishInterval / 1000);
    Serial.println(" seconds.");
    Serial.println("First cycle will run immediately.");
    Serial.println();

    // Force first cycle to run immediately on entering loop()
    lastCycle = millis() - publishInterval;
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
    if (now - lastCycle >= publishInterval) {
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
    // Close the TLS socket WITHOUT sending an MQTT DISCONNECT. This makes the
    // broker think we dropped unexpectedly, which:
    //   1. Preserves the persistent session (cleanSession=false) so QoS 1
    //      downlink messages are queued until the next connect.
    //   2. Triggers the LWT "offline" message for live subscribers.
    // We skip mqttClient.disconnect() intentionally — a clean disconnect
    // would destroy the session and stop message queuing.
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

    // connect(clientID, user, pass, willTopic, willQos, willRetain, willMessage, cleanSession)
    // cleanSession=false so the broker queues QoS 1 messages (e.g. downlink
    // commands) while the device is asleep between cycles.
    if (mqttClient.connect(clientId.c_str(),
                           NULL, NULL,
                           mqtt_status_topic, 1, false, willPayload, false)) {
        Serial.println(" Connected!");
        Serial.print("[MQTT] Client ID: ");
        Serial.println(clientId);
        Serial.print("[MQTT] Telemetry topic: ");
        Serial.println(mqtt_topic);
        Serial.print("[MQTT] Status topic:    ");
        Serial.println(mqtt_status_topic);

        // Subscribe to downlink topic for remote commands.
        if (mqttClient.subscribe(mqtt_downlink_topic, 1)) {
            Serial.print("[MQTT] Subscribed to: ");
            Serial.println(mqtt_downlink_topic);
        } else {
            Serial.println("[MQTT] Downlink subscribe failed");
        }

        // Publish an "online" marker on every fresh connect (non-retained).
        char onlinePayload[96];
        snprintf(onlinePayload, sizeof(onlinePayload),
            "{\"system_id\":\"%s\",\"status\":\"online\"}", system_id);
        mqttClient.publish(mqtt_status_topic, onlinePayload, false);

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
    char payload[300];
    snprintf(payload, sizeof(payload),
        "{"
        "\"system_id\":\"%s\","
        "\"status\":\"%s\","
        "\"interval_ms\":%lu,"
        "\"uptime_s\":%lu,"
        "\"wifi_rssi\":%d,"
        "\"fix_valid\":%s,"
        "\"satellites\":%d,"
        "\"hdop\":%.2f,"
        "\"chars_processed\":%lu"
        "}",
        system_id,
        status,
        (unsigned long)publishInterval,
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
    if (mqttClient.publish(mqtt_status_topic, payload, false)) {
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

    // Use the GPS's own UTC clock for the timestamp. If the GPS hasn't given
    // us a valid date/time yet (even with a fix this can lag a few seconds),
    // skip the publish — Timestream rejects records outside its ingestion
    // window and we don't want to send a bogus epoch.
    uint32_t epochS = gpsUnixSeconds();
    if (epochS == 0) {
        Serial.println("[GPS] No valid GPS UTC time yet, skipping telemetry publish");
        return;
    }

    // Format epoch as milliseconds via string concat — avoids 64-bit printf
    // formatter quirks on the Pico's newlib build.
    char tsMs[16];
    snprintf(tsMs, sizeof(tsMs), "%lu000", (unsigned long)epochS);

    // Build JSON payload
    char payload[300];
    snprintf(payload, sizeof(payload),
        "{"
        "\"system_id\":\"%s\","
        "\"timestamp\":%s,"
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
        tsMs,
        gps.location.lat(),
        gps.location.lng(),
        gps.altitude.isValid() ? gps.altitude.meters() : 0.0,
        gps.satellites.isValid() ? gps.satellites.value() : 0,
        gps.hdop.isValid() ? gps.hdop.hdop() : 99.99,
        WiFi.RSSI()
    );

    // Publish to AWS IoT
    if (mqttClient.publish(mqtt_topic, payload)) {
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

// Replace the first '+' in `tmpl` with `id`, writing the result to `out`.
// If there is no '+', the template is copied verbatim.
void resolveTopic(char* out, size_t outLen, const char* tmpl, const char* id) {
    const char* plus = strchr(tmpl, '+');
    if (!plus) {
        snprintf(out, outLen, "%s", tmpl);
        return;
    }
    size_t prefixLen = (size_t)(plus - tmpl);
    snprintf(out, outLen, "%.*s%s%s", (int)prefixLen, tmpl, id, plus + 1);
}

// MQTT downlink callback — fired by mqttClient.loop() when a message arrives
// on any subscribed topic. Parses simple JSON commands.
//
// Supported commands:
//   {"cmd":"set_interval","value":<milliseconds>}
//     Sets the cycle interval (clamped to MIN..MAX). Takes effect next cycle.
//   {"cmd":"set_system_id","value":"ABCD1234"}
//     Saves a new 8-char hex system_id to NVM. Requires reboot to take effect.
//   {"cmd":"reboot"}
//     Immediate reboot via rp2040.reboot().
//
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char msg[256];
    unsigned int len = min(length, (unsigned int)(sizeof(msg) - 1));
    memcpy(msg, payload, len);
    msg[len] = '\0';

    Serial.print("[MQTT] Downlink received on ");
    Serial.print(topic);
    Serial.print(": ");
    Serial.println(msg);

    // --- set_interval ---
    if (strstr(msg, "\"set_interval\"")) {
        char* valPtr = strstr(msg, "\"value\"");
        if (valPtr) {
            valPtr = strchr(valPtr, ':');
            if (valPtr) {
                uint32_t newInterval = strtoul(valPtr + 1, NULL, 10);
                if (newInterval >= MIN_PUBLISH_INTERVAL_MS && newInterval <= MAX_PUBLISH_INTERVAL_MS) {
                    publishInterval = newInterval;
                    nvmWriteInterval(publishInterval);
                    Serial.print("[CMD] Interval changed to ");
                    Serial.print(publishInterval / 1000);
                    Serial.println("s (saved to NVM) — takes effect next cycle");
                } else {
                    Serial.print("[CMD] Interval rejected (out of range ");
                    Serial.print(MIN_PUBLISH_INTERVAL_MS / 1000);
                    Serial.print("s - ");
                    Serial.print(MAX_PUBLISH_INTERVAL_MS / 1000);
                    Serial.println("s)");
                }
            }
        }
        return;
    }

    // --- set_system_id ---
    if (strstr(msg, "\"set_system_id\"")) {
        // Extract the value string — look for "value":"XXXXXXXX"
        char* valPtr = strstr(msg, "\"value\"");
        if (valPtr) {
            // Find the opening quote of the value
            valPtr = strchr(valPtr + 7, '\"');
            if (valPtr) {
                valPtr++;  // skip the opening quote
                // Validate: must be exactly SYSTEM_ID_LEN hex chars
                bool valid = true;
                for (int i = 0; i < SYSTEM_ID_LEN; i++) {
                    if (!isxdigit((unsigned char)valPtr[i])) { valid = false; break; }
                }
                if (valid) {
                    char newId[SYSTEM_ID_LEN + 1];
                    memcpy(newId, valPtr, SYSTEM_ID_LEN);
                    newId[SYSTEM_ID_LEN] = '\0';
                    // Uppercase for consistency
                    for (int i = 0; i < SYSTEM_ID_LEN; i++) newId[i] = toupper(newId[i]);
                    nvmWriteSystemId(newId);
                    Serial.print("[CMD] System ID set to ");
                    Serial.print(newId);
                    Serial.println(" (saved to NVM) — reboot to activate");
                } else {
                    Serial.print("[CMD] System ID rejected (must be ");
                    Serial.print(SYSTEM_ID_LEN);
                    Serial.println(" hex chars)");
                }
            }
        }
        return;
    }

    // --- reboot ---
    if (strstr(msg, "\"reboot\"")) {
        Serial.println("[CMD] Reboot requested — rebooting now");
        Serial.flush();
        delay(100);
        rp2040.reboot();
    }
}

// Convert the GPS module's current UTC date+time to a Unix epoch (seconds).
// Returns 0 if either the date or time component isn't valid yet. Uses the
// "days from civil" algorithm (Howard Hinnant, public domain) so we don't
// depend on libc timezone handling.
uint32_t gpsUnixSeconds() {
    if (!gps.date.isValid() || !gps.time.isValid()) return 0;

    int y = gps.date.year();
    unsigned m = gps.date.month();
    unsigned d = gps.date.day();
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return 0;

    // days_from_civil
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;

    return (uint32_t)(days * 86400L
         + (long)gps.time.hour() * 3600L
         + (long)gps.time.minute() * 60L
         + (long)gps.time.second());
}

// Read the publish interval from EEPROM. Returns the stored value if the magic
// word is present and the value is within range; otherwise returns the default.
uint32_t nvmReadInterval() {
    uint32_t magic = 0;
    EEPROM.get(NVM_ADDR_MAGIC, magic);
    if (magic != NVM_MAGIC) {
        Serial.println("[NVM] No saved interval — using default");
        return DEFAULT_PUBLISH_INTERVAL_MS;
    }

    uint32_t interval = 0;
    EEPROM.get(NVM_ADDR_INTERVAL, interval);
    if (interval < MIN_PUBLISH_INTERVAL_MS || interval > MAX_PUBLISH_INTERVAL_MS) {
        Serial.println("[NVM] Stored interval out of range — using default");
        return DEFAULT_PUBLISH_INTERVAL_MS;
    }

    Serial.print("[NVM] Loaded interval from NVM: ");
    Serial.print(interval / 1000);
    Serial.println("s");
    return interval;
}

// Write the publish interval to EEPROM and commit.
void nvmWriteInterval(uint32_t interval) {
    uint32_t magic = NVM_MAGIC;
    EEPROM.put(NVM_ADDR_MAGIC, magic);
    EEPROM.put(NVM_ADDR_INTERVAL, interval);
    if (EEPROM.commit()) {
        Serial.println("[NVM] Interval saved to flash");
    } else {
        Serial.println("[NVM] Flash write failed!");
    }
}

// Read system_id from EEPROM into `out` (must be at least SYSTEM_ID_LEN+1).
// If NVM magic doesn't match or stored ID is invalid, leaves `out` unchanged
// (caller pre-fills it with the compiled-in default).
void nvmReadSystemId(char* out) {
    uint32_t magic = 0;
    EEPROM.get(NVM_ADDR_MAGIC, magic);
    if (magic != NVM_MAGIC) {
        Serial.println("[NVM] No saved system_id — using default");
        return;
    }

    char buf[SYSTEM_ID_LEN + 1];
    for (int i = 0; i < SYSTEM_ID_LEN; i++) {
        buf[i] = EEPROM.read(NVM_ADDR_SYSTEM_ID + i);
    }
    buf[SYSTEM_ID_LEN] = '\0';

    // Validate: must be all hex digits
    bool valid = true;
    for (int i = 0; i < SYSTEM_ID_LEN; i++) {
        if (!isxdigit((unsigned char)buf[i])) { valid = false; break; }
    }
    if (!valid) {
        Serial.println("[NVM] Stored system_id invalid — using default");
        return;
    }

    memcpy(out, buf, SYSTEM_ID_LEN + 1);
    Serial.print("[NVM] Loaded system_id from NVM: ");
    Serial.println(out);
}

// Write system_id to EEPROM and commit.
void nvmWriteSystemId(const char* id) {
    uint32_t magic = NVM_MAGIC;
    EEPROM.put(NVM_ADDR_MAGIC, magic);
    // Preserve the current interval when writing system_id
    EEPROM.put(NVM_ADDR_INTERVAL, publishInterval);
    for (int i = 0; i < SYSTEM_ID_LEN; i++) {
        EEPROM.write(NVM_ADDR_SYSTEM_ID + i, id[i]);
    }
    if (EEPROM.commit()) {
        Serial.println("[NVM] System ID saved to flash");
    } else {
        Serial.println("[NVM] Flash write failed!");
    }
}
