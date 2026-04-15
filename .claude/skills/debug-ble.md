---
name: debug-ble
description: Debug BLE buyer issues - connection errors, state machine problems, payment failures
user_invocable: true
---

# Debug BLE Buyer Issues

## Quick Diagnosis

Check current state:
```bash
curl -s http://192.168.31.123/api/boat/buy | python3 -m json.tool
```

Key fields to check:
- `state`: Current state machine position
- `error`: Error message if state is "error"
- `auto`: Should be `true` for auto mode
- `mining`: `true` when fan is on
- `write_rc`: Return code from last BLE GATT write (0 = success)
- `mtu`: Negotiated MTU (should be 256, needed for 480-byte JSON)
- `h_pay`/`h_req`: Discovered GATT handles (0 = not discovered yet)

## Common Issues

### "connect: 14" (BLE_ERR_CONN_LIMIT)
Stale BLE connection from previous session. The firmware now cleans up stale connections on start, but if persistent:
- Reboot Bitaxe (power cycle or OTA)
- Check if eCandle is in a bad state (may need reboot too)

### auto=false / auto field missing
The HTTP handler expects `"auto_mode":true` or `"auto":true` in the POST body. The body buffer is 256 bytes. Check:
- JSON isn't malformed
- Field name matches

### State stuck at "complete" with auto=true
The auto watchdog timer (10s) should recover from stuck states. If it doesn't:
- Check `ble_buyer.c` auto_timer_cb logic
- The timer restarts scan when state is COMPLETE/ERROR/IDLE and auto_mode=1

### Fan not spinning
AxeOS fan control uses PATCH, not POST:
```c
// In ble_buyer.c: axeos_patch() uses HTTP_METHOD_PATCH
esp_http_client_set_method(client, HTTP_METHOD_PATCH);
```
If fan doesn't spin, check `axeos_patch()` function and the JSON payload `{"fanspeed":100}` / `{"fanspeed":0}`.

### Price mismatch between OLED and eCandle
OLED reads realtime price from DeviceInfo characteristic (0xEE06) after each payment. The field is `price_uslice` in the JSON. Formula: `$/kWh = price_uslice / 1e6 / 0.0000417`.

### Stack overflow during signing
EIP-712 + secp256k1 signing needs ~3-4KB stack. Check sdkconfig:
- `CONFIG_ESP_TIMER_TASK_STACK_DEPTH=8192`
- `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=8192`

## BLE State Machine Flow (for debugging)

```
POST /api/boat/buy
  → ble_buyer_start_auto()
    → ble_buyer_start()
      → ble_gap_disc() [scan 15s]
        → GAP_EVENT_DISC [found ECandle]
          → price check (auto mode)
            → price > threshold: WAITING, break (scan continues)
            → price <= threshold: ble_gap_connect()
              → GAP_EVENT_CONNECT
                → ble_gattc_disc_svc_by_uuid(0xEE00)
                  → on_svc_disc → chr discovery → cccd subscribe
                    → write StreamRequest (0xEE01)
                      → StreamOffer notify (0xEE02)
                        → SliceRequest notify (0xEE03)
                          → sign_and_send_slice() → write SlicePayment (0xEE04)
                            → StreamStatus notify (0xEE05)
                              → STREAMING/COMPLETE
```

## Useful Log Tags (if serial connected)

- `ble_buyer`: Main buyer state machine
- `boat_pay`: EIP-3009 signing
- `hashanchor`: Attestation task
- `NimBLE`: BLE stack internals
