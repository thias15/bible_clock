#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include <time.h>
#include <esp_sleep.h>

// E-paper libraries
#include "EPD_3in52b.h"
#include "GUI_Paint.h"
#include "fonts.h"
#include "ImageData.h"

// ---------------------------- Configuration ----------------------------

// WiFi credentials
const char* ssid = "";       // <-- Replace with your WiFi SSID
const char* password = ""; // <-- Replace with your WiFi Password

// NTP Configuration
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;       // UTC+1
const int   daylightOffset_sec = 3600;  // DST offset if applicable

// e-Paper
UBYTE *BlackImage;
UBYTE *RedImage;

// Timing Variables
unsigned long previousDisplayUpdateMillis = 0;
unsigned long previousTimeSyncMillis = 0;
const unsigned long displayUpdateIntervalMs = 60000UL;  // Update display every 60 seconds after the first update
int lastSyncedHour = -1;

// JSON Document for the current hour's verses
// Reduced size based on estimation. Adjust as needed.
StaticJsonDocument<100000> hourDoc;

// Track the last loaded hour to prevent redundant loads
int lastLoadedHour = -1;

// Display Dimensions (Adjust based on your e-Paper display)
const int displayWidth = EPD_3IN52B_HEIGHT - 5;  // Leave 5 pixels free on the right
const int displayHeight = EPD_3IN52B_WIDTH;

// --------------------------------------------------------------------
// 1) Initialize WiFi
// --------------------------------------------------------------------
void initWiFi() {
    Serial.print("Connecting to ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);
    unsigned long wifiStart = millis();
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        // Timeout after 30 seconds
        if (millis() - wifiStart > 30000UL) {
            Serial.println("\nFailed to connect to WiFi.");
            // Optionally, implement retry logic or enter a safe mode
            return;
        }
    }
    
    Serial.println("\nWiFi connected");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
}

// --------------------------------------------------------------------
// 2) Initialize Time via NTP
// --------------------------------------------------------------------
bool syncTime() {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("Synchronizing time with NTP...");
    
    struct tm timeinfo;
    unsigned long syncStart = millis();
    const unsigned long syncTimeout = 10000UL; // 10 seconds timeout
    
    while (!getLocalTime(&timeinfo)) {
        delay(500);
        Serial.print(".");
        if (millis() - syncStart > syncTimeout) {
            Serial.println("\nTime synchronization failed.");
            return false;
        }
    }
    
    Serial.println("\nTime synchronized");
    return true;
}

// --------------------------------------------------------------------
// 3) Load JSON for the given hour from SPIFFS
//    e.g., hour=3 -> "/bible_verses_hour03.json"
// --------------------------------------------------------------------
bool loadBibleVersesForHour(int hour) {
    // Clear previous JSON data
    hourDoc.clear();

    // Initialize SPIFFS
    if (!SPIFFS.begin(true)) { 
        Serial.println("SPIFFS Mount Failed");
        return false;
    }

    // Build the filename, e.g., "/bible_verses_hour03.json" for hour=3
    // If hour=0, treat it as 24 in the calling code
    char filename[32];
    snprintf(filename, sizeof(filename), "/bible_verses_hour%02d.json", hour);
    
    File file = SPIFFS.open(filename, "r");
    if (!file) {
        Serial.print("Failed to open file for hour = ");
        Serial.println(hour);
        return false;
    }

    // Deserialize JSON
    DeserializationError error = deserializeJson(hourDoc, file);
    file.close();
    if (error) {
        Serial.print("Failed to parse JSON for hour = ");
        Serial.print(hour);
        Serial.print(": ");
        Serial.println(error.c_str());
        return false;
    }

    Serial.print("Loaded verses for hour ");
    Serial.println(hour);
    return true;
}

// --------------------------------------------------------------------
// 4) Retrieve both reference and verse based on a single time fetch
// --------------------------------------------------------------------
struct VerseData {
    String reference;
    String text;
};

// Helper function to extract the book name from the reference string with parentheses ()
String extractBookName(const String &reference) {
    int start = reference.indexOf('(');
    int end = reference.indexOf(')');
    if (start != -1 && end != -1 && end > start) {
        String insideParentheses = reference.substring(start + 1, end);
        return "[" + insideParentheses + "]"; // If no space, return the whole string with brackets
    }
    return ""; // Return empty string if format is unexpected
}

