# Bus Feed

An ESP32-based e-paper display that shows the next bus departures at my stop in real time, so I know at a glance on my way out the door whether I need to rush or still have time.

<img width="3462" height="2598" alt="BusFeed_Main" src="https://github.com/user-attachments/assets/c1819866-6247-47f0-b5aa-5ec5bf9ea194" />

## Motivation

This project had two goals. First, I wanted to understand how to call external REST APIs and parse JSON responses on a microcontroller. Until then I'd only worked with data from onboard sensors (temperature, humidity), not live data from the internet. Second, I wanted something I'd actually use daily: instead of checking the BVG app, a glance at the display next to the door is now enough.

## Tech Stack

- **Hardware:** ESP32-S3 (Elecrow CrowPanel 2.13" e-paper, SSD1680 driver)
- **Framework:** Arduino / PlatformIO
- **Libraries:** GxEPD2 (display driver), ArduinoJson (JSON parsing), WiFiMulti, HTTPClient, WiFiClientSecure
- **Data source:** [v6.bvg.transport.rest](https://v6.bvg.transport.rest) (unofficial, free REST API for real-time BVG/VBB departures), with automatic fallback to the VBB mirror endpoint on outages

<img width="3438" height="2579" alt="BusFeed_Hand" src="https://github.com/user-attachments/assets/3855fd0b-c623-4700-98ca-0abc72eaa4c1" />

## Features

- Live departures for two stop directions at once (top/bottom sections), including delay indicators
- Automatic fallback between two API endpoints on server errors
- Alternates partial and full display refreshes (a full refresh every 10th cycle) to avoid e-paper ghosting while keeping refresh times low
- Dark mode toggle via button press (inverts foreground/background colors)
- Manual instant refresh via a second button, independent of the 60-second cycle
- Status messages for WiFi issues, JSON errors, or API outages instead of a blank or frozen display

## What I Learned

- First hands-on experience making HTTP requests and parsing JSON on a microcontroller, including the pitfalls specific to embedded environments (timeouts, TLS handshakes, limited memory)
- Learned why Arduino's `String` class can cause heap fragmentation under frequent, repeated use (as in this project's per-minute fetch cycle), and how to use fixed `char` arrays instead
- Debugged timezone/date parsing (`sscanf`, `struct tm`, `mktime`) to calculate minutes until departure
- Learned that free, unofficial APIs like this one can have occasional downtime, and why a fallback/status-display strategy matters more than assuming "it'll just work"
- Iterative layout fine-tuning directly on real hardware (pixel positioning, spacing, small UI details like a frame around the clock)

## Known Limitations

- Stop ID, lines, and direction matching (e.g. "Jungfernheide"/"Saatwinkler") are currently hardcoded rather than configurable
- TLS certificate validation is intentionally disabled (`setInsecure()`). Acceptable for private use on a home network, but not a production-grade approach
- During a longer API outage, the display currently only shows a status message instead of the last known departures
- No watchdog timer: if WiFi or HTTP hangs, a manual reset is currently the only fix
