# ME54BS62 (nRF54L15): môi trường phát triển

Project firmware cho **Helmet board**: mũ có 2 loa (MAX98357A, I2S), 2 micro (IM69D130, PDM), cảm biến nhiệt hồng ngoại MLX90632, 4 nút và LED Bluetooth, dùng module **Minew ME54BS62** (SoC Nordic nRF54L15 WLCSP, anten PCB). Phần mềm dùng **nRF Connect SDK v3.4.0** (Zephyr 4.4). Firmware được nạp và debug qua **J-Link (SWD)**.

Tài liệu thiết kế đầy đủ (yêu cầu, kiến trúc, luồng dữ liệu, đặc tả module, giám sát, triển khai, kiểm thử): **[docs/TAI_LIEU_THIET_KE.md](docs/TAI_LIEU_THIET_KE.md)**.

## Cấu trúc

```
.
├── env.sh                      # source để có west, GCC, nrfutil, cmake của NCS v3.4.0
├── Makefile                    # make build / flash / rtt / audio / uart / menuconfig / ... (APP mặc định: apps/main)
├── boards/minew/me54bs62/      # board definition: module + phần cứng Helmet board (target: me54bs62/nrf54l15/cpuapp)
├── lib/                        # mã DÙNG CHUNG của các app, chia theo lớp (là một Zephyr module)
│   ├── core/                   #   lớp 1b: thuật toán thuần C (âm báo, lấy mẫu lại, DSP giọng nói, công thức MLX90632...)
│   ├── hal/                    #   lớp 1a: phần cứng của board (I2S, micro PDM, nút, LED, nguồn, MLX90632)
│   ├── services/               #   lớp 2: dịch vụ có thread (đường ra loa, chiều lên cuộc gọi, cử chỉ nút, phát âm báo)
│   └── proto/                  #   chỉ header: giao thức giữa các thiết bị (mũ ↔ xe, mũ ↔ mũ)
├── apps/main/                  # FIRMWARE MŨ (app mặc định của make), xem apps/main/README.md
│   └── src/                    #   app/ (lớp 4) · features/{phone,intercom,broadcast,thermo}/ (lớp 3) · debug/
├── apps/bike/                  # FIRMWARE XE (bike hub), xem docs/BIKE_HUB_THIET_KE.md
│   └── src/                    #   app/ (lớp 4) · features/{phone,broadcast}/ (lớp 3)
├── scripts/rtt.sh              # mở terminal RTT (log + shell) chỉ qua dây SWD
├── scripts/helmet_audio.py     # nghe/lưu WAV âm thanh mũ nhận được, qua J-Link (make audio)
├── backup/                     # firmware gốc đọc ra từ chip trước lần nạp đầu tiên
├── docs/                       # tài liệu thiết kế, datasheet, schematic, PINOUT.md
├── tools/                      # udev rule chặn ModemManager
└── .vscode/                    # cấu hình IntelliSense và các task
```

## Kiến trúc mã nguồn

Phần trên (riêng từng app) chia theo **chức năng**; phần dưới (dùng chung, `lib/`) chia theo **lớp**:

