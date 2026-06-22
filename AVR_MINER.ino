

#include <EEPROM.h>
#include <avr/wdt.h>

// ==================== USER CONFIGURATION ====================
// EDIT THESE BEFORE UPLOADING
const char USERNAME[] PROGMEM = "XAVER123";                    // ← CHANGE
const char PRIVATE_KEY[] PROGMEM = "MCR_A87D9AF718F62C8D073FDDFE6BC0F039";  // ← CHANGE

// ==================== NETWORK CONSTANTS ====================
#define SYMBOL "MCX"
#define VERSION "7.0-MAINNET"
#define MIN_VALIDATORS_PER_BLOCK 10
#define SIGNING_WINDOW_MS 2500
#define SLASH_RATE 0.10
#define BAN_THRESHOLD 5
#define UPTIME_PING_INTERVAL 30000
#define RE_REGISTER_INTERVAL 30000
#define MAX_LEVEL 10
#define LEVEL_STAKE_RANGE 1000
#define INITIAL_STAKE 1000
#define DAILY_SECONDS 86400
#define LED_PIN 13
#define LED_ON HIGH
#define LED_OFF LOW

// ==================== LEVEL BLOCK INTERVALS ====================
const uint16_t LEVEL_BLOCK_INTERVALS[] = {0, 40, 35, 30, 25, 20, 15, 10, 9, 8, 7};

// ==================== EEPROM ADDRESSES ====================
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
#define EEPROM_MINER_VERSION_ADDR 44

#define MAGIC_NUMBER 0xA5A5A5A5

// ==================== GLOBAL VARIABLES ====================
uint32_t stake, rewards, blocks, uptime, todayUptime, lastReset;
uint32_t level, lastPing, lastChallenge, blockId, lastRegAttempt;
uint32_t consecutiveMisses, slashCount;
uint32_t lastBlockTime, blocksMissed, blocksAttempted;
uint8_t isValidator, isRegistered, miningEnabled, isBanned;
uint8_t currentLevelIndex, reconnectAttempts, powerSavingMode;

char vid[17];
char wallet[17];
char challenge[33];
char jsonBuf[250];
char tempBuf[50];

// ==================== LED FUNCTIONS ====================
void led_init() { pinMode(LED_PIN, OUTPUT); led_off(); }
void led_on() { digitalWrite(LED_PIN, LED_ON); }
void led_off() { digitalWrite(LED_PIN, LED_OFF); }
void led_blink(uint8_t n, uint16_t d) {
  for (uint8_t i = 0; i < n; i++) {
    led_on();
    delay(d);
    led_off();
    delay(d);
  }
}
void led_status_indicator(uint8_t mode) {
  // mode: 0=idle, 1=mining, 2=validator, 3=error, 4=banned
  switch(mode) {
    case 0: led_off(); break;
    case 1: led_on(); break;
    case 2: led_blink(1, 100); break;
    case 3: led_blink(5, 50); break;
    case 4: led_on(); delay(2000); led_off(); delay(500); break;
  }
}

// ==================== DJB2 HASH (8 chars) ====================
void djb2_hash(const char* in, char* out) {
  uint32_t h = 5381;
  uint8_t i = 0;
  while (in[i]) {
    h = ((h << 5) + h) + in[i];
    i++;
  }
  // Add randomness from hardware
  h = ((h << 5) + h) + millis();
  h = ((h << 5) + h) + analogRead(A0);
  sprintf(out, "%08lx", h);
}

// ==================== EEPROM MANAGEMENT ====================
uint32_t calcChecksum() {
  return (uint32_t)(stake + rewards + blocks + uptime + todayUptime + 
                    slashCount + consecutiveMisses + level);
}

void saveEEPROM() {
  EEPROM.put(EEPROM_STAKE_ADDR, stake);
  EEPROM.put(EEPROM_REWARDS_ADDR, rewards);
  EEPROM.put(EEPROM_BLOCKS_ADDR, blocks);
  EEPROM.put(EEPROM_UPTIME_ADDR, uptime);
  EEPROM.put(EEPROM_TODAY_UPTIME_ADDR, todayUptime);
  EEPROM.put(EEPROM_LAST_RESET_ADDR, lastReset);
  EEPROM.put(EEPROM_SLASH_COUNT_ADDR, slashCount);
  EEPROM.put(EEPROM_CONSECUTIVE_MISSES_ADDR, consecutiveMisses);
  EEPROM.put(EEPROM_LEVEL_ADDR, level);
  EEPROM.put(EEPROM_CHECKSUM_ADDR, calcChecksum());
  EEPROM.put(EEPROM_MAGIC_ADDR, MAGIC_NUMBER);
  EEPROM.put(EEPROM_MINER_VERSION_ADDR, 0x70);
}

