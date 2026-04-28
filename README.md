# Kryos Root Node

Kryos is a B.Tech final year project for fault-tolerant vaccine cold chain monitoring with edge intelligence. This repository contains the ESP32 firmware for the root node, which acts as the local edge controller for collecting node data and forwarding compact frames over SPI.

## Project Goal

Vaccines must stay inside a safe temperature range during storage and transport. Kryos is designed to monitor the cold chain close to the source, detect abnormal conditions early, and keep operating even when parts of the network are unreliable.

The root node is responsible for:

- Coordinating local communication with connected modules
- Sending compact data frames over SPI
- Raising an active-low interrupt before each SPI transfer
- Providing a firmware base for future edge intelligence and fault-tolerant decision logic

## Current Firmware Behavior

The current root node firmware in `main/root_node.c`:

- Uses an ESP32 DevKit V1 board
- Sends a 4-byte hexadecimal payload over SPI every 5 seconds
- Changes the payload on every send by incrementing a 32-bit counter
- Pulls the interrupt line low before sending data
- Releases the interrupt line high after the SPI transfer completes
- Logs each transmitted payload over the serial monitor

Initial test payload:

```text
0x12345678
```

Each next frame increments by one.

## ESP32 DevKit V1 Pin Mapping

The firmware is configured for the ESP32 DevKit V1 VSPI pins:

| Signal | ESP32 Pin | Board Label |
| --- | --- | --- |
| SPI SCLK | GPIO18 | D18 |
| SPI MOSI | GPIO23 | D23 |
| SPI MISO | GPIO19 | D19 |
| SPI CS | GPIO5 | D5 |
| Interrupt | GPIO25 | D25 |
| Ground | GND | GND |

The interrupt line is active low. It is normally high, then pulled low just before SPI data is transmitted.

## Build

Set up ESP-IDF, then build the firmware:

```bash
source /home/shawrhit/esp/esp-idf/export.sh
idf.py set-target esp32
idf.py build
```

If the existing build directory has environment mismatch issues, rebuild with:

```bash
idf.py fullclean
idf.py build
```

## Flash And Monitor

Replace `PORT` with the serial port for the ESP32 board, for example `/dev/ttyUSB0`:

```bash
idf.py -p PORT flash monitor
```

Exit monitor with `Ctrl+]`.

## Repository Layout

```text
.
├── CMakeLists.txt
├── main
│   ├── CMakeLists.txt
│   └── root_node.c
├── sdkconfig
└── README.md
```

## Roadmap

Planned firmware work:

- Replace test hex payloads with real sensor and node status data
- Add packet structure with node ID, timestamp, temperature, health, and checksum fields
- Add fault detection for missing nodes, stale data, and unsafe temperature ranges
- Add local edge intelligence for early anomaly classification
- Add retry, buffering, and recovery behavior for unreliable links