VerseData getCurrentVerseData(const struct tm &timeinfo) {
    VerseData data;
    
    int hour = timeinfo.tm_hour;
    int minute = timeinfo.tm_min;

    // If hour=0, treat as 24
    if (hour == 0) hour = 24;

    // Load JSON for the current hour if not already loaded
    if (hour != lastLoadedHour) {
        if (!loadBibleVersesForHour(hour)) {
            Serial.println("Failed to load data for this hour.");
            return data;  // Return empty data
        }
        lastLoadedHour = hour;
    }

    // Build minute as two-digit string: "00".."59"
    char minuteStr[3];
    snprintf(minuteStr, sizeof(minuteStr), "%02d", minute);

    // Retrieve reference and text from JSON
    JsonObject entry = hourDoc[minuteStr];
    if (!entry.isNull()) {
        if (entry.containsKey("reference") && entry.containsKey("text")) {
            String fullReference = entry["reference"].as<String>();
            data.reference = extractBookName(fullReference); // Now includes brackets
            data.text = entry["text"].as<String>();
        } else {
            Serial.print("Missing 'reference' or 'text' in JSON for minute ");
            Serial.println(minuteStr);
        }
    } else {
        Serial.print("No entry found in JSON for minute ");
        Serial.println(minuteStr);
    }

    return data;
}

// --------------------------------------------------------------------
// 5) Display Content: Time, Reference, and Verse
// --------------------------------------------------------------------
void displayContent(const String &currentTimeStr, const String &reference, const String &verseText) {
    // Clear both images
    Paint_SelectImage(RedImage);
    Paint_Clear(WHITE);  // Clear with white background
    // Draw Time at Top Center with Font32 in Red on White Background
    int timeTextLength = currentTimeStr.length(); // Should be 5 for "HH:MM"
    int timeFontWidth = Font32.Width * timeTextLength;
    int timeX = (displayWidth - timeFontWidth) / 2;
    Paint_DrawString_EN(timeX, 10, currentTimeStr.c_str(), &Font32, WHITE, RED);

    // Draw Reference Book if available
    if (!reference.isEmpty()) {
        int refTextLength = reference.length(); // Should include brackets, e.g., "(John)"
        int refFontWidth = Font20.Width * refTextLength;
        int refX = (displayWidth - refFontWidth) / 2; // Center horizontally
        Paint_DrawString_EN(refX, 50, reference.c_str(), &Font20, WHITE, RED);
    }

    // Determine appropriate font size for verse based on length
    sFONT *verseFont;
    if (verseText.length() < 80) {
        verseFont = &Font24;
    } else if (verseText.length() < 120) {
        verseFont = &Font20;
    } else if (verseText.length() < 240) {
        verseFont = &Font16;
    } else {
        verseFont = &Font12;
    }

    Paint_SelectImage(BlackImage);
    Paint_Clear(WHITE);  // Clear with white background
    // Calculate verse position
    int verseX = 10; // Starting X position with some padding
    int verseY = reference.isEmpty() ? 60 : 90; // Adjust Y based on whether reference is present

    // Wrap and center the verse text
    // Assuming Paint_DrawString_EN_WordWrap handles wrapping based on font size and display width
    Paint_DrawString_EN_WordWrap(verseX, verseY, verseText.c_str(), verseFont, WHITE, BLACK, 2);

    // Update e-Paper Display
    EPD_3IN52B_Display(BlackImage, RedImage);   // Red layer
    Serial.println("Display updated.");
}

// --------------------------------------------------------------------
// 6) Update display with time and corresponding bible verse
// --------------------------------------------------------------------
void updateDisplay() {
  struct tm currentTime;
  if (getLocalTime(&currentTime)) { // Refresh time after delay
      struct tm frozenTimeinfo = currentTime; // Freeze timeinfo immediately
      char timeBuffer[6]; // HH:MM
      snprintf(timeBuffer, sizeof(timeBuffer), "%02d:%02d", frozenTimeinfo.tm_hour, frozenTimeinfo.tm_min);
      String currentTimeStr = String(timeBuffer);
      VerseData verse = getCurrentVerseData(frozenTimeinfo);
      if (!verse.reference.isEmpty() || !verse.text.isEmpty()) {
          displayContent(currentTimeStr, verse.reference, verse.text);
      } else {
          Serial.println("Initial verse data is empty. Skipping display.");
      }
  } else {
      Serial.println("Failed to retrieve current time.");
  }
}

// --------------------------------------------------------------------
// 7) Calculate Milliseconds Until Next Minute
// --------------------------------------------------------------------
unsigned long millisUntilNextMinute(const struct tm &timeinfo) {
    int seconds = timeinfo.tm_sec;
    int millisec = millis() % 1000;
    unsigned long remainingSeconds = 59 - seconds;
    unsigned long remainingMillis = 1000 - millisec;
    return (remainingSeconds * 1000UL) + remainingMillis;
}

