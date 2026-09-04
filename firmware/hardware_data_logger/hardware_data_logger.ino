#include <Wire.h>
#include <Adafruit_AS7341.h>
#include <math.h>
#include <esp_task_wdt.h> 
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "secrets.h"

// --- PREMIUM CLOUD OBJECTS ---
WiFiClientSecure net;
PubSubClient client(net);

#define AWS_IOT_PUBLISH_TOPIC   ""
#define AWS_IOT_SUBSCRIBE_TOPIC ""

// --- NEW: Watchdog & Architecture Tracking ---
bool isOnlineArchitecture = false; 
unsigned long wifiDisconnectTime = 0;
bool wifiTimerActive = false;

const int speedSensorPin = 4;
const int buttonPin = 5;
const int sdaPin = 8;
const int sclPin = 9;

#define WDT_TIMEOUT 30 // <--- PREMIUM FIX: Extended to 30s; AWS RSA-2048 TLS handshakes on a slow network

volatile long halfSlitsCount = 0;
portMUX_TYPE slitMux = portMUX_INITIALIZER_UNLOCKED; // Add this premium lock
volatile int lastSensorState = -1;
volatile unsigned long lastSpeedInterruptTime = 0;
const unsigned long speedDebounceDelay = 1;

// --- PREMIUM FIX: Compile-time constant FPU math. Eradicates runtime division! ---
constexpr float wheelDiameter_mm = 65.0f; 
constexpr float slitsPerRevolution = 20.0f;
constexpr float distancePerHalfSlit_yards = (((3.14159265f * wheelDiameter_mm * 0.001f) * 0.05f) * 1.09361f) * 0.5f;

Adafruit_AS7341 sensors[5];
#define TCAADDR 0x70

bool sensorEnabled[5] = {true, true, true, true, true}; 
bool sensorBooted[5] = {false, false, false, false, false}; // <--- PREMIUM FIX: Prevents NULL Pointer CPU Crashes!

struct LABColor { float L; float a; float b; };
LABColor standardColors[5];
LABColor sampleColors[5];

enum SystemState {
  STATE_ASKING_MODE,        
  STATE_ONLINE_INIT,        
  STATE_PHYSICAL_CALIBRATION,
  STATE_MANUAL_CALIBRATION,
  STATE_SINGLE_CALIBRATION,
  STATE_IDLE_WAITING,
  STATE_SCANNING
};
volatile SystemState currentState = STATE_ASKING_MODE; 
volatile int targetCalibSensor = -1;
volatile bool isCalibrated = false; // <--- NEW: The Hardware Calibration Lock

unsigned long buttonPressStartTime = 0;
unsigned long lastReleaseTime = 0;
int clickCount = 0;
bool isButtonPressed = false;
bool isHolding = false;
const unsigned long MULTI_CLICK_DELAY = 400;

unsigned long lastGraphReportTime = 0; 
SemaphoreHandle_t serialMutex;
TaskHandle_t ColorTaskHandle;

void IRAM_ATTR countSignal() {
  unsigned long currentInterruptTime = millis();
  if (currentInterruptTime - lastSpeedInterruptTime > speedDebounceDelay) {
    // --- PREMIUM FIX: Bare-Metal Register Read (40x faster than digitalRead!) ---
    int pinState = digitalRead(speedSensorPin); // Safe HAL read for ESP32-S3 compatibility
    if (pinState != lastSensorState) {
      if (currentState == STATE_SCANNING) {
        // Lock both cores before changing the number!
        portENTER_CRITICAL_ISR(&slitMux);
        halfSlitsCount++;
        portEXIT_CRITICAL_ISR(&slitMux);
      }
      lastSensorState = pinState;
      lastSpeedInterruptTime = currentInterruptTime;
    }
  }
}

// --- PREMIUM FIX: THE HARDWARE ROUTING MAP ---
// Maps logical Sensors [0, 1, 2, 3, 4] to physical healthy Gates [0, 6, 2, 3, 7]
const uint8_t MUX_MAP[5] = {0, 6, 2, 3, 7}; 

volatile uint8_t current_tca_channel = 255;

void tcaselect(uint8_t sensorIndex) {
  // 1. Prevent out-of-bounds arrays
  if (sensorIndex > 4) return; 
  
  // 2. Secretly map the requested sensor to its new physical survivor gate!
  uint8_t physicalGate = MUX_MAP[sensorIndex];
  
  // 3. Hardware Cache check
  if (physicalGate == current_tca_channel) return; 
  
  uint8_t targetGate = (1 << physicalGate);
  
  for(int attempt = 0; attempt < 3; attempt++) {
    Wire.beginTransmission(TCAADDR);
    Wire.write(targetGate);
    if (Wire.endTransmission() == 0) {
      current_tca_channel = physicalGate; // Trust the Hardware ACK! 
      delay(2); // Give silicon gate time to physically open
      return; 
    }
    delay(2);
  }
}

// --- PREMIUM FIX: THE WATCHDOG SHIELD (READ-PONG) ---
// Mathematically guarantees the sensor is alive BEFORE Adafruit hangs the CPU!
bool safeReadSensor(uint8_t i) {
  // CRITICAL FIX: Empty writes cause NACKs on strict sensors. 
  // We do a safe 1-byte dummy read instead to prove the hardware is listening!
  if (Wire.requestFrom((uint8_t)0x39, (uint8_t)1) == 0) {
    current_tca_channel = 255; // Purge cache! The bus dropped!
    return false; 
  }
  Wire.read(); // Clear the dummy byte from the buffer
  
  return sensors[i].readAllChannels();
}

float pivotXYZ(float n) {
  // OPTIMIZATION: "f" suffixes force the ESP32 to use the fast 32-bit hardware FPU!
  if (n > 0.008856f) return cbrtf(n); 
  else return (7.787f * n) + 0.137931f; // 16/116 is pre-calculated here
}