| Lớp | Thư mục | Nội dung | Được làm / không được làm |
|---|---|---|---|
| 4. Ứng dụng | `apps/<app>/src/app/` | Máy trạng thái, khởi động BT, quảng bá, kết nối, shell, quy trình tắt máy, cử chỉ → hành động | Gọi được mọi lớp dưới |
| 3. Chức năng | `apps/<app>/src/features/<tên>/` | Mũ: `phone`, `intercom`, `broadcast`, `thermo`. Xe: `phone`, `broadcast` | Báo lên ứng dụng chỉ qua `app/app.h` (`app_post(EVT_*)`, `app_state()`, cài đặt) |
| 2. Dịch vụ | `lib/services/` | `audio_out`, `voice_tx`, `buttons`, `audio_stream`, `prompt_player` | Có thread / hàng đợi; ghép HAL + core; không biết chức năng nào đang dùng mình (báo lên bằng callback đăng ký lúc khởi động) |
| 1a. HAL | `lib/hal/` | `i2s_out`, `mic_in`, `keys`, `status_led`, `power_hw`, `mlx90632` | Nơi duy nhất gọi driver Zephyr / nrfx của board; không có khái niệm nghiệp vụ |
| 1b. Core | `lib/core/` | `prompts`, `pcm_ring`, `resampler`, `mixer`, `voice_dsp`, `mlx90632_calc`, `lc3_silence` | **C thuần, không include `<zephyr/...>`**: biên dịch và thử được trên PC |
| Giao thức | `lib/proto/` | `bhs_proto.h` (mũ ↔ xe), `his_proto.h` (mũ ↔ mũ) | Chỉ header; sửa ở đây là sửa cho cả hai firmware |
| 0. Nền tảng | `boards/`, NCS / Zephyr | Devicetree, kernel, BT host + SoftDevice Controller | Không phải code của dự án |

Quy tắc:
- **Chỉ gọi xuống.** Đi lên bằng sự kiện (`app_post`) hoặc callback đã đăng ký (vd `buttons_init(&cb)`, `voice_set_mic_unavailable_cb()`, `keys_init(cb)`).
- **CMake chặn include sai lớp:** mỗi lớp của `lib/` là một thư viện riêng, chỉ thấy header của chính nó và của lớp nó được dùng (`core` → chỉ core; `hal` → hal + core; `services` → services + hal + core + proto). Đường include ghi rõ lớp: `#include <me54/hal/i2s_out.h>`, `<me54/core/prompts.h>`, `<me54/services/audio_out.h>`, `<me54/proto/bhs_proto.h>`. Trong app, include theo đường dẫn từ `src/`: `"app/app.h"`, `"features/phone/le_audio.h"`.
- **Mỗi module của `lib/` được bật bằng Kconfig** trong `prj.conf` của app (menu *Modules → ME54BS62 shared library*). Module lớp trên tự chọn các module lớp dưới nó dùng. Ví dụ mũ bật `CONFIG_ME54_SVC_AUDIO_OUT`, xe bật `CONFIG_ME54_SVC_PROMPT_PLAYER`. App nạp `lib/` bằng `list(APPEND ZEPHYR_EXTRA_MODULES ...)` trong `CMakeLists.txt` (trước `find_package(Zephyr)`).
- **Các chức năng cùng lớp hiện vẫn gọi nhau** để phân xử ưu tiên loa / radio: `le_audio` ↔ `broadcast_sink` ↔ `intercom` ở mũ, `phone_sink` → `bcast_source` ở xe. Muốn bỏ thì gom luật ưu tiên về một chỗ ở lớp ứng dụng (vd `app/audio_policy.c`); việc này đổi hành vi nên phải thử lại trên mạch.

## Môi trường đã cài và kiểm tra## Môi trường đã cài và kiểm tra

| Thành phần | Phiên bản | Vị trí |
|---|---|---|
| nRF Connect SDK | v3.4.0 (Zephyr 4.4.0) | `~/ncs/v3.4.0` |
| Toolchain | Zephyr SDK 1.0.1, GCC 14.3, CMake 4.2, west 1.5 | `~/ncs/toolchains/fbf7391cab` |
| nrfutil + device | 8.2.1 / 2.22 (bản trong toolchain: 8.1.1 / 2.19) | `~/.local/bin/nrfutil` |
| SEGGER J-Link | V9.24a | `/opt/SEGGER/JLink` |
| VS Code | nRF Connect Extension Pack | |

## Bắt đầu nhanh

```bash
cd ~/Project/Bluetooth_ME54BS62

make build        # build firmware mũ apps/main cho me54bs62/nrf54l15/cpuapp -> build/main
make flash        # nạp qua J-Link
make rtt          # xem log và gõ lệnh shell qua RTT (Ctrl+C để thoát)
make audio        # (thay cho make rtt) nghe + lưu WAV âm thanh LE Audio mà mũ nhận được
```

