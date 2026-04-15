# Bitaxe BLE Energy Nanopayment Demo — 交接文档

## 1. 项目概述

Bitaxe（ESP32-S3 比特币矿机）改造固件，增加 **BLE 能源微支付**功能。Bitaxe 作为 **能源买家**，通过 BLE 扫描 eCandle（ESP32-C3 能源卖家），按 slice 使用 EIP-3009 `TransferWithAuthorization` 进行微支付。买电时风扇转（挖矿），价格过高时停止。

### Demo 效果
- eCandle 以正弦波模拟电价变化（$0.48 ~ $2.88/kWh，60秒周期）
- Bitaxe 自动循环：扫描 → 判价 → 低于阈值则买电开风扇 → 高于阈值则等待
- OLED 实时显示：电价、支付金额、状态
- Web 页面可同时观察双方支付记录

## 2. 系统架构

```
Bitaxe (ESP32-S3, 本仓库)           eCandle (ESP32-C3, 对方仓库)
├── BLE Central (买家)        ←BLE→  BLE Peripheral (卖家)
├── boat-mwr SDK (签名/支付)         boat-mwr SDK (验签)
├── hashanchor (挖矿证明)            能源计量 + 继电器
├── AxeOS Web UI (Angular)           Demo Web UI
└── BM1370 ASIC (挖矿)              加热元件
```

### BLE GATT 服务 (0xEE00)

| UUID | 名称 | 方向 | 用途 |
|------|------|------|------|
| 0xEE01 | StreamRequest | 买家→卖家 | 发起购买会话 |
| 0xEE02 | StreamOffer | 卖家→买家 | 报价（notify）|
| 0xEE03 | SliceRequest | 卖家→买家 | 每 slice 付款请求（notify）|
| 0xEE04 | SlicePayment | 买家→卖家 | EIP-3009 签名支付 |
| 0xEE05 | StreamStatus | 卖家→买家 | 会话状态变化（notify）|
| 0xEE06 | DeviceInfo | 读取 | 卖家设备信息 + 实时电价 |

### 买家状态机

```
IDLE → SCANNING → [WAITING] → CONNECTING → DISCOVERING → READING_INFO
  → NEGOTIATING → STREAMING (风扇ON) → DECIDING → (回到 SCANNING)
```

## 3. 关键文件

| 文件 | 作用 |
|------|------|
| `main/ble_buyer.c` | BLE 买家状态机（核心：扫描→连接→支付→循环）|
| `main/ble_buyer.h` | 状态枚举、结果结构体、公共 API |
| `main/http_server/http_server.c` | REST API `/api/boat/buy`（POST=启动, GET=状态）|
| `main/display.c` / `main/screen.c` | OLED 显示 |
| `components/boat-mwr/` | BoAT 机器钱包 SDK（EIP-712、EIP-3009 签名）|
| `components/hashanchor/` | 挖矿证明 + 支付编排 |
| `sdkconfig.ble` | NimBLE 配置覆盖文件 |
| `CLAUDE.md` | Claude Code 项目指南（自动加载）|
| `.claude/skills/` | 4 个 Claude Code slash command |

## 4. 环境搭建

### 前置条件
- macOS / Linux
- **ESP-IDF v5.5.1**（`~/esp/esp-idf/`）
- Bitaxe 硬件（ESP32-S3），与电脑同一 WiFi
- eCandle 硬件（对方提供）

### 首次构建
```bash
# 1. Clone
git clone https://github.com/linyao0177/ESP-Miner.git
cd ESP-Miner

# 2. 安装 ESP-IDF (如未安装)
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
cd ~/ESP-Miner

# 3. 环境变量
source ~/esp/esp-idf/export.sh

# 4. 设置目标芯片 + 修复 BLE 配置
idf.py set-target esp32s3
cat sdkconfig.ble >> sdkconfig    # ⚠️ 关键！必须在 set-target 之后追加

# 5. 构建
GITHUB_ACTIONS=true idf.py build
# GITHUB_ACTIONS=true 跳过 Angular 构建（需要 Java 的 OpenAPI generator）
```

