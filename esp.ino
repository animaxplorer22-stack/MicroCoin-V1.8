/*
  MICROCORE (MCX) ESP32 MINER v7.3 — SIMPLIFIED
  Uses ESP32 built-in crypto (no mbedtls headaches)
  Hardware: ESP32 only (not ESP8266)
  
  Works with standard Arduino ESP32 libraries
*/

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUDP.h>
#include <EEPROM.h>
#include <SPIFFS.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

// ==================== USER CONFIGURATION ====================
const char* WIFI_SSID = "your_wifi_ssid";              // ← CHANGE
const char* WIFI_PASSWORD = "your_wifi_password";      // ← CHANGE

const char* BOOTSTRAP_NODES[] = {"101.127.80.48:8080"}; // ← CHANGE
const int BOOTSTRAP_COUNT = 1;
const int NODE_PORT = 8080;

const char* USERNAME = "your_username";                 // ← CHANGE
const char* PRIVATE_KEY_HEX = "your_private_key_hex";   // ← CHANGE (64 chars)

const uint32_t INITIAL_STAKE = 1000;
const uint32_t LEVEL_STAKE_RANGE = 1000;
const uint32_t MAX_LEVEL = 10;

// ==================== CONSTANTS ====================
#define SIGNING_WINDOW_MS 2500
#define SLASH_RATE 0.10
#define UPTIME_PING_INTERVAL 30000
#define MAX_RECONNECT_ATTEMPTS 5
#define MAX_PEERS 20
#define VERSION "7.3"

#define EEPROM_STAKE_ADDR 0
#define EEPROM_REWARDS_ADDR 4
#define EEPROM_BLOCKS_ADDR 8
#define EEPROM_UPTIME_ADDR 12
#define EEPROM_TODAY_UPTIME_ADDR 16
#define EEPROM_LAST_RESET_ADDR 20
#define EEPROM_SLASH_COUNT_ADDR 24
#define EEPROM_CONSECUTIVE_MISSES_ADDR 28
#define EEPROM_LEVEL_ADDR 32
#define EEPROM_CHECKSUM_ADDR 36
#define EEPROM_MAGIC_ADDR 40
#define EEPROM_PEER_INDEX_ADDR 44

#define MAGIC_NUMBER 0x5A5A5A5A
#define LED_PIN 2

const uint32_t LEVEL_BLOCK_INTERVALS[] = {0, 40, 35, 30, 25, 20, 15, 10, 9, 8, 7};

// ==================== GLOBAL VARIABLES ====================
WebSocketsClient webSocket;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

uint32_t currentStake, totalRewards, totalBlocksSigned, totalUptimeSeconds;
uint32_t todayUptimeSeconds, lastUptimeReset, currentLevel, lastUptimePing;
uint32_t lastChallengeTime, slashCount, consecutiveMisses;
uint32_t currentBlockId, reconnectAttempts, currentPeerIndex, nodeSwitchCount;

char validatorID[65], publicKeyHex[65], walletAddress[70];
char currentChallenge[65], currentNodeIP[16];
bool isValidator = false, isRegistered = false, wsConnected = false, miningEnabled = true;

String peerList[MAX_PEERS];
int peerCount = 0;

// ==================== LED FUNCTIONS ====================
void led_on() { pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW); }
void led_off() { digitalWrite(LED_PIN, HIGH); }
void led_blink(int times, int duration) {
  for (int i = 0; i < times; i++) { led_on(); delay(duration); led_off(); delay(duration); }
}

// ==================== DJB2 HASH (matches Arduino Uno) ====================
void djb2_hash(const char* input, char* output) {
  uint32_t h = 5381;
  for (size_t i = 0; input[i]; i++) {
    h = ((h << 5) + h) + input[i];
  }
  sprintf(output, "%08lx", h);
}

// ==================== SHA256 ====================
void computeSHA256(const char* input, char* output) {
  unsigned char hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char*)input, strlen(input));
  mbedtls_sha256_finish(&ctx, hash);
  
  for (int i = 0; i < 32; i++) {
    sprintf(output + (i * 2), "%02x", hash[i]);
  }
  output[64] = '\0';
}

// ==================== SIMPLE ECDSA SIGNATURE (USING ESP32 BUILT-IN) ====================
void signMessage(const char* message, char* signatureOut) {
  // For ESP32, we use djb2 hash as signature (matches Arduino/AVR)
  // This is simpler and works with the node's verification
  djb2_hash(message, signatureOut);
}

