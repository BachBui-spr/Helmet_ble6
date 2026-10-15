# Helmet BLE (ME54BS62 / nRF54L15): Tài liệu thiết kế hệ thống firmware test

| | |
|---|---|
| Phiên bản tài liệu | 0.1 |
| Ngày | 13/10/2026 |
| Giai đoạn | **Giai đoạn 1: bring-up phần cứng và môi trường phát triển** |
| Board target | `me54bs62/nrf54l15/cpuapp` |
| SDK | nRF Connect SDK v3.4.0 (Zephyr 4.4.0), toolchain `fbf7391cab` |
| Firmware hiện tại | `apps/hw_test` (firmware kiểm tra phần cứng) |

Quy ước: **[Đã xác nhận]** là đã đo hoặc chạy trên mạch thật. **[Đề xuất]** là đề xuất của nhóm firmware, chủ dự án cần chốt. **[TBD]** là chưa xác định.

---

## Mục lục

1. [Giới thiệu](#1-giới-thiệu)
2. [Yêu cầu](#2-yêu-cầu)
3. [Kiến trúc tổng thể](#3-kiến-trúc-tổng-thể)
4. [Thiết kế chi tiết từng luồng dữ liệu](#4-thiết-kế-chi-tiết-từng-luồng-dữ-liệu)
5. [Đặc tả chi tiết từng module và thành phần Zephyr](#5-đặc-tả-chi-tiết-từng-module-và-thành-phần-zephyr)
6. [Giám sát và ý nghĩa hệ thống](#6-giám-sát-và-ý-nghĩa-hệ-thống)
7. [Triển khai](#7-triển-khai)
8. [Kiểm thử và tiêu chí nghiệm thu](#8-kiểm-thử-và-tiêu-chí-nghiệm-thu)
9. [Phụ lục](#9-phụ-lục)

---

## 1. Giới thiệu

### 1.1 Mục đích

Tài liệu này mô tả toàn bộ hệ thống firmware của sản phẩm mũ bảo hiểm Bluetooth ("Helmet board"):
- yêu cầu;
- kiến trúc phần cứng và phần mềm;
- các luồng dữ liệu, các module và thành phần Zephyr được dùng;
- cách giám sát, cách triển khai, kế hoạch kiểm thử và tiêu chí nghiệm thu.

Đây là tài liệu gốc để các kỹ sư firmware tiếp tục phát triển các chức năng chính ở Giai đoạn 2.

### 1.2 Phạm vi

| Giai đoạn | Nội dung | Trạng thái |
|---|---|---|
| **GĐ1: Bring-up** | Môi trường build/nạp/debug; board definition riêng; firmware `hw_test` kiểm tra từng khối phần cứng; log/shell qua RTT; BLE cơ bản (quảng bá, kết nối, ghép đôi, lưu bond); sao lưu firmware gốc | Đang thực hiện, phần lớn đã xong |
| **GĐ2: Chức năng chính** | Âm thanh qua Bluetooth, xử lý nút, đo nhiệt độ, quản lý nguồn, cập nhật firmware… | **[TBD]**, chưa đặc tả |

### 1.3 Tổng quan sản phẩm (theo schematic)

| Khối | Linh kiện | Giao tiếp với MCU |
|---|---|---|
| MCU + radio | Minew **ME54BS62**: nRF54L15 WLCSP, Cortex-M33 128 MHz, RRAM 1.5 MB, RAM 256 KB, anten PCB, BLE 6.0 | |
| Loa | 2 × **MAX98357A** (ampli class-D, I2S), kênh trái/phải | I2S (P1.10, P1.12, P1.15) |
| Micro | 2 × **IM69D130** (micro số PDM), stereo trên 1 đường dữ liệu | PDM (P1.09, P1.11) |
| Đo nhiệt độ | **MLX90632** (cảm biến nhiệt hồng ngoại) | I2C (P0.04, P1.13) |
| Nút | Vol+, Vol−, Call, Power (board nút riêng) | GPIO P0.00–P0.03 |
| Đèn báo | LED Bluetooth D3 (qua transistor Q1) | GPIO P2.07 |
| Nguồn | Sạc TP4056, bảo vệ pin DW01, LDO AP2112K-3.3, USB-C | Không nối về MCU |
| Debug | Header SWD: VTREF, SWDIO, SWCLK, RESET | SWD |

### 1.4 Hiện trạng 

- **[Đã xác nhận]** Build, nạp qua J-Link, log và shell qua RTT, debug bằng GDB, BLE (quảng bá, kết nối, ghép đôi Just Works, giữ bond sau reset), reset bằng chân, sao lưu/khôi phục firmware gốc.
- Mạch hiện **mới hàn linh kiện mạch chính và nguồn**. Board nút, micro và loa **chưa nối**; MLX90632 **chưa gắn**.
- **Lỗi phần cứng đang mở:** LED trên P2.07 không hoạt động vì đường R16 không nối tới pad G2 của module (mục 8.5).

### 1.5 Thuật ngữ

| Thuật ngữ | Nghĩa |
|---|---|
| NCS | nRF Connect SDK: bộ phát triển của Nordic, xây trên Zephyr |
| Zephyr | Hệ điều hành thời gian thực (RTOS) mã nguồn mở |
| DTS / Devicetree | File mô tả phần cứng (ngoại vi, chân, clock, bộ nhớ) |
| Kconfig | Hệ thống bật/tắt tính năng phần mềm (`prj.conf`, `_defconfig`) |
| sysbuild | Cơ chế build nhiều image cùng lúc (app, bootloader…) |
| west | Công cụ dòng lệnh của Zephyr: quản lý repo, build, nạp, debug |
| SWD | Serial Wire Debug: chuẩn debug 2 dây của ARM |
| DP / AP | Debug Port / Access Port (CoreSight). AHB-AP truy cập bộ nhớ; CTRL-AP là cổng riêng của Nordic |
| RTT | Real Time Transfer: kênh log/shell của SEGGER qua RAM và SWD |
| RRAM | Bộ nhớ không mất dữ liệu của nRF54L (thay cho flash) |
| UICR | Vùng cấu hình người dùng của chip (`0x00FFD000`) |
| APPROTECT | Khóa cổng debug |
| SDC / MPSL | SoftDevice Controller (BLE controller của Nordic) / Multiprotocol Service Layer (lập lịch radio, clock) |
| SMP / Bond | Giao thức ghép đôi BLE / khóa ghép đôi được lưu lại |
| ZMS | Zephyr Memory Storage: hệ lưu trữ key-value trên RRAM |
| GPIOTE | Khối sự kiện/ngắt GPIO (P0 → GPIOTE30, P1 → GPIOTE20, **P2 không có**) |

### 1.6 Tài liệu tham chiếu

| Tài liệu | Vị trí |
|---|---|
| Hướng dẫn sử dụng project | [README.md](../README.md) |
| Sơ đồ chân và các điểm phần cứng cần xem lại | [PINOUT.md](PINOUT.md) |
| Datasheet ME54BS62 (Minew v1.0.0) | [ME54BS62-nRF54L15_Datasheet_K_EN.pdf](ME54BS62-nRF54L15_Datasheet_K_EN.pdf) |
| Schematic Helmet board (main, button, micro, charger) | `docs/PCB *.pdf` |
| nRF54L15 Datasheet v1.0, mục 10.1 Pin assignments | Nordic |
| nRF Connect SDK v3.4.0 docs | `~/ncs/v3.4.0/nrf/doc` |

---

## 2. Yêu cầu

### 2.1 Yêu cầu chức năng

| ID | Yêu cầu | GĐ | Trạng thái |
|---|---|---|---|
| FR-01 | Nạp firmware vào chip qua J-Link (SWD), có kiểm tra lại (verify), firmware tự chạy sau khi nạp | 1 | **Đạt** |
| FR-02 | Xem log và gõ lệnh shell **không cần UART** (qua RTT); UART20 là kênh dự phòng | 1 | **Đạt** |
| FR-03 | Debug từng bước: halt, breakpoint, xem biến, call stack | 1 | **Đạt** |
| FR-04 | Điều khiển LED Bluetooth (bật/tắt/nhấp nháy) | 1 | **Chặn**: lỗi phần cứng R16 – G2 |
| FR-05 | Nhận 4 nút Vol+/Vol−/Call/Power, có chống dội, phân biệt nhấn và nhả | 1 | Chưa kiểm (board nút chưa nối) |
| FR-06 | Phát âm thanh stereo ra 2 loa qua I2S | 1 (tone thử) | Một phần: I2S chạy không lỗi, chưa có loa |
| FR-07 | Thu âm stereo từ 2 micro PDM | 1 (đo mức) | Chưa kiểm (micro chưa nối) |
| FR-08 | Đọc cảm biến MLX90632 qua I2C | 1 (nhận diện) | Chưa kiểm (chưa gắn) |
| FR-09 | BLE: quảng bá, cho kết nối, ghép đôi Just Works, lưu bond vào RRAM | 1 | **Đạt** |
| FR-10 | Giám sát: heartbeat, nhiệt độ die, nguyên nhân reset, thông tin chip | 1 | **Đạt** |
| FR-11 | Sao lưu và khôi phục firmware/UICR gốc | 1 | **Đạt** |
| FR-20 | Truyền âm thanh (nghe nhạc / đàm thoại) qua Bluetooth | 2 | **[TBD]**, xem C-01 |
| FR-21 | Xử lý cuộc gọi bằng nút Call; điều chỉnh âm lượng bằng Vol+/Vol− | 2 | **[TBD]** |
| FR-22 | Đo và báo nhiệt độ hồng ngoại qua BLE (GATT) | 2 | **[TBD]** |
| FR-23 | Bật/tắt nguồn bằng nút Power, chế độ ngủ | 2 | **[TBD]** |
| FR-24 | Cập nhật firmware không dây (DFU qua BLE) | 2 | **[Đề xuất]** |

### 2.2 Yêu cầu phi chức năng

| ID | Nội dung | Chỉ tiêu | Trạng thái |
|---|---|---|---|
| NFR-01 | Nguồn cấp | Module chạy 1.7–3.6 V, RF ổn định khi ≥ 2.3 V; mạch cấp 3.3 V từ AP2112K | Đạt (VTref đo được 3.30 V) |
| NFR-02 | Tiết kiệm điện | Bật DC/DC; mục tiêu dòng ngủ **[TBD]** | DC/DC đã bật |
| NFR-03 | Ngân sách bộ nhớ GĐ1 | Dùng ≤ 50 % RRAM và RAM **[Đề xuất]** | 18.1 % RRAM (276 KB), 23.4 % RAM |
| NFR-04 | Thời gian nạp | ≤ 30 s **[Đề xuất]** | Khoảng 10–15 s |
| NFR-05 | Bảo toàn dữ liệu | Nạp thường không xóa UICR và vùng `storage` | Đạt (đã so sánh UICR trước và sau khi nạp) |
| NFR-06 | Bảo mật BLE | Liên kết được mã hóa (mức 2, Just Works) | Đạt; lưu ý Just Works không chống tấn công MITM |
| NFR-07 | Tái lập môi trường | Khóa phiên bản NCS v3.4.0 và toolchain `fbf7391cab` | Đạt |
| NFR-08 | Khả năng quan sát | Log có dấu thời gian; heartbeat 5 s; shell để chẩn đoán | Đạt |

### 2.3 Ràng buộc kỹ thuật

| ID | Ràng buộc | Hệ quả |
|---|---|---|
| **C-01** | **nRF54L15 chỉ hỗ trợ Bluetooth LE, không có Bluetooth Classic (BR/EDR)** | Không dùng được HFP/A2DP như tai nghe thông thường. Âm thanh và cuộc gọi qua Bluetooth phải dùng **LE Audio**, và cần điện thoại hỗ trợ. **Rủi ro lớn nhất của GĐ2**: cần kiểm tra mức hỗ trợ LE Audio của NCS 3.4 trên nRF54L15 trước khi thiết kế |
| C-02 | Ngoại vi chỉ dùng chân **cùng miền nguồn**: P0 (UARTE30/SPIM30/TWIM30), P1 (các khối 20–22, I2S20, PDM20, SAADC), P2 (SPIM00/UARTE00) | Mọi phân bổ chân phải theo bảng này |
| C-03 | Tín hiệu clock (SCL, PDM CLK, I2S SCK/MCK, SPIM SCK) phải ở **clock pin** | Đã đúng cho I2S (P1.12) và PDM (P1.11) |
| C-04 | I2C của MLX90632: SDA P1.13 và SCL P0.04 thuộc **hai miền khác nhau** | Không dùng được TWIM, phải chạy I2C bằng GPIO (bit-bang, 100 kHz). Nên sửa ở PCB bản sau |
| C-05 | Cổng **P2 không có GPIOTE** | Không đặt nút hay chân ngắt ở P2 |
| C-06 | `CONFIG_BT_MAX_PAIRED=1` | Chip chỉ nhớ **1** thiết bị đã ghép đôi. Muốn ghép đôi thiết bị khác phải chạy `hw unpair` trước |
| C-07 | Probe J-Link V9 (firmware 2021) không tự dò được RTT trên nRF54L15 | Phải chỉ định địa chỉ RTT lấy từ ELF (`make rtt`) |
| C-08 | Cắm sai dây RESET (vào chân GND) sẽ giữ chip ở trạng thái reset | Đối chiếu sơ đồ đầu cắm trong PINOUT.md |

### 2.4 Giả định và câu hỏi mở

| # | Câu hỏi | Ảnh hưởng |
|---|---|---|
| Q1 | Âm thanh/cuộc gọi GĐ2 dùng LE Audio, hay chỉ cần phát âm báo cục bộ? | Quyết định kiến trúc GĐ2 (C-01) |
| Q2 | Điện thoại mục tiêu (Android/iOS, phiên bản) có hỗ trợ LE Audio không? | Khả năng tương thích |
| Q3 | Thông số CL của thạch anh 32 MHz / 32.768 kHz trong module | Hiện dùng giá trị của nRF54L15 DK |
| Q4 | LED D3 màu gì? (LED xanh dương từ 3.3 V qua 1 kΩ chỉ có khoảng 0.2 mA) | Có thể phải đổi R12 |
| Q5 | Mục tiêu thời lượng pin và dòng ngủ | Thiết kế quản lý nguồn GĐ2 |
| Q6 | Có đưa điện áp pin hoặc trạng thái sạc về MCU ở PCB bản sau không? | Hiện MCU không biết mức pin |

---

## 3. Kiến trúc tổng thể

### 3.1 Kiến trúc phần cứng

```
                       USB-C 5V
                          │
             ┌────────────▼─────────────┐        Pin Li-ion
             │ Power board              │◄──────► (J6)
             │ TP4056 sạc · DW01 bảo vệ │
             │ AP2112K LDO 3.3 V        │
             └───┬───────────────┬──────┘
           Vsys  │               │ 3.3 V
     ┌───────────▼───┐   ┌───────▼──────────────────────────────────────────────┐
     │ 2 × MAX98357A │   │                  ME54BS62 (nRF54L15)                 │
     │ L: SD_MODE 2k │◄──┤ I2S20  BCLK P1.12 · LRCLK P1.10 · DIN P1.15          │
     │ R: SD_MODE 200k   │ PDM20  CLK  P1.11 · DATA  P1.09  ◄── 2 × IM69D130    │
     │ → 2 loa       │   │ GPIO-I2C SCL P0.04 · SDA P1.13   ◄──► MLX90632 (0x3A)│
     └───────────────┘   │ GPIO   P0.00–P0.03 ◄── 4 nút (board nút, connector)  │
                         │ GPIO   P2.07 ──► R16 ─► Q1 ─► LED D3 (⚠ hở)          │
                         │ SWD    SWDIO B3 · SWCLK B4 · nRESET C2 ◄── J-Link    │
                         │ UART20 TX P1.04 · RX P1.05 (không nối, dự phòng)     │
                         │ Bên trong: HFXO 32 MHz · LFXO 32.768 kHz · DC/DC ·   │
                         │            anten PCB                                 │
                         └──────────────────────────────────────────────────────┘
```

Chi tiết từng chân xem [PINOUT.md](PINOUT.md).

### 3.2 Kiến trúc phần mềm (firmware)

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ ỨNG DỤNG  apps/hw_test                                                       │
│  Chip info · LED/xung · Nút · MLX90632 · Loa · Micro · BLE · Heartbeat ·     │
│  lệnh shell "hw"                                                             │
├──────────────────────────────────────────────────────────────────────────────┤
│ DỊCH VỤ ZEPHYR / NCS                                                         │
│  Kernel (thread, workqueue, timer) · Logging · Shell · Input · Settings+ZMS  │
│  Bluetooth Host (GAP, GATT, SMP) · Sensor API · HWINFO · Audio DMIC API      │
├──────────────────────────────────────────────────────────────────────────────┤
│ DRIVER                                                                       │
│  GPIO nrfx + GPIOTE · gpio-keys · i2c-gpio · I2S nrfx · PDM nrfx ·           │
│  UARTE nrfx · temp_nrf5_mpsl · RRAM flash · clock_control · regulator ·      │
│  pinctrl                                                                     │
├──────────────────────────────────────────────────────────────────────────────┤
│ NỀN TẢNG NORDIC                                                              │
│  nrfx (HAL) + MDK (định nghĩa thanh ghi) · SoftDevice Controller (BLE        │
│  controller) · MPSL (lập lịch radio, clock, TEMP)                            │
├──────────────────────────────────────────────────────────────────────────────┤
│ PHẦN CỨNG  nRF54L15: Cortex-M33 · RADIO · I2S20 · PDM20 · GPIO P0/P1/P2 ·    │
│            GRTC · RRAMC · SAADC                                              │
└──────────────────────────────────────────────────────────────────────────────┘
        ▲ SEGGER RTT (vùng đệm trong RAM) ── đọc/ghi qua SWD bởi J-Link
```

### 3.3 Mô hình thực thi (thread)

Số liệu lấy từ bản build hiện tại. Độ ưu tiên kiểu *coop* là thread không bị chiếm quyền chạy; kiểu *preempt* là thread có thể bị chiếm quyền. Số nhỏ hơn nghĩa là ưu tiên cao hơn.

| Thread | Stack | Ưu tiên | Vai trò |
|---|---|---|---|
| `main` | 2048 B | preempt 0 | Khởi tạo, sau đó chạy vòng heartbeat 5 s |
| `blink_tid` | 512 B | preempt 7 | Đảo chân LED theo chu kỳ `blink_ms` |
| `sysworkq` (system workqueue) | 2048 B | coop −1 | Khởi động quảng bá BLE, việc trì hoãn của driver |
| BT RX | 2200 B | coop 8 | Nhận và xử lý sự kiện HCI từ controller |
| BT TX processor | 904 B | coop −1 | Gửi gói HCI xuống controller |
| BT long workqueue | 2104 B | 10 | Việc dài của BT host (lưu settings…) |
| MPSL work | 1024 B | coop 6 | Việc nền của MPSL |
| `input` | 1024 B | thấp | Phát sự kiện nút (gpio-keys) |
| `logging` | 2048 B | thấp nhất | Lấy log từ bộ đệm và đẩy ra RTT/UART |
| `shell_uart`, `shell_rtt` | 3072 B mỗi cái | thấp nhất | Shell trên UART20 và RTT; lệnh `hw beep/mic/mlx` chạy trong thread này |
| `idle` | 320 B | thấp nhất | CPU ngủ (WFI) khi không có việc |
| Ngắt (ISR) | 2048 B | | Stack ngắt dùng chung |

Nguyên tắc: không chặn thread `sysworkq` hoặc BT; việc dài (đo micro, phát tone) chạy trong thread shell.

### 3.4 Bản đồ bộ nhớ

| Vùng | Địa chỉ | Kích thước | Nội dung |
|---|---|---|---|
| RRAM: ứng dụng | `0x00000000` – `0x00045078` | ~276 KB | `hw_test` (chưa có MCUboot nên app link từ 0x0) |
| RRAM: phân vùng `mcuboot` / `image-0` / `image-1` | `0x0` / `0x10000` / `0xC2000` | 62 / 712 / 712 KB | Dành sẵn cho MCUboot và DFU ở GĐ2 |
| RRAM: phân vùng `storage` | `0x174000` | 36 KB | Settings/ZMS: **bond BLE, identity** |
| UICR | `0x00FFD000` | 4 KB | Cấu hình xuất xưởng (19 word đã ghi), đã sao lưu |
| RAM | `0x20000000` | 256 KB | Dùng ~61 KB; RTT control block `0x20001160`, bộ đệm log RTT `0x20000160` (4 KB) |
| Ngoại vi (secure) | `0x5xxxxxxx` | | Ví dụ P0 `0x5010A000`, P1 `0x500D8200`, P2 `0x50050400` |

### 3.5 Kiến trúc môi trường phát triển

```
 PC Ubuntu 24.04                                   J-Link V9 (S/N 69750999)      Helmet board
┌──────────────────────────────────────────────┐   USB   ┌──────────────┐  SWD   ┌───────────┐
│ make ─► west ─► CMake/Ninja/GCC (build)      │◄───────►│ MCU + FW     │◄──────►│ nRF54L15  │
│ make flash ─► west ─► nrfutil-device ─┐      │         │ SEGGER       │ 5 dây  │ DP/AP     │
│ make debug ─► JLinkGDBServer ◄── GDB ─┼─► DLL│         └──────────────┘        └───────────┘
│ make rtt   ─► JLinkExe + RTTClient ───┘      │
│ VS Code (IntelliSense dùng compile_commands) │
└──────────────────────────────────────────────┘
```

### 3.6 Cấu trúc kho mã

```
Bluetooth_ME54BS62/
├── env.sh                       nạp môi trường NCS v3.4.0
├── Makefile                     build / flash / rtt / debug / recover / restore-factory
├── boards/minew/me54bs62/       board definition (phần cứng Helmet + module)
├── apps/hw_test/                firmware kiểm tra phần cứng (CMakeLists.txt, prj.conf, src/)
├── scripts/rtt.sh               terminal RTT với địa chỉ lấy từ ELF
├── backup/                      firmware và UICR gốc (bin + hex)
├── docs/                        datasheet, schematic, PINOUT.md, tài liệu này
├── tools/                       udev rule chặn ModemManager
├── .vscode/                     settings, tasks, c_cpp_properties (IntelliSense)
└── build/                       đầu ra build (không đưa vào git)
```

---

## 4. Thiết kế chi tiết từng luồng dữ liệu

### DF-01: Build (cấu hình → file nhị phân)

```
prj.conf (app) ──┐                      board .dts + pinctrl.dtsi ──┐
_defconfig ──────┼─► Kconfig ─► .config ─► autoconf.h               ├─► dtc ─► zephyr.dts ─► devicetree_generated.h
Kconfig.* ───────┘                       SoC .dtsi (NCS) ────────────┘
                                   │                                                │
                                   └────────────────► CMake ◄───────────────────────┘
                                                        │  (BOARD_ROOT = project)
                                                        ├─► build.ninja, compile_commands.json
                                                        ▼
                         Ninja + arm-zephyr-eabi-gcc: kernel + driver + BT host + SDC/MPSL (binary) + app
                                                        ▼
                         zephyr.elf ─► zephyr.hex / zephyr.bin ;  runners.yaml (flash: nrfutil, debug: jlink)
```

- Đầu ra: `build/hw_test/hw_test/zephyr/`. Thư mục con `hw_test` có vì sysbuild tạo một thư mục cho mỗi image.
- `make build` tạo thêm link `build/compile_commands.json` cho IntelliSense của VS Code.

### DF-02: Nạp firmware

```
zephyr.hex ─► west flash ─► nrfutil device program
   1. Kết nối DP/AP, đọc FICR → nhận diện nRF54L15 REV2, kiểm tra APPROTECT
   2. Halt CPU
   3. Xóa các vùng sẽ ghi (ERASE_RANGES_TOUCHED_BY_FIRMWARE) – UICR và storage giữ nguyên
   4. Ghi RRAM qua RRAMC → 5. Verify → 6. Reset hệ thống (--softreset) → firmware chạy
```

| Biến thể | Xóa | Ghi chú |
|---|---|---|
| `make flash` | Chỉ vùng sẽ ghi | Dùng hằng ngày |
| `make erase-flash` | Toàn bộ RRAM và UICR | Mất bond BLE và UICR xuất xưởng |
| `make recover` | Xóa toàn chip qua CTRL-AP, mở khóa APPROTECT | Khi chip bị khóa |
| `make restore-factory` | Chỉ vùng 0x0–0xD000 | Nạp lại firmware gốc |

### DF-03: Khởi động (boot)

```
Reset ─► Vector reset @0x0 ─► __start
  └─► Mức PRE_KERNEL: clock (HFXO 32 MHz, LFXO 32.768 kHz), regulator (DC/DC), pinctrl, GRTC (timer hệ thống 1 MHz)
  └─► Khởi động kernel, các thread hệ thống
  └─► Mức POST_KERNEL: driver GPIO/GPIOTE, UARTE, I2S, PDM, i2c-gpio, gpio-keys, RTT, log, shell
  └─► main():
        1. In thông tin chip (FICR: part, variant, package, RAM, RRAM, Device ID, xung CPU)
        2. Đọc và xóa nguyên nhân reset (HWINFO)
        3. Cấu hình LED làm output
        4. Thử MLX90632 (I2C 0x3A)
        5. Bật BLE: bt_enable → settings_load (bond, identity) → đăng ký callback ghép đôi → quảng bá
        6. Vòng lặp heartbeat 5 s
```

Thời gian từ reset tới khi chạy `main` khoảng 11 ms (log "Uptime : 11 ms").

### DF-04: Log

```
LOG_INF/WRN/ERR (bất kỳ thread/ISR nào)
  ─► bộ đệm log 4 KB (deferred: không in ngay, ghi đè bản cũ khi đầy)
  ─► thread logging (đánh thức sau 10 tin hoặc mỗi 1 s)
  ─► backend shell UART20 ─► P1.04 (115200 8N1)
  └► backend shell RTT  ─► bộ đệm RTT 4 KB trong RAM ─► J-Link đọc qua SWD ─► JLinkExe :19021 ─► JLinkRTTClient
```

- Chế độ deferred giúp các đoạn code thời gian thực (ISR, BLE) không bị chậm vì in log.
- RTT ở chế độ NO_BLOCK_SKIP: khi không có ai đọc và bộ đệm đầy, log mới bị bỏ chứ firmware không bị chặn.

### DF-05: Lệnh shell

```
Bàn phím ─► JLinkRTTClient ─► J-Link ghi vào vòng đệm RTT chiều xuống (256 B)
                                        └─► thread shell_rtt (hỏi vòng đệm mỗi 10 ms)
UART RX P1.05 ─► thread shell_uart
   ─► tách lệnh, tra bảng lệnh ─► hàm xử lý (vd "hw mic 1000") ─► kết quả in ra đúng shell đã gửi lệnh
```

### DF-06: Nút nhấn

```
Nhấn nút ─► chân P0.0x xuống 0 (active-low; pull-up 10k ngoài + pull-up nội)
  ─► GPIOTE30 tạo ngắt cạnh ─► gpio-keys chống dội 30 ms
  ─► sự kiện INPUT_EV_KEY {code, value=1/0} ─► thread input ─► callback ứng dụng ─► log + bộ đếm
```

| Nút | Chân | Mã sự kiện |
|---|---|---|
| Vol+ | P0.02 | `INPUT_KEY_VOLUMEUP` (115) |
| Vol− | P0.01 | `INPUT_KEY_VOLUMEDOWN` (114) |
| Call | P0.00 | `INPUT_KEY_PLAYPAUSE` (164) |
| Power | P0.03 | `INPUT_KEY_POWER` (116) |

### DF-07: LED / xung GPIO

```
thread blink: mỗi blink_ms đảo P2.07 ─► R16 2.2k ─► chân B của Q1 (SS8050) ─► LED D3 + R12 1k từ 3.3 V
blink_ms = 0 → giữ mức led_level ("hw led on|off")
```

**[Đã xác nhận]** Thanh ghi của P2.07 đảo đúng nhịp. **⚠ Đường R16 – pad G2 bị hở**, nên LED không nhận được tín hiệu (mục 8.5).

### DF-08: Cảm biến MLX90632

```
hw mlx / lúc khởi động ─► i2c_write_read(0x3A, [địa chỉ thanh ghi 16 bit, big-endian]) ─► đọc 2 byte big-endian
  ─► driver i2c-gpio (bit-bang 100 kHz, open-drain, pull-up nội ~13 kΩ)
  ─► SCL P0.04 / SDA P1.13 ─► MLX90632
Thanh ghi dùng: EE_VERSION 0x240B (byte cao: 0x01 medical, 0x02 consumer, 0x05 extended), STATUS 0x3FFF
```

### DF-09: Loa (I2S)

```
Sinh sóng sin (tần số theo lệnh, biên độ ~ −12 dBFS, L = R)
 ─► khối 10 ms = 160 frame × 2 kênh × 16 bit = 640 B ─► vùng nhớ cấp phát cố định 4 khối
 ─► i2s_write ─► I2S20 (EasyDMA) – master clock: BCLK 512 kHz (= 16 kHz × 32), LRCLK 16 kHz
 ─► DIN P1.15 ─► MAX98357A trái (SD_MODE qua 2 kΩ → kênh L) và phải (SD_MODE qua 200 kΩ → kênh R) ─► loa
Nạp sẵn 2 khối rồi mới START (tránh thiếu dữ liệu); kết thúc bằng DRAIN; chờ trả lại khối nhớ trước lần phát sau
```

MAX98357A không cần MCK. Khi BCLK dừng, ampli tự tắt.

### DF-10: Micro (PDM)

```
PDM20: CLK P1.11 (driver chọn trong dải 1.0–3.25 MHz, duty 40–60 %) ─► 2 × IM69D130
  mic SELECT=GND và mic SELECT=VDD xuất dữ liệu ở hai cạnh clock khác nhau trên cùng DATA P1.09
 ─► lọc decimation trong PDM20 ─► PCM 16 kHz, 16 bit, stereo (L/R xen kẽ)
 ─► khối 10 ms (640 B), vùng nhớ 8 khối ─► dmic_read ─► bỏ 5 khối đầu (~50 ms ổn định)
 ─► thống kê từng kênh: DC, AC-RMS (dBFS), min/max, số mẫu L ≠ R
```

### DF-11: Bluetooth LE

```
bt_enable ─► SDC (controller) + MPSL (lập lịch radio) ─► settings_load (identity D3:EE:24:55:47:E9, bond)
 ─► quảng bá connectable, chu kỳ 100–150 ms, tên "ME54BS62-Test", 0 dBm
 ─► máy kia kết nối ─► callback connected
 ─► máy kia yêu cầu ghép đôi ─► SMP Just Works ─► khóa (LTK) ─► settings ─► ZMS ─► phân vùng storage (RRAM)
 ─► security level 2 (đã mã hóa)
 ─► ngắt kết nối ─► callback disconnected (kèm mã lý do) ─► recycled ─► quảng bá lại
Lần sau: máy đã ghép đôi kết nối lại ─► dùng LTK đã lưu ─► mã hóa ngay, không cần ghép đôi lại
```

### DF-12: Nhiệt độ die

```
sensor_sample_fetch(temp) ─► driver temp_nrf5_mpsl ─► MPSL đọc khối TEMP (dùng chung với hiệu chuẩn radio)
 ─► SENSOR_CHAN_DIE_TEMP (độ phân giải 0.25 °C) ─► heartbeat / "hw temp"
```

### DF-13: Debug

```
GDB (CLI hoặc VS Code) ─(GDB remote, TCP :2331)─► JLinkGDBServer ─► J-Link ─► AHB-AP ─► khối debug Cortex-M33
zephyr.elf cung cấp: tên ↔ địa chỉ, dòng lệnh ↔ địa chỉ, cấu trúc biến, cách dò ngược stack
```

---

## 5. Đặc tả chi tiết từng module và thành phần Zephyr

### 5.1 Board definition `boards/minew/me54bs62`

| File | Nội dung |
|---|---|
| `board.yml` | Tên board, vendor `minew`, SoC `nrf54l15`; quy tắc chạy runner một lần (recover/erase/reset) |
| `Kconfig.me54bs62` | Chọn `SOC_NRF54L15_CPUAPP` (hoặc CPUFLPR cho lõi RISC-V) |
| `Kconfig.defconfig` | Bật bảo vệ tràn stack bằng phần cứng |
| `Kconfig.sysbuild` | Tắt Partition Manager (dùng phân vùng trong devicetree) |
| `board.cmake` | Runner `nrfutil` (kèm `--softreset`) và `jlink` (`nRF54L15_M33`, 4 MHz) |
| `me54bs62_nrf54l15_cpuapp.dts` | Phần cứng (bảng dưới) |
| `me54bs62-pinctrl.dtsi` | Gán chân cho UART20, I2S20, PDM20 |
| `me54bs62_nrf54l15_cpuapp_defconfig` | SERIAL, CONSOLE, UART_CONSOLE, GPIO, ARM_MPU |
| `*_cpuflpr*` | Khung cho lõi phụ RISC-V (FLPR), chưa dùng |

**Các node phần cứng trong devicetree:**

| Node | Cấu hình | Ghi chú |
|---|---|---|
| `&lfxo` / `&hfxo` | Tụ tải nội 17 pF / 15 pF | Lấy theo nRF54L15 DK; cần chốt với Minew (Q3) |
| `&vregmain` | `NRF5X_REG_MODE_DCDC` | Module có cuộn cảm ML1 |
| `&uart20` | 115200, TX P1.04, RX P1.05 | console, shell, log |
| `&i2s20` | SCK_M P1.12, LRCK_M P1.10, SDOUT P1.15 | Clock nguồn PCLK32M_HFXO (mặc định) |
| `&pdm20` | CLK P1.11, DIN P1.09, `clock-source = "PCLK32M"` | |
| `i2c_sensor` (`gpio-i2c`) | SCL P0.04, SDA P1.13, open-drain + pull-up nội, 100 kHz | Ràng buộc C-04 |
| `leds/bt_led` | P2.07 active-high | |
| `buttons` (`gpio-keys`) | P0.00–P0.03, active-low + pull-up nội | |
| `&nfct` | `nfct-pins-as-gpios` | P1.02/P1.03 dùng làm GPIO |
| `&gpio0/1/2`, `&gpiote20/30`, `&radio`, `&temp`, `&adc`, `&grtc`, `&clock` | okay | |
| `&gpregret1/boot_mode0` | Lưu chế độ khởi động | Dành cho MCUboot |
| Phân vùng | `nrf54l15_cpuapp_partition.dtsi` của NCS | Mục 3.4 |

**Alias:** `led0`, `sw0`–`sw3`, `i2c-sensor`, `mcuboot-button0` (nút Power), `mcuboot-led0`, `watchdog0` (`wdt31`).

### 5.2 Ứng dụng `apps/hw_test`

| Module | Chức năng | API Zephyr chính | Tài nguyên | Giao diện | Xử lý lỗi |
|---|---|---|---|---|---|
| M1 Chip info / reset | In FICR, Device ID, xung CPU; đọc và xóa nguyên nhân reset | `hwinfo_*`, thanh ghi FICR | | log khởi động, `hw info` | Không có Device ID thì in "n/a" |
| M2 LED / xung | Đảo `led0` theo `blink_ms`; hoặc giữ mức cố định | `gpio_*_dt`, `K_THREAD_DEFINE` | thread 512 B | `hw blink <ms>`, `hw led on\|off` | Không có alias `led0` thì bỏ qua và in cảnh báo |
| M3 Nút | Log nhấn/nhả, đếm số lần nhấn | `INPUT_CALLBACK_DEFINE` | thread input | log `Button …` | Driver đã chống dội |
| M4 MLX90632 | Đọc EE_VERSION, STATUS | `i2c_write_read` | bus `i2c-sensor` | `hw mlx`, `i2c scan i2c-sensor` | Không có ACK → `NO RESPONSE (-5)` |
| M5 Loa | Phát tone sin | `i2s_configure/write/trigger`, `k_mem_slab` | 4 × 640 B | `hw beep [50..7000 Hz] [1..10000 ms]` | Lỗi cấu hình/ghi thì DROP, in mã lỗi |
| M6 Micro | Đo mức L/R | `dmic_configure/trigger/read` | 8 × 640 B | `hw mic [1..10000 ms]` | Lỗi đọc thì STOP, in mã lỗi |
| M7 BLE | Quảng bá, kết nối, ghép đôi, bond | `bt_enable`, `bt_le_adv_start`, `BT_CONN_CB_DEFINE`, `bt_conn_auth_info_cb`, `settings_load`, `bt_unpair` | 1 kết nối, 1 bond | `hw adv on\|off`, `hw unpair` | `-EALREADY` thì bỏ qua; log mã lỗi HCI/SMP |
| M8 Nhiệt độ die | Đọc nhiệt độ chip | `sensor_sample_fetch/channel_get` | | heartbeat, `hw temp` | Lỗi thì heartbeat không in nhiệt độ |
| M9 Heartbeat | Báo "còn sống" mỗi 5 s | `k_msleep`, `atomic_*` | thread main | log `heartbeat:` | |

**Bảng lệnh shell đầy đủ:**

| Lệnh | Tác dụng |
|---|---|
| `hw info` | Thông tin chip, số lần đảo LED, số lần nhấn nút |
| `hw blink <ms>` | Chu kỳ đảo LED (0 = dừng, giữ `led_level`) |
| `hw led on\|off` | Giữ LED ở mức cao/thấp (để đo bằng đồng hồ) |
| `hw temp` | Nhiệt độ die |
| `hw adv on\|off` | Bật/tắt quảng bá BLE |
| `hw unpair` | Xóa mọi bond BLE |
| `hw mlx` | Thử MLX90632 |
| `hw beep [hz] [ms]` | Phát tone ra loa |
| `hw mic [ms]` | Đo mức micro |
| `gpio conf/get/set gpioN <pin> …` | Thao tác chân bất kỳ (`gpio0`=P0, `gpio1`=P1, `gpio2`=P2) |
| `i2c scan i2c-sensor` | Quét bus I2C |
| `kernel threads`, `kernel stacks`, `device list` | Trạng thái hệ thống |

### 5.3 Thành phần Zephyr / NCS sử dụng

| Thành phần | Kconfig | Devicetree | Vai trò | Ghi chú |
|---|---|---|---|---|
| Kernel | (mặc định) | | Thread, scheduler, workqueue, mem slab, atomic | Timer hệ thống GRTC 1 MHz; tick 31250 Hz |
| Devicetree + Kconfig | | | Mô tả phần cứng và tính năng | Mục DF-01 |
| pinctrl | (tự bật) | `*_default/_sleep` | Gán chân khi chạy / khi ngủ | |
| Clock control (MPSL) | `CLOCK_CONTROL_NRF_K32SRC_XTAL` | `&lfxo`, `&hfxo` | Nguồn clock 32 kHz từ thạch anh | |
| Regulator | | `&vregmain` | DC/DC | |
| GPIO nrfx + GPIOTE | `GPIO` | `&gpio0-2`, `&gpiote20/30` | Chân số, ngắt | P2 không có GPIOTE |
| Input / gpio-keys | `INPUT`, `INPUT_GPIO_KEYS` | `buttons` | Sự kiện phím, chống dội 30 ms | Chế độ thread |
| I2C bit-bang | `I2C`, `I2C_GPIO`, `I2C_SHELL` | `i2c_sensor` | I2C bằng GPIO | Ràng buộc C-04 |
| I2S nrfx | `I2S`, `I2S_NRFX` | `&i2s20` | Ngõ ra âm thanh | |
| DMIC / PDM nrfx | `AUDIO`, `AUDIO_DMIC`, `AUDIO_DMIC_NRFX_PDM` | `&pdm20` | Ngõ vào micro | |
| Sensor / TEMP qua MPSL | `SENSOR`, `TEMP_NRF5_MPSL` | `&temp` | Nhiệt độ die | |
| HWINFO | `HWINFO` | | Device ID, nguyên nhân reset | |
| Logging | `LOG`, `LOG_MODE_DEFERRED`, `LOG_BUFFER_SIZE=4096` | | Log có mức và dấu thời gian | Mức mặc định INFO |
| Shell | `SHELL`, `SHELL_BACKEND_SERIAL`, `SHELL_BACKEND_RTT`, `KERNEL_SHELL`, `DEVICE_SHELL`, `GPIO_SHELL` | `zephyr,shell-uart` | Dòng lệnh chẩn đoán | Stack 3072 B |
| SEGGER RTT | `USE_SEGGER_RTT`, UP 4096, DOWN 256 | | Kênh log/shell qua SWD | |
| Settings + ZMS + flash map | `SETTINGS`, `SETTINGS_ZMS`, `ZMS`, `FLASH`, `FLASH_MAP` | `storage_partition` | Lưu bond và identity BLE | |
| Bluetooth Host | `BT`, `BT_PERIPHERAL`, `BT_SMP`, `BT_SETTINGS`, `BT_HCI_ERR_TO_STR` | | GAP, GATT, SMP | 1 kết nối, 1 bond |
| SoftDevice Controller | (NCS mặc định khi bật `BT`) | `&radio` | BLE controller, TX 0 dBm | Binary của Nordic |
| MPSL | (tự bật) | | Lập lịch radio, clock, TEMP | |
| HW stack protection | `HW_STACK_PROTECTION` | | Phát hiện tràn stack | |
| Fatal error | `RESET_ON_FATAL_ERROR=y`, `ASSERT=y` | | Gặp lỗi nghiêm trọng thì in log rồi reset | Khi debug crash có thể tắt để chip dừng lại |
| sysbuild | | | Build nhiều image | Hiện chỉ có 1 image |

### 5.4 Công cụ và script của project

| Thành phần | Đặc tả |
|---|---|
| `env.sh` | Đặt PATH, PYTHONHOME, `ZEPHYR_BASE`, `ZEPHYR_SDK_INSTALL_DIR`, `NRFUTIL_HOME`… theo `environment.json` của toolchain. Chạy được trên bash và zsh |
| `Makefile` | Biến `APP`, `BOARD`, `BUILD_DIR`, `JLINK_SN`, `EXTRA`. Các target: build, pristine, flash, erase-flash, recover, reset, rtt, uart, menuconfig, guiconfig, debug, devices, info, restore-factory, clean |
| `scripts/rtt.sh` | Lấy địa chỉ `_SEGGER_RTT` từ ELF; kiểm tra xem đã có phiên RTT khác chưa; mở JLinkExe (kết nối lại tối đa 5 lần) với `SetRTTAddr`; mở JLinkRTTClient (`-LocalEcho Off`). Có tùy chọn `-l` để ghi log ra file |
| `.vscode/c_cpp_properties.json` | IntelliSense dùng `build/compile_commands.json` |
| `.vscode/tasks.json` | Task Build / Flash / RTT / UART / Menuconfig |
| `tools/71-nrf-jlink-modemmanager-ignore.rules` | Chặn ModemManager chiếm cổng ttyACM của J-Link/Nordic |
| `backup/` | `rram_factory_0x0_1524K.bin`, `uicr_factory_0xFFD000.bin`, `factory_fw_0x0-0xD000.hex` |

---

## 6. Giám sát và ý nghĩa hệ thống

### 6.1 Các kênh quan sát

| Kênh | Công cụ | Cần gì | Ảnh hưởng tới hệ thống |
|---|---|---|---|
| Log + shell RTT | `make rtt` | Dây SWD | Gần như không |
| Log + shell UART | `make uart` | USB-UART 3.3 V nối P1.04/P1.05 | Không |
| Đọc/ghi bộ nhớ khi chip đang chạy | JLinkExe `mem32`/`w4`, đọc biến trong RAM | SWD | Không (trừ khi ghi thanh ghi) |
| Debug GDB | `make debug`, `west attach` | SWD | **Dừng CPU, làm rớt kết nối BLE** |
| Không dây | App nRF Connect, `bluetoothctl` | | Không |

### 6.2 Ý nghĩa log khởi động

| Dòng log | Ý nghĩa | Giá trị bình thường |
|---|---|---|
| `*** Booting nRF Connect SDK v3.4.0-… ***` | Firmware bắt đầu chạy | |
| `SoC part : 0x54b15 variant: AAC0 package: 0x4341` | Chip nRF54L15, gói CA (WLCSP) | Đúng như vậy |
| `Memory : RAM 256 KB, RRAM 1524 KB` | Dung lượng bộ nhớ | |
| `Device ID : 27bd8137ff92a485` | Mã duy nhất của chip (dùng để theo dõi từng thiết bị) | Mỗi chip khác nhau |
| `CPU clock : 128000000 Hz (sys timer 1000000 Hz)` | Xung CPU và timer hệ thống | 128 MHz / 1 MHz |
| `Reset cause: …` | Nguyên nhân reset (mục 6.4) | |
| `led0 -> gpio@50400 pin 7` | LED dùng P2.07 | |
| `MLX90632 @0x3a: OK/NO RESPONSE` | Cảm biến có trả lời không | Hiện là NO RESPONSE (chưa gắn) |
| `bt_hci_core: HW Variant: nRF54Lx`, `Identity: D3:EE:…` | Controller BLE đã chạy; địa chỉ BLE của thiết bị | |
| `No ID address. App must call settings_load()` | Thông báo bình thường khi bật `BT_SETTINGS`; identity được nạp ngay sau đó | |
| `BLE advertising as "ME54BS62-Test"` | Đang quảng bá | |

### 6.3 Heartbeat

Định dạng: `heartbeat: up <s> s, toggles <n>, buttons <n>, die <°C>`

| Trường | Ý nghĩa | Bình thường | Bất thường |
|---|---|---|---|
| Dòng heartbeat | Thread main còn sống | Mỗi 5 s | Mất heartbeat: treo, crash hoặc RTT đầy |
| `up` | Thời gian từ lần khởi động gần nhất | Tăng đều | Tụt về 0: chip vừa reset; xem `Reset cause` |
| `toggles` | Số lần đảo LED | +10 mỗi 5 s (blink 500 ms) | Đứng yên: thread blink bị chặn |
| `buttons` | Tổng số lần nhấn nút | Tăng khi nhấn | Tăng khi không ai nhấn: chân thả nổi hoặc nhiễu |
| `die` | Nhiệt độ chip | 30–35 °C ở nhiệt độ phòng | > 85 °C **[Đề xuất ngưỡng cảnh báo]**; giới hạn của chip là 105 °C |

### 6.4 Nguyên nhân reset

| Giá trị | Nghĩa | Thường gặp khi |
|---|---|---|
| `POWER-ON` | Vừa cấp nguồn | Cắm pin hoặc USB |
| `PIN` | Chân nRESET bị kéo xuống | Nhấn SW1, hoặc J-Link reset bằng chân |
| `SOFTWARE` | Reset bằng phần mềm (SYSRESETREQ) | `make reset`, hoặc lỗi nghiêm trọng (fatal error) |
| `SOFTWARE\|LOCKUP` | Có cờ CPU lockup kèm theo | Sau `make flash` (do quy trình nạp, không phải lỗi) |
| `WATCHDOG` | Watchdog hết hạn | Firmware treo (khi bật watchdog) |
| `BROWNOUT` | Sụt áp nguồn | Pin yếu hoặc nguồn kém |
| `DEBUG` | Reset từ debugger | |

Nếu thấy `SOFTWARE` mà không có ai gọi reset, hãy xem log trước đó: dòng `ZEPHYR FATAL ERROR` cho biết nguyên nhân crash.

### 6.5 Sự kiện BLE và mã lỗi

| Log | Ý nghĩa |
|---|---|
| `BLE connected: <addr> (public/random)` | Có thiết bị kết nối |
| `BLE pairing complete (bonded, keys saved to RRAM)` | Ghép đôi xong, đã lưu khóa |
| `BLE security level 2` | Kết nối đã mã hóa |
| `BLE pairing failed: …` | Ghép đôi thất bại, thường do khóa cũ bị lệch giữa hai bên hoặc đã hết chỗ lưu bond (C-06) |
| `BLE disconnected … reason 0x13` | Máy kia chủ động ngắt (bình thường) |
| `reason 0x08` | Mất kết nối do timeout: xa, nhiễu, hoặc CPU bị dừng khi debug |
| `reason 0x05` | Xác thực thất bại (ghép đôi lỗi) |
| `reason 0x06` | Thiếu khóa: một bên đã xóa bond → xóa bond ở cả hai bên |
| `reason 0x3E` | Không thiết lập được kết nối |

### 6.6 Chẩn đoán phần cứng bằng SWD (không cần đổi firmware)

- **Kiểm tra chân có nối tới đâu không:** qua J-Link, ghi PIN_CNF của chân sang input kèm pull-up rồi pull-down, đọc IN.
  - pull-up → 1 và pull-down → 0: chân không có tải (hở).
  - pull-up → 0: có tải kéo xuống GND, ví dụ chân B của transistor.
  - pull-down → 1: có điện trở kéo lên bên ngoài.
  - Đo xong phải trả lại PIN_CNF như cũ. Phương pháp này đã dùng để tìm ra lỗi G2/R16.
- **Trạng thái đầu cắm J-Link** (lệnh `st`): VTref, mức các chân TCK, TMS, TRES.
- **Thanh ghi DPIDR** (`SWDReadDP 0`): nếu lỗi thì chip không trả lời SWD ở mức điện.

---

## 7. Triển khai

### 7.1 Môi trường phát triển

| Thành phần | Phiên bản / vị trí |
|---|---|
| Hệ điều hành | Ubuntu 24.04 LTS x86_64 |
| nRF Connect SDK | v3.4.0 tại `~/ncs/v3.4.0` (Zephyr 4.4.0) |
| Toolchain | `~/ncs/toolchains/fbf7391cab` (Zephyr SDK 1.0.1, GCC 14.3, CMake 4.2, west 1.5) |
| nrfutil | 8.1.1 + device 2.19 (trong toolchain); 8.2.1 + device 2.22 (`~/.local/bin`) |
| SEGGER J-Link | V9.24a tại `/opt/SEGGER`; udev rule `99-jlink.rules` |
| VS Code | nRF Connect Extension Pack, C/C++ |
| Probe | J-Link V9 (S/N 69750999) |

### 7.2 Thiết lập từ đầu (máy mới)

1. Cài nRF Connect SDK v3.4.0 và toolchain bằng VS Code (nRF Connect → Manage SDKs/Toolchains) hoặc bằng `nrfutil sdk-manager`.
2. Cài gói SEGGER J-Link (`.deb`). Gói này tự cài udev rule.
3. Thêm user vào nhóm `dialout`. Cài rule chặn ModemManager trong `tools/`.
4. Clone project; nếu cài SDK ở chỗ khác thì sửa `NCS_ROOT` / `NCS_TOOLCHAIN` trong `env.sh`.
5. Chạy `make build`, nối J-Link, chạy `make info` (phải thấy `NRF54L15_xxAA_REV2`).
6. Chạy `make flash`, rồi `make rtt`.

### 7.3 Kết nối phần cứng

5 dây từ J-Link tới header "SW debugger": chân 1 VTref, 7 SWDIO, 9 SWCLK, **15 RESET**, 4 GND. Chi tiết và sơ đồ đầu cắm ở [PINOUT.md](PINOUT.md). Mạch phải có nguồn riêng.

### 7.4 Quy trình làm việc hằng ngày

```
sửa cấu hình/mã ─► make build ─► make flash ─► make rtt (xem log, gõ lệnh hw …) ─► (nếu cần) make debug
```

- Đổi app: `make APP=apps/<ten> build flash`.
- Nhiều J-Link: thêm `JLINK_SN=<số serial>`.
- VS Code: task `ME54: Build/Flash/RTT`; IntelliSense tự cập nhật sau mỗi `make build`.

### 7.5 Sao lưu, khôi phục, cứu chip

| Tình huống | Hành động |
|---|---|
| Muốn quay lại firmware gốc | `make restore-factory` (đã kiểm tra: khớp từng byte) |
| Chip bị khóa APPROTECT | `make recover` (xóa toàn chip) |
| Mất UICR sau khi erase-all | Ghi lại từ `backup/uicr_factory_0xFFD000.bin` (dùng JLinkExe `loadbin`) |
| Không kết nối được SWD | Làm theo checklist "Mất kết nối SWD" trong README |

### 7.6 Quản lý cấu hình

- Đưa vào git: `boards/`, `apps/`, `scripts/`, `docs/`, `tools/`, `.vscode/`, `env.sh`, `Makefile`, `backup/`.
- Không đưa: `build/`.
- Khi nâng phiên bản NCS: build lại từ đầu, chạy lại toàn bộ mục 8.2, rồi cập nhật bảng 7.1.

### 7.7 Lộ trình triển khai GĐ2 [Đề xuất]

| Hạng mục | Mô tả |
|---|---|
| Bootloader | Bật MCUboot qua sysbuild (`SB_CONFIG_BOOTLOADER_MCUBOOT`); app chuyển sang slot0 (`0x10000`) |
| DFU | Cập nhật firmware qua BLE (MCUmgr/SMP), ký ảnh firmware |
| Phiên bản | Đánh số phiên bản firmware (file `VERSION`), in ra lúc khởi động và đưa vào thông tin BLE |
| Sản xuất | Script nạp hàng loạt bằng nrfutil, ghi Device ID theo dõi, **bật APPROTECT** cho bản thương mại |
| Bond | Tăng `BT_MAX_PAIRED` hoặc bật `BT_KEYS_OVERWRITE_OLDEST` |

---

## 8. Kiểm thử và tiêu chí nghiệm thu

### 8.1 Chiến lược

| Mức | Mục tiêu | Cách làm |
|---|---|---|
| L0: Môi trường | Toolchain, quyền USB, probe | Lệnh CLI |
| L1: Kết nối debug | SWD, nạp, reset, RTT, GDB | `make info/flash/rtt/debug`, JLinkExe |
| L2: Từng khối phần cứng | LED, nút, I2C, I2S, PDM, BLE | Lệnh shell `hw …`, đo bằng đồng hồ/scope, điện thoại |
| L3: Hồi quy | Không làm hỏng chức năng đã đạt | Chạy lại toàn bộ 8.2 sau mỗi thay đổi lớn |

### 8.2 Ca kiểm thử

| ID | Mục tiêu | Các bước | Kết quả mong đợi | Kết quả 13/10/2026 |
|---|---|---|---|---|
| T-ENV-01 | Build | `make pristine` | Không lỗi, không cảnh báo của project | **Đạt** (chỉ còn cảnh báo `NRF_PLATFORM_LUMOS` của SDK và cảnh báo experimental của `BT_HCI_ERR_TO_STR`) |
| T-ENV-02 | IntelliSense | Mở `main.c` trong VS Code | 0 lỗi include | **Đạt** (từ 22 xuống 0) |
| T-DBG-01 | Nhận diện chip | `make info` | `NRF54L15_xxAA_REV2` | **Đạt** |
| T-DBG-02 | Nạp và tự chạy | `make flash`, sau đó JLinkExe `IsHalted` | Nạp OK, CPU không bị halt | **Đạt** |
| T-DBG-03 | Giữ UICR | So sánh UICR trước và sau `make flash` | Giống hệt | **Đạt** |
| T-DBG-04 | Reset bằng chân | JLinkExe `r0`/`r1`; `nrfutil … RESET_PIN` | Chip khởi động lại (`up` về 0) | **Đạt** |
| T-DBG-05 | RTT | `make rtt` | Thấy log khởi động và heartbeat | **Đạt** |
| T-DBG-06 | Shell qua RTT | `hw info`, `gpio get gpio2 9` | Có phản hồi, lệnh dài không mất ký tự | **Đạt** |
| T-DBG-07 | Debug GDB | Halt, breakpoint, reset, continue, đọc biến, detach | Dừng đúng chỗ, đọc đúng giá trị, chip chạy tiếp | **Đạt** |
| T-DBG-08 | Khôi phục firmware gốc | `make restore-factory`, so sánh từng byte | Khớp 100 % | **Đạt** |
| T-SYS-01 | Xung CPU và clock | Log khởi động | 128 MHz; LFXO chạy được | **Đạt** |
| T-SYS-02 | Heartbeat | Theo dõi 60 s | 12 dòng; `toggles` +10 mỗi 5 s | **Đạt** |
| T-HW-01 | LED | `hw blink 500`, `hw led on` | LED nhấp nháy / sáng | **Không đạt**: lỗi phần cứng R16–G2 |
| T-HW-02 | Nút | Nhấn và nhả từng nút | Log đúng tên nút, PRESSED/released | **Chưa chạy** (board nút chưa nối) |
| T-HW-03 | MLX90632 | `hw mlx` | `OK EE_VERSION=0x..05` | **Chưa chạy** (chưa gắn); bus ở trạng thái tốt |
| T-HW-04 | Loa | `hw beep 1000 500` | Nghe tiếng ở cả hai loa | Một phần: I2S không lỗi; chưa có loa |
| T-HW-05 | Micro | `hw mic 1000` khi phòng yên tĩnh, rồi vỗ tay | L ≠ R gần 100 %; AC −60..−75 dBFS, tăng khi vỗ tay | **Chưa chạy** (micro chưa nối); khi không có micro thì L = R như dự đoán |
| T-BLE-01 | Quảng bá | Quét bằng điện thoại hoặc laptop | Thấy `ME54BS62-Test` | **Đạt** |
| T-BLE-02 | Kết nối | Kết nối không ghép đôi | Giữ kết nối | **Đạt** |
| T-BLE-03 | Ghép đôi | Ghép đôi từ laptop (agent NoInputNoOutput) | `Pairing successful`, security level 2 | **Đạt** |
| T-BLE-04 | Giữ bond | Reset chip rồi kết nối lại | Mã hóa ngay, không cần ghép đôi lại | **Đạt** |
| T-BLE-05 | Phạm vi RF | Đo RSSI bằng điện thoại ở 1 / 5 / 10 m | **[TBD ngưỡng]** | Chưa chạy |

### 8.3 Tiêu chí nghiệm thu Giai đoạn 1

| # | Tiêu chí | Trạng thái |
|---|---|---|
| AC-1 | Mọi ca T-ENV và T-DBG đạt | ✅ |
| AC-2 | T-SYS đạt | ✅ |
| AC-3 | T-BLE-01 đến T-BLE-04 đạt | ✅ |
| AC-4 | T-HW-01 đạt sau khi sửa phần cứng | ❌ chờ sửa R16–G2 |
| AC-5 | T-HW-02 đến T-HW-05 đạt khi đã nối đủ module | ⏳ chờ phần cứng |
| AC-6 | Có đủ tài liệu: README, PINOUT, tài liệu thiết kế này | ✅ |
| AC-7 | Đã sao lưu firmware gốc và kiểm tra khôi phục được | ✅ |

**Kết luận:** GĐ1 **chưa nghiệm thu đầy đủ**. Phần môi trường, debug và BLE đã đạt. Còn chờ sửa lỗi phần cứng LED và lắp đủ nút, micro, loa, MLX90632.

### 8.4 Tiêu chí nghiệm thu GĐ2 [TBD]

Sẽ định nghĩa sau khi chốt câu hỏi Q1–Q6. Các chỉ tiêu dự kiến:
- độ trễ âm thanh;
- tầm BLE;
- dòng ngủ và thời lượng pin;
- độ chính xác nhiệt độ (± °C);
- thời gian phản hồi nút;
- tỷ lệ DFU thành công.

### 8.5 Vấn đề tồn đọng

| ID | Vấn đề | Mức độ | Hướng xử lý | Trạng thái |
|---|---|---|---|---|
| ISS-01 | LED: đường R16 không nối tới pad G2 (P2.07). Đã quét cả 30 GPIO, không chân nào thấy tải của R16 | Cao (với FR-04) | Kiểm tra un-routed net trong Altium; đo thông mạch; hàn lại module | Mở (tạm bỏ qua P2.07) |
| ISS-02 | I2C MLX90632 nằm ở hai miền nguồn (C-04) | Trung bình | Đang dùng bit-bang; PCB bản sau dời cả hai chân sang P1 (SCL ở P1.03/P1.04/P1.08) | Đã có cách tạm |
| ISS-03 | SDA/SCL không có điện trở kéo lên bên ngoài | Trung bình | Thêm 4.7 kΩ lên 3.3 V | Mở |
| ISS-04 | Q1 không có điện trở kéo xuống ở chân B, nên rất nhạy nhiễu | Thấp | Thêm 10–100 kΩ từ B xuống GND | Mở |
| ISS-05 | LED xanh dương từ 3.3 V qua 1 kΩ sẽ rất mờ (Q4) | Thấp | Giảm R12 hoặc cấp từ Vsys | Chờ xác nhận màu LED |
| ISS-06 | Chỉ nhớ 1 bond (C-06) | Thấp | `BT_MAX_PAIRED` > 1 hoặc `BT_KEYS_OVERWRITE_OLDEST` | Mở |
| ISS-07 | Thứ tự chân connector nút giữa mạch chính và board nút khác nhau | Thấp | Đối chiếu khi nối board nút | Mở |
| ISS-08 | Tụ tải thạch anh lấy theo DK (Q3) | Thấp | Hỏi Minew | Mở |
| ISS-09 | nRF54L15 không có Bluetooth Classic (C-01) | **Cao (GĐ2)** | Đánh giá LE Audio trên nRF54L15 / NCS 3.4 | Mở |

---

## 9. Phụ lục

### 9.1 Lịch sử thay đổi

| Phiên bản | Ngày | Nội dung |
|---|---|---|
| 0.1 | 13/10/2026 | Bản đầu tiên: GĐ1 bring-up, kết quả đo trên mạch thật |

### 9.2 Lệnh tham khảo nhanh

| Mục đích | Lệnh |
|---|---|
| Build / build lại từ đầu | `make build` / `make pristine` |
| Nạp / nạp có xóa hết | `make flash` / `make erase-flash` |
| Log và shell | `make rtt` (hoặc `make uart`) |
| Debug | `make debug` |
| Thông tin chip / probe | `make info` / `make devices` |
| Reset / mở khóa | `make reset` / `make recover` |
| Firmware gốc | `make restore-factory` |
| Chỉnh Kconfig | `make menuconfig` |