### OTA 刷入
```bash
curl -s -X POST http://192.168.31.123/api/system/OTA \
  --data-binary @build/esp-miner.bin \
  -H "Content-Type: application/octet-stream" \
  --max-time 300
# 输出 "Firmware update complete, rebooting now!" 即成功
# 等待 ~8 秒设备重启
```

## 5. Demo 运行

### 启动自动模式
```bash
curl -s -X POST http://192.168.31.123/api/boat/buy \
  -H "Content-Type: application/json" \
  -d '{"device_name":"ECandle","threshold":60,"max_slices":2,"slice_seconds":6,"auto_mode":true}'
```

| 参数 | 说明 |
|------|------|
| `device_name` | BLE 设备名，必须是 **"ECandle"**（大写 E，区分大小写）|
| `threshold` | 最高价格 μUSDC/slice（60 ≈ $1.43/kWh）|
| `max_slices` | 每个 session 最多 slice 数（2 = 约 12 秒）|
| `slice_seconds` | 每个 slice 时长（秒）|
| `auto_mode` | `true` = 自动循环（扫描→买→扫描→...）|

### 监控状态
```bash
# 单次查询
curl -s http://192.168.31.123/api/boat/buy | python3 -m json.tool

# 持续监控（每 6 秒，共 6 分钟）
for i in $(seq 1 60); do
  echo "[$i] $(date +%H:%M:%S) $(curl -s http://192.168.31.123/api/boat/buy)"
  sleep 6
done
```

### 正常 Demo 输出示例
```
[1]  10:05:12 connecting slc=0/2 pps=24 auto=True
[2]  10:05:19 streaming  slc=1/2 pps=24 auto=True <<< FAN ON >>>
[3]  10:05:25 streaming  slc=1/2 pps=24 auto=True <<< FAN ON >>>
[4]  10:05:31 streaming  slc=2/2 pps=75 auto=True <<< FAN ON >>>
[5]  10:05:38 waiting    slc=0/2 pps=117 sess=1 auto=True
[6]  10:05:47 waiting    slc=0/2 pps=113 sess=1 auto=True
...
[9]  10:06:06 streaming  slc=1/2 pps=46 sess=1 auto=True <<< FAN ON >>>
```

- **streaming + FAN ON** = 正在买电，风扇转
- **waiting** = 电价太高，等待回落
- **sessions 递增** = 自动循环正常

### 阈值参考

eCandle 正弦波：20~120 μUSDC/slice（$0.48~$2.88/kWh），60 秒一个周期。

| threshold | 对应 $/kWh | 效果 |
|-----------|-----------|------|
| 40 | $0.96 | 严格，买少 |
| **60** | **$1.43** | **平衡，推荐** |
| 80 | $1.92 | 宽松，买多 |

## 6. EIP-3009 支付参数

| 参数 | 值 |
|------|-----|
| Domain name | `GatewayWalletBatched` |
| Version | `1` |
| Chain ID | `5042002`（Arc Testnet）|
| verifyingContract | `0x0077777d7EBA4688BDeF3E311b846F25870A19B9`（Gateway）|
| Token | USDC on Arc Testnet |
| validBefore | `now + 345600`（4 天，忽略 eCandle 的 10s 窗口）|
| 签名算法 | secp256k1 via `boat_pay_authorize()` |

### 电价公式
```
$/kWh = price_per_slice / 1,000,000 / 0.0000417
```
`0.0000417` = eCandle 的 `PRICE_WH_FACTOR`（每 slice 的 Wh）。

## 7. Claude Code 集成

项目已配置 `CLAUDE.md` 和 4 个 skill，接手后 Claude Code 自动加载：

| Slash Command | 功能 |
|---------------|------|
| `/build-and-ota` | 编译固件 + OTA 刷入 Bitaxe |
| `/demo-test` | 启动 Demo + 持续监控 |
| `/debug-ble` | BLE 诊断（状态机、连接、支付）|
| `/release` | 创建 GitHub Release |

## 8. 已知问题 & 踩坑记录

