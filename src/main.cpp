#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
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
#define MENU_KEY  2     // top button (IO2 per schematic) — toggles dark mode
#define EXIT_KEY  1     // bottom button (IO1 per schematic) — forces a manual data refresh
constexpr uint8_t OK_KEY = 5;

GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT> display(
    GxEPD2_213_B74(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

const char* ntp_server = "pool.ntp.org";
const char* tz_berlin = "CET-1CEST,M3.5.0,M10.5.0/3";
WiFiMulti wifiMulti;
WebServer server(80);   // create server (accessible on port 80)
Preferences prefs;      // memory object that survives restarts

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

Departure dirADeps[2];
int dirACount = 0;

Departure dirBDeps[2];
int dirBCount = 0;

char statusMsg[32] = "";

// Primary endpoint + fallback mirror, tried in order on each fetch.
const char* API_HOSTS[2] = {
    "v6.bvg.transport.rest",
    "v6.vbb.transport.rest"
};

unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 60000;     // 60 seconds between scheduled refreshes
constexpr unsigned long CONFIG_TIMEOUT_MS = 60000;   // 60 s for testing, rais to 10 minutes later on
int refreshCounter = 0;
bool configModeActive = false;
unsigned long configModeStart = 0;
bool showUBahn = true;
bool showSBahn = true;
bool showBus = true;
bool showTram = true;
char stopID[16] = "900002200";
char stopName[48] = "";  // display name, only needed for the web page

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

// Renders the full screen: two departure blocks (A/B) separated by a
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
        if (dirACount == 0) {
            display.setTextSize(1);
            display.setCursor(4, 15);
            if (strlen(statusMsg) > 0) {
                display.print(statusMsg);
            } else {
                display.print("Keine Abfahrten (Pankow / HBF)");
            }
        } else {
            int yPositionsTop[2] = {1, 28};
            for (int i = 0; i < dirACount; i++) {
                drawItem(4, yPositionsTop[i], dirADeps[i], inkColor);
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
        if (dirBCount == 0) {
            display.setTextSize(1);
            display.setCursor(4, 89);
            if (strlen(statusMsg) > 0) {
                display.print(statusMsg);
            } else {
                display.print("Keine Abfahrten (JH / Saatw.)");
            }
        } else {
            int yPositionsBottom[2] = {78, 105};
            for (int i = 0; i < dirBCount; i++) {
                drawItem(4, yPositionsBottom[i], dirBDeps[i], inkColor);
            }
        }

    } while (display.nextPage());
}

void drawConfigScreen() {
    display.setRotation(3);
    display.setFullWindow();
    uint16_t inkColor = DARK_MODE ? GxEPD_WHITE : GxEPD_BLACK;
    uint16_t bgColor   = DARK_MODE ? GxEPD_BLACK : GxEPD_WHITE;

    display.firstPage();
    do {
        display.fillScreen(bgColor);
        display.setTextColor(inkColor);
        display.setTextSize(1);
        display.setCursor(4, 10);
        display.print("Set station: ");
        display.setCursor(4, 20);
        display.print("http://");
        display.print(WiFi.localIP());
        display.setCursor(4, 30);
        display.print("(Valid for 1 min)");
    } while (display.nextPage());

}

// Fetches departures from the API (with fallback), parses them into
// dirADeps/dirBDeps, and triggers a redraw. Also handles WiFi/HTTP/JSON
// error states by falling back to a status message instead of a blank screen.
void fetchAndDraw(bool isPartial) {
    if (wifiMulti.run() != WL_CONNECTED) {
        Serial.println("WIFI getrennt, Re-Connect läuft...");
        if (dirACount == 0 && dirBCount == 0) strncpy(statusMsg, "WIFI Re-Connect...", sizeof(statusMsg) - 1);
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

        char url [200];
        snprintf(url, sizeof(url),
            "https://%s/stops/%s/departures?results=15&duration=60&subway=%s&suburban=%s&bus=%s&tram=%s",
            API_HOSTS[apiIdx],
            stopID,
            showUBahn ? "true" : "false",
            showSBahn ? "true" : "false",
            showBus ? "true" : "false",
            showTram ? "true" : "false"
            );

        Serial.println(url);    // test print of the used url

        HTTPClient http;
        if (http.begin(client, url)) {
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
            dirACount = 0;
            dirBCount = 0;

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

                // Sort each departure into direction group A or B (A = towards Pankow / HBF, B = towards Jungfernheide / Saatwinkler)
                bool isDirA = !(strstr(dirLower, "jungfernheide") != nullptr || strstr(dirLower, "saatwinkler") != nullptr);

                if (isDirA) {
                    if (dirACount < 2) {
                        strncpy(dirADeps[dirACount].line, lineRaw, LINE_LEN - 1);
                        dirADeps[dirACount].line[LINE_LEN - 1] = '\0';
                        strncpy(dirADeps[dirACount].direction, dirFinal, DIR_LEN - 1);
                        dirADeps[dirACount].direction[DIR_LEN - 1] = '\0';
                        dirADeps[dirACount].minutes = mins;
                        dirADeps[dirACount].delayMin = delayMin;
                        dirADeps[dirACount].hasDelay = hasDelay;
                        dirACount++;
                    }
                } else {
                    if (dirBCount < 2) {
                        strncpy(dirBDeps[dirBCount].line, lineRaw, LINE_LEN - 1);
                        dirBDeps[dirBCount].line[LINE_LEN - 1] = '\0';
                        strncpy(dirBDeps[dirBCount].direction, dirFinal, DIR_LEN - 1);
                        dirBDeps[dirBCount].direction[DIR_LEN - 1] = '\0';
                        dirBDeps[dirBCount].minutes = mins;
                        dirBDeps[dirBCount].delayMin = delayMin;
                        dirBDeps[dirBCount].hasDelay = hasDelay;
                        dirBCount++;
                    }
                }

                // Stop early once both blocks are full — no need to keep
                // scanning the rest of the API response.
                if (dirACount >= 2 && dirBCount >= 2) break;
            }
        } else {
            Serial.printf("JSON Error: %s\n", error.c_str());
            if (dirACount == 0 && dirBCount == 0) strncpy(statusMsg, "JSON Fehler", sizeof(statusMsg) - 1);
        }
    } else {
        Serial.printf("HTTP Error (beide APIs fehlgeschlagen): %d\n", code);
        if (dirACount == 0 && dirBCount == 0) strncpy(statusMsg, "API Server 503", sizeof(statusMsg) - 1);
    }

    drawUI(isPartial);
}

void handleRoot() {
    server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Bus Feed</title>
<style>
    body { font-family: sans-serif; max-width: 500px; margin: 20px auto; padding: 0 16px; }
    .toggles { display: flex; gap: 16px; flex-wrap: wrap; margin: 16px 0; }
    label { font-size: 18px; }
    input[type=submit] { margin-top: 16px; padding: 10px 24px; font-size: 18px; }
    #search { width: 100%; padding: 10px; font-size: 18px; box-sizing: border-box; }
    #results { list-style: none; padding: 0; margin: 4px 0; }
    #results li { padding: 10px; border-bottom: 1px solid #ccc; cursor: pointer; font-size: 18px; }
</style>
</head>
<body>
<h1>Bus Feed</h1>
<form method="POST" action="/save">
    <input type="text" id="search" placeholder="Search station..." autocomplete="off">
    <ul id="results"></ul>
    <p>Selected: <b id="selected">none</b></p>
    <input type="hidden" name="stop" id="stop">
    <input type="hidden" name="stopname" id="stopname">

    <div class="toggles">
        <label><input type="checkbox" name="ubahn" checked> U-Bahn</label>
        <label><input type="checkbox" name="sbahn" checked> S-Bahn</label>
        <label><input type="checkbox" name="bus" checked> Bus</label>
        <label><input type="checkbox" name="tram" checked> Tram</label>
    </div>
    <input type="submit" value="Save">
</form>

<script>
const search = document.getElementById('search');
const results = document.getElementById('results');
let timer;

search.addEventListener('input', () => {
    clearTimeout(timer);
    const q = search.value.trim();
    if (q.length < 3) { results.innerHTML = ''; return; }
    timer = setTimeout(async () => {
        const url = 'https://v6.bvg.transport.rest/locations?query=' + encodeURIComponent(q)
                  + '&results=6&addresses=false&poi=false&pretty=false';
        try {
            const res = await fetch(url);
            const items = await res.json();
            results.innerHTML = '';
            for (const s of items) {
                if (s.type !== 'stop') continue;
                const li = document.createElement('li');
                li.textContent = s.name;
                li.onclick = () => {
                    document.getElementById('stop').value = s.id;
                    document.getElementById('stopname').value = s.name;
                    document.getElementById('selected').textContent = s.name;
                    results.innerHTML = '';
                    search.value = '';
                };
                results.appendChild(li);
            }
        } catch (e) {
            results.textContent = 'Search failed (no internet?)';
        }
    }, 300);
});
</script>
</body>
</html>
)rawliteral");
}