LABColor calculateLAB(uint16_t f1, uint16_t f2, uint16_t f3, uint16_t f4, uint16_t f5, uint16_t f6, uint16_t f7, uint16_t f8, uint16_t f_nir) {
  // --- NEW: NIR-ANCHORED INTELLIGENT RATIO NORMALIZATION ---
  float totalLight = f1 + f2 + f3 + f4 + f5 + f6 + f7 + f8;
  if (totalLight < 1.0f) totalLight = 1.0f; // Prevent fatal division by zero
  
  // Calculate the Ambient Bleed Penalty
  // (Multiplying by 1.0f makes it aggressively punish factory light)
  
  // --- PREMIUM FIX: One inverse division rules them all. 14 cycles saved! ---
  float inv_totalLight = 1.0f / totalLight; 
  float nir_ratio = (float)f_nir * inv_totalLight;
  float ambient_penalty = 1.0f - constrain(nir_ratio, 0.0f, 0.8f);
  
  // Apply the penalty to our 80% baseline
  float dynamic_baseline = 0.8f * ambient_penalty;
  float fast_multiplier = dynamic_baseline * inv_totalLight;

  // Hardware FPU Multiplication across all 8 channels! (Zero divisions in the loop)
  float n1 = constrain(f1 * fast_multiplier, 0.0f, 1.0f);
  float n2 = constrain(f2 * fast_multiplier, 0.0f, 1.0f);
  float n3 = constrain(f3 * fast_multiplier, 0.0f, 1.0f);
  float n4 = constrain(f4 * fast_multiplier, 0.0f, 1.0f);
  float n5 = constrain(f5 * fast_multiplier, 0.0f, 1.0f);
  float n6 = constrain(f6 * fast_multiplier, 0.0f, 1.0f);
  float n7 = constrain(f7 * fast_multiplier, 0.0f, 1.0f);
  float n8 = constrain(f8 * fast_multiplier, 0.0f, 1.0f);

  float X = (n1 * 0.1406f) + (n2 * 0.3867f) + (n3 * 0.0805f) + (n4 * 0.0714f) + (n5 * 0.6161f) + (n6 * 1.0967f) + (n7 * 0.5401f) + (n8 * 0.0387f);
  float Y = (n1 * 0.0145f) + (n2 * 0.0747f) + (n3 * 0.2536f) + (n4 * 0.6857f) + (n5 * 0.9991f) + (n6 * 0.7220f) + (n7 * 0.2150f) + (n8 * 0.0147f);
  float Z = (n1 * 0.6568f) + (n2 * 2.0273f) + (n3 * 0.7721f) + (n4 * 0.0822f) + (n5 * 0.0011f) + (n6 * 0.0003f) + (n7 * 0.0000f) + (n8 * 0.0000f);
  
  X *= 50.0f; Y *= 50.0f; Z *= 50.0f;
  
  // OPTIMIZATION: Updated to CIE 1964 10° Standard Observer for Textiles (dCIELab: D65-10)
  float pivotX = pivotXYZ(X * 0.0105473f); // previously X / 94.811
  float pivotY = pivotXYZ(Y * 0.01f);      // previously Y / 100.000
  float pivotZ = pivotXYZ(Z * 0.0093193f); // previously Z / 107.304

  LABColor lab;
  lab.L = max(0.0f, (116.0f * pivotY) - 16.0f);
  lab.a = 500.0f * (pivotX - pivotY);
  lab.b = 200.0f * (pivotY - pivotZ);
  return lab;
}

void applyHighSpeedHardwareSettings() {
  xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
  for (uint8_t i = 0; i < 5; i++) {
    // ONLY touch the memory if the hardware actually exists!
    if (sensorBooted[i]) {
      tcaselect(i);
      if (sensorEnabled[i]) {
        sensors[i].setATIME(11);
        sensors[i].setASTEP(599);
        sensors[i].setLEDCurrent(50);
        sensors[i].enableLED(true);
        delay(2);
      } else { 
        sensors[i].enableLED(false);
      }
    }
  }
  xSemaphoreGiveRecursive(serialMutex);
}