// --------------------------------------------------------------------
// 8) Check for time sync at the beginning of each new hour
// --------------------------------------------------------------------
void checkTimeSync() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to retrieve current time.");
        return;
    }

    // Check if the hour has changed since the last sync
    if (timeinfo.tm_hour != lastSyncedHour) {
        lastSyncedHour = timeinfo.tm_hour;  // Update the last synced hour

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("Beginning of a new hour detected. Syncing time...");
            if (syncTime()) {   // Perform the time sync
                updateDisplay(); // Update the display immediately after sync
            }
        } else {
            Serial.println("WiFi not connected. Cannot synchronize time.");
        }
    }
}

// --------------------------------------------------------------------
// Setup
// --------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(1000); // Allow time for Serial to initialize

    // Initialize WiFi
    initWiFi();

    // Initialize Time
    if (!syncTime()) {
        Serial.println("Initial time synchronization failed. Continuing without accurate time.");
        // Optionally, handle this scenario (e.g., retry, use fallback time, etc.)
    }

    // Initialize e-Paper Module
    DEV_Module_Init();
    EPD_3IN52B_Init();
    EPD_3IN52B_Clear();
    DEV_Delay_ms(1000);

    // Allocate memory for e-Paper buffers
    UWORD Imagesize = ((EPD_3IN52B_WIDTH % 8 == 0) 
                        ? (EPD_3IN52B_WIDTH / 8) 
                        : (EPD_3IN52B_WIDTH / 8 + 1)) 
                        * EPD_3IN52B_HEIGHT;
    BlackImage = (UBYTE *)malloc(Imagesize);
    RedImage   = (UBYTE *)malloc(Imagesize);
    if (!BlackImage || !RedImage) {
        Serial.println("Failed to allocate memory for images");
        while (true); // Halt execution
    }

    // Initialize Paint for Black and Red Images
    Paint_NewImage(BlackImage, EPD_3IN52B_WIDTH, EPD_3IN52B_HEIGHT, 90, WHITE);
    Paint_SelectImage(BlackImage);
    Paint_Clear(WHITE);

    Paint_NewImage(RedImage, EPD_3IN52B_WIDTH, EPD_3IN52B_HEIGHT, 90, WHITE);
    Paint_SelectImage(RedImage);
    Paint_Clear(WHITE);

    // Retrieve current time
    struct tm currentTime;
    if (!getLocalTime(&currentTime)) {
      Serial.println("Failed to retrieve current time.");
      displayContent("0:00", "WiFi Error", "Display will not update until time is synchronized.");
    } else {
        unsigned long currentMs = millis();
        checkTimeSync();
        updateDisplay();
        unsigned long timeElapsedMs = millis() - currentMs;        
        // Calculate milliseconds until the next minute starts
        unsigned long initialDelay = millisUntilNextMinute(currentTime) - timeElapsedMs;
        // Wait until one second after the next minute before the first regular update
        delay(initialDelay + 1000UL);
        updateDisplay();
    }

    // Initialize timing variables
    previousDisplayUpdateMillis = millis();
    previousTimeSyncMillis = millis();
}

// --------------------------------------------------------------------
// Loop
// --------------------------------------------------------------------
void loop() {
    unsigned long currentMs = millis();

    // Check for time sync at the start of the hour
    checkTimeSync();

    // Update display
    updateDisplay();

    // Measure elapsed time
    unsigned long timeElapsedMs = millis() - currentMs;
    unsigned long remainingMs = displayUpdateIntervalMs - timeElapsedMs;
    Serial.print("Going to light sleep for ");
    Serial.print(remainingMs);
    Serial.println(" milliseconds...");
    delay(remainingMs);

    // // Limit sleep to 5 seconds (5000 ms) at a time
    // while (remainingMs > 0) {
    //     unsigned long sleepMs = min(remainingMs, 5000UL); // Sleep for 5 seconds or remaining time
    //     remainingMs -= sleepMs;
    //     // Needed so powerbank does not turn off
    //     if (sleepMs > 1000UL) {
    //         delay(1000UL);
    //         sleepMs -= 1000UL;
    //     }      
    //     uint64_t sleepUs = sleepMs * 1000; // Convert to microseconds

    //     // Enable timer wakeup
    //     esp_sleep_enable_timer_wakeup(sleepUs);
    //     esp_light_sleep_start();
    // }
}