Các lệnh khác: chạy `make help`. Có thể đổi app hoặc chọn probe bằng biến:

```bash
make APP=apps/bike build flash      # firmware xe (bike hub) -> build/bike
make APP=apps/bike rtt              # mọi lệnh khác cũng phải kèm APP=apps/bike
make JLINK_SN=69750999 flash      # khi cắm nhiều J-Link (xem số serial: make devices)
make EXTRA="-DCONFIG_LOG_DEFAULT_LEVEL=4" pristine
```

Nếu muốn dùng `west` trực tiếp:

```bash
source ./env.sh
west build -b me54bs62/nrf54l15/cpuapp -d build/main apps/main -- -DBOARD_ROOT=$PWD
west flash -d build/main
```

`-DBOARD_ROOT=$PWD` là bắt buộc. Nếu thiếu, west sẽ không tìm thấy board `me54bs62`.

## Firmware

| App | Vai trò | Tài liệu |
|---|---|---|
| `apps/main` | Mũ: nghe nhạc + cuộc gọi LE Audio, intercom mũ–mũ, nghe Auracast / vào xe, cảm biến nhiệt, nút, nguồn | [apps/main/README.md](apps/main/README.md), [docs/HELMET_THIET_KE_CHUC_NANG_MOI.md](docs/HELMET_THIET_KE_CHUC_NANG_MOI.md), [docs/HELMET_THIET_KE_INTERCOM.md](docs/HELMET_THIET_KE_INTERCOM.md), [docs/HELMET_AURACAST.md](docs/HELMET_AURACAST.md) |
| `apps/bike` | Xe (bike hub): nhận nhạc LE Audio từ điện thoại, phát Auracast cho mọi mũ, ghép mũ bằng nút | [docs/BIKE_HUB_THIET_KE.md](docs/BIKE_HUB_THIET_KE.md), [docs/BROADCAST_VAN_DE_VA_GIAI_PHAP.md](docs/BROADCAST_VAN_DE_VA_GIAI_PHAP.md) |

Firmware cũ `apps/Helmet` (bản đầu của mũ) và firmware kiểm tra phần cứng `apps/hw_test` đã được gỡ khỏi cây mã ngày 27/10/2026. Khi cần (vd bring-up board mới), lấy lại từ lịch sử git: `git show a1ab029:apps/hw_test/src/main.c`.

## Dùng trong VS Code

1. Mở thư mục này trong VS Code.
2. Chạy `make build` (mũ) và `make APP=apps/bike build` (xe) ít nhất một lần. Lệnh này tạo `compile_commands.json` trong thư mục build, file mà IntelliSense cần (xem phần dưới). File trong `lib/` lấy lệnh biên dịch từ bản build của app dùng nó.
3. Dùng **Terminal → Run Task**: `ME54: Build`, `ME54: Flash (J-Link)`, `ME54: RTT log/shell`, `ME54: Audio capture (helmet)`, `ME54: Build + Flash bike hub`... Hoặc dùng các nút của extension nRF Connect: vào *Applications* → `main` → **Add build configuration**, chọn SDK/Toolchain **v3.4.0** và board `me54bs62/nrf54l15/cpuapp` trong mục *Custom boards*.

### IntelliSense: vì sao trước đây báo lỗi đỏ, và cách đã sửa