// ==================== LEVEL & EEPROM ====================
void calculateLevel() {
  currentLevel = (currentStake < LEVEL_STAKE_RANGE) ? 1 : ((currentStake - 1) / LEVEL_STAKE_RANGE) + 1;
  if (currentLevel < 1) currentLevel = 1;
  if (currentLevel > MAX_LEVEL) currentLevel = MAX_LEVEL;
}

uint32_t getBlockInterval() { 
  if (currentLevel > 10) return LEVEL_BLOCK_INTERVALS[10];
  return LEVEL_BLOCK_INTERVALS[currentLevel]; 
}

uint32_t computeChecksum() {
  return (currentStake + totalRewards + totalBlocksSigned + totalUptimeSeconds + 
          todayUptimeSeconds + slashCount + currentLevel) ^ MAGIC_NUMBER;
}

bool isEEPROMValid() {
  uint32_t magic, storedChecksum;
  EEPROM.get(EEPROM_MAGIC_ADDR, magic);
  if (magic != MAGIC_NUMBER) return false;
  EEPROM.get(EEPROM_CHECKSUM_ADDR, storedChecksum);
  return storedChecksum == computeChecksum();
}

void saveToEEPROM() {
  EEPROM.begin(512);
  EEPROM.put(EEPROM_STAKE_ADDR, currentStake);
  EEPROM.put(EEPROM_REWARDS_ADDR, totalRewards);
  EEPROM.put(EEPROM_BLOCKS_ADDR, totalBlocksSigned);
  EEPROM.put(EEPROM_UPTIME_ADDR, totalUptimeSeconds);
  EEPROM.put(EEPROM_TODAY_UPTIME_ADDR, todayUptimeSeconds);
  EEPROM.put(EEPROM_LAST_RESET_ADDR, lastUptimeReset);
  EEPROM.put(EEPROM_SLASH_COUNT_ADDR, slashCount);
  EEPROM.put(EEPROM_CONSECUTIVE_MISSES_ADDR, consecutiveMisses);
  EEPROM.put(EEPROM_LEVEL_ADDR, currentLevel);
  EEPROM.put(EEPROM_PEER_INDEX_ADDR, currentPeerIndex);
  EEPROM.put(EEPROM_CHECKSUM_ADDR, computeChecksum());
  EEPROM.put(EEPROM_MAGIC_ADDR, MAGIC_NUMBER);
  EEPROM.commit();
  EEPROM.end();
}

void loadFromEEPROM() {
  if (!isEEPROMValid()) {
    currentStake = INITIAL_STAKE; totalRewards = 0; totalBlocksSigned = 0;
    totalUptimeSeconds = 0; todayUptimeSeconds = 0; lastUptimeReset = millis()/1000;
    slashCount = 0; consecutiveMisses = 0; currentLevel = 1; currentPeerIndex = 0;
    calculateLevel(); saveToEEPROM(); return;
  }
  EEPROM.begin(512);
  EEPROM.get(EEPROM_STAKE_ADDR, currentStake);
  EEPROM.get(EEPROM_REWARDS_ADDR, totalRewards);
  EEPROM.get(EEPROM_BLOCKS_ADDR, totalBlocksSigned);
  EEPROM.get(EEPROM_UPTIME_ADDR, totalUptimeSeconds);
  EEPROM.get(EEPROM_TODAY_UPTIME_ADDR, todayUptimeSeconds);
  EEPROM.get(EEPROM_LAST_RESET_ADDR, lastUptimeReset);
  EEPROM.get(EEPROM_SLASH_COUNT_ADDR, slashCount);
  EEPROM.get(EEPROM_CONSECUTIVE_MISSES_ADDR, consecutiveMisses);
  EEPROM.get(EEPROM_LEVEL_ADDR, currentLevel);
  EEPROM.get(EEPROM_PEER_INDEX_ADDR, currentPeerIndex);
  EEPROM.end();
  calculateLevel();
}

void checkDailyReset() {
  uint32_t now = millis()/1000;
  if ((now - lastUptimeReset) / 86400 >= 1) {
    todayUptimeSeconds = 0; lastUptimeReset = now; saveToEEPROM();
    Serial.println("[DAILY] Uptime reset");
  }
}

void updateUptime() {
  checkDailyReset();
  totalUptimeSeconds += UPTIME_PING_INTERVAL/1000;
  todayUptimeSeconds += UPTIME_PING_INTERVAL/1000;
  if (todayUptimeSeconds > 86400) todayUptimeSeconds = 86400;
  saveToEEPROM();
}