// Accepts only 1 to 15 digits, because the ID ends up in the request URL
bool isValidStopId(const String& id) {
    if (id.length() == 0 || id.length() > 15) return false;

    for (size_t i = 0; i < id.length(); i++) {
        if (!isdigit(id[i])) return false;
    }
    return true;
}

// Handles the POST from the config form. The browser only sends checked
// checkboxes, so hasArg() is true for checked and false for unchecked ones.
void handleSave() {
    showUBahn = server.hasArg("ubahn");
    showSBahn = server.hasArg("sbahn");
    showBus = server.hasArg("bus");
    showTram = server.hasArg("tram");

    if (isValidStopId(server.arg("stop"))) {
        strncpy(stopID, server.arg("stop").c_str(), sizeof(stopID) - 1);
        stopID[sizeof(stopID) - 1] = '\0';
    }

    if (server.arg("stopname").length() > 0) {
        strncpy(stopName, server.arg("stopname").c_str(), sizeof(stopName) - 1);
        stopName[sizeof(stopName) - 1] = '\0';
    }

    prefs.begin("busfeed", false);
    prefs.putBool("ubahn", showUBahn);  // key, value
    prefs.putBool("sbahn", showSBahn);  
    prefs.putBool("bus", showBus);  
    prefs.putBool("tram", showTram);  
    prefs.putString("stopid", stopID);
    prefs.putString("stopname", stopName);
    prefs.end();

    Serial.printf("U:%d S:%d Bus:%d Tram:%d\n", showUBahn, showSBahn, showBus, showTram);
    Serial.printf("stop: %s (%s)\n", stopID, stopName);
    server.send(200, "text/html", "<h1>Saved</h1>");
}

