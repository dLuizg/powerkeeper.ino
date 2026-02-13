#include <WiFi.h>
#include "EmonLib.h"
#include <Firebase_ESP_Client.h>
#include <time.h>

// ============================================================================
// CONFIGURATION & CONSTANTS
// ============================================================================

// Network Configuration
const char *WIFI_SSID = "Augusto";
const char *WIFI_PASSWORD = "internet100";
const unsigned long WIFI_TIMEOUT_MS = 10000;

// Firebase Configuration
#define FIREBASE_HOST "https://powerkeeper-synatec-default-rtdb.firebaseio.com/"
#define FIREBASE_AUTH "gNcMVY25PGjzd1If4GX7OZiLENZsnxehj1JYmaRv"

// Hardware Pin Definitions
const int PIN_SCT_SENSOR = 35;
const int PIN_BUTTON = 14;
const int PIN_LED_220V = 25;
const int PIN_LED_127V = 26;
const int PIN_LED_OFF = 27;

// Device Configuration
const int DEVICE_ID = 1;
const float CURRENT_CALIBRATION = 1.45;
const int ADC_SAMPLES = 2048;

// Timing Constants
const unsigned long READING_INTERVAL_MS = 1000;
const unsigned long FIREBASE_INTERVAL_MS = 5000;
const unsigned long LED_OFF_DURATION_MS = 10000;

// Energy Calculation Constants
const double NOISE_THRESHOLD_AMPS = 0.16;
const double MICROS_PER_HOUR = 3600000000.0;
const double WH_TO_KWH = 1000.0;

// Time Configuration
const long TIMEZONE_OFFSET_SECONDS = -3 * 3600; // UTC-3
const time_t MIN_VALID_TIMESTAMP = 1609459200;  // Jan 1, 2021

// Voltage States
enum VoltageMode
{
    VOLTAGE_127V = 0,
    VOLTAGE_220V = 1,
    VOLTAGE_OFF = 2,
    VOLTAGE_MODE_COUNT = 3
};

// ============================================================================
// GLOBAL OBJECTS & STATE
// ============================================================================

// Hardware Objects
EnergyMonitor currentSensor;
FirebaseData firebaseData;
FirebaseAuth firebaseAuth;
FirebaseConfig firebaseConfig;

// Voltage Control State
VoltageMode currentVoltageMode = VOLTAGE_127V;
int currentVoltageValue = 127;
int previousButtonState = HIGH;

// Energy Measurement State
double currentRMS = 0.0;
double energyAccumulator_Wh = 0.0;
double todayConsumption_kWh = 0.0;
double yesterdayConsumption_kWh = 0.0;
unsigned long long lastMicros = 0;

// Timing State
unsigned long lastReadingTime = 0;
unsigned long lastFirebaseTime = 0;
unsigned long ledOffStartTime = 0;
bool isLedOffTimerActive = false;

// Day Tracking State
int currentDayOfMonth = -1;
String lastClosedDate = "";
unsigned long readingCounter = 1;

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================

// Initialization Functions
void setupHardware();
void setupWiFi();
void setupTimeSync();
void setupFirebase();

// Time Functions
void waitForNTPSync();
String getCurrentTimestamp(); // "YYYY-MM-DD HH:MM:SS"
String getCurrentDate();      // "YYYY-MM-DD"
String getYesterdayDate();
int getCurrentDay();

// Hardware Control Functions
void handleButtonPress();
void updateVoltageMode();
void updateLEDIndicators();
void updateLEDOffTimer();

// Energy Measurement Functions
void measureEnergy();
double calculatePower();

// Firebase Functions
void sendReadingToFirebase();
void sendDailyClosingToFirebase(double consumption_kWh);
bool readLastClosedDateFromFirebase();
void updateLastClosedDateMarker(const String &date);

// Day Management Functions
void checkForDayRollover();
void performDailyClosing(double consumption_kWh);
void performRetroactiveClosing();

// Utility Functions
int getVoltageValue(VoltageMode mode);
bool isWiFiConnected();
bool isFirebaseReady();

// ============================================================================
// SETUP
// ============================================================================