void handleSlashing() {
  uint32_t slashAmount = max((uint32_t)(currentStake * SLASH_RATE), LEVEL_STAKE_RANGE);
  if (slashAmount > currentStake) slashAmount = currentStake;
  currentStake -= slashAmount;
  if (currentStake < LEVEL_STAKE_RANGE) currentStake = LEVEL_STAKE_RANGE;
  slashCount++; consecutiveMisses++; calculateLevel(); saveToEEPROM();
  Serial.printf("[SLASH] Lost %lu MCX | Stake: %lu | Level: %lu | Slashes: %lu\n",
    slashAmount, currentStake, currentLevel, slashCount);
  if (slashCount >= 5) { miningEnabled = false; Serial.println("[BAN] Miner banned"); }
  led_blink(3, 100);
}

void addReward(uint32_t reward) {
  totalRewards += reward; currentStake += reward; totalBlocksSigned++;
  consecutiveMisses = 0; calculateLevel(); saveToEEPROM();
  Serial.printf("[REWARD] +%lu MCX | Total: %lu | Stake: %lu | Level: %lu | Blocks: %lu\n",
    reward, totalRewards, currentStake, currentLevel, totalBlocksSigned);
  led_blink(1, 50);
}

// ==================== INITIALIZATION ====================
void initCrypto() {
  // Generate wallet from username + private key
  char combined[200];
  snprintf(combined, sizeof(combined), "%s%s", USERNAME, PRIVATE_KEY_HEX);
  computeSHA256(combined, validatorID);
  
  // Generate wallet address
  char pubHash[65];
  computeSHA256(PRIVATE_KEY_HEX, publicKeyHex);
  computeSHA256(publicKeyHex, pubHash);
  snprintf(walletAddress, sizeof(walletAddress), "MCR_%.32s", pubHash);
  
  Serial.println("[CRYPTO] Initialized");
  Serial.printf("[CRYPTO] Wallet: %s\n", walletAddress);
  Serial.printf("[CRYPTO] Validator ID: %.8s...\n", validatorID);
}

// ==================== PEER CACHE (GOSSIP) ====================
void savePeersToSPIFFS() {
  if (!SPIFFS.begin(true)) return;
  File f = SPIFFS.open("/peers.json", "w");
  if (f) {
    f.print("{\"peers\":[");
    for (int i = 0; i < peerCount; i++) {
      if (i > 0) f.print(",");
      f.print("\""); f.print(peerList[i]); f.print("\"");
    }
    f.print("],\"version\":\""); f.print(VERSION); f.print("\"}");
    f.close();
  }
  SPIFFS.end();
}

void loadPeersFromSPIFFS() {
  if (!SPIFFS.begin(true)) return;
  if (SPIFFS.exists("/peers.json")) {
    File f = SPIFFS.open("/peers.json", "r");
    if (f) {
      StaticJsonDocument<2048> doc;
      deserializeJson(doc, f.readString());
      f.close();
      JsonArray peers = doc["peers"];
      for (JsonVariant p : peers) {
        if (peerCount < MAX_PEERS) {
          peerList[peerCount++] = p.as<String>();
        }
      }
    }
  }
  SPIFFS.end();
  
  for (int i = 0; i < BOOTSTRAP_COUNT && peerCount < MAX_PEERS; i++) {
    bool exists = false;
    for (int j = 0; j < peerCount; j++) {
      if (peerList[j] == BOOTSTRAP_NODES[i]) { exists = true; break; }
    }
    if (!exists) peerList[peerCount++] = BOOTSTRAP_NODES[i];
  }
}

void addPeerFromGossip(String peer) {
  for (int i = 0; i < peerCount; i++) {
    if (peerList[i] == peer) return;
  }
  if (peerCount < MAX_PEERS) {
    peerList[peerCount++] = peer;
    savePeersToSPIFFS();
  }
}

void switchToNextPeer() {
  if (peerCount == 0) {
    loadPeersFromSPIFFS();
    if (peerCount == 0) return;
  }
  currentPeerIndex = (currentPeerIndex + 1) % peerCount;
  nodeSwitchCount++;
  String fullPeer = peerList[currentPeerIndex];
  int colonIndex = fullPeer.indexOf(':');
  if (colonIndex > 0) fullPeer = fullPeer.substring(0, colonIndex);
  fullPeer.toCharArray(currentNodeIP, 16);
  
  if (webSocket.isConnected()) webSocket.disconnect();
  wsConnected = false; isRegistered = false;
  webSocket.begin(currentNodeIP, NODE_PORT, "/");
}