uint8_t loadEEPROM() {
  uint32_t magic;
  uint32_t chk;
  
  EEPROM.get(EEPROM_MAGIC_ADDR, magic);
  EEPROM.get(EEPROM_CHECKSUM_ADDR, chk);
  
  if (magic != MAGIC_NUMBER || chk != calcChecksum()) {
    // Initialize with defaults
    stake = INITIAL_STAKE;
    rewards = 0;
    blocks = 0;
    uptime = 0;
    todayUptime = 0;
    lastReset = millis() / 1000;
    slashCount = 0;
    consecutiveMisses = 0;
    level = 1;
    saveEEPROM();
    return 0;
  }
  
  EEPROM.get(EEPROM_STAKE_ADDR, stake);
  EEPROM.get(EEPROM_REWARDS_ADDR, rewards);
  EEPROM.get(EEPROM_BLOCKS_ADDR, blocks);
  EEPROM.get(EEPROM_UPTIME_ADDR, uptime);
  EEPROM.get(EEPROM_TODAY_UPTIME_ADDR, todayUptime);
  EEPROM.get(EEPROM_LAST_RESET_ADDR, lastReset);
  EEPROM.get(EEPROM_SLASH_COUNT_ADDR, slashCount);
  EEPROM.get(EEPROM_CONSECUTIVE_MISSES_ADDR, consecutiveMisses);
  EEPROM.get(EEPROM_LEVEL_ADDR, level);
  
  return 1;
}

// ==================== LEVEL MANAGEMENT ====================
void calcLevel() {
  level = (stake < LEVEL_STAKE_RANGE) ? 1 : ((stake - 1) / LEVEL_STAKE_RANGE) + 1;
  if (level < 1) level = 1;
  if (level > MAX_LEVEL) level = MAX_LEVEL;
}

uint16_t getBlockInterval() {
  uint8_t idx = (level > MAX_LEVEL) ? MAX_LEVEL : level;
  return LEVEL_BLOCK_INTERVALS[idx];
}

uint8_t isLevelUnlocked(uint8_t targetLevel) {
  // Check if miner has enough stake for target level
  return (stake >= targetLevel * LEVEL_STAKE_RANGE);
}

uint8_t getMaxUnlockedLevel() {
  uint8_t maxLevel = stake / LEVEL_STAKE_RANGE;
  if (maxLevel > MAX_LEVEL) maxLevel = MAX_LEVEL;
  return maxLevel;
}

// ==================== UPTIME MANAGEMENT ====================
void checkDailyReset() {
  uint32_t now = millis() / 1000;
  if ((now - lastReset) >= DAILY_SECONDS) {
    todayUptime = 0;
    lastReset = now;
    saveEEPROM();
  }
}

void updateUptime() {
  checkDailyReset();
  uptime += (UPTIME_PING_INTERVAL / 1000);
  todayUptime += (UPTIME_PING_INTERVAL / 1000);
  if (todayUptime > DAILY_SECONDS) todayUptime = DAILY_SECONDS;
  saveEEPROM();
}

// ==================== SLASHING ====================
void handleSlash(const char* reason) {
  uint32_t slashAmount = (uint32_t)(stake * SLASH_RATE);
  if (slashAmount < LEVEL_STAKE_RANGE) slashAmount = LEVEL_STAKE_RANGE;
  if (slashAmount > stake) slashAmount = stake;
  
  stake -= slashAmount;
  if (stake < LEVEL_STAKE_RANGE) stake = LEVEL_STAKE_RANGE;
  
  slashCount++;
  consecutiveMisses++;
  calcLevel();
  saveEEPROM();
  
  // Send slash notification via serial
  char slashBuf[100];
  snprintf_P(slashBuf, sizeof(slashBuf),
    PSTR("{\"type\":\"slash_event\",\"amount\":%lu,\"stake\":%lu,\"level\":%lu,\"slashes\":%lu,\"reason\":\"%s\"}"),
    slashAmount, stake, level, slashCount, reason);
  sendJson(slashBuf);
  
  if (slashCount >= BAN_THRESHOLD) {
    isBanned = 1;
    miningEnabled = 0;
    led_status_indicator(4);
  }
}