void setup()
{
    Serial.begin(115200);
    delay(50);
    Serial.println("\n=== PowerKeeper System Starting ===");

    setupHardware();
    setupWiFi();

    if (isWiFiConnected())
    {
        setupTimeSync();
        currentDayOfMonth = getCurrentDay();
        Serial.printf("System start time: %s\n", getCurrentTimestamp().c_str());

        setupFirebase();
        performRetroactiveClosing();
    }
    else
    {
        Serial.println("WiFi unavailable - running in offline mode");
        currentDayOfMonth = getCurrentDay();
    }

    lastMicros = micros();
    Serial.println("=== System Ready ===\n");
}

// ============================================================================
// MAIN LOOP
// ============================================================================

void loop()
{
    handleButtonPress();
    checkForDayRollover();
    updateLEDOffTimer();

    // Periodic energy measurement
    if (millis() - lastReadingTime >= READING_INTERVAL_MS)
    {
        lastReadingTime = millis();
        measureEnergy();
    }

    // Periodic Firebase updates
    if (millis() - lastFirebaseTime >= FIREBASE_INTERVAL_MS)
    {
        lastFirebaseTime = millis();
        if (isWiFiConnected() && isFirebaseReady())
        {
            sendReadingToFirebase();
        }
        else if (!isWiFiConnected())
        {
            Serial.println("Skipping Firebase update - WiFi disconnected");
        }
    }
}

// ============================================================================
// INITIALIZATION IMPLEMENTATIONS
// ============================================================================

void setupHardware()
{
    currentSensor.current(PIN_SCT_SENSOR, CURRENT_CALIBRATION);

    pinMode(PIN_BUTTON, INPUT_PULLUP);
    pinMode(PIN_LED_220V, OUTPUT);
    pinMode(PIN_LED_127V, OUTPUT);
    pinMode(PIN_LED_OFF, OUTPUT);

    updateLEDIndicators();
    Serial.println("Hardware initialized");
}

void setupWiFi()
{
    Serial.printf("Connecting to WiFi: %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long startTime = millis();
    while (!isWiFiConnected() && millis() - startTime < WIFI_TIMEOUT_MS)
    {
        delay(500);
        Serial.print(".");
    }

    if (isWiFiConnected())
    {
        Serial.println("\nWiFi connected");
        Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
    }
    else
    {
        Serial.println("\nWiFi connection failed");
    }
}

void setupTimeSync()
{
    configTime(TIMEZONE_OFFSET_SECONDS, 0, "pool.ntp.org", "time.nist.gov");
    waitForNTPSync();
}

void setupFirebase()
{
    Serial.println("Configuring Firebase...");
    firebaseConfig.database_url = FIREBASE_HOST;
    firebaseConfig.signer.tokens.legacy_token = FIREBASE_AUTH;
    Firebase.begin(&firebaseConfig, &firebaseAuth);
    Firebase.reconnectWiFi(true);
    Serial.println("Firebase connected");
}

// ============================================================================
// TIME FUNCTION IMPLEMENTATIONS
// ============================================================================

void waitForNTPSync()
{
    Serial.print("Waiting for NTP synchronization");
    time_t now = time(nullptr);

    while (now < MIN_VALID_TIMESTAMP)
    {
        delay(500);
        Serial.print(".");
        now = time(nullptr);
    }

    Serial.println("\nNTP synchronized successfully");
}

String getCurrentTimestamp()
{
    time_t now = time(nullptr);
    struct tm *timeInfo = localtime(&now);

    char buffer[25];
    sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d",
            timeInfo->tm_year + 1900,
            timeInfo->tm_mon + 1,
            timeInfo->tm_mday,
            timeInfo->tm_hour,
            timeInfo->tm_min,
            timeInfo->tm_sec);

    return String(buffer);
}

String getCurrentDate()
{
    time_t now = time(nullptr);
    struct tm *timeInfo = localtime(&now);

    char buffer[12];
    sprintf(buffer, "%04d-%02d-%02d",
            timeInfo->tm_year + 1900,
            timeInfo->tm_mon + 1,
            timeInfo->tm_mday);

    return String(buffer);
}

String getYesterdayDate()
{
    time_t now = time(nullptr);
    struct tm yesterday = *localtime(&now);
    yesterday.tm_mday -= 1;
    mktime(&yesterday);

    char buffer[12];
    sprintf(buffer, "%04d-%02d-%02d",
            yesterday.tm_year + 1900,
            yesterday.tm_mon + 1,
            yesterday.tm_mday);

    return String(buffer);
}

