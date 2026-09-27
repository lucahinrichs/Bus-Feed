#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ctime>
#include "secrets.h"

// Bus Feed — real-time BVG bus departure display on an e-paper screen.

// CrowPanel 2.13" pin mapping
#define EPD_PWR   7
#define EPD_CS   14
#define EPD_DC   13
#define EPD_RST  10
#define EPD_BUSY  9
#define EPD_SCK  12
#define EPD_MOSI 11
#define MENU_KEY  2   // top button (IO2 per schematic) — toggles dark mode
#define EXIT_KEY  1   // bottom button (IO1 per schematic) — forces a manual data refresh

GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT> display(
    GxEPD2_213_B74(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

const char* ntp_server = "pool.ntp.org";
const char* tz_berlin = "CET-1CEST,M3.5.0,M10.5.0/3";
WiFiMulti wifiMulti;

// --- DARK MODE ---
// Rather than hardcoding black/white everywhere, drawUI()/drawItem() derive
// an "ink" (foreground) and "background" color from this flag, so a single
// bool flip inverts the whole display.
bool DARK_MODE = false;

#define LINE_LEN 8
#define DIR_LEN  32

struct Departure {
    char line[LINE_LEN];
    char direction[DIR_LEN];
    int minutes;
    int delayMin;
    bool hasDelay;
};

Departure topDeps[2];
int topCount = 0;

Departure bottomDeps[2];
int bottomCount = 0;

char statusMsg[32] = "";

// Primary endpoint + fallback mirror, tried in order on each fetch.
const char* API_URLS[2] = {
    "https://v6.bvg.transport.rest/stops/900002200/departures?results=15&duration=60",
    "https://v6.vbb.transport.rest/stops/900002200/departures?results=15&duration=60"
};

unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 60000; // 60 seconds between scheduled refreshes
int refreshCounter = 0;

// Parses an ISO 8601 timestamp (e.g. "2026-09-27T17:12:00+02:00") and returns
// the number of minutes from now until that time.
// Return value:
//   >= 0  -> valid, minutes until departure
//   -1    -> parse error (timestamp missing or malformed)
int minutenBisAbfahrt(const char* whenString) {
    if (whenString == nullptr) return -1;

    int jahr, monat, tag, stunde, minute, sekunde;
    if (sscanf(whenString, "%d-%d-%dT%d:%d:%d", &jahr, &monat, &tag, &stunde, &minute, &sekunde) != 6) {
        return -1;
    }

    struct tm departureTime = {};
    departureTime.tm_isdst = -1;
    departureTime.tm_year = jahr - 1900;
    departureTime.tm_mon  = monat - 1;
    departureTime.tm_mday = tag;
    departureTime.tm_hour = stunde;
    departureTime.tm_min  = minute;
    departureTime.tm_sec  = sekunde;

    time_t departureUnix = mktime(&departureTime);
    time_t now = time(nullptr);

    return (departureUnix - now) / 60;
}

// Draws a single departure row (line number, time/delay, truncated direction).
// inkColor is passed in explicitly rather than read from a global, so the
// color decision stays centralized in drawUI().
void drawItem(int x, int y, const Departure& dep, uint16_t inkColor) {
    display.setTextSize(2);
    display.setCursor(x, y);
    display.print(dep.line);

    // Build the time string ("5 min", "sofort", optionally "(+2)"/"(-1)" delay)
    // into a fixed buffer instead of using String concatenation — this runs
    // on every redraw, so avoiding heap allocations here matters for
    // long-term stability on a microcontroller with limited RAM.
    char timeStr[20];
    if (dep.minutes <= 0) {
        snprintf(timeStr, sizeof(timeStr), "sofort");
    } else {
        snprintf(timeStr, sizeof(timeStr), "%d min", dep.minutes);
    }

    if (dep.hasDelay) {
        char delayPart[12];
        if (dep.delayMin > 0) {
            snprintf(delayPart, sizeof(delayPart), " (+%d)", dep.delayMin);
        } else {
            snprintf(delayPart, sizeof(delayPart), " (%d)", dep.delayMin);
        }
        strncat(timeStr, delayPart, sizeof(timeStr) - strlen(timeStr) - 1);
    }

    int timeWidth = strlen(timeStr) * 6;
    int xTime = 246 - timeWidth;

    display.setTextSize(1);
    display.setCursor(xTime, y + 4);
    display.print(timeStr);

    // Truncate the direction text with ".." if it would overlap the time column.
    int maxDirWidth = xTime - (x + 46) - 4;
    int maxDirChars = maxDirWidth / 6;

    char dir[DIR_LEN];
    strncpy(dir, dep.direction, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    int dirLen = strlen(dir);
    if (maxDirChars > 0 && dirLen > maxDirChars) {
        if (maxDirChars > 2 && maxDirChars < (int)sizeof(dir)) {
            dir[maxDirChars - 2] = '.';
            dir[maxDirChars - 1] = '.';
            dir[maxDirChars] = '\0';
        } else if (maxDirChars < (int)sizeof(dir)) {
            dir[maxDirChars] = '\0';
        }
    }

    display.setCursor(x + 46, y + 4);
    display.print(dir);
}

// Renders the full screen: two departure blocks (top/bottom) separated by a
// framed clock and a dashed divider line. isPartial controls whether GxEPD2
// does a fast partial refresh (less flicker, can leave faint ghosting over
// many cycles) or a full refresh (slower, clears ghosting).
void drawUI(bool isPartial) {
    uint16_t inkColor = DARK_MODE ? GxEPD_WHITE : GxEPD_BLACK;
    uint16_t bgColor   = DARK_MODE ? GxEPD_BLACK : GxEPD_WHITE;

    display.setRotation(3);

    if (isPartial) {
        display.setPartialWindow(0, 0, 250, 122);
    } else {
        display.setFullWindow();
    }

    struct tm timeinfo;
    char clockStr[6] = "--:--";
    if (getLocalTime(&timeinfo)) {
        strftime(clockStr, sizeof(clockStr), "%H:%M", &timeinfo);
    }

    display.firstPage();
    do {
        display.fillScreen(bgColor);
        display.setTextColor(inkColor);

        // --- TOP HALF ---
        if (topCount == 0) {
            display.setTextSize(1);
            display.setCursor(4, 15);
            if (strlen(statusMsg) > 0) {
                display.print(statusMsg);
            } else {
                display.print("Keine Abfahrten (JH / Saatw.)");
            }
        } else {
            int yPositionsTop[2] = {1, 28};
            for (int i = 0; i < topCount; i++) {
                drawItem(4, yPositionsTop[i], topDeps[i], inkColor);
            }
        }

        // --- LEFT-ALIGNED FRAMED CLOCK + DASHED DIVIDER LINE (Y = 61) ---
        display.setTextSize(1);
        display.setCursor(4, 58);
        display.print(clockStr);
        display.drawRect(1, 55, 36, 14, inkColor);

        // Line starts at x=44 (not x=0) so it doesn't run through the clock box.
        for (int x = 44; x <= 246; x += 10) {
            int w = min(6, 246 - x + 1);
            if (w > 0) {
                display.drawFastHLine(x, 61, w, inkColor);
            }
        }

        // --- BOTTOM HALF ---
        if (bottomCount == 0) {
            display.setTextSize(1);
            display.setCursor(4, 89);
            if (strlen(statusMsg) > 0) {
                display.print(statusMsg);
            } else {
                display.print("Keine Abfahrten (M27 / 123)");
            }
        } else {
            int yPositionsBottom[2] = {78, 105};
            for (int i = 0; i < bottomCount; i++) {
                drawItem(4, yPositionsBottom[i], bottomDeps[i], inkColor);
            }
        }

    } while (display.nextPage());
}

// Fetches departures from the API (with fallback), parses them into
// topDeps/bottomDeps, and triggers a redraw. Also handles WiFi/HTTP/JSON
// error states by falling back to a status message instead of a blank screen.
void fetchAndDraw(bool isPartial) {
    if (wifiMulti.run() != WL_CONNECTED) {
        Serial.println("WIFI getrennt, Re-Connect läuft...");
        if (topCount == 0 && bottomCount == 0) strncpy(statusMsg, "WIFI Re-Connect...", sizeof(statusMsg) - 1);
        drawUI(isPartial);
        return;
    }

    int code = -1;
    String payload = ""; // kept as String on purpose: only ONE allocation per
                          // fetch here, so it doesn't cause the fragmentation
                          // risk that repeated String ops elsewhere would.

    // Try the primary endpoint first; on any non-200 result, retry with the
    // fallback mirror before giving up.
    for (int apiIdx = 0; apiIdx < 2; apiIdx++) {
        WiFiClientSecure client;
        client.setInsecure(); // skips certificate validation — acceptable for
                               // this private/home-network use case, but note
                               // this is not how you'd do it for anything
                               // handling sensitive data.
        client.setTimeout(10000);

        HTTPClient http;
        if (http.begin(client, API_URLS[apiIdx])) {
            http.setTimeout(10000);
            http.addHeader("User-Agent", "ESP32-BVG-Display/1.0");
            http.addHeader("Connection", "close");

            code = http.GET();
            Serial.printf("API [%d] Status Code: %d\n", apiIdx, code);

            if (code == 200) {
                payload = http.getString();
                http.end();
                break; // success, no need to try the fallback
            }
            http.end();
        }
        delay(1000); // brief pause before retrying with the fallback endpoint
    }

    if (code == 200 && payload.length() > 0) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
            statusMsg[0] = '\0';
            topCount = 0;
            bottomCount = 0;

            for (JsonObject dep : doc["departures"].as<JsonArray>()) {
                const char* lineRaw = dep["line"]["name"] | "?";
                const char* dirRaw = dep["direction"] | "Unbekannt";
                const char* whenStr = dep["when"] | dep["plannedWhen"];

                int mins = minutenBisAbfahrt(whenStr);
                // Skip anything invalid or already in the past (this also
                // filters out the -1 parse-error sentinel from above).
                if (mins < 0) continue;

                int delayMin = 0;
                bool hasDelay = false;
                if (!dep["delay"].isNull()) {
                    int delaySec = dep["delay"].as<int>();
                    int dMin = delaySec / 60;
                    if (dMin != 0) {
                        hasDelay = true;
                        delayMin = dMin;
                    }
                }

                // Fixed-size buffers throughout this loop instead of String,
                // since this runs on every fetch cycle — see note on
                // heap fragmentation above drawItem().
                char dirLower[DIR_LEN];
                strncpy(dirLower, dirRaw, sizeof(dirLower) - 1);
                dirLower[sizeof(dirLower) - 1] = '\0';
                for (char* p = dirLower; *p; p++) *p = tolower(*p);

                char dirFinal[DIR_LEN];
                strncpy(dirFinal, dirRaw, sizeof(dirFinal) - 1);
                dirFinal[sizeof(dirFinal) - 1] = '\0';

                // Line 123 reports several different destination strings for
                // the same physical direction — normalize them to one label.
                bool isLine123 = strstr(lineRaw, "123") != nullptr;
                if (isLine123) {
                    bool matchesSaatwinkler =
                        strstr(dirLower, "mäckeritzwiesen") != nullptr ||
                        strstr(dirLower, "saatwinkler") != nullptr ||
                        strstr(dirLower, "rohrdamm") != nullptr ||
                        (strstr(dirLower, "hauptbahnhof") == nullptr && strstr(dirLower, "turmstr") == nullptr);

                    if (matchesSaatwinkler) {
                        strncpy(dirFinal, "Saatwinkler Damm", sizeof(dirFinal) - 1);
                        dirFinal[sizeof(dirFinal) - 1] = '\0';
                        strncpy(dirLower, "saatwinkler damm", sizeof(dirLower) - 1);
                        dirLower[sizeof(dirLower) - 1] = '\0';
                    }
                }

                // Sort each departure into the top or bottom block based on
                // its direction (top = towards Jungfernheide/Saatwinkler).
                bool isTop = (strstr(dirLower, "jungfernheide") != nullptr || strstr(dirLower, "saatwinkler") != nullptr);

                if (isTop) {
                    if (topCount < 2) {
                        strncpy(topDeps[topCount].line, lineRaw, LINE_LEN - 1);
                        topDeps[topCount].line[LINE_LEN - 1] = '\0';
                        strncpy(topDeps[topCount].direction, dirFinal, DIR_LEN - 1);
                        topDeps[topCount].direction[DIR_LEN - 1] = '\0';
                        topDeps[topCount].minutes = mins;
                        topDeps[topCount].delayMin = delayMin;
                        topDeps[topCount].hasDelay = hasDelay;
                        topCount++;
                    }
                } else {
                    if (bottomCount < 2) {
                        strncpy(bottomDeps[bottomCount].line, lineRaw, LINE_LEN - 1);
                        bottomDeps[bottomCount].line[LINE_LEN - 1] = '\0';
                        strncpy(bottomDeps[bottomCount].direction, dirFinal, DIR_LEN - 1);
                        bottomDeps[bottomCount].direction[DIR_LEN - 1] = '\0';
                        bottomDeps[bottomCount].minutes = mins;
                        bottomDeps[bottomCount].delayMin = delayMin;
                        bottomDeps[bottomCount].hasDelay = hasDelay;
                        bottomCount++;
                    }
                }

                // Stop early once both blocks are full — no need to keep
                // scanning the rest of the API response.
                if (topCount >= 2 && bottomCount >= 2) break;
            }
        } else {
            Serial.printf("JSON Error: %s\n", error.c_str());
            if (topCount == 0 && bottomCount == 0) strncpy(statusMsg, "JSON Fehler", sizeof(statusMsg) - 1);
        }
    } else {
        Serial.printf("HTTP Error (beide APIs fehlgeschlagen): %d\n", code);
        if (topCount == 0 && bottomCount == 0) strncpy(statusMsg, "API Server 503", sizeof(statusMsg) - 1);
    }

    drawUI(isPartial);
}

void setup() {
    Serial.begin(115200);

    pinMode(EPD_PWR, OUTPUT);
    digitalWrite(EPD_PWR, HIGH);
    delay(100);

    // Both buttons use INPUT_PULLUP, so a press reads as LOW.
    pinMode(MENU_KEY, INPUT_PULLUP);
    pinMode(EXIT_KEY, INPUT_PULLUP);

    // Set SPI pins explicitly — otherwise the ESP32-S3 default MISO pin
    // collides with EPD_DC on this board.
    SPI.begin(EPD_SCK, -1, EPD_MOSI, -1);
    display.init(0);

    wifiMulti.addAP(SECRET_SSID, SECRET_PASSWORD);
    wifiMulti.addAP(SECRET_SSID_2, SECRET_PASSWORD_2);

    unsigned long wifiStart = millis();
    while (wifiMulti.run() != WL_CONNECTED && millis() - wifiStart < 15000) {
        delay(300);
    }

    configTzTime(tz_berlin, ntp_server);
    struct tm t_time;
    unsigned long ntpStart = millis();
    while (!getLocalTime(&t_time) && millis() - ntpStart < 10000) {
        delay(200);
    }

    fetchAndDraw(false);
    lastUpdate = millis();
}

void loop() {
    unsigned long now = millis();

    // Top button: toggle dark mode. Uses a full refresh so the color
    // inversion is clean, with no leftover ghosting from the old colors.
    if (digitalRead(MENU_KEY) == LOW) {
        DARK_MODE = !DARK_MODE;
        drawUI(false);
        delay(300); // basic debounce
    }

    // Bottom button: force an immediate data refresh outside the normal
    // schedule. lastUpdate/refreshCounter are reset afterwards so the
    // scheduled refresh below doesn't fire again right on top of this one.
    if (digitalRead(EXIT_KEY) == LOW) {
        fetchAndDraw(false);
        lastUpdate = now;
        refreshCounter = 0;
        delay(300); // basic debounce
    }

    // Scheduled refresh: partial refresh most cycles (fast, less flicker),
    // full refresh every 10th cycle to clear any accumulated ghosting.
    if (now - lastUpdate >= UPDATE_INTERVAL) {
        lastUpdate = now;
        refreshCounter++;

        if (refreshCounter >= 10) {
            refreshCounter = 0;
            fetchAndDraw(false);
        } else {
            fetchAndDraw(true);
        }
    }

    delay(100);
}