# 第三方依赖清单

> 本仓库**只跟踪自研代码**。下列第三方依赖不入库（见根目录 `.gitignore`），
> 换机器或重新 clone 后按本文档还原。
>
> 相关文档：系统架构见 [../02-esp32-s3/docs/ARCHITECTURE.md]

---

## 1. STM32 侧（01-test）

| 依赖 | 版本 | 仓库内路径 | 获取方式 |
|---|---|---|---|
| STM32CubeF4 HAL 驱动 | STM32CubeF4 系列 | `common/Drivers/STM32F4xx_HAL_Driver/` | github.com/STMicroelectronics/stm32f4xx-hal-driver |
| CMSIS Core | CMSIS 5.x | `common/Drivers/CMSIS/Include/` | github.com/ARM-software/CMSIS_5 |
| CMSIS Device F4 | STM32F4 系列 | `common/Drivers/CMSIS/Device/` | github.com/STMicroelectronics/cmsis_device_f4 |
| BSP 驱动（W25QXX Flash） | — | `common/Drivers/BSP/W25QXX/` | 自备驱动模板 |
| FreeRTOS Kernel | **V10.5.1** | `common/FreeRTOS/` | github.com/FreeRTOS/FreeRTOS-Kernel，tag `V10.5.1` |
| 启动文件 | — | `common/Startup/startup_stm32f411xe.s` | STM32CubeF4 |
| 链接脚本 | — | `common/Linker/STM32F411CEUx_FLASH.ld` | STM32CubeF4 |
| 系统时钟初始化 | — | `common/Core/system_stm32f4xx.c` | STM32CubeF4 |

**工具链**

| 工具 | 版本 | 安装 |
|---|---|---|
| arm-none-eabi-gcc | **13.2.1 (20231009)** | `sudo apt install gcc-arm-none-eabi` |
| GNU Make | 4.3 | `sudo apt install make` |

**工程内第三方副本（未被任何工程引用，可忽略）**

- `stm32f4xx-hal-driver/`：HAL 驱动的上游原始副本。实际编译使用的是 `common/Drivers/` 下的那份
  （见 `01-test/Makefile` 中的 `COMMON_DIR`）。已从版本控制移除，保留在磁盘仅供对照。
- `FreeRTOS-Kernel/`：同上的上游副本，实际使用 `common/FreeRTOS/`。

---

## 2. ESP32 侧（02-esp32-s3 / 03-esp32-cam）

| 依赖 | 版本 | 路径 | 获取方式 |
|---|---|---|---|
| ESP-IDF | **v5.5** | `tools/esp-idf/` | github.com/espressif/esp-idf，tag `v5.5` |
| esp-idf 工具链 | 随 IDF 分发（xtensa-esp-elf-gcc 14.2） | `~/.espressif/` | 由 IDF 的 `install.sh` 安装 |
| esp32-camera | `^2.0.0` | `managed_components/`（自动拉取） | 由 `03-esp32-cam/main/idf_component.yml` 声明 |
| esp-mqtt、lwIP、mbedTLS | 随 IDF 内置 | — | 无需单独获取 |

**激活环境**（每次开新终端都要执行）

```bash
source /home/wangkang/embedded-projects/tools/esp-idf/export.sh
```

---

## 3. 自研代码结构（入库部分）

| 路径 | 说明 |
|---|---|
| `01-test/` | STM32F411 工程：UART DMA 循环接收 + 环形缓冲 + 帧协议状态机 + OLED(硬件 I2C) |
| `02-esp32-s3/` | ESP32-S3 中枢：UART 链路 / WiFi / SNTP / MQTT 四模块 |
| `03-esp32-cam/` | ESP32-CAM 视觉节点（骨架） |
| `docs/` | 本目录：依赖清单 |
| `02-esp32-s3/docs/ARCHITECTURE.md` | 系统架构文档（拓扑、协议、任务模型、里程碑） |

---

## 4. 环境还原步骤

### 4.1 STM32 工程

```bash
# ① 安装工具链
sudo apt install gcc-arm-none-eabi make git

# ② 获取 ESP-IDF 之外的第三方库，按上表路径逐个 clone 到 common/ 对应位置
#    例如 FreeRTOS：
git clone -b V10.5.1 https://github.com/FreeRTOS/FreeRTOS-Kernel.git /tmp/frk
cp -r /tmp/frk/* common/FreeRTOS/          # 只需 include/、*.c、portable/

# ③ 编译（会生成 build/01-test.elf/.hex/.bin）
cd 01-test && make
```

### 4.2 ESP32 工程

```bash
# ① 获取 ESP-IDF v5.5（约 3GB，含子模块）
git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git tools/esp-idf
# 国内加速可用 Gitee 镜像：
# git clone -b v5.5 --recursive https://gitee.com/EspressifSystems/esp-idf.git tools/esp-idf

# ② 安装工具链并激活
cd tools/esp-idf && ./install.sh esp32s3 && . ./export.sh

# ③ 编译（managed_components 会自动拉取依赖）
cd /home/wangkang/embedded-projects/02-esp32-s3 && idf.py build
```

### 4.3 本地机密配置（不入库，必须手工补）

`sdkconfig` 被 `.gitignore` 排除（内含 WiFi 密码），首次编译后需：

```bash
idf.py menuconfig
```

| 配置项 | 位置 | 说明 |
|---|---|---|
| WiFi SSID / Password | `WiFi Configuration` | 必填，否则连不上 |
| LLM API Key | `Cloud LLM` | 第 5 步云端问答时填 |
| VLM 中转站 url/key | `Vision VLM` | 第 7 步图生文时填 |
| 其余项 | — | 已固化在 `sdkconfig.defaults`（IP/DNS/MQTT 地址，无密码，已入库） |

> 配置分层原则：**默认值入库（sdkconfig.defaults）+ 机密本地留（sdkconfig）**。
> 切勿把 `sdkconfig.defaults` 也加入忽略列表，否则新环境无法还原网络配置。

---

## 5. PC 侧工具（联调用，非编译必需）

| 工具 | 版本 | 用途 | 备注 |
|---|---|---|---|
| Mosquitto | 2.1.2 | MQTT broker | Windows 原生安装；需 `listener 1883 0.0.0.0` + `allow_anonymous true` + 防火墙放行 1883 |
| usbipd-win | — | 将 CH343 串口透传给 WSL | `usbipd attach --wsl --busid 2-3`（需管理员） |
| Windows 移动热点 | — | 给设备提供 2.4GHz WiFi | ESP32-S3 不支持 5GHz |

**MQTT 联调参数**

| 项 | 值 |
|---|---|
| Broker | `192.168.137.1:1883` |
| 设备静态 IP | `192.168.137.100` |
| 主题 | `stm32/temp`、`stm32/hum`（上报）；`esp32/status`（在线状态/LWT）；`esp32/cmd`（下行命令） |

---

## 6. 目录性质速查

| 路径 | 入库 | 说明 |
|---|---|---|
| `01-test/` `02-esp32-s3/` `03-esp32-cam/` | 是 | 自研工程代码 |
| `docs/` | 是 | 文档 |
| `common/` | 否 | STM32 第三方库集合（约 20MB） |
| `FreeRTOS-Kernel/` `stm32f4xx-hal-driver/` | 否 | 上游库副本 |
| `tools/` | 否 | ESP-IDF 及配套工具（约 3GB） |
| 各 `build/` `managed_components/` | 否 | 编译产物 |
| `02-esp32-s3/sdkconfig` | 否 | 含 WiFi 密码 |
| `02-esp32-s3/sdkconfig.defaults` | 是 | 脱敏模板 |