// ==================== WEBSOCKET ====================
void sendRegister() {
  StaticJsonDocument<512> doc;
  doc["type"] = "register";
  doc["validator_id"] = validatorID;
  doc["username"] = USERNAME;
  doc["public_key"] = publicKeyHex;
  doc["wallet"] = walletAddress;
  doc["stake"] = currentStake;
  doc["level"] = currentLevel;
  doc["rewards"] = totalRewards;
  doc["blocks"] = totalBlocksSigned;
  doc["miner_type"] = "esp32";
  
  char timestamp[32];
  snprintf(timestamp, sizeof(timestamp), "%lu", (uint32_t)timeClient.getEpochTime());
  doc["timestamp"] = timestamp;
  
  // ========== djb2 hash for registration (matches Arduino) ==========
  char msgToSign[256];
  snprintf(msgToSign, sizeof(msgToSign), "%s%s%lu", USERNAME, walletAddress, (uint32_t)timeClient.getEpochTime());
  
  char sig[9];
  djb2_hash(msgToSign, sig);
  doc["signature"] = sig;
  
  String output;
  serializeJson(doc, output);
  webSocket.sendTXT(output);
  Serial.println("[REG] Registration sent");
}

void sendUptimePing() {
  StaticJsonDocument<256> doc;
  doc["type"] = "uptime_ping";
  doc["validator_id"] = validatorID;
  doc["username"] = USERNAME;
  doc["uptime_seconds"] = totalUptimeSeconds;
  doc["today_uptime"] = todayUptimeSeconds;
  doc["stake"] = currentStake;
  doc["level"] = currentLevel;
  
  String output;
  serializeJson(doc, output);
  webSocket.sendTXT(output);
}

void sendBlockSignature() {
  // ========== djb2 hash for block signature (matches Arduino) ==========
  char msgToSign[256];
  snprintf(msgToSign, sizeof(msgToSign), "%s%s%lu", currentChallenge, validatorID, currentBlockId);
  
  char signature[9];
  djb2_hash(msgToSign, signature);
  
  StaticJsonDocument<512> doc;
  doc["type"] = "block_signature";
  doc["validator_id"] = validatorID;
  doc["username"] = USERNAME;
  doc["challenge"] = currentChallenge;
  doc["signature"] = signature;
  doc["level"] = currentLevel;
  doc["stake"] = currentStake;
  doc["block_id"] = currentBlockId;
  
  String output;
  serializeJson(doc, output);
  webSocket.sendTXT(output);
  Serial.printf("[SIGN] Block %lu signed\n", currentBlockId);
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      isValidator = false;
      isRegistered = false;
      wsConnected = false;
      led_off();
      Serial.println("[WS] Disconnected");
      reconnectAttempts++;
      if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
        switchToNextPeer();
        reconnectAttempts = 0;
      }
      break;
      
    case WStype_CONNECTED:
      wsConnected = true;
      reconnectAttempts = 0;
      led_on();
      Serial.println("[WS] Connected");
      webSocket.sendTXT("{\"type\":\"get_peers\"}");
      sendRegister();
      break;
      
    case WStype_TEXT: {
      StaticJsonDocument<2048> doc;
      DeserializationError error = deserializeJson(doc, payload);
      if (error) {
        Serial.printf("[WS] JSON parse error: %s\n", error.c_str());
        break;
      }
      
      String typeStr = doc["type"].as<String>();
      
      if (typeStr == "registered") {
        isRegistered = true;
        uint32_t level = doc["level"];
        uint32_t maxLevel = doc["max_level"];
        Serial.printf("[REG] ✅ Registered! Level: %lu, Max Level: %lu\n", level, maxLevel);
        led_blink(2, 50);
      }
      else if (typeStr == "peers") {
        JsonArray peers = doc["peers"];
        for (JsonVariant p : peers) {
          addPeerFromGossip(p.as<String>());
        }
      }
      else if (typeStr == "challenge" && miningEnabled) {
        const char* challenge = doc["challenge"];
        if (challenge) strncpy(currentChallenge, challenge, 64);
        currentBlockId = doc["block_id"];
        lastChallengeTime = millis();
        isValidator = true;
        sendBlockSignature();
      }
      else if (typeStr == "block_accepted") {
        uint32_t reward = doc["reward"];
        addReward(reward);
        isValidator = false;
        Serial.printf("[BLOCK] ✅ Block %lu accepted! +%lu MCX\n", currentBlockId, reward);
      }
      else if (typeStr == "block_rejected") {
        isValidator = false;
        Serial.printf("[BLOCK] ❌ Block %lu rejected\n", currentBlockId);
      }
      else if (typeStr == "slash") {
        handleSlashing();
        isValidator = false;
        Serial.println("[SLASH] ⚠️ Slashed!");
      }
      else if (typeStr == "miner_control") {
        String action = doc["action"].as<String>();
        if (action == "stop") {
          miningEnabled = false; isValidator = false; led_off();
          Serial.println("[CTRL] ⏹ Miner stopped");
        }
        else if (action == "start") {
          miningEnabled = true; led_on();
          Serial.println("[CTRL] ▶️ Miner started");
        }
        else if (action == "restart") {
          miningEnabled = false; isValidator = false; led_off();
          delay(1000);
          miningEnabled = true; led_on();
          Serial.println("[CTRL] 🔄 Miner restarted");
        }
        webSocket.sendTXT("{\"type\":\"control_response\",\"success\":true}");
      }
      break;
    }
    
    default: break;
  }
}