int getCurrentDay()
{
    time_t now = time(nullptr);
    struct tm *timeInfo = localtime(&now);
    return timeInfo->tm_mday;
}

// ============================================================================
// HARDWARE CONTROL IMPLEMENTATIONS
// ============================================================================

void handleButtonPress()
{
    int currentButtonState = digitalRead(PIN_BUTTON);

    // Detect button press (falling edge)
    if (previousButtonState == HIGH && currentButtonState == LOW)
    {
        updateVoltageMode();
        updateLEDIndicators();

        // Handle LED OFF mode timer
        if (currentVoltageMode == VOLTAGE_OFF)
        {
            isLedOffTimerActive = true;
            ledOffStartTime = millis();
            Serial.println("OFF mode - LED will turn off in 10 seconds");
        }
        else
        {
            isLedOffTimerActive = false;
            digitalWrite(PIN_LED_OFF, LOW);
        }

        Serial.printf("Voltage mode changed to: %d V\n", currentVoltageValue);
        delay(250); // Debounce delay
    }

    previousButtonState = currentButtonState;
}

void updateVoltageMode()
{
    currentVoltageMode = (VoltageMode)(((int)currentVoltageMode + 1) % VOLTAGE_MODE_COUNT);
    currentVoltageValue = getVoltageValue(currentVoltageMode);
}

void updateLEDIndicators()
{
    digitalWrite(PIN_LED_220V, LOW);
    digitalWrite(PIN_LED_127V, LOW);
    digitalWrite(PIN_LED_OFF, LOW);

    switch (currentVoltageMode)
    {
    case VOLTAGE_127V:
        digitalWrite(PIN_LED_127V, HIGH);
        break;
    case VOLTAGE_220V:
        digitalWrite(PIN_LED_220V, HIGH);
        break;
    case VOLTAGE_OFF:
        digitalWrite(PIN_LED_OFF, HIGH);
        break;
    }
}

void updateLEDOffTimer()
{
    if (isLedOffTimerActive && millis() - ledOffStartTime >= LED_OFF_DURATION_MS)
    {
        digitalWrite(PIN_LED_OFF, LOW);
        isLedOffTimerActive = false;
    }
}

// ============================================================================
// ENERGY MEASUREMENT IMPLEMENTATIONS
// ============================================================================

void measureEnergy()
{
    // Calculate time delta with microsecond precision
    unsigned long long currentMicros = micros();
    unsigned long long deltaMicros = currentMicros - lastMicros;
    lastMicros = currentMicros;

    // Measure RMS current
    currentRMS = currentSensor.calcIrms(ADC_SAMPLES);

    // Filter noise
    if (currentRMS < NOISE_THRESHOLD_AMPS)
    {
        currentRMS = 0.0;
    }

    // Calculate and accumulate energy
    double power = calculatePower();
    if (power > 0)
    {
        double hours = deltaMicros / MICROS_PER_HOUR;
        energyAccumulator_Wh += (power * hours);
        todayConsumption_kWh = energyAccumulator_Wh / WH_TO_KWH;
    }

    Serial.printf("Voltage: %d V | Current: %.3f A | Power: %.2f W | Today: %.6f kWh\n",
                  currentVoltageValue, currentRMS, power, todayConsumption_kWh);
}

double calculatePower()
{
    return currentRMS * currentVoltageValue;
}

// ============================================================================
// FIREBASE IMPLEMENTATIONS
// ============================================================================

void sendReadingToFirebase()
{
    FirebaseJson json;
    double power = calculatePower();

    json.set("tensao", currentVoltageValue);
    json.set("corrente", currentRMS);
    json.set("potencia", power);
    json.set("consumoAtual_kWh", todayConsumption_kWh);
    json.set("idDispositivo", DEVICE_ID);
    json.set("timestamp", getCurrentTimestamp());

    // Send to readings log
    String readingPath = "/leituras/leitura_" + String(readingCounter++);
    Serial.printf("Sending reading to: %s\n", readingPath.c_str());

    if (Firebase.RTDB.setJSON(&firebaseData, readingPath.c_str(), &json))
    {
        Serial.println("Reading sent successfully");
    }
    else
    {
        Serial.printf("Failed to send reading: %s\n", firebaseData.errorReason().c_str());
    }

    // Update latest reading snapshot
    if (!Firebase.RTDB.setJSON(&firebaseData, "/ultima_leitura", &json))
    {
        Serial.printf("Failed to update latest reading: %s\n", firebaseData.errorReason().c_str());
    }
}