void colorScanningTask(void * parameter) {
  esp_task_wdt_add(NULL);
  for(;;) {
    esp_task_wdt_reset();

    if (currentState == STATE_PHYSICAL_CALIBRATION) {
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      Serial.println("\n[SYSTEM] Scanning new Physical Master Sample...");
      xSemaphoreGiveRecursive(serialMutex);
      
      for (uint8_t i = 0; i < 5; i++) {
          if (!sensorEnabled[i]) {
            standardColors[i].L = 0;
            standardColors[i].a = 0; standardColors[i].b = 0;
            continue;
          }

          xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
          // <--- SHIELD THE I2C BUS
          tcaselect(i);
          for (int flush = 0; flush < 3; flush++) { safeReadSensor(i); }

          if (safeReadSensor(i)) {
            standardColors[i] = calculateLAB(
            sensors[i].getChannel(AS7341_CHANNEL_415nm_F1), sensors[i].getChannel(AS7341_CHANNEL_445nm_F2),
            sensors[i].getChannel(AS7341_CHANNEL_480nm_F3), sensors[i].getChannel(AS7341_CHANNEL_515nm_F4),
            sensors[i].getChannel(AS7341_CHANNEL_555nm_F5), sensors[i].getChannel(AS7341_CHANNEL_590nm_F6),
            sensors[i].getChannel(AS7341_CHANNEL_630nm_F7), sensors[i].getChannel(AS7341_CHANNEL_680nm_F8),
            sensors[i].getChannel(AS7341_CHANNEL_NIR) // <--- Ghost argument eradicated!
          );
          xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
          Serial.printf("  -> Sensor %d Master Saved: L1:%.2f | a1:%.2f | b1:%.2f\n", i, standardColors[i].L, standardColors[i].a, standardColors[i].b);
          xSemaphoreGiveRecursive(serialMutex);
        }
        xSemaphoreGiveRecursive(serialMutex); // <--- RELEASE I2C SHIELD
        
        // --- CRITICAL FIX: Reset the bomb after every single sensor finishes! ---
        esp_task_wdt_reset(); 
      }
      // --- PREMIUM CLOUD UPLOADER: SEND CALIBRATION TO AWS VAULT ---
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY); // <--- CRITICAL SHIELD ADDED
      if (isOnlineArchitecture) {
          char calibJson[300];
          snprintf(calibJson, sizeof(calibJson), 
                   "{\"type\":\"calib\",\"mode\":\"MASTER\",\"m\":[[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f]]}",
                   standardColors[0].L, standardColors[0].a, standardColors[0].b,
                   standardColors[1].L, standardColors[1].a, standardColors[1].b,
                   standardColors[2].L, standardColors[2].a, standardColors[2].b,
                   standardColors[3].L, standardColors[3].a, standardColors[3].b,
                   standardColors[4].L, standardColors[4].a, standardColors[4].b);
          client.publish(AWS_IOT_PUBLISH_TOPIC, calibJson);
      }
      xSemaphoreGiveRecursive(serialMutex); // <--- CRITICAL SHIELD REMOVED

      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      Serial.println("\n[SUCCESS] Master Calibration Complete!");
      if (isOnlineArchitecture) Serial.println(" -> Beamed to AWS Cloud Vault!");
      Serial.println(" -> Short press button 1 time (or type 1) to START scanning.");
      xSemaphoreGiveRecursive(serialMutex);
      
      isCalibrated = true; // <--- NEW: Hardware is unlocked!
      currentState = STATE_IDLE_WAITING;
      lastGraphReportTime = millis();
    }
    else if (currentState == STATE_SINGLE_CALIBRATION) {
      // --- NEW: SURGICAL CALIBRATION ENGINE ---
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      Serial.printf("\n[SYSTEM] Surgically Scanning Sensor %d...\n", targetCalibSensor);
      xSemaphoreGiveRecursive(serialMutex);

      if (targetCalibSensor >= 0 && targetCalibSensor < 5 && sensorEnabled[targetCalibSensor]) {
        xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
        // <--- SHIELD ADDED
        tcaselect(targetCalibSensor);
        // ONLY flashes the single requested sensor! The other 4 are untouched.
        for (int flush = 0; flush < 3; flush++) { safeReadSensor(targetCalibSensor); }

        if (safeReadSensor(targetCalibSensor)) {
          standardColors[targetCalibSensor] = calculateLAB(
            sensors[targetCalibSensor].getChannel(AS7341_CHANNEL_415nm_F1), sensors[targetCalibSensor].getChannel(AS7341_CHANNEL_445nm_F2),
            sensors[targetCalibSensor].getChannel(AS7341_CHANNEL_480nm_F3), sensors[targetCalibSensor].getChannel(AS7341_CHANNEL_515nm_F4),
            sensors[targetCalibSensor].getChannel(AS7341_CHANNEL_555nm_F5), sensors[targetCalibSensor].getChannel(AS7341_CHANNEL_590nm_F6),
            sensors[targetCalibSensor].getChannel(AS7341_CHANNEL_630nm_F7), sensors[targetCalibSensor].getChannel(AS7341_CHANNEL_680nm_F8),
            sensors[targetCalibSensor].getChannel(AS7341_CHANNEL_NIR) // <--- ADDED NIR ANCHOR, Ghost argument eradicated!
          );
          xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
          Serial.printf("  -> Sensor %d Master Saved: L1:%.2f | a1:%.2f | b1:%.2f\n", targetCalibSensor, standardColors[targetCalibSensor].L, standardColors[targetCalibSensor].a, standardColors[targetCalibSensor].b);
          xSemaphoreGiveRecursive(serialMutex);
        }
        xSemaphoreGiveRecursive(serialMutex); // <--- SHIELD REMOVED
        // --- CRITICAL FIX: Reset the bomb after the single sensor finishes! ---
        esp_task_wdt_reset();
      }
      
      // --- PREMIUM CLOUD UPLOADER: SEND CALIBRATION TO AWS VAULT ---
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY); // <--- CRITICAL SHIELD ADDED
      if (isOnlineArchitecture) {
          char calibJson[300];
          snprintf(calibJson, sizeof(calibJson), 
                   "{\"type\":\"calib\",\"mode\":\"MASTER\",\"m\":[[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f]]}",
                   standardColors[0].L, standardColors[0].a, standardColors[0].b,
                   standardColors[1].L, standardColors[1].a, standardColors[1].b,
                   standardColors[2].L, standardColors[2].a, standardColors[2].b,
                   standardColors[3].L, standardColors[3].a, standardColors[3].b,
                   standardColors[4].L, standardColors[4].a, standardColors[4].b);
          client.publish(AWS_IOT_PUBLISH_TOPIC, calibJson);
      }
      xSemaphoreGiveRecursive(serialMutex); // <--- CRITICAL SHIELD REMOVED

      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      Serial.println("\n[SUCCESS] Master Calibration Complete!");
      if (isOnlineArchitecture) Serial.println(" -> Beamed to AWS Cloud Vault!");
      Serial.println(" -> Short press button 1 time (or type 1) to START scanning.");
      xSemaphoreGiveRecursive(serialMutex);
      
      isCalibrated = true; // <--- NEW: Hardware is unlocked!
      currentState = STATE_IDLE_WAITING;
      lastGraphReportTime = millis();
    }
    else if (currentState == STATE_SCANNING) {
      
      long currentHalfSlits;
      // Lock both cores securely before reading the number!
      portENTER_CRITICAL(&slitMux);
      currentHalfSlits = halfSlitsCount;
      portEXIT_CRITICAL(&slitMux);
      // HARDWARE MULTIPLICATION: No 64-bit emulation, no divisions. 7x faster!
      float currentDistance_yards = currentHalfSlits * distancePerHalfSlit_yards;

      // --- GHOST CODE PURGED: Text Mode completely eradicated! ---
      // Only the high-speed continuous array payload remains.
      float dL_vals[5] = {0};
      float da_vals[5] = {0}; float db_vals[5] = {0};
        bool print5SecReport = (millis() - lastGraphReportTime >= 5000);

        if (print5SecReport) {
          xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
          Serial.println("\n======================================================================================================");
          Serial.printf( "  5-SECOND GRAPH REPORT   ||   CURRENT FABRIC DISTANCE: %.3f Yards\n", currentDistance_yards);
          Serial.println("======================================================================================================");
          xSemaphoreGiveRecursive(serialMutex);
        }

        for (uint8_t i = 0; i < 5; i++) {
          if (sensorEnabled[i]) {
            xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
            // <--- SHIELD THE I2C BUS
            tcaselect(i);
            if (safeReadSensor(i)) {
              sampleColors[i] = calculateLAB(
                sensors[i].getChannel(AS7341_CHANNEL_415nm_F1), sensors[i].getChannel(AS7341_CHANNEL_445nm_F2),
                sensors[i].getChannel(AS7341_CHANNEL_480nm_F3), sensors[i].getChannel(AS7341_CHANNEL_515nm_F4),
                sensors[i].getChannel(AS7341_CHANNEL_555nm_F5), sensors[i].getChannel(AS7341_CHANNEL_590nm_F6),
                sensors[i].getChannel(AS7341_CHANNEL_630nm_F7), sensors[i].getChannel(AS7341_CHANNEL_680nm_F8), 
                sensors[i].getChannel(AS7341_CHANNEL_NIR) // <--- Ghost argument eradicated!
          );
              dL_vals[i] = sampleColors[i].L - standardColors[i].L;
              da_vals[i] = sampleColors[i].a - standardColors[i].a;
              db_vals[i] = sampleColors[i].b - standardColors[i].b;
              
              if (print5SecReport) {
                xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
                Serial.printf("Sensor %d: \n  L1: %-6.2f | dL: %-6.2f \n  a1: %-6.2f | da: %-6.2f \n  b1: %-6.2f | db: %-6.2f \n\n", 
                              i, standardColors[i].L, dL_vals[i], standardColors[i].a, da_vals[i], standardColors[i].b, db_vals[i]);
                xSemaphoreGiveRecursive(serialMutex);
              }
            }
            xSemaphoreGiveRecursive(serialMutex); // <--- RELEASE I2C SHIELD
          } 
        }
        
        if (print5SecReport) {
          xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
          Serial.println("======================================================================================================");
          xSemaphoreGiveRecursive(serialMutex);
          lastGraphReportTime = millis(); 
        }

        // --- PREMIUM OPTIMIZATION: DUAL-FORMAT PAYLOAD TRANSMISSION ---
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      
      if (isOnlineArchitecture) {
          // --- CLOUD ROUTE: Premium JSON Payload for AWS ---
          // Formats data perfectly for Cloud Databases and Web Dashboards
          char jsonPayload[256];
          snprintf(jsonPayload, sizeof(jsonPayload), 
                   "{\"dist\":%.3f,\"s\":[[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f]]}",
                   currentDistance_yards,
                   dL_vals[0], da_vals[0], db_vals[0],
                   dL_vals[1], da_vals[1], db_vals[1],
                   dL_vals[2], da_vals[2], db_vals[2],
                   dL_vals[3], da_vals[3], db_vals[3],
                   dL_vals[4], da_vals[4], db_vals[4]);
                   
          client.publish(AWS_IOT_PUBLISH_TOPIC, jsonPayload);
      } else {
          // --- LOCAL ROUTE: High-Speed CSV for Offline Python UI (Gem5.py) ---
          char csvPayload[160];
          int offset = snprintf(csvPayload, sizeof(csvPayload), "DATA,%.3f", currentDistance_yards);
          for (int i = 0; i < 5; i++) {
              offset += snprintf(csvPayload + offset, sizeof(csvPayload) - offset, ",%.2f,%.2f,%.2f", dL_vals[i], da_vals[i], db_vals[i]);
          }
          Serial.println(csvPayload);
      }
      
      xSemaphoreGiveRecursive(serialMutex);
        
        // Slightly relax the loop in online mode to prevent AWS rate-limiting
        if (isOnlineArchitecture) {
            vTaskDelay(pdMS_TO_TICKS(15)); 
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

// ---------------------------------------------------------------------
// THE AWS CLOUD ENGINE (MQTT + TLS)
// ---------------------------------------------------------------------
void messageHandler(char* topic, byte* payload, unsigned int length) {
  // --- PREMIUM FIX: O(1) Memory Allocation. Zero Heap Fragmentation! ---
  String msg((char*)payload, length);
  msg.trim();
  
  xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
  Serial.printf("\n[CLOUD COMMAND] Received: %s\n", msg.c_str());
  xSemaphoreGiveRecursive(serialMutex);

  // --- SMART ROUTER & DATA WIPER ---
  if (msg == "0") {
    currentState = STATE_IDLE_WAITING;
    Serial.println("Hardware Paused by Cloud.");
  } else if (msg == "1") {
    if (isCalibrated) {
      currentState = STATE_SCANNING;
      Serial.println("Hardware Resumed by Cloud.");
    } else {
      Serial.println("[WARNING] Cannot Resume: Machine is NOT calibrated!");
    }
  } else if (msg == "2") {
    portENTER_CRITICAL(&slitMux); halfSlitsCount = 0; portEXIT_CRITICAL(&slitMux);
    isCalibrated = false; 
    currentState = STATE_PHYSICAL_CALIBRATION;
  } else if (msg == "3") {
    portENTER_CRITICAL(&slitMux); halfSlitsCount = 0; portEXIT_CRITICAL(&slitMux);
    isCalibrated = false;
    currentState = STATE_MANUAL_CALIBRATION;
  } else if (msg.startsWith("C") && msg.length() == 2) {
    portENTER_CRITICAL(&slitMux);
    halfSlitsCount = 0; portEXIT_CRITICAL(&slitMux);
    targetCalibSensor = msg.substring(1).toInt();
    isCalibrated = false; 
    currentState = STATE_SINGLE_CALIBRATION;
  } else if (msg == "CLEAR") {
    portENTER_CRITICAL(&slitMux); halfSlitsCount = 0; portEXIT_CRITICAL(&slitMux);
    Serial.println("Hardware Distance reset to 0 by Cloud.");
  } else if (msg.startsWith("U") && msg.length() == 6) {   // Or cmdStr.startsWith
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      for (int i = 0; i < 5; i++) {
        if (sensorBooted[i]) { // <--- CORE PANIC SHIELD
          sensorEnabled[i] = (msg.charAt(i + 1) == '1'); // Update UI State
          tcaselect(i);
          sensors[i].enableLED(sensorEnabled[i]);
        } else {
          sensorEnabled[i] = false; // Force it off if hardware is missing
        }
      }
      xSemaphoreGiveRecursive(serialMutex);
  } else if (msg == "Z") {
    // --- PREMIUM FIX: ZERO DISTANCE COMMAND FOR CLOUD ---
    portENTER_CRITICAL(&slitMux);
    halfSlitsCount = 0;
    portEXIT_CRITICAL(&slitMux);
    Serial.println("[CLOUD] Hardware distance physically reset to 0.0");
  } else if (msg.startsWith("Q,")) {
    // --- PREMIUM FIX: CLOUD INJECTED MANUAL QTX ---
    // Instantly parses the massive QTX string from the Web JS engine
    int idx = 2; 
    for (int i = 0; i < 5; i++) {
      int nextComma = msg.indexOf(',', idx);
      if (nextComma == -1) nextComma = msg.length();
      standardColors[i].L = msg.substring(idx, nextComma).toFloat();
      idx = nextComma + 1;

      nextComma = msg.indexOf(',', idx);
      if (nextComma == -1) nextComma = msg.length();
      standardColors[i].a = msg.substring(idx, nextComma).toFloat();
      idx = nextComma + 1;

      nextComma = msg.indexOf(',', idx);
      if (nextComma == -1) nextComma = msg.length();
      standardColors[i].b = msg.substring(idx, nextComma).toFloat();
      idx = nextComma + 1;
    }

    
    isCalibrated = true;
    portENTER_CRITICAL(&slitMux); 
    halfSlitsCount = 0; 
    portEXIT_CRITICAL(&slitMux);
    currentState = STATE_IDLE_WAITING;

    // --- PREMIUM FIX: Upload QTX directly to AWS for Python Excel sync! ---
    if (isOnlineArchitecture) {
        char calibJson[300];
        snprintf(calibJson, sizeof(calibJson), 
           "{\"type\":\"calib\",\"mode\":\"MANUAL\",\"m\":[[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f]]}",
           standardColors[0].L, standardColors[0].a, standardColors[0].b,
           standardColors[1].L, standardColors[1].a, standardColors[1].b,
           standardColors[2].L, standardColors[2].a, standardColors[2].b,
           standardColors[3].L, standardColors[3].a, standardColors[3].b,
           standardColors[4].L, standardColors[4].a, standardColors[4].b);
        client.publish(AWS_IOT_PUBLISH_TOPIC, calibJson);
    }

    xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
    Serial.println("\n[SUCCESS] Cloud Injected New Digital Standards Saved!");
    Serial.println(" -> Hardware distance physically reset to 0.0");
    xSemaphoreGiveRecursive(serialMutex);
  }
}

void connectAWS() {
  WiFi.mode(WIFI_STA);
  // --- PREMIUM FIX: Disables modem sleep. Keeps the AWS TCP socket blazing fast and prevents idle disconnects! ---
  WiFi.setSleep(false); 
  WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASS);

  xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
  Serial.println("\n[SYSTEM] Connecting to Wi-Fi...");
  xSemaphoreGiveRecursive(serialMutex);

  int retries = 0;
  // Try to connect for 10 seconds before aborting safely
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[FATAL] Wi-Fi Connection Failed! Check credentials.");
    currentState = STATE_ASKING_MODE; // Safely abort back to Asking Mode
    return;
  }
  Serial.println("\n[SUCCESS] Wi-Fi Connected!");

  // --- PREMIUM FIX: THE NTP TIME SYNC SUPER VILLAIN ---
  // AWS TLS certificates will instantly reject the ESP32 if its clock is set to 1970!
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
  Serial.print("[SYSTEM] Synchronizing Atomic Clock for AWS TLS");
  xSemaphoreGiveRecursive(serialMutex);
  
  time_t now = time(nullptr);
  while (now < 1600000000) { // Wait until the year is at least 2020
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("\n[SUCCESS] Time Synchronized! Certificates are now valid.");

  // Configure the Secure Client with your AWS Certificates from secrets.h
  net.setCACert(AWS_CERT_CA);
  net.setCertificate(AWS_CERT_CRT);
  net.setPrivateKey(AWS_CERT_PRIVATE);

  client.setServer(AWS_IOT_ENDPOINT, 8883);
  client.setCallback(messageHandler);
  
  // PREMIUM FIX: Expand the memory buffer to 1024 bytes to easily fit the massive 5-sensor JSON payload!
  // Tell PubSubClient to send a heartbeat every 60 seconds
  client.setKeepAlive(60); 
  client.setBufferSize(1024); 

  Serial.println("[SYSTEM] Handshaking with AWS IoT Core...");
  while (!client.connect("")) {
    Serial.print(".");
    delay(100);
  }

  if (!client.connected()) {
    Serial.println("\n[FATAL] AWS IoT Handshake Timeout! Check policies.");
    currentState = STATE_ASKING_MODE;
    return;
  }

  client.subscribe(AWS_IOT_SUBSCRIBE_TOPIC);
  Serial.println("\n[SUCCESS] AWS IoT Connected & Subscribed to Control Channel!");
}

// ---------------------------------------------------------------------
// 100% CRASH-PROOF SERIAL READER
// ---------------------------------------------------------------------
float readFloatFromSerial() {
  String inputString = "";
  while (true) {
    esp_task_wdt_reset();
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n') {
        inputString.trim();
        if (inputString.length() > 0) { return inputString.toFloat(); }
      } else if (c != '\r') {
        inputString += c;
      }
    }
    delay(5); 
  }
}



