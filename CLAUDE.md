# Bitaxe BLE Energy Nanopayment - Claude Code Guide

## Project Overview

This is a modified [Bitaxe](https://github.com/skot/ESP-Miner) (ESP32-S3 Bitcoin miner) firmware that adds **BLE energy nanopayment** capability. The Bitaxe acts as an **energy buyer**: it scans for an eCandle (ESP32-C3 energy seller) over BLE, negotiates a price, and pays per-slice using EIP-3009 `TransferWithAuthorization` micropayments. When buying energy, the Bitaxe fan turns on (mining); when price is too high, it stops.

## Architecture

```
Bitaxe (ESP32-S3, this repo)          eCandle (ESP32-C3, separate repo)
├── BLE Central (buyer)         ←BLE→  BLE Peripheral (seller)
├── boat-mwr SDK (crypto/pay)          boat-mwr SDK (crypto/verify)
├── hashanchor (attestation)            Energy metering + relay control
├── AxeOS web UI (Angular)              Demo web UI
└── BM1370 ASIC (mining)               Heating element
```

### Key Files

| File | Purpose |
|------|---------|
| `main/ble_buyer.c` | BLE buyer state machine (scan → connect → pay → repeat) |
| `main/ble_buyer.h` | Buyer states, result struct, public API |
| `main/ble_x402.c` | BLE peripheral server (x402 GATT service, not used in demo) |
| `main/http_server/http_server.c` | REST API including `/api/boat/buy` (POST=start, GET=status) |
| `main/display.c` / `main/screen.c` | OLED display (shows price, state, payment info) |
| `components/boat-mwr/` | BoAT Machine Wallet Runtime SDK (crypto, EIP-712, EIP-3009) |
| `components/hashanchor/` | Mining attestation + payment orchestration |
| `sdkconfig.ble` | NimBLE config overrides (must append after `idf.py set-target`) |

### BLE GATT Service (0xEE00)

| UUID | Name | Direction | Purpose |
|------|------|-----------|---------|
| 0xEE01 | StreamRequest | Buyer→Seller | Initiate purchase session |
| 0xEE02 | StreamOffer | Seller→Buyer | Price offer (notify) |
| 0xEE03 | SliceRequest | Seller→Buyer | Per-slice payment request (notify) |
| 0xEE04 | SlicePayment | Buyer→Seller | EIP-3009 signed payment |
| 0xEE05 | StreamStatus | Seller→Buyer | Session state changes (notify) |
| 0xEE06 | DeviceInfo | Read | Seller device info + realtime price |

### BLE Buyer State Machine

```
IDLE → SCANNING → [WAITING] → CONNECTING → DISCOVERING → READING_INFO
  → NEGOTIATING → STREAMING → DECIDING → (back to SCANNING)
```

- **SCANNING**: Passive BLE scan for "ECandle" device
- **WAITING**: Found seller but price > threshold, keep scanning
- **STREAMING**: Actively paying per-slice, fan ON, mining active
- **DECIDING**: Session complete, disconnecting before rescan

## Build & Flash

### Prerequisites

- ESP-IDF v5.5.1 (`source ~/esp/esp-idf/export.sh`)
- Target: ESP32-S3

### Build

```bash
cd /Users/leolin/Downloads/ESP-Miner
source ~/esp/esp-idf/export.sh
GITHUB_ACTIONS=true idf.py build    # GITHUB_ACTIONS=true skips Angular build
```

The `GITHUB_ACTIONS=true` env var is **required** unless you have Java installed (needed for OpenAPI generator in the Angular build).

### OTA Flash

```bash
curl -s -X POST http://192.168.31.123/api/system/OTA \
  --data-binary @build/esp-miner.bin \
  -H "Content-Type: application/octet-stream" \
  --max-time 300
```

Device reboots automatically after OTA. Wait ~8s before sending commands.

### sdkconfig Gotcha

`idf.py set-target esp32s3` **overwrites** sdkconfig and removes NimBLE Central role. After set-target, you must re-append:

```bash
cat sdkconfig.ble >> sdkconfig
```

Key settings in sdkconfig.ble:
- `BT_NIMBLE_ROLE_CENTRAL=y` (required for BLE buyer)
- `BT_NIMBLE_ROLE_OBSERVER=y`
- `BT_NIMBLE_MAX_CONNECTIONS=1`
- `BT_NIMBLE_ATT_PREFERRED_MTU=512` (SlicePayment JSON ~480 bytes)

## Demo Operation

### Start Auto Mode

```bash
curl -s -X POST http://192.168.31.123/api/boat/buy \
  -H "Content-Type: application/json" \
  -d '{"device_name":"ECandle","threshold":60,"max_slices":2,"slice_seconds":6,"auto_mode":true}'
```

Parameters:
- `device_name`: BLE name to scan for (**"ECandle"** with capital E, case-sensitive)
- `threshold`: Max price in μUSDC/slice (60 = ~$1.43/kWh)
- `max_slices`: Slices per session before re-evaluating price
- `slice_seconds`: Duration of each slice
- `auto_mode`: `true` for continuous loop

### Monitor Status

```bash
curl -s http://192.168.31.123/api/boat/buy
```

Returns JSON with: `state`, `slices_paid`, `price_per_slice`, `mining`, `auto`, `threshold`, `sessions`, `error`

### Cancel

There is no cancel endpoint currently; reboot the device or OTA new firmware.

### Price Formula

```
$/kWh = price_per_slice / 1,000,000 / 0.0000417
```

Where `0.0000417` is eCandle's `PRICE_WH_FACTOR` (Wh per slice). Example: 60 μUSDC/slice = $1.44/kWh.

## EIP-3009 Payment Details

- **Domain**: `GatewayWalletBatched`, version `1`, chainId `5042002` (Arc Testnet)
- **verifyingContract**: `0x0077777d7EBA4688BDeF3E311b846F25870A19B9` (Gateway)
- **Token**: USDC on Arc Testnet
- **validBefore**: Always `now + 345600` (4 days) — ignores eCandle's 10s window
- **Signature**: secp256k1 via `boat_pay_authorize()`

## Known Issues & Gotchas

1. **eCandle name is "ECandle"** (capital E) — `strstr` is case-sensitive
2. **POST /api/system doesn't work** — must use `PATCH /api/system` for fan/mining control
3. **No vTaskDelay in NimBLE GAP callback** — blocks host task, breaks auto rescan
4. **Stack overflow during signing** — Timer and NimBLE host task stacks must be 8192 bytes
5. **NimBLE must be pinned to core 1** — core 0 conflicts with WiFi
6. **eCandle settle worker** has `invalid_signature` issue (their side, not ours)
7. **eCandle build environment** is broken (they can't OTA themselves currently)
8. **HTTP body buffer** was 64 bytes, now 256 — API field names `auto_mode`/`device_name` are supported alongside `auto`/`target`

## Network Info (Demo Setup)

- Bitaxe IP: `192.168.31.123`
- eCandle IP: `192.168.31.88`
- WiFi: same LAN required for both devices + your Mac

## Release

- Current: **v2.13.1-hashanchor-6** (but local code has newer fixes not yet released)
- Repo: https://github.com/linyao0177/ESP-Miner
- Release: https://github.com/linyao0177/ESP-Miner/releases/tag/v2.13.1-hashanchor-6