**Vì sao báo lỗi**: Zephyr không phải thư viện C thông thường có sẵn một thư mục include cố định. Để editor hiểu được một file nguồn, nó cần chính xác những gì trình biên dịch nhận được lúc build:
- **Khoảng 50 thư mục include** rải khắp `~/ncs/v3.4.0` (zephyr/include, các module hal_nordic, nrfxlib, bluetooth...). Danh sách này phụ thuộc vào các tính năng bật trong `prj.conf`.
- **Các header được sinh ra lúc build**, nằm trong thư mục build chứ không có trong SDK:
  - `autoconf.h`: chứa mọi `CONFIG_*` lấy từ Kconfig/prj.conf.
  - `devicetree_generated.h`: sinh từ file `.dts` của board, là nơi định nghĩa các macro `DT_ALIAS(led0)`, `DT_NODELABEL(i2s20)`...
  - Các header syscall.

  Các header này được nạp ngầm bằng cờ `-imacros autoconf.h`.
- **Trình biên dịch ARM** `arm-zephyr-eabi-gcc` cùng các define của chip (`NRF54L15_XXAA`, `NRF_APPLICATION`...), thay vì gcc của máy PC.

Extension C/C++ của VS Code đang được đặt để hỏi các thông tin trên từ **extension nRF Connect** (`configurationProvider`). Nhưng nRF Connect chỉ biết những bản build do chính nó tạo ra. Ta build bằng `make`/`west` ra `build/<app>`, nên nRF Connect không có gì để trả lời. Kết quả là editor không tìm thấy `zephyr/kernel.h`, và báo lỗi đỏ ở mọi `#include`, `LOG_MODULE_REGISTER`, `SHELL_CMD_REGISTER`... Code vẫn build bình thường; lỗi chỉ nằm ở phía editor.

**Cách đã sửa**:
- CMake luôn xuất file `compile_commands.json`, ghi lại *chính xác lệnh biên dịch của từng file .c*: compiler, mọi `-I`, `-D`, `-imacros`.
- [.vscode/c_cpp_properties.json](.vscode/c_cpp_properties.json) chỉ cho extension C/C++ dùng **cùng lúc** file của cả hai app: `build/main/main/compile_commands.json` và `build/bike/bike/compile_commands.json`, không hỏi nRF Connect nữa. Mỗi file `.c` (kể cả file trong `lib/`) lấy đúng lệnh biên dịch của bản build chứa nó, mở file nào cũng đúng mà không phụ thuộc lần build gần nhất.
- File `.c` **mới** chưa build lần nào thì chưa có trong `compile_commands.json`. Khi đó IntelliSense dùng danh sách dự phòng (`includePath`/`defines` trong cùng file cấu hình: các thư mục include chính của Zephyr, nrfx, CMSIS, liblc3, SEGGER RTT, SoftDevice Controller), nên `#include <zephyr/...>` vẫn tìm thấy. Build lại để có đủ `CONFIG_*` của app.
- `make build` / `make pristine` vẫn tạo symlink `build/compile_commands.json` trỏ tới app vừa build, cho công cụ khác (clangd, ...).

Lưu ý:
- Sau khi đổi `prj.conf` hoặc devicetree, chạy lại `make build` để IntelliSense cập nhật các `CONFIG_*`/`DT_*` mới.
- Kiểm tra nhanh: mở *Output* → **C/C++ Configuration Warnings**. Dòng `"x.c" not found in compile_commands.json files` nghĩa là app chứa `x.c` chưa được build hoặc chưa có trong danh sách `compileCommands`: file đó đang dùng danh sách dự phòng, thiếu `CONFIG_*` nên bị gạch đỏ.
- Nếu một ngày IntelliSense lại báo lỗi: chạy `make build`, rồi chọn *Command Palette* → **C/C++: Reset IntelliSense Database** (hoặc **Developer: Reload Window**).
- Gợi ý mờ (Hint) kiểu *"Function definition for 'k_sleep' not found"* không phải lỗi: phần thân các hàm đó nằm trong mã nguồn Zephyr, ngoài thư mục project.
- Nếu muốn dùng các bản build của extension nRF Connect thay cho `make`: xóa file `.vscode/c_cpp_properties.json`, khi đó nRF Connect sẽ lo IntelliSense cho build configuration của nó.

## Nối dây