### ⚠️ 必须知道的

| 问题 | 说明 |
|------|------|
| **sdkconfig 被覆盖** | `idf.py set-target` 会重置 sdkconfig，**必须**之后执行 `cat sdkconfig.ble >> sdkconfig` |
| **ECandle 大写 E** | BLE 扫描名是 `"ECandle"` 不是 `"eCandle"`，`strstr` 区分大小写 |
| **PATCH 不是 POST** | AxeOS 风扇控制必须用 `HTTP_METHOD_PATCH`，POST 无效 |
| **不能在 NimBLE 回调里 delay** | `vTaskDelay` 在 GAP callback 中会阻塞 host task，导致自动重扫失败 |
| **签名栈溢出** | EIP-712 签名需 ~4KB 栈，Timer 和 NimBLE host task 都要 8192 |
| **NimBLE 绑核** | `BT_NIMBLE_PINNED_TO_CORE=1`，core 0 和 WiFi 冲突 |
| **Angular 构建需要 Java** | 用 `GITHUB_ACTIONS=true` 跳过 |
| **HTTP body 256 字节** | API 支持 `auto_mode`/`device_name` 和 `auto`/`target` 两种字段名 |

### eCandle 侧待修复（非 Bitaxe 问题）

- [ ] settle worker `invalid_signature`：payload 格式问题（签名本身已用 ethers.js 验证正确）
- [ ] build 环境坏了（无法 OTA 自己的设备）
- [ ] SliceRequest 缺少 price 字段（目前从 DeviceInfo 读取）
- [ ] Web Payment Log 显示实时价格而非签名价格
- [ ] 动态定价周期太短（60s），拍视频建议改 5~10 分钟
- [ ] 恢复 device-side ecrecover（目前是 trust mode）

## 9. 网络配置

| 设备 | IP | 说明 |
|------|-----|------|
| Bitaxe | `192.168.31.123` | AxeOS Web UI + API |
| eCandle | `192.168.31.88` | Demo Web UI |

所有设备（Bitaxe、eCandle、你的电脑）必须在**同一 WiFi** 网络（192.168.31.x）。

## 10. 版本信息

| 项目 | 值 |
|------|-----|
| 当前 Release | [v2.13.1-hashanchor-6](https://github.com/linyao0177/ESP-Miner/releases/tag/v2.13.1-hashanchor-6) |
| 最新代码 | master `e256212`（含 auto mode 修复，尚未发 release）|
| boat-mwr SDK | git submodule @ `components/boat-mwr/` |
| ESP-IDF | v5.5.1 |
| 固件大小 | ~1.55MB（全量编译，非增量补丁）|

## 11. 开发历程（关键 commit）

```
e256212 fix: auto mode loop + handoff docs for demo maintenance
fae2698 feat: realtime dynamic pricing from DeviceInfo per-slice update
9536f82 fix: correct $/kWh formula to match eCandle exactly
89ce551 fix: BLE buyer demo improvements — fan control, auto mode, settle compatibility
5bc3220 revert: switch back to GatewayWalletBatched domain
7e72948 feat: OLED display for BLE buyer states
ab83103 fix: EIP-712 verifyingContract = Gateway, not USDC
f9c62bb fix: validBefore 3600→345600 (4 days) for Gateway settle
6cdc81c feat: auto-buy demo — dynamic pricing + autonomous purchase loop
23ae633 feat: BLE energy streaming buyer — EIP-3009 nanopayment to eCandle
```

## 12. 迭代方向建议

1. **发新 Release**：本地有多个修复未发布，建议 `/release` 发 v2.13.1-hashanchor-7
2. **拍视频**：让 eCandle 把定价周期从 60s 改到 5~10 分钟，效果更直观
3. **settle 修复**：需要 eCandle 配合修复 Gateway settle 的 payload 格式
4. **ecrecover 恢复**：eCandle 当前 trust mode，需恢复链上验签
5. **多买家竞价**：当前单 BLE 连接，可扩展为多设备场景
6. **电价源接入**：接入真实电价 API 替代正弦波模拟
