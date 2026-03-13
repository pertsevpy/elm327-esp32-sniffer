# ELM327 UART Sniffer / Bridge for ESP32-C6


This project turns an ESP32-C6 into a **USB-to-UART bridge + WiFi sniffer** for ELM327 OBD2 adapters. It replaces the common FT232/CH340 USB-UART converter, allowing you to:

- Use your ELM327 with any computer/tablet via USB (CDC) – the ESP32 acts as a transparent bridge.
- **Sniff all UART traffic** between the PC and ELM327 and view it live in a web browser over WiFi.
- **Send custom commands** (text or HEX) to the ELM327 directly from the web interface.

All communication is logged and can be saved/cleared from the browser.

## Features

- ✅ **USB‑UART Bridge** – transparently forwards data between USB (PC) and UART (ELM327).
- ✅ **WiFi Sniffer** – ESP32‑C6 acts as an access point; connect any device to `ELM327-Sniffer` and watch live traffic at `http://192.168.4.1`.
- ✅ **Custom Command Input** – send any AT command or raw HEX sequence.
- ✅ **Baud Rate Switching** – select from 9600, 19200, 38400, 115200, 230400 via dropdown.
- ✅ **UART Error Diagnostics** – write errors and buffer congestion are logged.
- ✅ **Persistent Log History** – new WebSocket clients receive the last 4KB of log.
- ✅ **Optional DS18B20** – connect a Dallas temperature sensor.

## Hardware Requirements

- **ESP32-C6** development board (with USB‑C connected to CDC).
- **ELM327 OBD2 adapter**.
- **(Optional)** DS18B20 temperature sensor + 4.7kΩ pull‑up resistor.

## Wiring

| ESP32‑C6       | ELM327           | DS18B20 (optional) |
|----------------|------------------|--------------------|
| GPIO4 (RX1)    | TX               | –                  |
| GPIO5 (TX1)    | RX               | –                  |
| GND            | GND              | GND                |
| 3.3V (or 5V)   | VCC (if 5V, use level shifter) | VCC (3.3V) |
| GPIO18         | –                | DATA (with 4.7kΩ pull‑up to 3.3V) |

## Software Setup

### Arduino IDE

1. Install **ESP32 board support** via Board Manager.
2. Select board: **ESP32C6 Dev Module**.
3. Enable **USB CDC On Boot**: `Tools → USB CDC On Boot → Enabled`.
4. Install required libraries (Library Manager):
   - `WebSockets` by Markus Sattler (Links2004)
   - (Optional) `OneWire` and `DallasTemperature` if using DS18B20.

### Configuration

Before uploading, you can adjust these settings in the sketch:
- `ap_ssid`, `ap_password` – WiFi credentials.
- `ELM_UART_BAUD` – default baud rate (38400).
- `NORMALIZE_LF_TO_CR` – if `true`, converts `\n` to `\r` in USB commands.
- `USE_DS18B20` – uncomment to enable the temperature sensor.

## Usage

1. Upload the sketch to your ESP32-C6.
2. Connect the ESP32 to your computer via USB – it will appear as a virtual COM port (CDC).
3. Power the ELM327 (ensure common GND).
4. Connect to WiFi network **ELM327-Sniffer** (password: `12345678`).
5. Open a browser and go to **http://192.168.4.1**.
6. You will see the live UART log.

### Understanding the Log

Lines are prefixed with direction:
- `[USB->ELM]` – data from PC to ELM327.
- `[ELM->USB]` – data from ELM327 to PC (also sent to USB).
- `[WEB->ELM]` – commands sent from the web interface.
- `[ELM->USB (timeout)]` – indicates that a response was broken by a pause > 100 ms (normal for SEARCHING... or multi‑packet replies).

## Troubleshooting

- **No response from ELM327** – check power, GND, logic levels. Ensure baud rate matches (default 38400).
- **PC doesn't see COM port** – verify USB CDC On Boot is enabled. Try a different USB cable.
- **Web interface not loading** – make sure you are connected to the ESP32's WiFi (IP 192.168.4.1).

## Disclaimer

THIS PRODUCT IS TARGETED TO EXPERIENCED USERS AT THEIR OWN RISK. 

TO THE FULLEST EXTENT PERMISSIBLE BY THE APPLICABLE LAW, I HEREBY DISCLAIM ANY AND ALL RESPONSIBILITY RESULTING FROM OPERATION OF THIS PRODUCT.

## Acknowledgements

- Based on ideas from various OBD2 and ESP32 community projects.
- Special thanks to everyone who shared UDS commands.