// ==================== REWARDS ====================
void addReward(uint32_t reward) {
  rewards += reward;
  stake += reward;
  blocks++;
  consecutiveMisses = 0;
  calcLevel();
  saveEEPROM();
  blocksAttempted++;
}

void recordMiss() {
  consecutiveMisses++;
  blocksMissed++;
  blocksAttempted++;
}

// ==================== JSON BUILDERS ====================
void buildRegister(char* buf) {
  char username[13];
  char priv[33];
  
  strcpy_P(username, USERNAME);
  strcpy_P(priv, PRIVATE_KEY);
  
  // Generate VID
  char combo[50];
  sprintf(combo, "%s%s", username, priv);
  djb2_hash(combo, vid);
  
  // Generate Wallet
  char wHash[9];
  djb2_hash(vid, wHash);
  sprintf(wallet, "MCR_%.8s", wHash);
  
  // Generate timestamp
  uint32_t timestamp = millis() / 1000;
  
  // Generate signature
  char msg[100];
  sprintf(msg, "%s%s%lu", username, wallet, timestamp);
  char sigInput[150];
  sprintf(sigInput, "%s%s", priv, msg);
  char sig[9];
  djb2_hash(sigInput, sig);
  
  // Build JSON
  snprintf_P(buf, 250,
    PSTR("{\"type\":\"register\","
         "\"validator_id\":\"%s\","
         "\"public_key\":\"%s\","
         "\"username\":\"%s\","
         "\"wallet\":\"%s\","
         "\"stake\":%lu,"
         "\"level\":%lu,"
         "\"rewards\":%lu,"
         "\"blocks\":%lu,"
         "\"uptime\":%lu,"
         "\"today_uptime\":%lu,"
         "\"miner_type\":\"uno\","
         "\"version\":\"%s\","
         "\"timestamp\":%lu,"
         "\"signature\":\"%s\"}"),
    vid, priv, username, wallet, stake, level, rewards, blocks,
    uptime, todayUptime, VERSION, timestamp, sig);
}

void buildUptime(char* buf) {
  snprintf_P(buf, 200,
    PSTR("{\"type\":\"uptime_ping\","
         "\"validator_id\":\"%s\","
         "\"username\":\"%s\","
         "\"uptime_seconds\":%lu,"
         "\"today_uptime\":%lu,"
         "\"stake\":%lu,"
         "\"level\":%lu,"
         "\"blocks_signed\":%lu}"),
    vid, USERNAME, uptime, todayUptime, stake, level, blocks);
}

void buildSignature(char* buf) {
  char sigMsg[100];
  sprintf(sigMsg, "%s%s%lu", challenge, vid, blockId);
  char sig[9];
  djb2_hash(sigMsg, sig);
  
  snprintf_P(buf, 200,
    PSTR("{\"type\":\"block_signature\","
         "\"validator_id\":\"%s\","
         "\"username\":\"%s\","
         "\"challenge\":\"%s\","
         "\"signature\":\"%s\","
         "\"block_id\":%lu,"
         "\"level\":%lu,"
         "\"stake\":%lu}"),
    vid, USERNAME, challenge, sig, blockId, level, stake);
}

void buildStatus(char* buf) {
  snprintf_P(buf, 200,
    PSTR("{\"type\":\"miner_status\","
         "\"validator_id\":\"%s\","
         "\"username\":\"%s\","
         "\"wallet\":\"%s\","
         "\"stake\":%lu,"
         "\"level\":%lu,"
         "\"blocks\":%lu,"
         "\"rewards\":%lu,"
         "\"uptime\":%lu,"
         "\"today_uptime\":%lu,"
         "\"slashes\":%lu,"
         "\"misses\":%lu,"
         "\"mining\":%d,"
         "\"banned\":%d}"),
    vid, USERNAME, wallet, stake, level, blocks, rewards,
    uptime, todayUptime, slashCount, consecutiveMisses,
    miningEnabled, isBanned);
}

// ==================== SEND JSON ====================
void sendJson(const char* buf) {
  if (buf[0] == '{') {
    Serial.println(buf);
  }
}

