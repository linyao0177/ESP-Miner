---
name: demo-test
description: Start and monitor the BLE energy nanopayment demo on Bitaxe
user_invocable: true
---

# BLE Energy Nanopayment Demo Test

## Start Auto Mode Demo

```bash
curl -s -X POST http://192.168.31.123/api/boat/buy \
  -H "Content-Type: application/json" \
  -d '{"device_name":"ECandle","threshold":60,"max_slices":2,"slice_seconds":6,"auto_mode":true}'
```

Key parameters:
- `device_name`: Must be **"ECandle"** (capital E, case-sensitive BLE name match)
- `threshold`: 60 μUSDC/slice = ~$1.43/kWh. Below this → buy + fan ON. Above → wait.
- `max_slices`: 2 slices per session before price re-evaluation
- `auto_mode`: true for continuous scan → buy → rescan loop

## Monitor Demo Status

Run a monitoring loop (6s interval, ~6 minutes):

```bash
for i in $(seq 1 60); do
  ts=$(date +%H:%M:%S)
  j=$(curl -s --max-time 5 http://192.168.31.123/api/boat/buy 2>/dev/null)
  st=$(echo "$j" | python3 -c "
import sys,json
d=json.load(sys.stdin)
a=d.get('auto',False)
m='<<< FAN ON >>>' if d.get('mining') else ''
s=d.get('sessions','?')
print(f'{d[\"state\"]} slc={d[\"slices_paid\"]}/{d[\"max_slices\"]} pps={d.get(\"price_per_slice\",\"?\")} sess={s} auto={a} {m}')
" 2>/dev/null || echo "no response")
  echo "[$i] $ts $st"
  sleep 6
done
```

## Expected Behavior

The demo cycles through eCandle's sine-wave pricing (~60s period):
- **waiting** (pps > 60): Price too high, fan OFF, scanning for price drop
- **streaming** (pps <= 60): Buying energy, fan ON, paying per-slice
- **deciding**: Session complete, disconnecting before rescan

A successful demo shows alternating waiting/streaming states across multiple sessions (sess incrementing).

## Single Status Check

```bash
curl -s http://192.168.31.123/api/boat/buy | python3 -m json.tool
```

## What to Watch For

- `auto=True` must appear — if `False`, the auto_mode parameter wasn't parsed
- `sessions` should increment after each streaming cycle
- Fan should physically spin during `streaming` state
- OLED should show $/kWh price and payment info
- eCandle's demo page (http://192.168.31.88) shows payment log from seller side

## Threshold Guide

eCandle sine wave: ~20-120 μUSDC/slice ($0.48-$2.88/kWh), 60s period.
- threshold=60 → ~$1.43/kWh → roughly 50/50 buy/wait split
- threshold=40 → ~$0.96/kWh → buys less often (stricter)
- threshold=80 → ~$1.92/kWh → buys more often (looser)