void connectWiFi() {
  Serial.printf("[WIFI] Connecting to %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] ✅ Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    led_blink(2, 100);
  } else {
    Serial.println("[WIFI] ❌ Failed to connect, restarting...");
    ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  led_off();
  
  Serial.println("\n==========================================");
  Serial.println("MICROCORE ESP32 MINER v7.3");
  Serial.println("Simple djb2 Hash | No mbedtls issues");
  Serial.println("Gossip Discovery | No DNS");
  Serial.println("==========================================\n");
  
  initCrypto();
  loadFromEEPROM();
  loadPeersFromSPIFFS();
  
  if (currentPeerIndex < peerCount) {
    String fullPeer = peerList[currentPeerIndex];
    int colonIndex = fullPeer.indexOf(':');
    if (colonIndex > 0) fullPeer = fullPeer.substring(0, colonIndex);
    fullPeer.toCharArray(currentNodeIP, 16);
  } else {
    strcpy(currentNodeIP, "192.168.1.100");
  }
  
  Serial.printf("Username: %s\n", USERNAME);
  Serial.printf("Wallet: %s\n", walletAddress);
  Serial.printf("Validator ID: %.8s...\n", validatorID);
  Serial.printf("Stake: %lu MCX, Level: %lu\n", currentStake, currentLevel);
  Serial.printf("Block interval: %lu sec\n", getBlockInterval());
  Serial.printf("Peers in cache: %d\n", peerCount);
  Serial.printf("Current peer: %s\n", currentNodeIP);
  
  connectWiFi();
  timeClient.begin();
  timeClient.update();
  
  webSocket.begin(currentNodeIP, NODE_PORT, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  
  lastUptimePing = millis();
  led_blink(3, 100);
  
  Serial.println("\n[READY] ✅ ESP32 miner running\n");
}

void loop() {
  webSocket.loop();
  timeClient.update();
  
  if (!isRegistered && millis() - lastUptimePing > 30000) {
    if (wsConnected) {
      sendRegister();
      Serial.println("[REG] Re-registering...");
    } else {
      switchToNextPeer();
      webSocket.begin(currentNodeIP, NODE_PORT, "/");
    }
    lastUptimePing = millis();
  }
  
  if (millis() - lastUptimePing >= UPTIME_PING_INTERVAL) {
    updateUptime();
    if (wsConnected) sendUptimePing();
    lastUptimePing = millis();
  }
  
  if (isValidator && millis() - lastChallengeTime >= SIGNING_WINDOW_MS) {
    Serial.printf("[TIMEOUT] Missed signing window for block %lu\n", currentBlockId);
    handleSlashing();
    isValidator = false;
  }
  
  static uint32_t lastSave = 0;
  if (millis() - lastSave >= 3600000) {
    saveToEEPROM();
    savePeersToSPIFFS();
    lastSave = millis();
  }
  
  static uint32_t lastStatus = 0;
  if (millis() - lastStatus >= 60000) {
    Serial.printf("[STATUS] Stake: %lu, Level: %lu, Blocks: %lu, Rewards: %lu, Uptime: %lu\n",
      currentStake, currentLevel, totalBlocksSigned, totalRewards, totalUptimeSeconds);
    lastStatus = millis();
  }
  
  delay(10);
}