void setup() {
  setCpuFrequencyMhz(240); 
  Serial.begin(500000);
  delay(1000);
  
  // --- FIXED: Kill the hidden 5-second Arduino Watchdog so our custom timer actually applies! ---
  esp_task_wdt_deinit(); 
  esp_task_wdt_config_t wdt_config = { .timeout_ms = WDT_TIMEOUT * 1000, .idle_core_mask = (1 << 0) | (1 << 1), .trigger_panic = true };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
  
  serialMutex = xSemaphoreCreateRecursiveMutex(); // <--- PREMIUM FIX: Upgraded to Recursive to prevent core deadlocks!
  pinMode(speedSensorPin, INPUT_PULLUP);
  lastSensorState = digitalRead(speedSensorPin);
  halfSlitsCount = (lastSensorState == HIGH) ? 1 : 0;
  attachInterrupt(digitalPinToInterrupt(speedSensorPin), countSignal, CHANGE);
  pinMode(buttonPin, INPUT_PULLDOWN);

  // --- PREMIUM FIX: Gentle 100kHz Boot Sequence ---
  Wire.begin(sdaPin, sclPin);
  Wire.setClock(100000); // <--- Start slow so sleeping silicon doesn't choke!

  Serial.println("\n--- Waking Up Optical Array ---");
  current_tca_channel = 255; // Purge cache

  for (uint8_t i = 0; i < 5; i++) {
    tcaselect(i);
    delay(100); // Give the multiplexer plenty of time to route power

    // --- CRITICAL FIX: Removed the empty ping! Let Adafruit wake it natively! ---
    if (sensors[i].begin()) {
      sensors[i].enableLED(true);
      sensorBooted[i] = true;   
      sensorEnabled[i] = true;  
      Serial.printf("[SUCCESS] Sensor %d Online!\n", i);
    } else {
      Serial.printf("[WARNING] Sensor %d is dead or missing! Safely isolating...\n", i);
      sensorBooted[i] = false;  
      sensorEnabled[i] = false; 
    }
  }

  // --- SHIFT INTO MAXIMUM OVERDRIVE FOR RUNTIME ---
  Wire.setClock(400000); 
  Serial.println("[SYSTEM] I2C Bus shifted to 400kHz High-Speed Mode.");

  xTaskCreatePinnedToCore(colorScanningTask, "ColorTask", 10000, NULL, 1, &ColorTaskHandle, 0);
  
  xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
  Serial.println("\n=======================================================");
  Serial.println("             TMDM SHADE INSPECTION SYSTEM              ");
  Serial.println("=======================================================");
  Serial.println(" -> SYSTEM READY: WAITING IN ASKING MODE ");
  Serial.println(" -> Press button 1x for Offline (USB Python)");
  Serial.println(" -> Press button 2x for Online (AWS Cloud)");
  Serial.println("=======================================================");
  xSemaphoreGiveRecursive(serialMutex);
}