Xem [docs/PINOUT.md](docs/PINOUT.md). Header "SW debugger" trên Helmet board có VTREF, SWDIO, SWCLK, RESET; nối thêm GND về J-Link. Mạch phải được cấp nguồn từ pin hoặc USB-C.

**Trước khi làm PCB bản sau, đọc mục "⚠ Các điểm cần xem lại" trong [docs/PINOUT.md](docs/PINOUT.md)**, đặc biệt là I2C của MLX90632 đang bị đặt ở hai miền nguồn khác nhau.

## Firmware gốc đã sao lưu

Trước lần nạp đầu tiên, chip đang chứa một firmware khoảng 52 KB, build bằng NCS v3.0.2 (có thể là firmware test RF của nhà sản xuất). Thư mục `backup/` có:

- `rram_factory_0x0_1524K.bin`: toàn bộ RRAM
- `uicr_factory_0xFFD000.bin`: UICR (có 19 word đã được ghi từ xưởng)
- `factory_fw_0x0-0xD000.hex`: bản để nạp lại

Nạp lại bằng `make restore-factory`. Luồng này đã được thử: sau khi khôi phục, RRAM và UICR khớp từng byte với bản backup.

## Lưu ý và xử lý sự cố

- **"RTT Control Block not found"** khi dùng `JLinkRTTLogger`, `west rtt` hoặc RTT Viewer ở chế độ Auto: với probe J-Link hiện tại, chức năng tự quét RTT trên nRF54L15 không hoạt động. Hãy dùng `make rtt`, vì script này lấy địa chỉ `_SEGGER_RTT` từ file ELF. Với J-Link RTT Viewer, chọn *Address* rồi nhập địa chỉ do `make rtt` in ra.
- **`make erase-flash` (`west flash --erase`) và `make recover` xóa cả UICR.** `make flash` thông thường chỉ xóa vùng firmware ghi đè, UICR giữ nguyên (đã kiểm tra).
- **Chip bị khóa (APPROTECT)**, ví dụ nrfutil báo "access port protection": chạy `make recover`. Lệnh này xóa toàn bộ chip rồi mở khóa.
- **Nguyên nhân reset `SOFTWARE|LOCKUP` sau mỗi lần nạp** do quy trình lập trình của probe gây ra, không phải lỗi firmware. Kết quả đo: `make reset` (reset qua SWD) cho ra `SOFTWARE` (0x2). Nhấn reset (C2) hoặc cấp nguồn lại sẽ cho `PIN` hoặc `POWER-ON`.
- **Nạp xong mà firmware không chạy** (CPU đứng ở `__start`): runner nrfutil mặc định reset bằng chân nRESET sau khi nạp. Nếu dây RESET sai hoặc lỏng thì chip không được reset và cứ đứng ở đó. [board.cmake](boards/minew/me54bs62/board.cmake) đặt `--softreset` (reset qua SWD) để việc nạp không phụ thuộc vào dây này. Từ 13/10/2026 dây RESET đã đúng và cả hai cách reset đều đã được kiểm tra. Muốn dùng reset chân giống mặc định của Nordic thì xóa dòng `board_runner_args(nrfutil "--softreset")`.
- **"There already is an active connection"** khi chạy `make rtt`: đã có một phiên `make rtt` khác đang mở (có thể ở terminal khác). Script giờ sẽ báo điều này và in PID; đóng phiên cũ bằng Ctrl+C rồi chạy lại. Hai phiên RTT cùng lúc sẽ giành log của nhau.
- **Mất kết nối SWD** (`Could not connect to the target device`, hoặc nrfutil báo `Setting the debug port SELECT register failed`): đây là lỗi ở mức tín hiệu điện, firmware không thể gây ra (APPROTECT đang tắt). Kiểm tra theo thứ tự:
  1. Rút hết nguồn của mạch (pin và USB-C) khoảng 5 s rồi cắm lại.
  2. Kiểm tra 5 dây theo sơ đồ đầu cắm trong [docs/PINOUT.md](docs/PINOUT.md): VTref → chân 1, **SWDIO → chân 7**, **SWCLK → chân 9**, **RESET → chân 15** (chân 14 và 16 bên cạnh là GND, rất dễ cắm nhầm), GND → chân 4.
  3. Đo điện áp tại pad RST của module khi J-Link không chạy: phải khoảng 3.3 V (có R20 10k kéo lên). Nếu bằng 0 V là dây RESET đang chạm GND hoặc nút SW1 bị kẹt.
  4. Quan sát LED Bluetooth: nhấp nháy 1 Hz nghĩa là firmware vẫn chạy và chỉ có đường SWD bị hỏng; tắt hẳn nghĩa là chip đang bị giữ reset hoặc mất nguồn.
  5. Chạy `make info`. Kết nối được thì sẽ in ra `deviceVersion: NRF54L15_xxAA_REV2`.