// ==================== PROCESS MESSAGES ====================
void processMessage(const char* buf) {
  // Registration response
  if (strstr_P(buf, PSTR("\"type\":\"registered\""))) {
    isRegistered = 1;
    isBanned = 0;
    led_blink(2, 50);
    
    // Parse level and reward from response
    const char* lStart = strstr_P(buf, PSTR("\"level\":"));
    if (lStart) {
      lStart += 8;
      uint32_t newLevel = 0;
      while (*lStart >= '0' && *lStart <= '9') {
        newLevel = newLevel * 10 + (*lStart - '0');
        lStart++;
      }
      if (newLevel > level) {
        level = newLevel;
        saveEEPROM();
      }
    }
    return;
  }
  
  // Challenge
  if (strstr_P(buf, PSTR("\"type\":\"challenge\""))) {
    if (!miningEnabled || isBanned) return;
    
    const char* cStart = strstr_P(buf, PSTR("\"challenge\":\""));
    if (cStart) {
      cStart += 12;
      uint8_t i = 0;
      while (*cStart && *cStart != '"' && i < 32) {
        challenge[i++] = *cStart++;
      }
      challenge[i] = 0;
    }
    
    const char* bStart = strstr_P(buf, PSTR("\"block_id\":"));
    if (bStart) {
      bStart += 11;
      blockId = 0;
      while (*bStart >= '0' && *bStart <= '9') {
        blockId = blockId * 10 + (*bStart - '0');
        bStart++;
      }
    }
    
    lastChallenge = millis();
    isValidator = 1;
    led_status_indicator(2);
    
    char sigBuf[200];
    buildSignature(sigBuf);
    sendJson(sigBuf);
    return;
  }
  
  // Block accepted
  if (strstr_P(buf, PSTR("\"type\":\"block_accepted\""))) {
    uint32_t reward = 0;
    const char* rStart = strstr_P(buf, PSTR("\"reward\":"));
    if (rStart) {
      rStart += 8;
      while (*rStart >= '0' && *rStart <= '9') {
        reward = reward * 10 + (*rStart - '0');
        rStart++;
      }
    }
    
    addReward(reward);
    isValidator = 0;
    led_blink(1, 50);
    return;
  }
  
  // Block rejected
  if (strstr_P(buf, PSTR("\"type\":\"block_rejected\""))) {
    isValidator = 0;
    led_status_indicator(1);
    return;
  }
  
  // Slash
  if (strstr_P(buf, PSTR("\"type\":\"slash\""))) {
    const char* rStart = strstr_P(buf, PSTR("\"reason\":\""));
    char reason[32] = "Node slashing";
    if (rStart) {
      rStart += 10;
      uint8_t i = 0;
      while (*rStart && *rStart != '"' && i < 31) {
        reason[i++] = *rStart++;
      }
      reason[i] = 0;
    }
    handleSlash(reason);
    isValidator = 0;
    return;
  }
  
  // Control
  if (strstr_P(buf, PSTR("\"type\":\"miner_control\""))) {
    const char* aStart = strstr_P(buf, PSTR("\"action\":\""));
    if (aStart) {
      aStart += 10;
      if (strncmp_P(aStart, PSTR("stop"), 3) == 0) {
        miningEnabled = 0;
        isValidator = 0;
        led_off();
        sendJson(PSTR("{\"type\":\"control_response\",\"success\":true,\"action\":\"stop\"}"));
      }
      else if (strncmp_P(aStart, PSTR("start"), 4) == 0) {
        miningEnabled = 1;
        isBanned = 0;
        led_on();
        sendJson(PSTR("{\"type\":\"control_response\",\"success\":true,\"action\":\"start\"}"));
        // Re-register after start
        char regBuf[250];
        buildRegister(regBuf);
        sendJson(regBuf);
      }
      else if (strncmp_P(aStart, PSTR("restart"), 7) == 0) {
        miningEnabled = 0;
        isValidator = 0;
        led_off();
        delay(1000);
        miningEnabled = 1;
        isBanned = 0;
        led_on();
        char regBuf[250];
        buildRegister(regBuf);
        sendJson(regBuf);
        sendJson(PSTR("{\"type\":\"control_response\",\"success\":true,\"action\":\"restart\"}"));
      }
    }
    return;
  }
  
  // Status request
  if (strstr_P(buf, PSTR("\"type\":\"get_status\""))) {
    char statusBuf[200];
    buildStatus(statusBuf);
    sendJson(statusBuf);
    return;
  }
  
  // Level update
  if (strstr_P(buf, PSTR("\"type\":\"level_update\""))) {
    const char* sStart = strstr_P(buf, PSTR("\"stake\":"));
    if (sStart) {
      sStart += 8;
      uint32_t newStake = 0;
      while (*sStart >= '0' && *sStart <= '9') {
        newStake = newStake * 10 + (*sStart - '0');
        sStart++;
      }
      if (newStake != stake) {
        stake = newStake;
        calcLevel();
        saveEEPROM();
      }
    }
    return;
  }
}