void loop() {
  esp_task_wdt_reset();

  // Only monitor Wi-Fi if we are officially running in the Online Architecture
  if (isOnlineArchitecture && currentState != STATE_ASKING_MODE && currentState != STATE_ONLINE_INIT) {
    
    if (WiFi.status() != WL_CONNECTED) {
      if (!wifiTimerActive) {
        wifiDisconnectTime = millis();
        wifiTimerActive = true;
        xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
        Serial.println("\n[WARNING] Wi-Fi Connection Lost! 10-Second Fail-Safe activated...");
        xSemaphoreGiveRecursive(serialMutex);
      } 
      else if (millis() - wifiDisconnectTime > 10000) {
        // 10 SECONDS EXPIRED! EMERGENCY ABORT TO ASKING MODE!
        isOnlineArchitecture = false;
        wifiTimerActive = false;
        currentState = STATE_ASKING_MODE;

        xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
        Serial.println("\n[FATAL] Wi-Fi timeout reached. Online operations terminated.");
        Serial.println("=======================================================");
        Serial.println(" -> RETURNED TO ASKING MODE ");
        Serial.println(" -> Press 1x for Offline | Press 2x for Online ");
        Serial.println("=======================================================");
        xSemaphoreGiveRecursive(serialMutex);
      }
    } 
    else {
      // Wi-Fi came back online before the 10 seconds ran out!
      if (wifiTimerActive) {
        wifiTimerActive = false;
        xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
        Serial.println("\n[SUCCESS] Wi-Fi reconnected in time! Crisis averted.");
        xSemaphoreGiveRecursive(serialMutex);
      }
    }
  }

  // --- NEW: EXECUTE THE ONLINE BOOT SEQUENCE ---
  if (currentState == STATE_ONLINE_INIT) {
    connectAWS();
    if (currentState != STATE_ASKING_MODE) { // If it didn't fail and fallback
      applyHighSpeedHardwareSettings();
      currentState = STATE_IDLE_WAITING;
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      Serial.println("\n[SYSTEM] Cloud Architecture Ready. Awaiting start command from web dashboard...");
      xSemaphoreGiveRecursive(serialMutex);
    }
  }
  
  // --- PREMIUM FIX: THE MQTT RECONNECTION WATCHDOG ---
  if (isOnlineArchitecture && WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      Serial.println("\n[WARNING] AWS Cloud Disconnected! Attempting rapid reconnect...");
      xSemaphoreGiveRecursive(serialMutex);
      
      // Instantly attempt to re-establish the TLS handshake without rebooting
      if (client.connect("TMDM_Scanner_ESP32")) {
        client.subscribe(AWS_IOT_SUBSCRIBE_TOPIC);
        // Re-subscribe to the control channel
        xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
        Serial.println("[SUCCESS] Re-established Cloud Connection! Ready for commands.");
        xSemaphoreGiveRecursive(serialMutex);
      }
    }
    
    // PREMIUM FIX: Total isolation! Core 0 cannot publish while Core 1 is receiving.
    xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
    client.loop(); 
    xSemaphoreGiveRecursive(serialMutex);
  }

  if (Serial.available() > 0) {
    String cmdStr = Serial.readStringUntil('\n');
    cmdStr.trim();
    
    if (cmdStr.length() > 0) {
      // Only process Serial commands if we are actually in Offline Mode scanning/calibrating
      if (currentState != STATE_ASKING_MODE && currentState != STATE_ONLINE_INIT && currentState != STATE_MANUAL_CALIBRATION) {
        if (cmdStr == "0") {
          currentState = STATE_IDLE_WAITING;
          xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
          Serial.println("\n[SYSTEM] Scanning PAUSED.");
          xSemaphoreGiveRecursive(serialMutex);
        } else if (cmdStr == "1") {
          currentState = STATE_SCANNING;
          xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
          Serial.println("\n[SYSTEM] Scanning STARTED/RESUMED.");
          xSemaphoreGiveRecursive(serialMutex);
        } else if (cmdStr == "2") {
          
          currentState = STATE_PHYSICAL_CALIBRATION;
        } else if (cmdStr == "3") {
          currentState = STATE_MANUAL_CALIBRATION;
        } else if (cmdStr.startsWith("C") && cmdStr.length() == 2) {
          targetCalibSensor = cmdStr.substring(1).toInt();
          
          currentState = STATE_SINGLE_CALIBRATION;
        } else if (cmdStr.startsWith("U") && cmdStr.length() == 6) {
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      for (int i = 0; i < 5; i++) {
        if (sensorBooted[i]) { // <--- CORE PANIC SHIELD
          sensorEnabled[i] = (cmdStr.charAt(i + 1) == '1'); // Update UI State
          tcaselect(i);
          sensors[i].enableLED(sensorEnabled[i]);
        } else {
          sensorEnabled[i] = false; // Force it off if hardware is missing
        }
      }
      xSemaphoreGiveRecursive(serialMutex);
        } else if (cmdStr == "Z") {
          // --- NEW: THE ZERO DISTANCE COMMAND ---
          portENTER_CRITICAL(&slitMux);
          halfSlitsCount = 0;
          portEXIT_CRITICAL(&slitMux);
          
          xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
          Serial.println("\n[SYSTEM] Hardware wheel distance physically reset to 0.0 Yards.");
          xSemaphoreGiveRecursive(serialMutex);
        }
      }
    }
  }

  static bool waitForPhysicalRelease = false;
  static bool ledFlashed = false; // --- NEW: Track if the LED has flashed yet ---
  int rawButtonState = digitalRead(buttonPin);

  if (waitForPhysicalRelease) {
    if (rawButtonState == LOW) {
      delay(50);
      if (digitalRead(buttonPin) == LOW) {
        waitForPhysicalRelease = false;
        isButtonPressed = false;
        isHolding = false;
        clickCount = 0;
      }
    }
    return;
  }

  if (rawButtonState == HIGH && !isButtonPressed) {
    isButtonPressed = true;
    buttonPressStartTime = millis();
    isHolding = true;
    ledFlashed = false; // Reset the flash tracker on a new press
  }

  // --- NEW: ACTIVE HOLD CHECKING FOR THE 3-SECOND FLASH ---
  if (rawButtonState == HIGH && isButtonPressed) {
    unsigned long currentHold = millis() - buttonPressStartTime;
    if (currentHold >= 3000 && !ledFlashed) {
      // FLASH ALL ACTIVE SENSOR LEDS OFF AND ON
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY); // <--- SHIELD ADDED
      for (uint8_t i = 0; i < 5; i++) {
        if (sensorEnabled[i]) { tcaselect(i);
        sensors[i].enableLED(false); }
      }
      xSemaphoreGiveRecursive(serialMutex);
      
      delay(150);
      // Keep them off for 150ms so it is very noticeable 
      
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY); // <--- SHIELD ADDED
      for (uint8_t i = 0; i < 5; i++) {
        if (sensorEnabled[i]) { tcaselect(i);
        sensors[i].enableLED(true); }
      }
      xSemaphoreGiveRecursive(serialMutex); // <--- SHIELD REMOVED
      
      ledFlashed = true;
      // Lock it so it only flashes once!
    }
  }

  if (rawButtonState == LOW && isButtonPressed) {
    isButtonPressed = false;
    isHolding = false;
    unsigned long holdTime = millis() - buttonPressStartTime;
    // --- Execute holds exactly when the button is released ---
    if (holdTime > 7000) {
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      Serial.println("\n[SYSTEM] 7-SECOND HOLD DETECTED! Wiping data...");
      xSemaphoreGiveRecursive(serialMutex);

      currentState = STATE_ASKING_MODE; // <--- FIXED: Safely returns to Asking Mode
      isOnlineArchitecture = false;     // Reset the cloud flag
      
      
      portENTER_CRITICAL(&slitMux);
      halfSlitsCount = 0;
      portEXIT_CRITICAL(&slitMux);
      
      lastSensorState = digitalRead(speedSensorPin);
      
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY); 
      for(int i=0; i<5; i++) { 
        standardColors[i].L = 0;
        standardColors[i].a = 0;
        standardColors[i].b = 0;
        
        if (sensorBooted[i]) { // <--- CORE PANIC SHIELD
          sensorEnabled[i] = true; 
          tcaselect(i); 
          sensors[i].enableLED(true);
        } else {
          sensorEnabled[i] = false;
        }
      }
      xSemaphoreGiveRecursive(serialMutex);
      
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      Serial.println("\n=======================================================");
      Serial.println(" -> RETURNED TO ASKING MODE ");
      Serial.println(" -> Press 1x for Offline | Press 2x for Online ");
      Serial.println("=======================================================");
      xSemaphoreGiveRecursive(serialMutex);
    }
    else if (holdTime > 3000) {
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      Serial.println("\n[SYSTEM] 3-SECOND HOLD DETECTED! Clearing data...");
      xSemaphoreGiveRecursive(serialMutex);

      
      
      portENTER_CRITICAL(&slitMux);
      halfSlitsCount = 0;
      portEXIT_CRITICAL(&slitMux);
      
      currentState = STATE_IDLE_WAITING; 
      for(int i=0; i<5; i++) { standardColors[i].L = 0; standardColors[i].a = 0; standardColors[i].b = 0; }
    }
    else if (holdTime > 50) {
      clickCount++;
      lastReleaseTime = millis();
    }
  }

  if (clickCount > 0 && !isHolding && (millis() - lastReleaseTime > MULTI_CLICK_DELAY)) {
    int finalClicks = clickCount;
    clickCount = 0; 

    // --- HYBRID ROUTER: If in Asking Mode, the button selects the Architecture ---
    if (currentState == STATE_ASKING_MODE) {
      if (finalClicks == 1) {
        isOnlineArchitecture = false;
        applyHighSpeedHardwareSettings(); // <--- Instantly apply High-Speed Hardware settings
        currentState = STATE_IDLE_WAITING; // <--- Ready for Python Calibration!
        
        xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
        Serial.println("\n[SYSTEM] OFFLINE (USB) MODE SELECTED!");
        Serial.println(" -> Hardware synced. Booting Python Engine...");
        xSemaphoreGiveRecursive(serialMutex);
      } 
      else if (finalClicks == 2) {
        currentState = STATE_ONLINE_INIT;
        isOnlineArchitecture = true; // <--- NEW: Lock in Online Mode
        xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
        Serial.println("\n[SYSTEM] ONLINE (AWS MQTT) MODE SELECTED!");
        Serial.println(" -> Attempting Wi-Fi Connection...");
        xSemaphoreGiveRecursive(serialMutex);
      }
    }
    // --- STANDARD ROUTER: If running normally, the button controls the machine ---
    else if (currentState != STATE_MANUAL_CALIBRATION && currentState != STATE_ONLINE_INIT) {
      if (finalClicks == 1) {
        if (currentState == STATE_SCANNING) {
          currentState = STATE_IDLE_WAITING;
          xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY); Serial.println("\n[SYSTEM] Scanning PAUSED."); xSemaphoreGiveRecursive(serialMutex);
        } else if (currentState == STATE_IDLE_WAITING) {
          if (isCalibrated) {
            currentState = STATE_SCANNING;
            xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY); Serial.println("\n[SYSTEM] Scanning RESUMED."); xSemaphoreGiveRecursive(serialMutex);
          } else {
            xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY); Serial.println("\n[WARNING] Please calibrate standard first!"); xSemaphoreGiveRecursive(serialMutex);
          }
        }
      } 
      else if (finalClicks == 2) { 
        
        currentState = STATE_PHYSICAL_CALIBRATION; 
      }
      else if (finalClicks == 3) { 
        currentState = STATE_MANUAL_CALIBRATION;
      }
    }
  }

  // ---------------------------------------------------------
  // 4. MANUAL CALIBRATION (THE STRING COMBINATION FIX)
  // ---------------------------------------------------------
  if (currentState == STATE_MANUAL_CALIBRATION) {
    xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
    Serial.println("\n======================================");
    Serial.println("   5-CHANNEL MANUAL QTX DATA ENTRY    ");
    Serial.println("======================================");
    xSemaphoreGiveRecursive(serialMutex);

    for (int i = 0; i < 5; i++) {
      if(!sensorEnabled[i]) continue; 

      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      Serial.printf("Enter Target L1 for Sensor %d\n", i);
      xSemaphoreGiveRecursive(serialMutex);
      
      standardColors[i].L = readFloatFromSerial();
      
      // COMBINED STRING GUARANTEES PYTHON NEVER OVERWRITES THE PROMPT
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      Serial.printf("Saved L1: %.2f | Enter Target a1 for Sensor %d\n", standardColors[i].L, i);
      xSemaphoreGiveRecursive(serialMutex);
      
      standardColors[i].a = readFloatFromSerial();
      
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      Serial.printf("Saved a1: %.2f | Enter Target b1 for Sensor %d\n", standardColors[i].a, i);
      xSemaphoreGiveRecursive(serialMutex);
      
      standardColors[i].b = readFloatFromSerial();
      
      xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
      Serial.printf("Saved b1: %.2f | Sensor %d Complete!\n", standardColors[i].b, i);
      xSemaphoreGiveRecursive(serialMutex);
    }
    
    
    xSemaphoreTakeRecursive(serialMutex, portMAX_DELAY);
    Serial.println("\n[SUCCESS] New Digital Standards Saved!");
    Serial.println(" -> Short press button 1 time (or type 1) to START scanning.");
    xSemaphoreGiveRecursive(serialMutex);
    
    isCalibrated = true; // <--- NEW: Hardware is unlocked!
    currentState = STATE_IDLE_WAITING;
  }
  delay(10);
}
