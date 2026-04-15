---
name: release
description: Create a new GitHub release for ESP-Miner with firmware binary
user_invocable: true
---

# Create ESP-Miner Release

## Steps

1. Ensure all changes are committed:
```bash
cd /Users/leolin/Downloads/ESP-Miner && git status
```

2. Build firmware:
```bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && GITHUB_ACTIONS=true idf.py build 2>&1 | tail -5
```

3. Get binary size:
```bash
ls -lh build/esp-miner.bin
```

4. Determine version tag. Current convention: `v2.13.1-hashanchor-N` (increment N).
Check latest:
```bash
git tag --sort=-creatordate | head -5
```

5. Create tag and release:
```bash
VERSION="v2.13.1-hashanchor-7"  # adjust as needed
git tag $VERSION
git push origin $VERSION

gh release create $VERSION build/esp-miner.bin \
  --title "$VERSION" \
  --notes "$(cat <<'EOF'
## BLE Energy Nanopayment Demo

### Changes
- (list changes since last release)

### Build
```bash
source ~/esp/esp-idf/export.sh
GITHUB_ACTIONS=true idf.py build
```

### OTA Flash
```bash
curl -X POST http://<bitaxe-ip>/api/system/OTA \
  --data-binary @esp-miner.bin \
  -H "Content-Type: application/octet-stream"
```

### Demo
```bash
curl -X POST http://<bitaxe-ip>/api/boat/buy \
  -H "Content-Type: application/json" \
  -d '{"device_name":"ECandle","threshold":60,"max_slices":2,"slice_seconds":6,"auto_mode":true}'
```
EOF
)"
```

6. Push any unpushed commits:
```bash
git push origin master
```

## Notes
- Firmware is a full build (~1.55MB), not incremental patch
- The repo is at https://github.com/linyao0177/ESP-Miner
- Always include build instructions in release notes for reproducibility
