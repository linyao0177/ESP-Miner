---
name: build-and-ota
description: Build ESP-Miner firmware and OTA flash to Bitaxe device
user_invocable: true
---

# Build and OTA Flash Bitaxe Firmware

## Steps

1. Build the firmware:
```bash
cd /Users/leolin/Downloads/ESP-Miner && source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && GITHUB_ACTIONS=true idf.py build 2>&1 | tail -8
```

IMPORTANT: `GITHUB_ACTIONS=true` is required to skip the Angular web UI build (needs Java). The build takes ~30-60 seconds for incremental changes.

2. Verify build succeeded (look for "Project build complete" in output).

3. OTA flash to Bitaxe:
```bash
curl -s -X POST http://192.168.31.123/api/system/OTA \
  --data-binary @/Users/leolin/Downloads/ESP-Miner/build/esp-miner.bin \
  -H "Content-Type: application/octet-stream" \
  --max-time 300
```

Expected response: `Firmware update complete, rebooting now!`

4. Wait for reboot and verify device is back online:
```bash
curl -s --max-time 15 --retry 5 --retry-delay 3 http://192.168.31.123/api/system/info | head -3
```

## Troubleshooting

- If OTA times out: check that your Mac is on the same WiFi as Bitaxe (192.168.31.x network)
- If build fails with NimBLE errors: run `cat sdkconfig.ble >> sdkconfig` and rebuild
- If build fails with "command not found: idf.py": ESP-IDF not sourced, run `source ~/esp/esp-idf/export.sh`
- Bitaxe IP is `192.168.31.123` — if unreachable, check OLED display for current IP
