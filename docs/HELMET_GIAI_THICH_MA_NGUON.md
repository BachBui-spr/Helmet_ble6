# Helmet: giải thích chi tiết mã nguồn

> **Tài liệu lưu trữ (27/10/2026):** tài liệu này mô tả `apps/Helmet`, đã gỡ khỏi cây mã. Lấy lại mã tương ứng từ lịch sử git: `git show a1ab029:apps/Helmet/...`. Cấu trúc mã nguồn hiện tại: [README.md](../README.md), mục *Kiến trúc mã nguồn*; firmware mũ hiện tại là `apps/main`.

| | |
|---|---|
| Áp dụng cho | `apps/Helmet` (firmware chính), board `me54bs62`, `scripts/helmet_audio.py` |
| Trạng thái mã | 15/10/2026. Bản build: RRAM 25.2 % (393 KB), RAM 56.8 % (149 KB) |
| Cách đọc | Mở tài liệu này song song với file mã. Mọi tham chiếu dạng `main.c:83` đều bấm được trong VS Code |

Tài liệu đi từ tổng thể đến chi tiết:
- Mục 1–2: bức tranh chung và kiến thức nền cần có để đọc mã.
- Mục 3–10: từng file, theo đúng thứ tự dòng trong file.
- Mục 11: ghép các file lại thành các luồng hoạt động thực tế (khởi động, kết nối, một khung âm thanh đi từ sóng tới loa…).
- Mục 12–14: đọc log, tự quan sát bằng debugger, và những điểm cần nhớ khi sửa mã.

Nếu chỉ có 30 phút: đọc mục 1, mục 11, rồi mở `audio_out.c` theo mục 7.

---

## Mục lục