// ==================== SERIAL INPUT ====================
void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    static uint16_t idx = 0;
    
    if (c == '\n' || c == '\r') {
      if (idx > 0) {
        jsonBuf[idx] = 0;
        processMessage(jsonBuf);
        idx = 0;
      }
    } else if (idx < 249) {
      jsonBuf[idx++] = c;
    }
  }
}

// ==================== STATUS REPORT ====================
void printStatus() {
  char statusBuf[200];
  buildStatus(statusBuf);
  sendJson(statusBuf);
  
  // Also print debug info
  snprintf_P(tempBuf, sizeof(tempBuf),
    PSTR("[STATUS] Level:%lu Interval:%u Stake:%lu Blocks:%lu Rewards:%lu Uptime:%lu"),
    level, getBlockInterval(), stake, blocks, rewards, uptime);
  Serial.println(tempBuf);
}

// ==================== SETUP ====================
void setup() {
  // Initialize
  led_init();
  Serial.begin(115200);
  delay(2000);
  
  // Initialize random seed
  randomSeed(analogRead(0) + millis());
  
  // Load EEPROM
  loadEEPROM();
  calcLevel();
  
  // Set initial state
  isRegistered = 0;
  isValidator = 0;
  isBanned = 0;
  miningEnabled = 1;
  lastPing = millis();
  lastRegAttempt = 0;
  blocksAttempted = 0;
  blocksMissed = 0;
  
  // Send registration
  char regBuf[250];
  buildRegister(regBuf);
  sendJson(regBuf);
  
  // Startup blink pattern
  led_blink(3, 100);
  delay(200);
  led_blink(2, 50);
  
  // Print startup info
  Serial.println(F("{\"type\":\"miner_startup\",\"version\":\"" VERSION "\",\"level\":"));
  Serial.print(level);
  Serial.print(F(",\"stake\":"));
  Serial.print(stake);
  Serial.print(F(",\"block_interval\":"));
  Serial.print(getBlockInterval());
  Serial.println(F(",\"validator_id\":\"") + String(vid) + "\"}");
}

// ==================== LOOP ====================
void loop() {
  // Process incoming messages
  readSerial();
  
  // Check for ban expiration (auto-recover after 24 hours)
  if (isBanned && millis() - lastReset > DAILY_SECONDS * 3) {
    isBanned = 0;
    miningEnabled = 1;
    slashCount = 0;
    saveEEPROM();
  }
  
  // Send uptime ping every 30 seconds
  if (millis() - lastPing >= UPTIME_PING_INTERVAL) {
    lastPing = millis();
    updateUptime();
    
    if (isRegistered) {
      char upBuf[200];
      buildUptime(upBuf);
      sendJson(upBuf);
    }
  }
  
  // Re-register if not registered
  if (!isRegistered && millis() - lastRegAttempt >= RE_REGISTER_INTERVAL) {
    if (!isBanned) {
      char regBuf[250];
      buildRegister(regBuf);
      sendJson(regBuf);
    }
    lastRegAttempt = millis();
  }
  
  // Auto-slash if validator missed signing window
  if (isValidator && millis() - lastChallenge >= SIGNING_WINDOW_MS) {
    recordMiss();
    handleSlash("Missed signing window");
    isValidator = 0;
    led_status_indicator(3);
    Serial.print(F("{\"type\":\"auto_slash\",\"block_id\":"));
    Serial.print(blockId);
    Serial.print(F(",\"misses\":"));
    Serial.print(consecutiveMisses);
    Serial.println(F("}"));
  }
  
  // Status indicator
  if (!isBanned) {
    if (miningEnabled) {
      if (isValidator) {
        led_status_indicator(2);
      } else {
        led_status_indicator(1);
      }
    } else {
      led_status_indicator(0);
    }
  }
  
  // Periodic status report (every 5 minutes)
  static uint32_t lastStatusReport = 0;
  if (millis() - lastStatusReport >= 300000) { // 5 minutes
    printStatus();
    lastStatusReport = millis();
  }
  
  // Power saving - reduce CPU usage
  delay(10);
}