void setup() {
    Serial.begin(115200);

    pinMode(EPD_PWR, OUTPUT);
    digitalWrite(EPD_PWR, HIGH);
    delay(100);

    // All buttons use INPUT_PULLUP, so a press reads as LOW.
    pinMode(MENU_KEY, INPUT_PULLUP);
    pinMode(EXIT_KEY, INPUT_PULLUP);
    pinMode(OK_KEY, INPUT_PULLUP);

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

    // Wifi diagnosis in case of connection problems
    Serial.printf("WiFi status: %d, SSID: %s\n", WiFi.status(), WiFi.SSID().c_str());

    configTzTime(tz_berlin, ntp_server);
    struct tm t_time;
    unsigned long ntpStart = millis();
    while (!getLocalTime(&t_time) && millis() - ntpStart < 10000) {
        delay(200);
    }

    prefs.begin("busfeed", true);       // true => read only
    showUBahn = prefs.getBool("ubahn", true);
    showSBahn = prefs.getBool("sbahn", true);
    showBus = prefs.getBool("bus", true);
    showTram = prefs.getBool("tram", true);

    String saveId = prefs.getString("stopid", stopID);          // default = current value
    strncpy(stopID, saveId.c_str(), sizeof(stopID) - 1);
    stopID[sizeof(stopID) - 1] = '\0';

    String saveName = prefs.getString("stopname", stopName);    // default = current value
    strncpy(stopName, saveName.c_str(), sizeof(stopName) - 1);
    stopName[sizeof(stopName) - 1] = '\0';
    prefs.end();

    Serial.printf("U:%d S:%d Bus:%d Tram:%d\n", showUBahn, showSBahn, showBus, showTram);
    Serial.printf("stop: %s (%s)\n", stopID, stopName);

    fetchAndDraw(false);
    lastUpdate = millis();
    server.on("/", handleRoot);         // register route once, server.begin() happens on button press
    server.on("/save", handleSave);     // receives the form data
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

    if (digitalRead(OK_KEY) == LOW) {
        Serial.println("OK button was pressed.");
        if (!configModeActive) {
            configModeActive = true;
            configModeStart = now;
            server.begin();
            Serial.println(WiFi.localIP());
            drawConfigScreen();
        }
        delay(300);
    }

    if (configModeActive) {
        server.handleClient();
    }

    if (configModeActive && now - configModeStart >= CONFIG_TIMEOUT_MS) {
        configModeActive = false;
        server.stop();
        Serial.println("Config mode ended.");
        fetchAndDraw(false);
        lastUpdate = now;
        refreshCounter = 0;
    }

    // Scheduled refresh: partial refresh most cycles (fast, less flicker),
    // full refresh every 10th cycle to clear any accumulated ghosting.
    if (!configModeActive && now - lastUpdate >= UPDATE_INTERVAL) {
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