1. [Bức tranh tổng thể](#1-bức-tranh-tổng-thể)
2. [Kiến thức nền](#2-kiến-thức-nền)
3. [Cấu hình build: CMakeLists.txt, prj.conf, board](#3-cấu-hình-build)
4. [helmet.h: giao kèo giữa các module](#4-helmeth-giao-kèo-giữa-các-module)
5. [main.c: khởi động, quảng bá, kết nối, nút, shell, heartbeat](#5-mainc)
6. [le_audio.c: BAP Unicast Server, nhận luồng âm thanh](#6-le_audioc)
7. [audio_out.c: giải mã LC3 và phát ra I2S](#7-audio_outc)
8. [media_ctrl.c: âm lượng (VCP) và play/pause (MCP)](#8-media_ctrlc)
9. [capture.c: gửi gói âm thanh về PC qua J-Link](#9-capturec)
10. [scripts/helmet_audio.py: chương trình trên PC](#10-scriptshelmet_audiopy)
11. [Các luồng hoạt động từ đầu đến cuối](#11-các-luồng-hoạt-động-từ-đầu-đến-cuối)
12. [Đọc log và lệnh shell](#12-đọc-log-và-lệnh-shell)
13. [Tự quan sát bằng debugger](#13-tự-quan-sát-bằng-debugger)
14. [Những điểm cần nhớ khi sửa mã](#14-những-điểm-cần-nhớ-khi-sửa-mã)

---

## 1. Bức tranh tổng thể

### 1.1 Thiết bị hiện làm được gì

| Chức năng | File chính | Đã thử trên mạch |
|---|---|---|
| Quảng bá là "tai nghe LE Audio", cho kết nối, ghép đôi có mã hóa, nhớ tối đa 4 thiết bị | `main.c` | Có |
| Nhận nhạc nén LC3 qua kênh đẳng thời CIS: 1–2 luồng, 16–48 kHz, mono hoặc stereo | `le_audio.c` | Có (nguồn Radxa) |
| Giải mã LC3, ghép kênh trái/phải, áp âm lượng, phát ra I2S tới 2 ampli | `audio_out.c` | Có, chưa nối loa |
| Điện thoại chỉnh âm lượng của mũ (VCP), nút trên mũ chỉnh âm lượng và báo lại cho điện thoại | `media_ctrl.c` | Chưa (cần điện thoại LE Audio) |
| Nút Call gửi play/pause tới trình phát nhạc của điện thoại (MCP) | `media_ctrl.c`, `main.c` | Chưa (chưa có nút) |
| Gửi nguyên gói LC3 về PC qua J-Link để nghe và lưu WAV | `capture.c`, `scripts/helmet_audio.py` | Có |
| Log, shell `helmet …`, heartbeat 10 s, chẩn đoán sóng (PHY, ISO link quality) | `main.c`, `le_audio.c` | Có |

### 1.2 Các lớp phần mềm

```
┌──────────────────────────── apps/Helmet (mã của dự án) ────────────────────────────┐
│ main.c         le_audio.c          audio_out.c         media_ctrl.c     capture.c  │
├──────────────────────────────── Zephyr / NCS ──────────────────────────────────────┤
│ Bluetooth Host: GAP · GATT · SMP · ISO · BAP (ASCS, PACS) · CAP · TMAP · VCP · MCC │
│ liblc3 · driver I2S · input (gpio-keys) · shell · logging · settings + ZMS · RTT   │
├──────────────────────────────── Nordic ────────────────────────────────────────────┤
│ SoftDevice Controller (lớp sóng BLE, tạo CIS) · MPSL (lập lịch radio) · nrfx (HAL) │
├──────────────────────────────── Phần cứng nRF54L15 ────────────────────────────────┤
│ RADIO · I2S20 (EasyDMA) · GPIO P0 (nút) · RRAM (bond) · RAM · FPU                   │
└────────────────────────────────────────────────────────────────────────────────────┘
```

Mã của dự án chỉ khoảng 1 600 dòng C, vì phần lớn công việc do Zephyr làm:
- Bluetooth host lo toàn bộ giao thức: GATT, mã hóa, máy trạng thái ASE. Ứng dụng chỉ nhận **callback** khi có sự kiện.
- liblc3 lo phần giải mã.
- Driver I2S lo DMA ra chân.

Phần việc của mã dự án là **nối các khối đó lại** và **ra quyết định**: chấp nhận cấu hình nào, đưa kênh nào ra loa nào, xử lý gói mất ra sao, bù lệch clock thế nào.

### 1.3 Module nào gọi module nào

```
                        ┌─────────────────────── main.c ───────────────────────┐
     khởi động ───────► │ audio_out_init → capture_init → bt_enable →           │
                        │ settings_load → bt_tmap_register → media_ctrl_init → │
                        │ le_audio_init → quảng bá → vòng heartbeat             │
                        │ + callback kết nối, nút, lệnh shell                   │
                        └───────┬──────────────────────┬───────────────┬────────┘
            media_ctrl_connected│            le_audio_print_*          │capture_set_enabled
             media_ctrl_volume_*│            le_audio_is_streaming     │capture_print_status
                                ▼                      ▼               ▼
                        ┌──────────────┐      ┌────────────────┐   ┌───────────┐
                        │ media_ctrl.c │      │   le_audio.c   │──►│ capture.c │
                        │  VCP + MCP   │      │  BAP server    │   │ RTT kênh 1│
                        └──────┬───────┘      └───────┬────────┘   └───────────┘
              audio_out_set_volume                    │ audio_out_stream_start/stop/data
                               ▼                      ▼
                        ┌──────────────────────────────────────┐
                        │ audio_out.c (thread riêng "audio")   │──► I2S20 ──► 2 × MAX98357A
                        └──────────────────────────────────────┘
```

Quy tắc ngầm của thiết kế:
- `le_audio.c` **không** tự giải mã. Nó chỉ nhận gói và chuyển cho `audio_out.c` (phát) và `capture.c` (gửi về PC).
- `audio_out.c` **không** biết gì về Bluetooth. Nó chỉ nhận "cấu hình luồng" và "gói dữ liệu + timestamp".
- Mọi hàm public đều được khai báo trong một chỗ duy nhất: `helmet.h`.

### 1.4 Các thread và hàm nào chạy trong thread nào

Đây là điều quan trọng nhất để hiểu vì sao mã được viết như vậy. Thread *coop* (cooperative) không bị thread khác chiếm quyền giữa chừng; thread *preempt* thì có thể.

| Thread | Ưu tiên | Hàm của Helmet chạy trong đó |
|---|---|---|
| `main` | preempt 0 | `main()`: khởi tạo, rồi vòng heartbeat 10 s (`log_link`, đọc ISO link quality) |
| `bt_rx` (Bluetooth host) | coop 8 | Callback kết nối (`connected`, `disconnected`, `security_changed`…); callback ASCS (`lc3_config`, `lc3_qos`, `lc3_enable`…); stream ops (`stream_enabled`, `stream_started`, **`stream_recv` mỗi 10 ms**); callback VCP/MCC khi điện thoại ghi/thông báo |
| `sysworkq` (system workqueue) | coop −1 | `adv_work_handler` (bật quảng bá), `mcs_discover_work_handler` (tìm dịch vụ media của điện thoại) |
| `audio_tid` (của `audio_out.c`) | preempt 2, có dùng FPU | `audio_thread`: giải mã LC3, ghép kênh, âm lượng, ghi I2S |
| `input` | thread của Zephyr | `on_input` (nút) |
| `shell_rtt`, `shell_uart` | thấp | Các lệnh `cmd_*` |
| `logging` | thấp nhất | Lấy log từ bộ đệm, đưa qua shell ra RTT kênh 0 và UART20 |

Hệ quả:
- **Không được làm việc nặng trong `bt_rx`.** Giải mã LC3 tốn CPU và dùng FPU. Nếu làm ngay trong `stream_recv`, thread Bluetooth bị chặn và các gói kế tiếp bị trễ. Vì vậy `stream_recv` chỉ **chép** gói vào hàng đợi rồi trả về ngay; `audio_thread` giải mã sau (mục 7).
- **Không được chờ lâu trong callback Bluetooth.** Việc như "bật lại quảng bá" hay "dò dịch vụ GATT của điện thoại" được đẩy sang workqueue bằng `k_work_submit`.

### 1.5 RAM: các khối lớn (đo từ `zephyr.elf`)

| Biến | Kích thước | File | Vai trò |
|---|---|---|---|
| `ds` | 36 320 B | `audio_out.c` | 4 bộ giải mã LC3 48 kHz: 2 luồng × 2 kênh, khoảng 9 KB mỗi bộ |
| `rtt_buf` | 16 384 B | `capture.c` | Bộ đệm RTT kênh 1 (âm thanh gửi về PC) |
| `i2s_slab` | 15 360 B | `audio_out.c` | 8 khối PCM × 1 920 B cho DMA của I2S |
| stack `audio_tid` | 4 096 B | `audio_out.c` | Stack thread giải mã |
| `_acUpBuffer` | 4 096 B | SEGGER RTT | RTT kênh 0 (log + shell) |
| stack `shell_rtt` + `shell_uart` | 2 × 3 072 B | Zephyr | Thread shell |
| `audio_q` | 2 656 B | `audio_out.c` | Hàng đợi 8 thông điệp × 332 B |
| `frame_pcm` | 1 920 B | `audio_out.c` | Khung PCM stereo đang ghép |

Riêng phần âm thanh của Helmet chiếm khoảng 80 KB. Phần còn lại là Bluetooth host, controller (SDC), shell và log.

---

## 2. Kiến thức nền

### 2.1 LE Audio trong một trang

**Hai loại liên kết vô tuyến chạy song song giữa điện thoại và mũ:**

| Liên kết | Chở gì | Nhịp |
|---|---|---|
| **ACL** (kết nối BLE thường) | Lệnh điều khiển qua GATT: đọc khả năng (PACS), cấu hình luồng (ASCS), âm lượng (VCS), play/pause (MCS) | Mỗi 50–70 ms (connection interval) |
| **CIS** (Connected Isochronous Stream) | Dữ liệu âm thanh đã nén LC3 | Đều đặn mỗi **ISO interval** = 10 ms; mỗi lần là một gói **SDU** |

Nhiều CIS thuộc cùng một nhóm **CIG**. Ví dụ: tai nghe 2 bên nhận 2 CIS, mỗi bên 1 kênh, trong cùng một CIG nên phát đồng bộ.

**Vai trò:**
- Điện thoại là *BAP Unicast Client*: nó quyết định codec, chất lượng, lúc bắt đầu và kết thúc.
- Helmet là *BAP Unicast Server*: nó công bố "tôi nhận được gì" và trả lời các yêu cầu.

**Các dịch vụ và profile Helmet triển khai:**

| Tên | Helmet là | Ý nghĩa |
|---|---|---|
| **PACS** (Published Audio Capabilities) | server | "Tôi nhận được LC3 16–48 kHz, 1–2 kênh, vị trí loa trái + phải, các ngữ cảnh âm thanh…" |
| **ASCS** (Audio Stream Control) | server | Mỗi **ASE** (Audio Stream Endpoint) là một "ổ cắm" nhận luồng. Helmet có 2 ASE sink |
| **CAP Acceptor** | acceptor | Profile chung cho thiết bị nhận LE Audio. Quảng bá dịch vụ CAS |
| **TMAP UMR** | Unicast Media Receiver | Báo cho điện thoại "tôi là thiết bị nghe nhạc" (dịch vụ TMAS) |
| **VCP Volume Renderer** | server (VCS) | Điện thoại đọc/ghi âm lượng của mũ; mũ thông báo khi âm lượng đổi |
| **MCP client** | client (MCC) | Mũ gửi lệnh PLAY/PAUSE tới dịch vụ media (GMCS) trên điện thoại |

**Máy trạng thái của một ASE** (điện thoại điều khiển bằng cách ghi vào *ASE Control Point*):

```
 Idle ──Config Codec──► Codec Configured ──Config QoS──► QoS Configured ──Enable──► Enabling
                                                                                      │
          (CIS được tạo + phía nhận báo "Receiver Start Ready")                       ▼
 Idle ◄──Release── Releasing ◄─Release─ QoS Configured ◄──Disable── Streaming ◄───────┘
```

**LC3** (Low Complexity Communication Codec):
- Nén từng **khung** 10 ms (hoặc 7.5 ms). Ở 48 kHz một khung có 480 mẫu.
- Mỗi khung nén thành một số byte cố định (*octets per frame*). Ví dụ 120 B mỗi 10 ms ≈ 96 kbps.
- Một SDU chứa 1 khung cho mỗi kênh, nối liền nhau: kênh trái trước, kênh phải sau.
- Khi mất gói, bộ giải mã có thể **nội suy** ra âm thanh hợp lý (PLC, Packet Loss Concealment) thay vì im lặng hay tiếng tách.

**Cờ của SDU nhận được** (controller gắn vào mỗi SDU):

| Cờ | Nghĩa |
|---|---|
| `BT_ISO_FLAGS_VALID` | Dữ liệu tốt |
| `BT_ISO_FLAGS_ERROR` | Nhận được nhưng có thể sai (CRC) |
| `BT_ISO_FLAGS_LOST` | Không nhận được gì trong khoảng thời gian của SDU này |
| `BT_ISO_FLAGS_TS` | Có timestamp hợp lệ |

Controller vẫn báo lên đều đặn 1 SDU mỗi 10 ms **kể cả khi mất** (gắn cờ LOST). Nhờ vậy ứng dụng luôn có nhịp đều để chạy PLC.

**Timestamp** (`info->ts`, đơn vị µs) là thời điểm tham chiếu của SDU. Các CIS cùng một CIG có **cùng timestamp** cho cùng một khoảng 10 ms. `audio_out.c` dùng điều này để ghép kênh trái của CIS 0 với kênh phải của CIS 1 (mục 7.8).

### 2.2 Các "viên gạch" Zephyr xuất hiện trong mã

| Cú pháp | Ý nghĩa | Gặp ở |
|---|---|---|
| `prj.conf` (Kconfig) | Bật/tắt **tính năng phần mềm** và kích thước bộ đệm | mục 3.2 |
| `.dts` (devicetree) | Mô tả **phần cứng**: chân, ngoại vi, nút, phân vùng bộ nhớ | mục 3.3 |
| `LOG_MODULE_REGISTER(ten, LOG_LEVEL_INF)` | Tạo một "nguồn log" tên `ten`; `LOG_INF/WRN/ERR` in kèm tên đó. Log được đệm và in sau (deferred) bởi thread `logging` | đầu mỗi file |
| `K_WORK_DEFINE`, `K_MSGQ_DEFINE`, `K_MEM_SLAB_DEFINE_STATIC`, `K_THREAD_DEFINE`, `K_MUTEX_DEFINE` | Tạo đối tượng kernel **lúc biên dịch**, không cần gọi hàm khởi tạo. Thread tạo bằng `K_THREAD_DEFINE` tự chạy khi boot | `main.c`, `audio_out.c`, `capture.c` |
| `BT_CONN_CB_DEFINE`, `INPUT_CALLBACK_DEFINE`, `SHELL_CMD_REGISTER` | Đăng ký callback/lệnh **lúc biên dịch** (linker gom vào một bảng) | `main.c` |
| `k_work_submit(&work)` | Nhờ thread `sysworkq` chạy hàm sau | `main.c`, `media_ctrl.c` |
| `k_msgq_put/get` | Hàng đợi **chép nguyên** thông điệp giữa các thread | `audio_out.c` |
| `k_mem_slab` | Kho khối nhớ cùng kích thước, cấp phát/giải phóng nhanh, không phân mảnh. Driver I2S bắt buộc dùng | `audio_out.c` |
| `atomic_t`, `atomic_get/set` | Biến nguyên đọc/ghi an toàn giữa các thread | `audio_out.c`, `capture.c` |
| `CONTAINER_OF(ptr, type, member)` | Từ con trỏ tới **một trường** suy ra con trỏ tới **struct chứa nó** | `le_audio.c` |
| `bt_conn_ref/unref` | Đếm tham chiếu đối tượng kết nối. Giữ con trỏ lâu dài thì phải `ref`, bỏ thì `unref` | `main.c`, `media_ctrl.c` |
| `struct net_buf` | Bộ đệm gói của Bluetooth: `buf->data`, `buf->len` | `le_audio.c` |
| `DEVICE_DT_GET(DT_NODELABEL(i2s20))` | Lấy con trỏ driver của node `i2s20` trong devicetree, ngay lúc biên dịch | `audio_out.c` |
| `ARG_UNUSED(x)` | Báo trình biên dịch tham số `x` cố ý không dùng | khắp nơi |
| `IS_ENABLED(CONFIG_X)` | Bằng 1 nếu Kconfig `X` bật. Dùng trong `if` thường, trình biên dịch tự bỏ nhánh chết | `main.c` |
| Mã lỗi âm (`-EINVAL`, `-ENOTCONN`…) | Quy ước Zephyr: 0 là thành công, số âm là lỗi errno | khắp nơi |

---

## 3. Cấu hình build

### 3.1 `CMakeLists.txt`

[apps/Helmet/CMakeLists.txt](../apps/Helmet/CMakeLists.txt)

| Dòng | Ý nghĩa |
|---|---|
| `cmake_minimum_required(VERSION 3.20.0)` | Phiên bản CMake tối thiểu Zephyr yêu cầu |
| `find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})` | Nạp toàn bộ hệ build Zephyr (`env.sh` đặt `ZEPHYR_BASE`). Bước này đọc board, devicetree, Kconfig và tạo target `app` |
| `project(helmet)` | Tên project CMake |
| `target_sources(app PRIVATE src/…)` | 5 file `.c` của ứng dụng. **Thêm file mới thì phải thêm vào đây** |

Không có dòng nào cho liblc3, Bluetooth hay I2S: các thư viện đó được kéo vào tự động khi bật Kconfig tương ứng trong `prj.conf`.

### 3.2 `prj.conf`: từng nhóm cấu hình

[apps/Helmet/prj.conf](../apps/Helmet/prj.conf)

**Log và shell**

| Cấu hình | Ý nghĩa |
|---|---|
| `CONFIG_LOG=y`, `CONFIG_LOG_MODE_DEFERRED=y` | Bật log. *Deferred*: `LOG_INF` chỉ ghi vào bộ đệm (rất nhanh), thread `logging` in ra sau. Nhờ vậy gọi log trong callback Bluetooth không làm chậm |
| `CONFIG_LOG_BUFFER_SIZE=4096` | Bộ đệm log 4 KB. Log quá dày thì bị bỏ (báo "messages dropped") |
| `CONFIG_USE_SEGGER_RTT=y` | Có thư viện SEGGER RTT (log/shell qua dây SWD) |
| `CONFIG_SEGGER_RTT_BUFFER_SIZE_UP=4096`, `…_DOWN=256` | RTT kênh 0: chip → PC 4 KB, PC → chip 256 B |
| `CONFIG_SHELL=y`, `CONFIG_SHELL_BACKEND_SERIAL=y`, `CONFIG_SHELL_BACKEND_RTT=y` | Shell chạy đồng thời trên UART20 và RTT. Log đi **qua shell** ra cả hai kênh |
| `CONFIG_SHELL_STACK_SIZE=3072` | Stack cho mỗi thread shell (lệnh `helmet status` in khá nhiều) |
| `CONFIG_KERNEL_SHELL=y`, `CONFIG_THREAD_NAME=y`, `CONFIG_THREAD_MONITOR=y` | Có lệnh `kernel threads` / `kernel stacks` để xem thread và mức dùng stack |

**Bluetooth LE cơ bản**

| Cấu hình | Ý nghĩa |
|---|---|
| `CONFIG_BT=y`, `CONFIG_BT_PERIPHERAL=y` | Bật Bluetooth, vai trò peripheral (được điện thoại kết nối tới) |
| `CONFIG_BT_DEVICE_NAME="Helmet"` | Tên hiện trên điện thoại |
| `CONFIG_BT_EXT_ADV=y` | Quảng bá mở rộng: gói quảng bá của Helmet dài 44 B, quá giới hạn 31 B của quảng bá thường (mục 5.3) |
| `CONFIG_BT_HCI_ERR_TO_STR=y` | Có hàm đổi mã lỗi HCI ra chữ (dùng trong log ngắt kết nối) |
| `CONFIG_BT_USER_PHY_UPDATE=y` | Có callback `le_phy_updated` và trường PHY trong `bt_conn_get_info` |
| `CONFIG_BT_GAP_AUTO_UPDATE_CONN_PARAMS=n` | Không tự xin đổi connection interval; để điện thoại quyết |
| `CONFIG_BT_PERIPHERAL_PREF_MIN_INT=50`, `…_MAX_INT=70`, `…_LATENCY=0`, `…_TIMEOUT=400` | Giá trị "ưa thích" công bố trong GATT: interval 62.5–87.5 ms (đơn vị 1.25 ms), timeout 4 s |

**Bảo mật và lưu ghép đôi**

| Cấu hình | Ý nghĩa |
|---|---|
| `CONFIG_BT_SMP=y` | Ghép đôi/mã hóa. LE Audio bắt buộc liên kết mã hóa |
| `CONFIG_BT_PRIVACY=y` | Helmet dùng địa chỉ ngẫu nhiên đổi định kỳ (RPA); chỉ thiết bị đã ghép đôi nhận ra |
| `CONFIG_BT_MAX_PAIRED=4`, `CONFIG_BT_KEYS_OVERWRITE_OLDEST=y` | Nhớ 4 thiết bị; thiết bị thứ 5 ghi đè thiết bị cũ nhất |
| `CONFIG_BT_SETTINGS=y`, `CONFIG_SETTINGS=y`, `CONFIG_FLASH=y`, `CONFIG_FLASH_MAP=y`, `CONFIG_ZMS=y` | Lưu khóa ghép đôi và địa chỉ định danh vào phân vùng `storage` của RRAM qua hệ lưu trữ ZMS |

**GATT**

| Cấu hình | Ý nghĩa |
|---|---|
| `CONFIG_BT_GATT_CLIENT=y` | Helmet cũng là GATT client (để làm MCP client đọc dịch vụ media trên điện thoại) |
| `CONFIG_BT_GATT_AUTO_DISCOVER_CCC=y` | Khi đăng ký nhận thông báo, tự tìm descriptor CCC |
| `CONFIG_BT_GATT_DYNAMIC_DB=y` | Cho phép đăng ký dịch vụ GATT lúc chạy (các dịch vụ LE Audio cần) |
| `CONFIG_BT_ATT_PREPARE_COUNT=1` | Hỗ trợ ghi dài (prepare write) |
| `CONFIG_BT_ATT_TX_COUNT=20` | Đủ bộ đệm để gửi nhiều thông báo cùng lúc (ASE state, PACS, VCS…) |
| `CONFIG_UTF8=y` | Hàm xử lý chuỗi UTF-8 (tên bài hát của MCS) |

**LE Audio**

| Cấu hình | Ý nghĩa |
|---|---|
| `CONFIG_BT_AUDIO=y` | Bật khung LE Audio |
| `CONFIG_BT_ISO_PERIPHERAL=y`, `CONFIG_BT_ISO_MAX_CHAN=2` | Nhận kênh CIS do central tạo; tối đa 2 kênh |
| `CONFIG_BT_CTLR_SDC_ISO_TX_HCI_BUFFER_COUNT=1` | Helmet chỉ **nhận** ISO nên controller chỉ cần 1 bộ đệm gửi |
| `CONFIG_BT_BAP_UNICAST_SERVER=y`, `CONFIG_BT_ASCS=y` | Vai trò BAP Unicast Server và dịch vụ ASCS |
| `CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT=2`, `…_SRC_COUNT=0` | 2 ASE nhận, 0 ASE gửi (chưa có micro) |
| `CONFIG_BT_PAC_SNK=y`, `CONFIG_BT_PAC_SNK_LOC=y`, `CONFIG_BT_PAC_SRC=n` | PACS có bản ghi "sink" và vị trí loa; không có "source" |
| `CONFIG_BT_CAP_ACCEPTOR=y`, `CONFIG_BT_TMAP=y` | Profile CAP và TMAP |
| `CONFIG_BT_VCP_VOL_REND=y` | Dịch vụ âm lượng VCS |
| `CONFIG_BT_MCC=y` | MCP client |

**Giải mã và âm thanh**

| Cấu hình | Ý nghĩa |
|---|---|
| `CONFIG_FPU=y`, `CONFIG_FPU_SHARING=y` | Dùng FPU phần cứng của Cortex-M33; *sharing*: kernel lưu/khôi phục thanh ghi FPU khi đổi thread (liblc3 tính bằng số thực) |
| `CONFIG_LIBLC3=y` | Thư viện LC3 của Google (`modules/lib/liblc3`) |
| `CONFIG_I2S=y` | Driver I2S |

**Khác**

| Cấu hình | Ý nghĩa |
|---|---|
| `CONFIG_INPUT=y` | Hệ thống input: driver `gpio-keys` biến nhấn nút thành sự kiện |
| `CONFIG_MAIN_STACK_SIZE=2048`, `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048` | Stack thread `main` và `sysworkq` |

Một số giá trị Zephyr tự chọn mà mã phụ thuộc vào (xem trong `build/Helmet/Helmet/zephyr/.config`):

| Cấu hình | Giá trị | Hệ quả |
|---|---|---|
| `CONFIG_BT_MAX_CONN` | 1 | Chỉ 1 kết nối. Khi đã kết nối thì không quảng bá nữa |
| `CONFIG_BT_MCC_READ_MEDIA_STATE` | y | Có callback trạng thái phát nhạc (dùng cho nút Call) |
| `CONFIG_I2S_NRFX_TX_BLOCK_COUNT` | 4 | Hàng đợi gửi của driver I2S chứa 4 khối |
| `CONFIG_SEGGER_RTT_MAX_NUM_UP_BUFFERS` | 3 | Đủ cho kênh 1 của `capture.c` |
| `CONFIG_FLASH_LOAD_OFFSET` | 0 | Chưa có MCUboot nên ứng dụng nằm từ địa chỉ 0x0 |

### 3.3 Board `me54bs62`: phần Helmet dùng tới

[boards/minew/me54bs62/me54bs62_nrf54l15_cpuapp.dts](../boards/minew/me54bs62/me54bs62_nrf54l15_cpuapp.dts)

| Node | Dòng | Helmet dùng thế nào |
|---|---|---|
| `chosen` | 20–32 | Console, shell UART là `uart20`; vùng nhớ RRAM/RAM |
| `buttons` (`gpio-keys`) | 53–79 | 4 nút ở P0.00–P0.03, **active low** (nhấn = nối GND), bật pull-up nội. Mỗi nút có `zephyr,code` (ví dụ `INPUT_KEY_VOLUMEUP`). Driver tự chống dội 30 ms (giá trị mặc định của binding) rồi phát sự kiện `INPUT_EV_KEY` |
| `leds` | 37–45 | LED P2.07. **Helmet không dùng** (mạch hở) |
| `i2c-sensor` | 88–96 | I2C bit-bang cho MLX90632. **Helmet không dùng** |
| `&i2s20` | 156–161 | Bật I2S20, chân lấy từ `pinctrl` |
| `&pdm20` | 164–170 | Micro. **Helmet chưa dùng** |
| `&hfxo`, `&lfxo` | 120–128 | Thạch anh 32 MHz và 32.768 kHz, tụ tải nội. HFXO là nguồn clock của radio và I2S |
| `&vregmain` | 135–138 | Bật DC/DC (giảm dòng) |
| `nrf54l15_cpuapp_partition.dtsi` | 231 | Bảng phân vùng: `storage` (36 KB ở `0x174000`) là nơi lưu bond |

[me54bs62-pinctrl.dtsi](../boards/minew/me54bs62/me54bs62-pinctrl.dtsi) dòng 41–47: I2S20 dùng `SCK_M` = P1.12 (BCLK), `LRCK_M` = P1.10 (LRCLK), `SDOUT` = P1.15 (dữ liệu tới ampli). Không có MCK vì MAX98357A không cần.

`board.cmake`: `--softreset` để sau khi nạp chip được reset qua SWD và chạy ngay.

---

## 4. `helmet.h`: giao kèo giữa các module

[apps/Helmet/src/helmet.h](../apps/Helmet/src/helmet.h)

| Dòng | Nội dung | Giải thích |
|---|---|---|
| 1–9 | Chú thích | Bản đồ 5 file |
| 11–12, 110 | `#ifndef HELMET_H_` … | Chống include hai lần |
| 14–19 | include | Kiểu `bool`, `uint*_t`; kiểu Bluetooth audio, `bt_conn`, `shell` |
| 22–26 | `HELMET_SINK_CONTEXTS` | Các **ngữ cảnh âm thanh** Helmet chấp nhận: Unspecified, Media, Game, Instructional, Sound effects, Notifications, Alerts. Giá trị bit = `0x059D`. **Không có** Conversational (cuộc gọi), vì chưa có micro |
| 29 | `HELMET_SINK_LOCATION` | Vị trí loa: Front Left \| Front Right (`0x3`). Điện thoại dựa vào đây để gửi stereo |
| 31 | `HELMET_MAX_STREAMS` | = `CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT` = 2. Kích thước các mảng theo luồng trong mọi module |
| 34 | `HELMET_MAX_SDU_LEN 320` | SDU lớn nhất: 2 kênh × 155 B (mức LC3 tối đa Helmet công bố), làm tròn lên 320 |
| 37–43 | `struct helmet_stream_cfg` | Cấu hình một luồng sau khi điện thoại chọn: tần số (Hz), độ dài khung (µs), byte/khung/kênh, số kênh, vị trí kênh (bitmask, 0 = mono). `le_audio.c` tạo, `audio_out.c` và `capture.c` dùng |
| 46–61 | `struct helmet_audio_stats` | Bộ đếm của đường phát âm thanh. Từng trường giải thích ở mục 12.1 |
| 64–70 | API `audio_out.c` | Khởi tạo, báo bắt đầu/dừng luồng, đưa dữ liệu, đặt âm lượng, lấy thống kê |
| 73–78 | `struct helmet_rx_flags` | Đếm SDU theo cờ ISO: tốt / sai CRC / mất / rỗng |
| 81–87 | API `le_audio.c` | Khởi tạo, hỏi "có đang phát không", in trạng thái, đổi QoS ưa thích, lấy bộ đếm cờ, in chất lượng liên kết ISO |
| 90–99 | API `media_ctrl.c` | Khởi tạo, báo kết nối/ngắt, play/pause, tăng/giảm/đặt âm lượng, in trạng thái |
| 102–108 | API `capture.c` | Khởi tạo kênh RTT, bật/tắt, báo bắt đầu/dừng luồng, đưa SDU, in trạng thái |

---

## 5. `main.c`

[apps/Helmet/src/main.c](../apps/Helmet/src/main.c): khởi động, quảng bá, kết nối và bảo mật, nút, shell, heartbeat.

### 5.1 Đầu file ([dòng 1–48](../apps/Helmet/src/main.c#L1))

- Dòng 1–16: mô tả vai trò.
- Dòng 18–37: include. `zephyr/bluetooth/audio/*` cho LE Audio; `input.h` cho nút; `settings.h` để nạp bond; `shell.h` cho lệnh.
- Dòng 39: `LOG_MODULE_REGISTER(helmet, LOG_LEVEL_INF)`. Mọi log của file này có tiền tố `helmet:`.
- Dòng 41: heartbeat mỗi 10 s.
- Dòng 43–45: ba biến trạng thái của file.
  - `adv`: con trỏ tới **bộ quảng bá** (advertising set) do stack cấp.
  - `active_conn`: kết nối hiện tại, `NULL` khi chưa kết nối. Giữ bằng `bt_conn_ref` để đối tượng không bị giải phóng khi còn dùng.
  - `sec_level`: mức bảo mật hiện tại, chỉ để in trạng thái.
- Dòng 47–48: khai báo trước hàm `adv_work_handler` và tạo một **work item** `adv_work` trỏ tới hàm đó. Gọi `k_work_submit(&adv_work)` nghĩa là "nhờ `sysworkq` chạy `adv_work_handler` khi rảnh".

### 5.2 Vì sao quảng bá qua work item

`recycled()` (dòng 159) được gọi từ thread Bluetooth ngay khi đối tượng kết nối cũ được giải phóng. Bật quảng bá từ đó có thể đụng khóa nội bộ của stack, nên mã đẩy việc này sang `sysworkq`. Đây cũng là cách mẫu chính thức của Zephyr khuyên.

### 5.3 Dữ liệu quảng bá ([dòng 50–81](../apps/Helmet/src/main.c#L50))

Điện thoại lọc thiết bị LE Audio dựa trên gói quảng bá, nên từng byte đều có nghĩa. Mỗi mục AD có dạng `[độ dài][kiểu][dữ liệu]`:

| Mục | Mã nguồn | Byte phát ra sóng | Ý nghĩa |
|---|---|---|---|
| Flags | dòng 72 | `02 01 06` | General discoverable (`0x02`) + không hỗ trợ BR/EDR (`0x04`) |
| Appearance | dòng 73–74 | `03 19 42 09` | `0x0942` = Wearable Audio Device: Headset. Điện thoại hiện biểu tượng tai nghe |
| UUID 16 bit | dòng 75–76 | `07 02 4E 18 53 18 55 18` | Có dịch vụ ASCS (`0x184E`), CAS (`0x1853`), TMAS (`0x1855`) |
| Service data TMAS | dòng 66–69, 77 | `05 16 55 18 08 00` | Vai trò TMAP = UMR (bit 3 = `0x0008`) |
| Service data CAS | dòng 61–64, 78 | `04 16 53 18 00` | CAP: thông báo *General* (`0x00`): "kết nối với tôi khi muốn" |
| Service data ASCS | dòng 53–59, 79 | `09 16 4E 18 00 9D 05 00 00 00` | General; ngữ cảnh **nhận** = `0x059D`; ngữ cảnh **gửi** = `0x0000` (chưa có micro); metadata dài 0 |
| Tên | dòng 80 | `07 09 48 65 6C 6D 65 74` | "Helmet". `sizeof(...) - 1` để bỏ ký tự `\0` cuối chuỗi |

Tổng cộng 44 B, vượt 31 B của quảng bá thường, nên phải dùng quảng bá mở rộng (`CONFIG_BT_EXT_ADV`).

**`adv_work_handler`** ([dòng 83–99](../apps/Helmet/src/main.c#L83)):
- Dòng 89: đang có kết nối hoặc chưa tạo bộ quảng bá thì thôi (chỉ hỗ trợ 1 kết nối).
- Dòng 93: `bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_DEFAULT)` bắt đầu quảng bá, không giới hạn thời gian.
- Dòng 94: `-EALREADY` (đang quảng bá rồi) không phải lỗi.

**`advertising_create`** ([dòng 101–118](../apps/Helmet/src/main.c#L101)):
- Dòng 105: tạo bộ quảng bá với `BT_BAP_ADV_PARAM_CONN_QUICK` = quảng bá **có thể kết nối**, chu kỳ **20–30 ms**. Điện thoại tìm thấy rất nhanh, đổi lại tốn điện.
- Dòng 111: nạp mảng `ad[]` vào bộ quảng bá. Không có scan response (`NULL, 0`).

### 5.4 Kết nối và bảo mật ([dòng 120–199](../apps/Helmet/src/main.c#L120))

**`connected(conn, err)`** (dòng 122): stack gọi khi điện thoại kết nối xong.
- Dòng 126: đổi địa chỉ điện thoại ra chuỗi để in.
- Dòng 127–130: `err != 0` là kết nối thất bại; chỉ ghi log.
- Dòng 132–134: lưu kết nối vào `active_conn` và tăng tham chiếu. Điều kiện `== NULL` phòng trường hợp gọi lặp.
- Dòng 135: chưa mã hóa thì mức bảo mật là L1.
- Dòng 139: `bt_conn_set_security(conn, BT_SECURITY_L2)`: Helmet chủ động yêu cầu mã hóa. Điện thoại đã ghép đôi thì dùng khóa cũ; chưa ghép đôi thì bắt đầu ghép đôi *Just Works* (không cần nhập mã). LE Audio bắt buộc liên kết mã hóa: điện thoại sẽ không đọc ASCS/PACS nếu chưa mã hóa.

**`disconnected(conn, reason)`** (dòng 145): in lý do (ví dụ `0x13` = điện thoại chủ động ngắt, `0x08` = mất sóng/timeout), bỏ tham chiếu `active_conn` và báo `media_ctrl` dọn trạng thái MCP. Các luồng âm thanh được stack dừng tự động: `le_audio.c` nhận `stream_stopped` / `stream_released`.

**`recycled()`** (dòng 159): stack gọi khi đối tượng kết nối **thực sự được giải phóng**, sau `disconnected`. Chỉ lúc này mới bật lại quảng bá chắc chắn thành công: với `BT_MAX_CONN=1`, nếu bật sớm hơn thì stack chưa có đối tượng kết nối trống.

**`security_changed`** (dòng 164): mã hóa xong (hoặc thất bại).
- Thành công thì lưu mức bảo mật và gọi `media_ctrl_connected(conn)` để bắt đầu dò dịch vụ media trên điện thoại. Dịch vụ đó yêu cầu liên kết đã mã hóa, nên phải đợi tới đây.

**`le_phy_updated`, `le_param_updated`** (dòng 177–190): chỉ ghi log khi PHY (1M/2M/Coded) hoặc connection interval của ACL thay đổi. `interval * 1250` đổi đơn vị 1.25 ms sang µs; `timeout * 10` đổi đơn vị 10 ms sang ms.

**`BT_CONN_CB_DEFINE(conn_callbacks)`** (dòng 192–199): đăng ký 6 callback trên lúc biên dịch. Không cần gọi hàm đăng ký trong `main()`.

### 5.5 `log_link`: dòng chẩn đoán "link:" ([dòng 203–231](../apps/Helmet/src/main.c#L203))

Heartbeat gọi hàm này mỗi 10 s.
- Dòng 205: lấy thêm một tham chiếu tới kết nối, vì `main` là thread khác với thread Bluetooth: kết nối có thể bị ngắt đúng lúc đang đọc.
- Dòng 216–222: `bt_conn_get_info` cho connection interval (µs) và PHY hai chiều.
- Dòng 226: lấy bộ đếm cờ ISO từ `le_audio.c` (mục 6.10).
- Dòng 230: in thêm chất lượng liên kết ISO do controller đếm (mục 6.8).
- Dòng 225: controller SDC không hỗ trợ lệnh HCI đọc RSSI, nên dòng này không có RSSI.

### 5.6 Ghép đôi ([dòng 233–249](../apps/Helmet/src/main.c#L233))

`pairing_complete` / `pairing_failed` chỉ ghi log. `bonded = true` nghĩa là khóa đã được lưu vào RRAM, lần sau không cần ghép đôi lại. Struct `auth_info_cb` được đăng ký trong `main()` (dòng 507).

### 5.7 Nút nhấn ([dòng 251–288](../apps/Helmet/src/main.c#L251))

Đường đi của một lần nhấn:

```
nút nối GND → P0.0x xuống 0 → ngắt GPIOTE30 → gpio-keys chống dội 30 ms
  → sự kiện {type = INPUT_EV_KEY, code = INPUT_KEY_xxx, value = 1 (nhấn) / 0 (nhả)}
  → thread input → on_input()
```

- Dòng 259: chỉ xử lý **lúc nhấn** (`value == 1`), bỏ qua lúc nhả và các loại sự kiện khác.
- Dòng 263–281: theo mã phím (định nghĩa bởi `zephyr,code` trong DTS) gọi hàm của `media_ctrl.c`:
  - Vol+ → `media_ctrl_volume_up()`;
  - Vol− → `media_ctrl_volume_down()`;
  - Call (`INPUT_KEY_PLAYPAUSE`) → `media_ctrl_play_pause()`;
  - Power: chưa có chức năng.
- Dòng 288: `INPUT_CALLBACK_DEFINE(NULL, on_input, NULL)`. `NULL` đầu tiên nghĩa là nhận sự kiện từ **mọi** thiết bị input.

### 5.8 Lệnh shell ([dòng 290–478](../apps/Helmet/src/main.c#L290))

Mỗi lệnh là một hàm `int cmd_x(const struct shell *sh, size_t argc, char **argv)`: `argv[0]` là tên lệnh con, `argv[1]…` là tham số. Hàm trả 0 nếu thành công.

| Lệnh | Hàm | Hoạt động |
|---|---|---|
| `helmet status` | `cmd_status` (292) | In địa chỉ kết nối và mức bảo mật, rồi gọi `media_ctrl_print_status` và `le_audio_print_status` |
| `helmet vol <0..255>` | `cmd_vol` (309) | `strtol` đổi chữ ra số, kiểm tra 0–255, gọi `media_ctrl_set_volume` (đi qua VCP nên điện thoại cũng được báo) |
| `helmet play` / `pause` | `cmd_play` / `cmd_pause` (320, 328) | Gửi lệnh MCP |
| `helmet unpair` | `cmd_unpair` (336) | `bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY)` xóa **mọi** bond trong RRAM |
| `helmet capture [on\|off]` | `cmd_capture` (353) | Bật/tắt gửi âm thanh về PC (mục 9), rồi in thống kê |
| `helmet aclphy <1m\|2m\|coded>` | `cmd_aclphy` (384) | Thử nghiệm: xin đổi PHY của ACL bằng `bt_conn_le_phy_update` |
| `helmet aclint <ms>` | `cmd_aclint` (411) | Thử nghiệm: xin đổi connection interval. `units = ms × 4 / 5` đổi ms sang đơn vị 1.25 ms; điều kiện `(ms × 4) % 5 == 0` nghĩa là ms phải là bội của 5. Timeout đặt 400 × 10 ms = 4 s |
| `helmet cispref <phy> <rtn> <ms>` | `cmd_cispref` (443) | Thử nghiệm: đổi QoS Helmet đề nghị cho CIS (mục 6.2) |

Bảng lệnh ([dòng 458–478](../apps/Helmet/src/main.c#L458)):
- `SHELL_CMD(tên, NULL, mô tả, hàm)`: lệnh không tham số.
- `SHELL_CMD_ARG(tên, NULL, mô tả, hàm, bắt_buộc, tùy_chọn)`: số tham số **tính cả tên lệnh**.
  - Ví dụ `vol` là `2, 0`: phải có đúng 1 tham số. Shell tự kiểm tra trước khi gọi hàm, nên `cmd_vol` dùng `argv[1]` mà không cần kiểm tra `argc`.
  - `capture` là `1, 1`: 0 hoặc 1 tham số, nên `cmd_capture` phải tự kiểm tra `argc > 1`.
- `SHELL_CMD_REGISTER(helmet, &helmet_cmds, …)`: tạo lệnh gốc `helmet`.

### 5.9 `main()` ([dòng 482–545](../apps/Helmet/src/main.c#L482))

Thứ tự khởi tạo có chủ đích:

| Dòng | Lệnh | Vì sao ở vị trí này |
|---|---|---|
| 489 | `audio_out_init()` | Kiểm tra driver I2S sẵn sàng. Lỗi thì dừng luôn: không phát được âm thanh thì không cần Bluetooth |
| 495 | `capture_init()` | Tạo kênh RTT 1. Lỗi chỉ mất chức năng nghe trên PC nên bỏ qua kết quả (`(void)`) |
| 497 | `bt_enable(NULL)` | Bật controller và host. `NULL` = chạy đồng bộ, chờ xong mới trả về |
| 504–506 | `settings_load()` | Nạp **địa chỉ định danh, khóa riêng tư (IRK) và các bond** từ RRAM. Phải làm **sau** `bt_enable` và **trước** quảng bá; nếu không, Helmet dùng địa chỉ mới và điện thoại đã ghép đôi không nhận ra |
| 507 | `bt_conn_auth_info_cb_register` | Đăng ký callback ghép đôi |
| 509 | `bt_tmap_register(BT_TMAP_ROLE_UMR)` | Đăng ký dịch vụ TMAS với vai trò UMR |
| 515 | `media_ctrl_init()` | Đăng ký dịch vụ âm lượng VCS và khởi tạo MCP client |
| 520 | `le_audio_init()` | Đăng ký PACS, ASCS, callback luồng |
| 525–529 | `advertising_create()` + `k_work_submit(&adv_work)` | Quảng bá **sau cùng**: khi điện thoại tới thì mọi dịch vụ GATT đã sẵn sàng |
| 531–542 | Vòng lặp | Mỗi 10 s đọc thống kê `audio_out` và in dòng `heartbeat:`, rồi `log_link()` |

`main()` không tự xử lý sự kiện nào. Mọi thứ xảy ra trong callback ở các thread khác; thread `main` chỉ ngủ và in heartbeat.

---

## 6. `le_audio.c`

[apps/Helmet/src/le_audio.c](../apps/Helmet/src/le_audio.c): đóng vai BAP Unicast Server. File này trả lời điện thoại "tôi nhận được gì", chấp nhận hoặc từ chối cấu hình, và nhận gói âm thanh mỗi 10 ms.

### 6.1 Khả năng công bố trong PACS ([dòng 31–39](../apps/Helmet/src/le_audio.c#L31))

`BT_AUDIO_CODEC_CAP_LC3(tần số, độ dài khung, số kênh, octet min, octet max, khung tối đa/SDU, ngữ cảnh ưa thích)`:

| Tham số | Giá trị | Ý nghĩa |
|---|---|---|
| Tần số | 16, 24, 32, 48 kHz | Điện thoại chọn một trong số này. Nghe nhạc thường là 48 kHz |
| Độ dài khung | 7.5 ms và 10 ms | |
| Số kênh | 1 hoặc 2 | Cho phép 1 CIS chở stereo, hoặc 2 CIS mỗi CIS một kênh |
| Octet/khung | 26–155 | 155 B/10 ms ≈ 124 kbps mỗi kênh, mức chất lượng cao nhất của BAP |
| Khung/SDU | 1 | Mỗi SDU chỉ chứa 1 khung mỗi kênh. Đơn giản hóa `audio_out.c` |
| Ngữ cảnh ưa thích | Media | |

`struct bt_pacs_cap cap` bọc khả năng này thành một bản ghi PAC để đăng ký ở dòng 470.

### 6.2 QoS ưa thích ([dòng 41–46](../apps/Helmet/src/le_audio.c#L41))

`BT_BAP_QOS_CFG_PREF(unframed, PHY, RTN, latency, PD min, PD max, PD ưa thích min, max)`:
- `true`: Helmet hỗ trợ kiểu unframed (một SDU vừa một gói vô tuyến).
- `BT_GAP_LE_PHY_2M`: muốn CIS chạy 2 Mbit/s (ngắn thời gian phát, ít va chạm).
- `0x02`: RTN, số lần phát lại mỗi gói.
- `10`: độ trễ truyền tối đa 10 ms.
- `20000, 40000, 20000, 40000`: *presentation delay* 20–40 ms. Đây là thời gian từ lúc nhận tới lúc phát ra loa mà Helmet chấp nhận.

Biến này **không phải `const`**, vì lệnh `helmet cispref` sửa được nó (hàm `le_audio_set_qos_pref`, dòng 503). Giá trị mới có hiệu lực từ lần điện thoại cấu hình codec kế tiếp. Điện thoại chỉ coi đây là gợi ý và có quyền chọn khác.

### 6.3 Bộ nhớ cho các luồng ([dòng 48–63](../apps/Helmet/src/le_audio.c#L48))

```c
struct helmet_stream {
	struct bt_bap_stream stream; /* đối tượng của Zephyr, phải nằm trong bộ nhớ của app */
	struct helmet_stream_cfg cfg; /* cấu hình LC3 đã phân tích */
	bool streaming;               /* đang ở trạng thái Streaming */
	uint32_t rx_count;            /* số SDU nhận từ lúc bắt đầu */
};
static struct helmet_stream streams[2];
```

Zephyr chỉ đưa cho callback con trỏ `struct bt_bap_stream *`. Vì `stream` nằm **bên trong** `helmet_stream`, nên:
- `CONTAINER_OF(stream, struct helmet_stream, stream)` lấy lại struct cha, để truy cập `cfg`, `streaming`;
- `stream_idx()` (dòng 60) trừ con trỏ cho đầu mảng để ra **chỉ số 0 hoặc 1**. Chỉ số này là "tên" của luồng khi gọi sang `audio_out.c` và `capture.c`.

`rx_flags` (dòng 49): bộ đếm SDU theo cờ, **cộng dồn từ lúc khởi động** (không reset khi luồng mới bắt đầu).

### 6.4 `parse_codec_cfg`: đọc cấu hình điện thoại chọn ([dòng 65–112](../apps/Helmet/src/le_audio.c#L65))

Cấu hình codec điện thoại gửi là một chuỗi LTV (length-type-value). Các hàm `bt_audio_codec_cfg_get_*` tách từng trường:

| Dòng | Đọc | Xử lý |
|---|---|---|
| 72 | Mã codec | Phải là LC3 (`0x06`) |
| 76–80 | Tần số | Hàm trả về một mã liệt kê, đổi ra Hz |
| 82–86 | Độ dài khung | Đổi ra µs |
| 88–92 | Octet/khung | Phải > 0 |
| 95–100 | Vị trí kênh (Audio Location) | Tham số `true` = nếu điện thoại không gửi thì coi là mono (0). Số kênh = số bit 1 trong bitmask (mono tính là 1 kênh) |
| 102–105 | Khung/SDU | Phải là 1. Thiếu trường thì mặc định 1 |
| 107–109 | Giới hạn | Tối đa 2 kênh, và tổng byte không vượt 320 |

Kết quả ghi vào `struct helmet_stream_cfg`. Trả 0 là chấp nhận.

### 6.5 Callback ASCS: điện thoại điều khiển ASE ([dòng 121–249](../apps/Helmet/src/le_audio.c#L121))

Mỗi lần điện thoại ghi vào ASE Control Point, stack kiểm tra định dạng rồi gọi callback tương ứng. **Trả 0 = đồng ý**; trả lỗi kèm `*rsp` = từ chối, với mã lý do gửi về điện thoại.

| Thao tác của điện thoại | Callback | Helmet làm gì | Trạng thái ASE sau đó |
|---|---|---|---|
| Config Codec (ASE đang rảnh) | `lc3_config` (123) | Từ chối nếu không phải ASE sink (dòng 129). Kiểm tra cấu hình. Tìm phần tử `streams[i]` còn trống (`stream.conn == NULL`), trả về qua `*stream`, và trả `qos_pref` qua `*pref` | Codec Configured |
| Config Codec (ASE đã có cấu hình) | `lc3_reconfig` (155) | Kiểm tra lại, trả `qos_pref` | Codec Configured |
| Config QoS | `lc3_qos` (172) | Chỉ ghi log các thông số điện thoại chọn: interval, framing, PHY, SDU, RTN, latency, PD. Luôn đồng ý | QoS Configured |
| Enable (kèm metadata) | `lc3_enable` (183) | Phân tích cấu hình codec **lần cuối** và lưu vào `s->cfg`. Đây là cấu hình dùng khi phát | Enabling |
| Update Metadata | `lc3_metadata` (215) | Duyệt từng mục metadata; mục nào có kiểu lạ thì từ chối (`metadata_check_cb`, dòng 204) | không đổi |
| Disable | `lc3_disable` (221) | Ghi log | QoS Configured |
| Receiver Stop Ready | `lc3_stop` (227) | Ghi log. Thao tác này chỉ dành cho ASE *source*, nên Helmet hầu như không gặp | |
| Release | `lc3_release` (233) | Ghi log | Releasing → Idle |
| Receiver Start Ready | `lc3_start` (198) | Ghi log. **Với ASE sink, hàm này không được gọi**: Helmet tự báo sẵn sàng (mục 6.6) | |

`unicast_server_cb` (dòng 239–249) gom các callback trên, được đăng ký trong `le_audio_init`.

Các callback Disable/Stop/Release chỉ ghi log. Việc dừng phát thật sự nằm ở **stream ops** (mục 6.9), vì stream ops được gọi trong mọi trường hợp luồng dừng, kể cả khi mất kết nối đột ngột (lúc đó điện thoại không kịp gửi Disable).

### 6.6 `stream_enabled`: Helmet tự báo "sẵn sàng nhận" ([dòng 253–267](../apps/Helmet/src/le_audio.c#L253))

Theo BAP, với ASE **sink** thì bên nhận (Helmet) tự khởi động thao tác *Receiver Start Ready*. Hàm này:
- kiểm tra đây là ASE sink (dòng 258);
- gọi `bt_bap_stream_start(stream)` (dòng 263).

Bên trong Zephyr (`bt_bap_unicast_server_start`): nếu CIS đã nối thì chuyển ngay sang Streaming; chưa nối thì ghi nhớ `receiver_ready`. Khi điện thoại tạo CIS xong, stack tự chuyển sang Streaming và gọi `stream_started`.

### 6.7 `print_cis_info`: thông số kênh âm thanh ([dòng 269–300](../apps/Helmet/src/le_audio.c#L269))

Lấy thông tin CIS bằng `bt_bap_ep_get_info` → `iso_chan` → `bt_iso_chan_get_info`, rồi in:

| Trường | Ý nghĩa |
|---|---|
| ISO interval | Chu kỳ CIS. `iso_interval` tính theo 1.25 ms nên nhân 1250 ra µs |
| NSE | Số lần phát tối đa mỗi chu kỳ (gồm lần đầu và các lần phát lại) |
| subinterval | Khoảng cách giữa hai lần phát trong một chu kỳ |
| CIG sync delay | Thời gian từ điểm neo CIS tới điểm đồng bộ của cả nhóm |
| C→P PHY | PHY chiều điện thoại → Helmet |
| BN | Số gói mới mỗi chu kỳ (thường 1) |
| FT (flush timeout) | Gói được thử trong bao nhiêu chu kỳ trước khi bỏ. Zephyr lưu theo 1.25 ms nên chia cho `iso_interval` ra số chu kỳ |
| max PDU, max SDU | Kích thước gói vô tuyến và SDU |
| latency | Độ trễ truyền |

`sh == NULL` thì ghi vào log (gọi lúc luồng bắt đầu), khác `NULL` thì in ra shell (lệnh `helmet status`).

### 6.8 Chất lượng liên kết ISO do controller đếm ([dòng 302–372](../apps/Helmet/src/le_audio.c#L302))

`read_iso_link_quality` gửi trực tiếp lệnh HCI *LE Read ISO Link Quality* (opcode `0x2075`) xuống controller SDC:
1. Dòng 313–321: tìm **connection handle** của CIS (số định danh kênh trong controller).
2. Dòng 323–328: cấp bộ đệm lệnh HCI, ghi handle theo little-endian.
3. Dòng 330: `bt_hci_cmd_send_sync` gửi và **chờ** controller trả lời. Vì phải chờ, hàm này chỉ được gọi từ thread `main` hoặc shell, không gọi từ callback Bluetooth.
4. Dòng 335–338: chép kết quả, giải phóng bộ đệm trả lời.

`le_audio_print_link_quality` (dòng 342) in 3 bộ đếm cho mỗi luồng đang phát:

| Bộ đếm | Ý nghĩa |
|---|---|
| CRC error | Gói vô tuyến nhận được nhưng sai CRC: sóng yếu hoặc nhiễu |
| unreceived | Gói lẽ ra phải tới nhưng không nhận được gì |
| duplicate | Gói nhận trùng (phát lại khi Helmet đã nhận rồi) |

Bộ đếm này ở **mức gói vô tuyến** và reset khi CIS mới được tạo. `rx_flags` ở mục 6.10 thì ở **mức SDU** và cộng dồn từ lúc khởi động.

### 6.9 Bắt đầu và kết thúc luồng ([dòng 374–407](../apps/Helmet/src/le_audio.c#L374))

- **`stream_started`**: ASE vào Streaming. Đặt cờ `streaming`, reset `rx_count`, ghi log cấu hình và thông số CIS. Sau đó báo cho:
  - `audio_out_stream_start(idx, &cfg)`: chuẩn bị bộ giải mã và I2S;
  - `capture_stream_start(idx, &cfg)`: ghi bản ghi cấu hình sang PC.
- **`stream_end`**: hàm chung cho `stream_stopped` (CIS đứt hoặc Disable) và `stream_released` (Release). Kiểm tra `if (s->streaming)` để không báo dừng hai lần, vì cả hai callback có thể cùng xảy ra cho một luồng.

### 6.10 `stream_recv`: nhận một SDU, mỗi 10 ms mỗi luồng ([dòng 409–428](../apps/Helmet/src/le_audio.c#L409))

Hàm được gọi nhiều nhất trong toàn hệ thống, trong thread Bluetooth. Nó chỉ làm 3 việc nhẹ:

1. **Phân loại** SDU (dòng 415–424), theo thứ tự ưu tiên:

   | Điều kiện | Đếm vào | Nghĩa |
   |---|---|---|
   | Có cờ VALID và độ dài > 0 | `ok` | Gói tốt |
   | Có cờ LOST | `lost` | Không nhận được gói nào trong các lần phát |
   | Có cờ ERROR | `crc` | Nhận được nhưng có thể sai |
   | Còn lại | `empty` | Hợp lệ nhưng dài 0 byte (bên phát không có dữ liệu) |

2. **Chuyển cho `audio_out.c`** (dòng 425): kèm cờ "dùng được" (`valid && len > 0`), timestamp và dữ liệu.
3. **Chuyển cho `capture.c`** (dòng 427): kèm nguyên cờ ISO và số thứ tự (`seq_num`), để PC chẩn đoán chi tiết.

`buf` thuộc về stack và bị thu hồi ngay khi hàm trả về. Vì vậy cả `audio_out_stream_data` lẫn `capture_sdu` đều **chép** dữ liệu ra trước khi trả về.

`stream_ops` (dòng 430–436) gom các callback luồng. Mỗi `streams[i].stream` được gắn các ops này trong `le_audio_init`.

### 6.11 `le_audio_init` ([dòng 440–487](../apps/Helmet/src/le_audio.c#L440))

| Dòng | Lệnh | Ý nghĩa |
|---|---|---|
| 452 | `bt_pacs_register` | Tạo dịch vụ PACS có bản ghi sink và vị trí sink |
| 458 | `bt_bap_unicast_server_register` | Tạo dịch vụ ASCS với 2 ASE sink, 0 ASE source |
| 464 | `bt_bap_unicast_server_register_cb` | Gắn các callback ở mục 6.5 |
| 470 | `bt_pacs_cap_register(SINK, &cap)` | Công bố khả năng LC3 ở mục 6.1 |
| 476 | `bt_pacs_set_location` | Vị trí loa FL \| FR |
| 477–478 | `bt_pacs_set_supported_contexts` / `…available_contexts` | Ngữ cảnh *hỗ trợ* (cố định) và *đang sẵn sàng* (có thể đổi lúc chạy, ví dụ khi bận). Hiện hai giá trị bằng nhau |
| 480–482 | `bt_bap_stream_cb_register` | Gắn `stream_ops` cho từng phần tử `streams[]` |

### 6.12 Các hàm nhỏ và `le_audio_print_status` ([dòng 489–564](../apps/Helmet/src/le_audio.c#L489))

- `le_audio_phy_str`: đổi mã PHY ra chữ.
- `le_audio_set_qos_pref`: cho lệnh `cispref`.
- `le_audio_get_rx_flags`: chép bộ đếm cờ cho heartbeat.
- `le_audio_is_streaming`: có ít nhất 1 luồng đang phát. Dùng trong heartbeat và trong `media_ctrl_play_pause`.
- `le_audio_print_status`: phần lớn nội dung của lệnh `helmet status`: từng ASE, thông số CIS, ISO link quality, bộ đếm cờ, QoS ưa thích, rồi thống kê của `audio_out`.

---

## 7. `audio_out.c`

[apps/Helmet/src/audio_out.c](../apps/Helmet/src/audio_out.c): biến SDU LC3 thành âm thanh ra loa. Đây là file có nhiều logic nhất.

### 7.1 Ý tưởng chung

```
 thread Bluetooth                          thread audio (ưu tiên preempt 2, dùng FPU)
 ─────────────────                         ───────────────────────────────────────────
 audio_out_stream_start ─┐
 audio_out_stream_stop  ─┼─► audio_q ─────► audio_thread ─► handle_start / handle_stop / handle_data
 audio_out_stream_data  ─┘  (8 thông điệp,                     │
   (chỉ chép, không chờ)     mỗi cái 332 B)                    │ lc3_decode từng kênh → frame_pcm (L,R xen kẽ)
                                                               │ đủ kênh → flush_frame: âm lượng
                                                               ▼
                                                         i2s_submit ─► i2s_slab (8 khối × 1920 B)
                                                               │       │
                                                               ▼       ▼
                                                         driver I2S (hàng đợi 4 + 2 khối DMA)
                                                               │
                                                     EasyDMA → P1.12 BCLK, P1.10 LRCLK, P1.15 DIN
                                                               │
                                                     2 × MAX98357A → 2 loa
```

Ba nguyên tắc:
1. **Mỗi khung LC3 sinh ra đúng một khối PCM** cho I2S. Gói mất vẫn được controller báo lên (cờ LOST), và được "giải mã" bằng PLC. Nhờ vậy I2S luôn có dữ liệu đều đặn.
2. **Mọi phép tính nặng nằm trong `audio_thread`.** Các hàm API chỉ đóng gói thông điệp.
3. **Clock của điện thoại và của mũ lệch nhau**, nên số khối chờ trong I2S trôi dần. Mã bù bằng cách bỏ hoặc lặp nguyên một khung (mục 7.5).

### 7.2 Hằng số ([dòng 33–48](../apps/Helmet/src/audio_out.c#L33))

| Hằng | Giá trị | Ý nghĩa |
|---|---|---|
| `MAX_FRAME_SAMPLES` | 480 | 48 000 Hz × 10 ms |
| `I2S_BLOCK_MAX` | 1 920 B | 480 mẫu × 2 kênh × 2 byte |
| `I2S_BLOCK_COUNT` | 8 | Số khối trong kho: driver giữ tối đa 2 (đang DMA) + 4 (hàng đợi) = 6, còn 2 khối dự phòng |
| `PREFILL_BLOCKS` | 3 | Chỉ bắt đầu phát khi đã có 3 khung (~30 ms) để chịu được gói đến không đều |
| `HIGH_WATER` | 5 | Từ 5 khối đang chờ trở lên: bỏ 1 khung |
| `LOW_WATER` | 1 | Từ 1 khối trở xuống: lặp 1 khung |
| `CH_L`, `CH_R` | bit 0, bit 1 | Mặt nạ kênh trái/phải |
| `AUDIO_THREAD_STACK` | 4 096 B | liblc3 cần stack khá lớn |
| `AUDIO_THREAD_PRIO` | preempt 2 | Ưu tiên cao hơn shell và log, thấp hơn thread Bluetooth (coop) |

### 7.3 Thông điệp và hàng đợi ([dòng 50–69](../apps/Helmet/src/audio_out.c#L50))

`struct audio_msg` có 3 loại: `MSG_START` (kèm cấu hình), `MSG_STOP`, `MSG_DATA` (kèm SDU). Phần `union` dùng chung bộ nhớ cho `cfg` và `data`, vì một thông điệp chỉ mang một trong hai. Kích thước 332 B.

- `K_MSGQ_DEFINE(audio_q, sizeof(struct audio_msg), 8, 4)`: hàng đợi 8 thông điệp, căn lề 4 byte. `k_msgq_put` **chép** toàn bộ 332 B vào hàng đợi, nên bên gửi dùng lại biến của mình ngay được.
- `K_MEM_SLAB_DEFINE_STATIC(i2s_slab, 1920, 8, 4)`: kho 8 khối PCM. Driver I2S tự trả khối về kho sau khi DMA phát xong.

### 7.4 Biến trạng thái ([dòng 71–99](../apps/Helmet/src/audio_out.c#L71))

| Biến | Ý nghĩa |
|---|---|
| `i2s_dev` | Driver I2S20 lấy từ devicetree |
| `ds[2]` (`struct dec_stream`) | Mỗi luồng: đang hoạt động không, cấu hình, mặt nạ loa cho từng kênh LC3 (`out_mask`), con trỏ bộ giải mã và **bộ nhớ** của 2 bộ giải mã (`lc3_decoder_mem_48k_t`, khoảng 9 KB mỗi bộ, cấp sẵn cho trường hợp lớn nhất 48 kHz) |
| `frame_pcm[960]` | Khung stereo đang ghép: mẫu trái ở chỉ số chẵn, mẫu phải ở chỉ số lẻ (đúng thứ tự I2S cần) |
| `frame_mask` | Kênh nào đã có dữ liệu trong khung đang ghép |
| `expected_mask` | Kênh nào các luồng đang hoạt động sẽ cung cấp |
| `frame_ts` | Timestamp của khung đang ghép |
| `out_rate`, `out_samples` | Tần số và số mẫu/khung đã cấu hình cho I2S |
| `i2s_configured`, `i2s_running`, `prefill` | Trạng thái I2S và số khối đã nạp trước khi START |
| `gain_q15` | Hệ số âm lượng dạng số nguyên Q15 (32 767 = 1.0 = 0 dB). `atomic_t` vì thread Bluetooth/input ghi, thread audio đọc |
| `stats` | Bộ đếm (mục 12.1) |

### 7.5 Lớp I2S ([dòng 101–214](../apps/Helmet/src/audio_out.c#L101))

**`blocks_in_use()`**: 8 trừ số khối còn trống trong kho = số khối **đang chờ phát hoặc đang phát**. Đây là "mực nước" của bộ đệm.

**`i2s_halt()`** (dòng 116): dừng ngay và bỏ dữ liệu.
- `I2S_TRIGGER_DROP` dừng phát và trả mọi khối về kho.
- Nếu driver đang ở trạng thái lỗi (thiếu dữ liệu, *underrun*) thì DROP không được, phải `PREPARE` để về trạng thái sẵn sàng.
- Sau đó chờ tối đa 100 ms (50 × 2 ms) cho DMA trả hết khối về kho.

**`i2s_setup(rate, samples)`** (dòng 131): cấu hình I2S.

| Trường | Giá trị | Ý nghĩa |
|---|---|---|
| `word_size` | 16 | Mỗi mẫu 16 bit |
| `channels` | 2 | Stereo; mỗi ampli MAX98357A tự chọn kênh trái hoặc phải theo điện trở ở chân SD_MODE |
| `format` | I2S chuẩn | |
| `options` | BIT_CLK + FRAME_CLK controller | nRF54L15 **phát** BCLK và LRCLK (master) |
| `frame_clk_freq` | `rate` | Tần số lấy mẫu yêu cầu, ví dụ 48 000 |
| `block_size` | samples × 4 | 1 920 B ở 48 kHz/10 ms |
| `timeout` | 20 ms | `i2s_write` chờ tối đa 20 ms khi hàng đợi driver đầy |

⚠ **Đây là nơi phát sinh lỗi lệch tần số.** I2S của nRF54L15 tạo clock bằng cách chia nguyên 32 MHz. Với 48 kHz, driver chọn MCK = 32 MHz / 21 ≈ 1.5238 MHz, BCLK = MCK, LRCLK = BCLK / 32 = **47 619 Hz** (thấp hơn 0.8 %). Biến `out_rate` lưu **tần số yêu cầu** (48 000), nên log và heartbeat vẫn in "48000 Hz", không phải tần số thật.

**`i2s_submit(pcm)`** (dòng 162): đưa một khung PCM ra I2S, kèm cơ chế bù lệch clock.

```
used = blocks_in_use()
nếu I2S đang chạy:
    used ≥ 5 (HIGH_WATER) → bỏ khung này     (stats.drift_drop++)  — dữ liệu đến nhanh hơn I2S tiêu thụ
    used ≤ 1 (LOW_WATER)  → ghi khung 2 lần  (stats.drift_repeat++) — sắp cạn, tránh underrun
với mỗi bản (1 hoặc 2):
    lấy 1 khối trong kho (không chờ); hết khối → bỏ
    memcpy khung vào khối
    i2s_write: giao khối cho driver (driver sẽ tự trả về kho sau khi phát)
        lỗi → i2s_halt, bắt đầu lại từ đầu
    nếu chưa chạy và đã nạp đủ 3 khối → I2S_TRIGGER_START
```

Ví dụ với lỗi 47 619 Hz:
- Điện thoại gửi 100 khung/s.
- I2S chỉ tiêu thụ 47 619 / 480 = 99.2 khung/s.
- Mỗi giây dư 0.8 khung, mực nước tăng dần. Cứ khoảng 1.26 s lại chạm `HIGH_WATER` và một khung 10 ms bị bỏ.
- Trên heartbeat thấy `sync -8/+0` mỗi 10 s. Khi có loa sẽ nghe tiếng "tách".

### 7.6 `flush_frame`: hoàn tất khung và áp âm lượng ([dòng 218–242](../apps/Helmet/src/audio_out.c#L218))

- Dòng 222: khung chưa có kênh nào thì bỏ qua.
- Dòng 227–238: với từng cặp mẫu:
  - thiếu kênh trái thì chép kênh phải sang, và ngược lại. Mũ có 2 loa nên luôn phát cả hai, ví dụ khi một luồng bị mất hẳn;
  - nhân với `gain` rồi dịch phải 15 bit (phép nhân số thực 0…1 bằng số nguyên).
- Dòng 240–241: gửi ra I2S, xóa `frame_mask` để ghép khung mới.

### 7.7 Ánh xạ kênh ([dòng 244–275](../apps/Helmet/src/audio_out.c#L244))

**`map_channels`**: kênh LC3 thứ `c` của một luồng sẽ ra loa nào.

| Luồng nhận | `out_mask` | Kết quả |
|---|---|---|
| 2 kênh trong 1 SDU | `[L, R]` | Kênh 0 → trái, kênh 1 → phải. Theo BAP, kênh xếp theo thứ tự bit vị trí tăng dần: FL (bit 0) trước FR (bit 1) |
| 1 kênh, vị trí chỉ có FL | `[L]` | Chỉ loa trái (luồng phải đến từ CIS khác) |
| 1 kênh, vị trí chỉ có FR | `[R]` | Chỉ loa phải |
| 1 kênh, mono (0) hoặc FL\|FR | `[L\|R]` | Cùng âm thanh ra cả hai loa |

**`update_expected_mask`**: OR tất cả `out_mask` của các luồng đang hoạt động. Ví dụ: 2 luồng mono L và R → `expected_mask = L|R`.

### 7.8 Xử lý thông điệp ([dòng 277–385](../apps/Helmet/src/audio_out.c#L277))

**`handle_start(idx, cfg)`** (dòng 279):
1. `lc3_frame_samples(10000, 48000)` = 480. Kiểm tra cấu hình hợp lệ.
2. Với mỗi kênh, `lc3_setup_decoder(độ dài khung, tần số, 0, &mem)`. Bộ giải mã dùng vùng nhớ cấp sẵn, không cần cấp phát động. Tham số `0` = PCM ra cùng tần số với codec.
3. Ánh xạ kênh, đánh dấu luồng hoạt động, cập nhật `expected_mask`.
4. Nếu tần số hoặc số mẫu khác cấu hình I2S hiện tại thì cấu hình lại I2S. Hai luồng cùng cấu hình chỉ cấu hình I2S một lần.

**`handle_stop(idx)`** (dòng 318): bỏ luồng khỏi `expected_mask`. Không còn luồng nào thì dừng I2S (ampli tự tắt khi mất BCLK).

**`handle_data(m)`** (dòng 335): trái tim của đường phát.

| Dòng | Việc |
|---|---|
| 341 | Luồng chưa bắt đầu hoặc I2S chưa cấu hình thì bỏ |
| 345–351 | Đếm SDU. SDU "dùng được" nhưng ngắn hơn `octets × số kênh` thì coi như hỏng |
| 354–357 | **SDU thuộc khoảng 10 ms mới** (timestamp khác khung đang ghép) mà khung cũ chưa đủ kênh: phát luôn khung cũ (`flush_frame` sẽ chép kênh còn lại sang). Tình huống: SDU của luồng kia không tới |
| 359–380 | Với từng kênh LC3 trong SDU: |
| 361 | `dst` trỏ vào vị trí mẫu đầu tiên của loa đích: `&frame_pcm[0]` (trái) hoặc `&frame_pcm[1]` (phải) |
| 362 | Dữ liệu vào: phần của kênh `c` trong SDU, hoặc **`NULL` nếu SDU hỏng/mất → liblc3 tự nội suy (PLC)** |
| 363 | `lc3_decode(dec, in, octets, S16, dst, 2)`. **Bước nhảy (stride) = 2**: liblc3 ghi mẫu vào `dst[0], dst[2], dst[4]…`. Đó chính là các vị trí của một kênh trong mảng L/R xen kẽ, nên giải mã xong là đã xen kẽ, không cần chép lại |
| 365–372 | Trả 1 = đã chạy PLC (đếm `plc`); trả âm = lỗi, ghi im lặng vào kênh đó |
| 374–378 | Luồng mono ra cả hai loa: chép mẫu trái sang phải |
| 379 | Đánh dấu kênh đã có |
| 382–384 | Đủ mọi kênh mong đợi thì phát khung |

**Ba tình huống cụ thể:**

1. *Điện thoại gửi 1 luồng stereo (1 CIS, 2 kênh, SDU 240 B)*: một SDU có 120 B kênh trái + 120 B kênh phải. Giải mã kênh 0 vào vị trí trái, kênh 1 vào vị trí phải. `frame_mask = L|R` khớp `expected_mask`, nên phát ngay.
2. *Điện thoại gửi 2 luồng mono (2 CIS, mỗi CIS 1 kênh: FL và FR)*:
   - SDU của luồng 0 (timestamp T) → giải mã vào trái, `frame_mask = L`, chưa đủ, chờ.
   - SDU của luồng 1 (cùng timestamp T, vì cùng CIG) → giải mã vào phải, đủ, phát.
3. *Gói của luồng 1 bị mất*:
   - Thường thì controller vẫn báo SDU với cờ LOST, nên PLC lấp vào kênh phải và mọi thứ chạy như tình huống 2.
   - Nếu SDU không tới hẳn, khi SDU kế tiếp của luồng 0 (timestamp T+10 ms) tới, dòng 354 phát khung T với kênh phải chép từ trái.

### 7.9 Thread audio ([dòng 387–415](../apps/Helmet/src/audio_out.c#L387))

- `audio_thread` chờ (`K_FOREVER`) lấy thông điệp rồi chuyển cho hàm xử lý tương ứng.
- `msg` là `static` (không nằm trên stack) để tiết kiệm 332 B stack.
- `K_THREAD_DEFINE(audio_tid, 4096, audio_thread, …, AUDIO_THREAD_PRIO, K_FP_REGS, 0)`:
  - thread tự chạy lúc khởi động, không cần gọi hàm tạo;
  - `K_FP_REGS` báo kernel thread này dùng FPU, để lưu/khôi phục thanh ghi FPU khi đổi thread;
  - tham số cuối `0` = chạy ngay, không trì hoãn.

### 7.10 API cho module khác ([dòng 417–492](../apps/Helmet/src/audio_out.c#L417))

| Hàm | Gọi từ | Ghi chú |
|---|---|---|
| `audio_out_init` | `main` | Chỉ kiểm tra driver I2S sẵn sàng |
| `audio_out_stream_start` / `_stop` | thread Bluetooth | `k_msgq_put` **chờ tối đa 100 ms**: thông điệp điều khiển không được mất |
| `audio_out_stream_data` | thread Bluetooth, mỗi 10 ms | Chép SDU vào thông điệp `static` rồi `k_msgq_put` với `K_NO_WAIT`: **không bao giờ** chặn thread Bluetooth. Hàng đợi đầy (thread audio không theo kịp) thì bỏ gói và đếm `q_full`. Biến `static` chỉ an toàn vì chỉ có một thread (Bluetooth) gọi hàm này |
| `audio_out_set_volume` | VCP callback (thread Bluetooth hoặc thread gọi lệnh) | Xem bảng dưới |
| `audio_out_get_stats` | heartbeat, shell | Chép thống kê và đọc mực nước hiện tại |

**Đường cong âm lượng** (dòng 479): `gain = volume² × 32767 / 255²`. Đường bình phương gần với cảm nhận của tai hơn đường thẳng.

| Âm lượng VCP | gain (Q15) | Tương đương |
|---|---|---|
| 255 | 32 767 | 0 dB |
| 192 | 18 576 | −4.9 dB |
| 128 (mặc định) | 8 256 | −12.0 dB |
| 64 | 2 064 | −24.0 dB |
| 16 | 129 | −48.1 dB |
| 0 hoặc mute | 0 | im lặng |

---

## 8. `media_ctrl.c`

[apps/Helmet/src/media_ctrl.c](../apps/Helmet/src/media_ctrl.c): âm lượng (VCP, Helmet là server) và play/pause (MCP, Helmet là client).

### 8.1 Biến ([dòng 26–36](../apps/Helmet/src/media_ctrl.c#L26))

- `VOLUME_DEFAULT 128`: khoảng 50 % lúc khởi động.
- `VOLUME_STEP 16`: mỗi lần nhấn đổi 16/255, khoảng 16 nấc từ nhỏ nhất tới lớn nhất.
- `mcc_conn`: kết nối đang dùng cho MCP (có `ref`).
- `mcs_ready`: đã tìm thấy dịch vụ media trên điện thoại.
- `media_state`: trạng thái trình phát của điện thoại (playing, paused…).
- `cur_volume`, `cur_mute`: bản sao để in trạng thái.
- `mcs_discover_work`: work item để dò dịch vụ media ngoài callback Bluetooth.

### 8.2 VCP: âm lượng ([dòng 38–64](../apps/Helmet/src/media_ctrl.c#L38) và [138–153](../apps/Helmet/src/media_ctrl.c#L138))

`bt_vcp_vol_rend_register` (dòng 148) tạo dịch vụ **VCS** trên Helmet với bước 16, âm lượng 128, không tắt tiếng.

**Mọi thay đổi âm lượng đi qua cùng một đường:**
- Điện thoại ghi vào VCS, **hoặc** nút / lệnh shell gọi `bt_vcp_vol_rend_vol_down()`, `…unmute_vol_up()`, `…set_vol()`.
- Bên trong Zephyr, cả hai trường hợp chạy cùng hàm xử lý điều khiển VCS. Hàm này cập nhật trạng thái, tăng bộ đếm thay đổi, gửi thông báo cho điện thoại (thanh âm lượng trên điện thoại nhảy theo), rồi gọi **`vcs_state_cb`**.
- `vcs_state_cb` (dòng 40) lưu giá trị và gọi `audio_out_set_volume(volume, mute)`. **Đây là điểm duy nhất** âm lượng thực sự được áp vào âm thanh.

Nhờ vậy âm lượng trên mũ và trên điện thoại luôn khớp nhau.

`vcs_flags_cb` chỉ ghi log mức debug.

### 8.3 MCP: play/pause ([dòng 66–134](../apps/Helmet/src/media_ctrl.c#L66) và [164–182](../apps/Helmet/src/media_ctrl.c#L164))

Trình tự:
1. `bt_mcc_init(&mcc_cb)` (dòng 155): đăng ký 3 callback.
2. Mã hóa xong → `main.c` gọi `media_ctrl_connected(conn)` (dòng 165): giữ tham chiếu kết nối, đẩy `mcs_discover_work` sang workqueue.
3. `mcs_discover_work_handler` (dòng 107): `bt_mcc_discover_mcs(conn, true)` dò dịch vụ media (GMCS) trên điện thoại và **đăng ký nhận thông báo** (`true`).
4. `mcc_discover_mcs_cb` (dòng 68):
   - không tìm thấy (ví dụ máy phát là Radxa/BlueZ không có MCS) thì ghi cảnh báo, play/pause bị tắt;
   - tìm thấy thì `mcs_ready = true` và đọc trạng thái phát nhạc.
5. `mcc_read_media_state_cb` (dòng 80) được gọi **cả khi đọc lẫn khi điện thoại thông báo** đổi trạng thái (người dùng bấm play/pause trên điện thoại), nên `media_state` luôn cập nhật.
6. Ngắt kết nối → `media_ctrl_disconnected` (dòng 174) bỏ tham chiếu và xóa trạng thái.

`send_media_cmd(opcode)` (dòng 121): gửi lệnh tới *Media Control Point* của điện thoại. `use_param = false` vì PLAY/PAUSE không có tham số. Kết quả về sau qua `mcc_send_cmd_cb`.

### 8.4 API ([dòng 184–225](../apps/Helmet/src/media_ctrl.c#L184))

**`media_ctrl_play_pause`** (dòng 194) quyết định gửi PLAY hay PAUSE:

```
"đang phát" = (điện thoại báo PLAYING)
           hoặc (điện thoại chưa báo trạng thái gì VÀ Helmet đang nhận luồng âm thanh)
đang phát → PAUSE, ngược lại → PLAY
```

Vế thứ hai xử lý trường hợp chưa đọc được trạng thái từ điện thoại: khi đó dùng việc "có luồng âm thanh hay không" để đoán.

`media_ctrl_volume_up` dùng thao tác **"bỏ tắt tiếng + tăng"** (`unmute_vol_up`): đang tắt tiếng mà nhấn Vol+ thì có tiếng lại luôn.

---

## 9. `capture.c`

[apps/Helmet/src/capture.c](../apps/Helmet/src/capture.c): chép nguyên các SDU LC3 (chưa giải mã) ra **RTT kênh 1** để PC đọc qua J-Link. Dùng để nghe thử khi chưa có loa và để chẩn đoán đường vô tuyến.

### 9.1 SEGGER RTT trong một đoạn

RTT là các **vòng đệm nằm trong RAM của chip**. J-Link đọc/ghi các vòng đệm đó qua dây SWD trong khi CPU vẫn chạy bình thường. Mỗi kênh có vòng đệm riêng:
- kênh 0 ("Terminal", 4 KB): log và shell;
- kênh 1 ("HelmetAudio", 16 KB): do `capture.c` tạo.

Chế độ `NO_BLOCK_SKIP`: nếu vòng đệm không đủ chỗ cho **cả** bản ghi thì bỏ cả bản ghi, **không bao giờ chờ**. Vì vậy cắm hay không cắm J-Link, PC đọc nhanh hay chậm đều không ảnh hưởng tới luồng nhạc.

### 9.2 Định dạng bản ghi ([dòng 14–20](../apps/Helmet/src/capture.c#L14))

```
Đầu bản ghi (8 B):  A5 5A | loại | luồng | độ dài payload (u16) | số thứ tự bản ghi (u16)
CFG  (loại 1, 16 B): tần số u32 | độ dài khung µs u32 | octet/khung u16 | số kênh u8 | 0 | vị trí u32
SDU  (loại 2):       cờ ISO u8 | 0 | số thứ tự ISO u16 | timestamp u32 | dữ liệu LC3
STOP (loại 3):       không có payload
```

Ví dụ một bản ghi SDU của luồng 0, mono 120 B, bản ghi số 0x0102, cờ VALID|TS (0x09), số thứ tự ISO 0x0457, timestamp 0x12345678:

```
A5 5A 02 00 80 00 02 01 | 09 00 57 04 78 56 34 12 | <120 byte LC3>
```

Độ dài payload = 8 + 120 = 128 = `0x0080`. Mọi số đều little-endian.

- `A5 5A` giúp PC tìm lại đầu bản ghi khi dữ liệu bị cắt giữa chừng.
- Số thứ tự bản ghi nhảy cóc cho PC biết có bản ghi bị bỏ (vòng đệm đầy).

### 9.3 Mã nguồn

| Dòng | Nội dung |
|---|---|
| 36–44 | Kênh 1, vòng đệm 16 KB (khoảng 0.5 s ở tốc độ lớn nhất 33 KB/s), gửi lại cấu hình mỗi 100 SDU (~1 s), các hằng số định dạng |
| 46–47 | `BUILD_ASSERT`: báo lỗi **lúc biên dịch** nếu Kconfig không có đủ kênh RTT |
| 61 | `rtt_buf[16384]`: bộ nhớ của vòng đệm kênh 1 |
| 63–64 | `enabled`, `resend_cfg` là `atomic_t`, vì lệnh shell ghi còn thread Bluetooth đọc |
| 67 | `cap_lock`: mutex bảo vệ `cs[]`, `rec[]` và `rec_seq` khỏi nhiều thread cùng ghi |
| 69 | `rec[]`: **một** bộ đệm dựng bản ghi dùng chung (nên mới cần mutex) |
| `send_rec` (74) | Điền 8 B đầu, tăng số thứ tự, `SEGGER_RTT_Write`. Trả 0 nghĩa là bị bỏ do vòng đệm đầy, đếm `rec_dropped` |
| `send_cfg` (91) | Dựng payload CFG từ `cs[idx].cfg`, gửi, reset bộ đếm "SDU từ lần gửi cấu hình" |
| `capture_init` (107) | `SEGGER_RTT_ConfigUpBuffer(1, "HelmetAudio", rtt_buf, 16384, NO_BLOCK_SKIP)`. Tên kênh để script PC kiểm tra đúng firmware |
| `capture_set_enabled` (119) | Bật thì đặt cờ "gửi lại cấu hình ngay" để PC vừa mở có cấu hình để giải mã |
| `capture_stream_start` (128) | Lưu cấu hình luồng; đang bật thì gửi CFG |
| `capture_stream_stop` (143) | Gửi bản ghi STOP |
| `capture_sdu` (159) | Tắt thì về ngay (chỉ tốn một phép đọc atomic). Bật thì: gửi lại cấu hình nếu cần (lần đầu sau khi bật, hoặc mỗi 100 SDU), dựng payload SDU, gửi |
| `capture_print_status` (199) | Trạng thái, số bản ghi đã gửi và bị bỏ, số byte đang chờ PC đọc |

---

## 10. `scripts/helmet_audio.py`

[scripts/helmet_audio.py](../scripts/helmet_audio.py): chương trình Python trên PC. Chỉ dùng thư viện chuẩn và `ctypes` để gọi trực tiếp hai thư viện C có sẵn trên máy: `libjlinkarm.so` (SEGGER) và `liblc3.so.1` (cùng bộ giải mã với firmware). Không cần cài thêm gói Python nào.

| Thành phần | Dòng | Việc |
|---|---|---|
| Hằng số | 39–58 | Định dạng bản ghi (`struct.Struct`, khớp `capture.c`), cờ ISO, tên kênh |
| `find_elf`, `elf_symbol` | 67–102 | Tìm `zephyr.elf` trong thư mục build và đọc **địa chỉ biến `_SEGGER_RTT`** từ bảng ký hiệu ELF. Probe J-Link V9 không tự dò được RTT trên nRF54L15 nên phải chỉ địa chỉ |
| `JLink` | 117–203 | Bọc các hàm DLL: mở probe theo số serial, chọn chip `nRF54L15_M33`, SWD, kết nối (thử lại 5 lần), khởi động RTT tại địa chỉ đã biết, đọc/ghi từng kênh |
| `Lc3`, `Lc3Decoder`, `Lc3Encoder` | 208–272 | Bọc liblc3. `decode(None)` = PLC, giống firmware. Bộ mã hóa chỉ dùng cho tự kiểm tra |
| `RecordParser` | 277–314 | Gom byte từ RTT, tìm `A5 5A`, kiểm tra loại và độ dài, cắt ra từng bản ghi; số thứ tự nhảy cóc thì đếm vào `lost` |
| `Stream`, `Mixer` | 319–489 | **Bản sao logic của `audio_out.c` trên PC**: ánh xạ kênh, ghép khung theo timestamp, PLC cho gói hỏng. Thêm hai việc firmware không cần: lấp chỗ trống khi bản ghi bị bỏ trên đường RTT (`MAX_GAP` 50 khung), và chờ kênh còn lại tối đa 8 khung (`PAIR_WINDOW`) |
| `AudioSink` | 494–559 | Ghi WAV stereo 16 bit vào `build/audio/`, đồng thời đẩy PCM vào `paplay` để phát ra loa máy tính |
| `stats_line` | 589–607 | Dòng thống kê mỗi 10 s; nếu ≥ 90 % SDU lỗi thì đoán nguyên nhân theo loại lỗi nhiều nhất |
| `run_live` | 610–686 | Chế độ chính (xem dưới) |
| `run_replay` | 689–696 | Giải mã lại file dữ liệu thô đã lưu bằng `--raw` |
| `run_selftest` | 709–795 | Tự sinh âm thanh thử, mã hóa LC3, đóng gói giống firmware, đưa qua parser và mixer, rồi kiểm tra kết quả. Không cần mạch |

**Vòng lặp của `run_live`:**

```
kiểm tra không có "make rtt" đang mở (cổng 19021) → mở J-Link → khởi động RTT
gửi "helmet capture on" vào kênh 0
lặp:
    đọc kênh 0 → in log ra màn hình
    đọc kênh 1 → (lưu thô nếu có --raw) → parser → mixer → WAV + paplay
    có dòng gõ từ bàn phím → gửi vào kênh 0 (shell)
    mỗi 10 s in thống kê
Ctrl+C → gửi "helmet capture off" → đóng file WAV, J-Link
```

---

## 11. Các luồng hoạt động từ đầu đến cuối

### 11.1 Khởi động

| Bước | Ở đâu | Log |
|---|---|---|
| Kernel chạy, các thread `K_THREAD_DEFINE` (audio) được tạo và chờ hàng đợi | Zephyr | |
| `main()` | `main.c:487` | `=== Helmet LE Audio headset ===` |
| Kiểm tra I2S | `audio_out.c:419` | `Audio out ready: LC3 decoder …` |
| Tạo RTT kênh 1 | `capture.c:107` | |
| Bật Bluetooth, nạp bond | `main.c:497–506` | `bt_hci_core: …` |
| Dịch vụ TMAS, VCS, MCC, PACS, ASCS | `main.c:509–520` | `BAP unicast server: 2 sink ASE, …` |
| Quảng bá | `main.c:525–529` → `adv_work_handler` | `Advertising as "Helmet" (LE Audio headset)` |
| Mỗi 10 s | `main.c:531` | `heartbeat: advertising, idle, …` |

### 11.2 Điện thoại kết nối và ghép đôi

```
Điện thoại                                  Helmet
──────────                                  ──────
kết nối ─────────────────────────────────► connected(): active_conn, yêu cầu mã hóa L2
                                             log "Connected: …"
ghép đôi Just Works / dùng khóa cũ ◄──────► SMP (Zephyr)
                                             pairing_complete: "Pairing complete (bonded, keys saved)"
                                             security_changed: "Link encrypted (security level 2)"
                                               └─► media_ctrl_connected → (workqueue) dò MCS
đọc PACS, ASCS, VCS, CAS, TMAS ──────────► stack tự trả lời từ dữ liệu đã đăng ký
cung cấp MCS ◄────────────────────────────  mcc_discover_mcs_cb: "Media Control Service found on phone"
```

### 11.3 Điện thoại mở luồng nhạc

| Điện thoại làm | Hàm Helmet được gọi | Log |
|---|---|---|
| Config Codec cho ASE 1 (và ASE 2 nếu gửi 2 luồng) | `lc3_config` → trả QoS ưa thích | `le_audio: Config ASE 0: LC3 48000 Hz, 10000 us, 2 ch (loc 0x3), 120 B/frame` |
| Config QoS | `lc3_qos` | `QoS ASE 0: interval 10000 us, unframed, PHY 2M, SDU 240 B, RTN 2, latency 10 ms, PD 40000 us` |
| Enable | `lc3_enable` → `stream_enabled` → `bt_bap_stream_start` (sẵn sàng nhận) | `Enable ASE 0 (metadata … B)` |
| Tạo CIS (lớp vô tuyến) | stack chuyển ASE sang Streaming → `stream_started` | `Streaming ASE 0: …` và `CIS ASE 0: ISO interval 10000 us, NSE …` |
| | `audio_out_stream_start` → (thread audio) `handle_start` | `audio_out: I2S configured: 48000 Hz, 480 samples/frame …`, `Stream 0 decoding: … -> L+R` |
| Bắt đầu gửi SDU mỗi 10 ms | `stream_recv` … (mục 11.4) | sau 3 khung: `I2S output running (48000 Hz)` |

### 11.4 Hành trình của một khung âm thanh 10 ms

```
 1. Điện thoại phát gói CIS trên sóng 2M PHY (phát lại tối đa NSE lần nếu cần)
 2. Controller SDC trong nRF54L15 nhận, kiểm tra CRC, giải mã (AES-CCM)
    → ghép thành SDU, gắn cờ VALID / ERROR / LOST và timestamp
 3. Host Zephyr nhận SDU qua HCI nội bộ → thread bt_rx
 4. le_audio.c: stream_recv()
       đếm cờ (rx_flags)
       audio_out_stream_data(): chép SDU vào thông điệp, k_msgq_put (không chờ)
       capture_sdu(): nếu đang bật thì chép ra RTT kênh 1
    → trả về, thread Bluetooth rảnh
 5. Thread audio thức dậy: handle_data()
       với từng kênh: lc3_decode (hoặc PLC nếu hỏng) → ghi thẳng vào frame_pcm theo bước 2
       đủ kênh → flush_frame(): lấp kênh thiếu, nhân âm lượng
 6. i2s_submit(): kiểm tra mực nước (bỏ hoặc lặp khung nếu cần)
       lấy khối từ i2s_slab, chép 1920 B, i2s_write
 7. Driver I2S: khối vào hàng đợi; EasyDMA đọc khối ra chân P1.15 theo nhịp BCLK/LRCLK
 8. Phát xong → driver trả khối về i2s_slab
 9. MAX98357A trái lấy mẫu kênh trái, ampli phải lấy kênh phải → loa
```

Thời gian từ bước 3 tới lúc âm thanh ra loa chủ yếu là bộ đệm I2S: khoảng 3 khung (30 ms) lúc bình thường, dao động trong khoảng 1–5 khung theo cơ chế bù clock.

### 11.5 Nhấn Vol+

```
nút → gpio-keys → on_input (thread input) → media_ctrl_volume_up
  → bt_vcp_vol_rend_unmute_vol_up → (Zephyr) volume += 16, bỏ mute, báo điện thoại
  → vcs_state_cb → audio_out_set_volume → gain_q15 (atomic)
  → khung kế tiếp trong flush_frame dùng gain mới
log: "helmet: Button Vol+", "media_ctrl: Volume 144/255 (56%)"
```

### 11.6 Nhấn Call

```
on_input → media_ctrl_play_pause → đang phát? → send_media_cmd(PAUSE hoặc PLAY)
  → GATT write vào Media Control Point của điện thoại → trình phát nhạc dừng/chạy
  → điện thoại thông báo trạng thái mới → mcc_read_media_state_cb cập nhật media_state
  → (nếu dừng) điện thoại Disable/Release ASE → stream_stopped → audio_out dừng I2S
```

### 11.7 Ngắt kết nối

```
disconnected(): log lý do, bỏ active_conn, media_ctrl_disconnected
stream_stopped / stream_released (mỗi luồng): audio_out_stream_stop → handle_stop → dừng I2S
recycled(): k_work_submit(adv_work) → quảng bá lại
```

### 11.8 Nghe lại trên PC

```
make audio → helmet_audio.py → J-Link → gửi "helmet capture on" vào shell RTT
→ cmd_capture → capture_set_enabled(true) → resend_cfg = 1
→ SDU kế tiếp: capture_sdu gửi CFG rồi SDU vào RTT kênh 1
→ PC: RecordParser → Mixer (giải mã LC3) → WAV + loa máy tính
```

---

## 12. Đọc log và lệnh shell

### 12.1 Dòng heartbeat (mỗi 10 s, `main.c:535`)

```
heartbeat: connected, STREAMING, SDU 5230 (bad 3), PLC 3, I2S on 48000 Hz q3, sync -8/+0, err 0
```

| Trường | Biến | Nguồn | Bình thường |
|---|---|---|---|
| `connected` / `advertising` | `active_conn` | `main.c` | |
| `STREAMING` / `idle` | `le_audio_is_streaming()` | `le_audio.c` | |
| `SDU` | `stats.sdu_rx` | `handle_data` | tăng 100/s mỗi luồng |
| `bad` | `stats.sdu_bad` | SDU không dùng được | < 1 % |
| `PLC` | `stats.plc` | số **kênh-khung** được nội suy (stereo mất 1 SDU = 2) | tăng chậm |
| `I2S on … Hz` | `i2s_running`, `out_rate` | `audio_out.c` (tần số **yêu cầu**, không phải tần số thật) | |
| `q3` | `blocks_in_use()` | mực nước bộ đệm I2S | 2–4 |
| `sync -x/+y` | `drift_drop` / `drift_repeat` | số khung bỏ/lặp để bù clock | hiện tăng 8 mỗi 10 s do lỗi 47 619 Hz |
| `err` | `i2s_err + dec_err + q_full` | lỗi I2S, lỗi giải mã, hàng đợi đầy | 0 |

### 12.2 Dòng `link:` và `ISO LQ`

```
link: ACL interval 50000 us, PHY tx 2M / rx 2M; ISO rx ok 5230, CRC 0, lost 3, empty 0
ISO LQ ASE 0 (goi vo tuyen): CRC error 1, unreceived 3, duplicate 0
```

- `ok/CRC/lost/empty` là `rx_flags` (mức SDU, cộng dồn từ lúc khởi động, mục 6.10).
- `ISO LQ` là bộ đếm của controller (mức gói vô tuyến, tính từ khi CIS được tạo, mục 6.8).
- Cách đọc nhanh:
  - `unreceived` tăng theo mọi khung và `CRC error` ≈ 0: Helmet không thấy gói nào, hoặc chỉ thấy gói rỗng. Vấn đề ở phía phát.
  - `CRC error` tăng: sóng yếu hoặc nhiễu.

### 12.3 `helmet status`

In theo thứ tự:
1. Kết nối và mức bảo mật (`main.c`).
2. Âm lượng và trạng thái MCP (`media_ctrl.c`).
3. Từng ASE, thông số CIS, ISO LQ, bộ đếm cờ, QoS ưa thích (`le_audio.c`).
4. Thống kê I2S và giải mã (`audio_out.c`).

---

## 13. Tự quan sát bằng debugger

Cách nhanh nhất để thật sự hiểu mã là cho chip dừng ở từng chỗ và xem biến. Chạy `make debug` (GDB qua J-Link), hoặc dùng debugger của VS Code.

| Đặt breakpoint tại | Khi nào dừng | Nên xem |
|---|---|---|
| `connected` (`main.c:122`) | Điện thoại vừa kết nối | `conn`, call stack (thấy thread `bt_rx`) |
| `lc3_config` (`le_audio.c:123`) | Điện thoại cấu hình codec | `*codec_cfg` (chuỗi LTV thô), sau `parse_codec_cfg` xem `cfg` |
| `stream_started` (`le_audio.c:374`) | Luồng bắt đầu | `s->cfg` |
| `handle_start` (`audio_out.c:279`) | Thread audio nhận lệnh bắt đầu | `samples`, `d->out_mask` |
| `handle_data` (`audio_out.c:335`) | Mỗi SDU | `m->ts`, `m->valid`, `frame_mask`, `expected_mask` |
| `flush_frame` (`audio_out.c:218`) | Mỗi khung ra loa | `gain`, vài mẫu `frame_pcm[0..7]` |
| `i2s_submit`, nhánh `drift_drop` (`audio_out.c:174`) | Khi bỏ khung bù clock | `used` |
| `vcs_state_cb` (`media_ctrl.c:40`) | Đổi âm lượng | `conn` (`NULL` = do nút trên mũ, khác `NULL` = do điện thoại) |

Lưu ý: dừng CPU khi đang phát làm CIS mất gói và có thể làm điện thoại ngắt kết nối. Chỉ dùng breakpoint để hiểu luồng chạy. Muốn đo hiệu năng thì dùng log và heartbeat.

Các lệnh shell hữu ích khi tìm hiểu:
- `kernel threads`: các thread và ưu tiên;
- `kernel stacks`: mức dùng stack của từng thread (xem `audio_tid` còn dư bao nhiêu).

---

## 14. Những điểm cần nhớ khi sửa mã

1. **Không làm việc nặng hoặc chờ lâu trong callback Bluetooth.** Đẩy sang workqueue (`k_work`) hoặc thread riêng như `audio_out.c` đã làm.
2. **`audio_out_stream_data` dùng biến `static`.** Chỉ an toàn khi một thread duy nhất gọi (hiện là thread Bluetooth). Nếu sau này có nguồn dữ liệu khác (ví dụ âm báo), phải thêm hàm riêng hoặc khóa.
3. **`stats` không được bảo vệ.** Hai thread cùng tăng vài bộ đếm (`len_err`, `q_full`). Chấp nhận được vì chỉ là thống kê; đừng dùng chúng cho logic điều khiển.
4. **`out_rate` là tần số yêu cầu.** Tần số thật của I2S là 47 619 Hz cho 48 kHz (mục 7.5). Đây là lỗi cần sửa bằng lấy mẫu lại.
5. **Số luồng tối đa** đi theo `CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT`. Tăng giá trị này thì `ds[]` (36 KB cho 2 luồng) tăng theo.
6. **`lc3_start` không chạy với ASE sink.** Mọi việc "bắt đầu phát" phải đặt trong `stream_started`.
7. **Thêm file `.c`**: thêm vào `CMakeLists.txt`, chạy `make build` để IntelliSense nhận file mới.
8. **Thêm lệnh shell**: nhớ số tham số trong `SHELL_CMD_ARG` tính cả tên lệnh.
9. **Đổi ngữ cảnh hoặc vị trí loa**: sửa `helmet.h`. Cả PACS lẫn gói quảng bá lấy từ cùng hằng số nên tự khớp nhau.
10. **Sau khi đổi `prj.conf` hoặc devicetree**: build lại và xem `build/Helmet/Helmet/zephyr/.config` để biết giá trị cuối cùng Zephyr chọn.