void sendDailyClosingToFirebase(double consumption_kWh)
{
    FirebaseJson json;
    json.set("consumo_kWh", consumption_kWh);
    json.set("idDispositivo", DEVICE_ID);
    json.set("timestamp", getCurrentTimestamp());

    String date = getYesterdayDate();
    String path = "/consumos_diarios/" + date;

    if (Firebase.RTDB.setJSON(&firebaseData, path.c_str(), &json))
    {
        Serial.printf("Daily closing sent: %s\n", path.c_str());
        updateLastClosedDateMarker(date);
    }
    else
    {
        Serial.printf("Failed to send daily closing: %s\n", firebaseData.errorReason().c_str());
    }
}

bool readLastClosedDateFromFirebase()
{
    if (Firebase.RTDB.getString(&firebaseData, "/ultimo_fechamento/data"))
    {
        lastClosedDate = firebaseData.stringData();
        Serial.printf("Last closed date from Firebase: %s\n", lastClosedDate.c_str());
        return true;
    }
    else
    {
        Serial.println("No last closed date found in Firebase");
        lastClosedDate = "";
        return false;
    }
}

void updateLastClosedDateMarker(const String &date)
{
    lastClosedDate = date;
    if (!Firebase.RTDB.setString(&firebaseData, "/ultimo_fechamento/data", date))
    {
        Serial.printf("Warning: Failed to update last closed date marker: %s\n",
                      firebaseData.errorReason().c_str());
    }
}

// ============================================================================
// DAY MANAGEMENT IMPLEMENTATIONS
// ============================================================================

void checkForDayRollover()
{
    int today = getCurrentDay();

    if (today != currentDayOfMonth)
    {
        performDailyClosing(todayConsumption_kWh);

        // Reset daily counters
        yesterdayConsumption_kWh = todayConsumption_kWh;
        todayConsumption_kWh = 0.0;
        energyAccumulator_Wh = 0.0;
        currentDayOfMonth = today;

        Serial.println("===== NEW DAY ===== Consumption reset");
    }
}

void performDailyClosing(double consumption_kWh)
{
    String yesterday = getYesterdayDate();

    // Read last closed date if not already in memory
    if (lastClosedDate.isEmpty() && isWiFiConnected() && isFirebaseReady())
    {
        readLastClosedDateFromFirebase();
    }

    // Prevent duplicate closings
    if (yesterday == lastClosedDate)
    {
        Serial.printf("Daily closing already performed for: %s\n", yesterday.c_str());
        return;
    }

    // Send closing to Firebase
    if (isWiFiConnected() && isFirebaseReady())
    {
        sendDailyClosingToFirebase(consumption_kWh);
    }
    else
    {
        Serial.println("WiFi/Firebase unavailable - daily closing postponed");
    }
}

void performRetroactiveClosing()
{
    Serial.println("Checking for retroactive closing...");

    if (!isWiFiConnected() || !isFirebaseReady())
    {
        Serial.println("Firebase unavailable - skipping retroactive check");
        return;
    }

    readLastClosedDateFromFirebase();
    String yesterday = getYesterdayDate();

    if (yesterday != lastClosedDate)
    {
        Serial.printf("Retroactive closing needed for: %s\n", yesterday.c_str());
        performDailyClosing(todayConsumption_kWh);
    }
    else
    {
        Serial.println("No retroactive closing needed");
    }
}

// ============================================================================
// UTILITY FUNCTION IMPLEMENTATIONS
// ============================================================================

int getVoltageValue(VoltageMode mode)
{
    switch (mode)
    {
    case VOLTAGE_127V:
        return 127;
    case VOLTAGE_220V:
        return 220;
    case VOLTAGE_OFF:
        return 0;
    default:
        return 127;
    }
}

bool isWiFiConnected()
{
    return WiFi.status() == WL_CONNECTED;
}

bool isFirebaseReady()
{
    return Firebase.ready();
}