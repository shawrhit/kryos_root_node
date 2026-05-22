# KryOS Root Node: Mesh-to-SPI Bridge

This repository contains the firmware for the **KryOS Root Node Bridge**. It acts as the master anchor for the sensor mesh and the physical gateway to the Raspberry Pi Linux gateway.

## Core Features

### 1. Kingmaker Arbitration
- **Centralized Selection:** Evaluates all sensor candidates and selects a leader based on best RSSI.
- **Term Lock:** Enforces a **60-second leadership term** to prevent frequent switching and ensure monotonic round numbering.
- **Dead-Leader Failover:** Implements a 15-second heartbeat monitor; triggers immediate re-election if the "King" is unresponsive.

### 2. Physical Gateway (SPI Bridge)
- **IRQ-Driven:** Uses an Active Low interrupt (`GPIO_25`) to signal the Raspberry Pi when new consensus data is ready.
- **High-Payload SPI:** Transmits a 47-byte authenticated packet (15 bytes telemetry + 32 bytes HMAC).

### 3. Cryptographic Re-Signing
- **Leader Verification:** Verifies mesh internal signatures using `KRYOS_NODE_PSK`.
- **Master Authentication:** Re-signs finalized consensus data with the `KRYOS_MASTER_PSK` for the Linux Kernel driver.

## Hardware Pinout (per Project Bible)

| Signal | ESP32 Pin | RPi Pin |
|--------|-----------|---------|
| MOSI   | GPIO 23   | MOSI    |
| MISO   | GPIO 19   | MISO    |
| SCLK   | GPIO 18   | SCLK    |
| CS     | GPIO 4    | CE0     |
| IRQ    | GPIO 25   | GPIO 24 |

## Configuration
The bridge operates in **APSTA mode** without an external router to ensure 100% offline capability.

## Build & Flash
```bash
idf.py build flash monitor
```