- **BLE kết nối xong bị ngắt ngay, `reason 0x05` (Authentication Failure)**: Ubuntu (BlueZ), Android và iOS thường **yêu cầu ghép đôi ngay khi kết nối**. Firmware không bật `CONFIG_BT_SMP` thì ghép đôi thất bại và máy kia tự ngắt sau khoảng 0.3 s. Cả hai firmware đều bật SMP (Just Works) và lưu bond bằng settings + ZMS vào phân vùng `storage`. Các bước sau đã được thử với Bluetooth của laptop: ghép đôi, reset chip, kết nối lại có mã hóa luôn mà không cần ghép đôi lại.
  - Khi ghép đôi bằng `bluetoothctl`, phải đăng ký agent trước: `agent NoInputNoOutput`, rồi `default-agent`, rồi `pair D3:EE:24:55:47:E9`. Chạy riêng lệnh `bluetoothctl pair ...` không có agent sẽ báo `AuthenticationFailed`.
  - Nếu xóa bond ở một phía (`helmet unpair` / `bike unpair` trên chip, hoặc *Forget/Remove* trên máy tính/điện thoại), phải xóa luôn ở phía kia rồi mới ghép đôi lại. Nếu không sẽ gặp lỗi khóa (`PIN or Key Missing`).
- **ModemManager** đang chạy trên máy và có thể chiếm `/dev/ttyACM*` của J-Link VCOM. Cài rule chặn nó:
  ```bash
  sudo cp tools/71-nrf-jlink-modemmanager-ignore.rules /etc/udev/rules.d/
  sudo udevadm control --reload-rules && sudo udevadm trigger
  ```
- **Tụ tải thạch anh** (`&lfxo`, `&hfxo` trong file dts của board) đang dùng giá trị của nRF54L15 DK. Nên hỏi Minew thông số CL của Y1/Y2 để chỉnh cho chính xác. Sai lệch nhỏ chỉ làm lệch tần số vài ppm.
- **Cảnh báo `Deprecated symbol NRF_PLATFORM_LUMOS`** khi build đến từ chính NCS 3.4.0 và xuất hiện với mọi board nRF54L, có thể bỏ qua.
- **Tạo app mới**: copy `apps/bike` thành `apps/<ten_app>` (giữ phần nạp `lib/` trong `CMakeLists.txt`: `list(APPEND ZEPHYR_EXTRA_MODULES ...)` + `target_include_directories`), sửa `prj.conf` (bật các module `CONFIG_ME54_*` cần dùng) và `src/`, rồi chạy `make APP=apps/<ten_app> build flash`. Thêm `${workspaceFolder}/build/<ten_app>/<ten_app>/compile_commands.json` vào danh sách `compileCommands` trong [.vscode/c_cpp_properties.json](.vscode/c_cpp_properties.json) để IntelliSense hiểu app mới. Nếu cần đổi chân cho riêng app, thêm file `apps/<ten_app>/boards/me54bs62_nrf54l15_cpuapp.overlay`.
# Helmet_ble6
