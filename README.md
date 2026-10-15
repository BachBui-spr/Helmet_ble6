# ME54BS62 (nRF54L15): môi trường phát triển

Project firmware cho **Helmet board**: mũ có 2 loa (MAX98357A, I2S), 2 micro (IM69D130, PDM), cảm biến nhiệt hồng ngoại MLX90632, 4 nút và LED Bluetooth, dùng module **Minew ME54BS62** (SoC Nordic nRF54L15 WLCSP, anten PCB). Phần mềm dùng **nRF Connect SDK v3.4.0** (Zephyr 4.4). Firmware được nạp và debug qua **J-Link (SWD)**.

Tài liệu thiết kế đầy đủ (yêu cầu, kiến trúc, luồng dữ liệu, đặc tả module, giám sát, triển khai, kiểm thử): **[docs/TAI_LIEU_THIET_KE.md](docs/TAI_LIEU_THIET_KE.md)**.

## Cấu trúc

```
.
├── env.sh                      # source để có west, GCC, nrfutil, cmake của NCS v3.4.0
├── Makefile                    # make build / flash / rtt / audio / uart / menuconfig / ...
├── boards/minew/me54bs62/      # board definition: module + phần cứng Helmet board (target: me54bs62/nrf54l15/cpuapp)
├── apps/Helmet/                # FIRMWARE CHÍNH: tai nghe LE Audio (xem apps/Helmet/README.md) - app mặc định của make
├── apps/hw_test/               # firmware kiểm tra phần cứng (LED, nút, I2C, loa, micro, BLE)
├── scripts/rtt.sh              # mở terminal RTT (log + shell) chỉ qua dây SWD
├── scripts/helmet_audio.py     # nghe/lưu WAV âm thanh Helmet nhận được, qua J-Link (make audio)
├── backup/                     # firmware gốc đọc ra từ chip trước lần nạp đầu tiên
├── docs/                       # datasheet, schematic, PINOUT.md, TAI_LIEU_THIET_KE.md (tài liệu thiết kế)
├── tools/                      # udev rule chặn ModemManager
└── .vscode/                    # cấu hình cho extension nRF Connect và các task
```

## Môi trường đã cài và kiểm tra

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

make build        # build firmware chính apps/Helmet cho me54bs62/nrf54l15/cpuapp -> build/Helmet
make flash        # nạp qua J-Link
make rtt          # xem log và gõ lệnh shell qua RTT (Ctrl+C để thoát)
make audio        # (thay cho make rtt) nghe + lưu WAV âm thanh LE Audio mà Helmet nhận được
```

Các lệnh khác: chạy `make help`. Có thể đổi app hoặc chọn probe bằng biến:

```bash
make APP=apps/hw_test build flash   # firmware kiểm tra phần cứng -> build/hw_test
make APP=apps/hw_test rtt           # mọi lệnh khác cũng phải kèm APP=apps/hw_test
make JLINK_SN=69750999 flash      # khi cắm nhiều J-Link (xem số serial: make devices)
make EXTRA="-DCONFIG_LOG_DEFAULT_LEVEL=4" pristine
```

Nếu muốn dùng `west` trực tiếp:

```bash
source ./env.sh
west build -b me54bs62/nrf54l15/cpuapp -d build/Helmet apps/Helmet -- -DBOARD_ROOT=$PWD
west flash -d build/Helmet
```

`-DBOARD_ROOT=$PWD` là bắt buộc. Nếu thiếu, west sẽ không tìm thấy board `me54bs62`.

## Firmware chính `apps/Helmet`

Tai nghe **LE Audio** (BAP Unicast Server, VCP, MCP; giải mã LC3 → I2S → 2×MAX98357A). Cách ghép đôi, lệnh shell `helmet …` và cách nghe lại âm thanh qua J-Link: xem [apps/Helmet/README.md](apps/Helmet/README.md).

## Firmware kiểm tra `apps/hw_test`

Firmware này dùng để kiểm tra từng khối của **Helmet board** (build: `make APP=apps/hw_test build flash`). Sơ đồ chân xem tại [docs/PINOUT.md](docs/PINOUT.md).

| Khối | Tự động khi khởi động | Kiểm tra bằng lệnh |
|---|---|---|
| Chip | in part, variant, package, Device ID, xung CPU, nguyên nhân reset | `hw info` |
| **LED Bluetooth** (P2.07) | nhấp nháy 1 Hz (đồng thời là xung vuông để đo bằng scope) | `hw blink <ms>`; `hw led on\|off` để giữ một mức cố định khi đo bằng đồng hồ |
| **4 nút** Vol+/Vol−/Call/Power (P0.02/P0.01/P0.00/P0.03) | log `Button Vol Up PRESSED/released` | nhấn nút |
| **MLX90632** (I2C bit-bang, 0x3A) | đọc EE_VERSION | `hw mlx`, `i2c scan i2c-sensor` |
| **Loa** 2×MAX98357A (I2S20) | | `hw beep [hz] [ms]`, ví dụ `hw beep 1000 500` |
| **Micro** 2×IM69D130 (PDM20) | | `hw mic [ms]`: in DC, AC-RMS (dBFS), min/max cho từng kênh L/R và số mẫu L≠R |
| **BLE / anten** | quảng bá `ME54BS62-Test` (connectable), hỗ trợ ghép đôi Just Works, khóa lưu trong RRAM | quét/kết nối bằng app **nRF Connect** hoặc Bluetooth của Ubuntu; `hw unpair` xóa mọi bond |
| Nhiệt độ die | heartbeat mỗi 5 s | `hw temp` |

Lệnh shell khác (chạy đồng thời trên RTT và UART20):

| Lệnh | Tác dụng |
|---|---|
| `gpio conf gpio1 14 o` rồi `gpio set gpio1 14 1` | điều khiển một chân bất kỳ (ví dụ P1.14) để dò tín hiệu |
| `gpio get gpio0 2` | đọc mức logic của một chân (ví dụ nút Vol Up) |
| `i2c scan i2c-sensor` | quét bus I2C |
| `kernel threads`, `device list` | trạng thái hệ thống |

Tên thiết bị GPIO trong shell: `gpio0` = P0, `gpio1` = P1, `gpio2` = P2.

Kết quả đo trên mạch ngày 13/10/2026:

```
hw_test: SoC part  : 0x54b15  variant: AAC0  package: 0x4341      <- "CA" = WLCSP CAAA
hw_test: CPU clock : 128000000 Hz (sys timer 1000000 Hz)
hw_test: led0 -> gpio@50400 pin 7 (toggle every 500 ms)          <- LED Bluetooth P2.07: OK
hw_test: MLX90632 @0x3a: NO RESPONSE (-5)                         <- xem docs/PINOUT.md, muc 3
bt_hci_core: HW Variant: nRF54Lx (0x0005)
hw_test: BLE advertising as "ME54BS62-Test"                       <- BLE controller: OK
Mic L: DC -4255.3  AC-RMS 4310.3 (-17.6 dBFS) ...
L != R in 0/16000 samples  -> L/R IDENTICAL                       <- xem docs/PINOUT.md, muc 4
Beep 1000 Hz, 500 ms -> I2S BCLK P1.12 / LRCLK P1.10 / DIN P1.15  <- I2S chay khong loi, can nghe thu
```

## Dùng trong VS Code

1. Mở thư mục này trong VS Code.
2. Chạy `make build` ít nhất một lần (và `make APP=apps/main build`, `make APP=apps/hw_test build` nếu cần sửa hai app đó). Lệnh này tạo `compile_commands.json` trong thư mục build, file mà IntelliSense cần (xem phần dưới).
3. Dùng **Terminal → Run Task**: `ME54: Build`, `ME54: Flash (J-Link)`, `ME54: RTT log/shell`, `ME54: Audio capture (Helmet)`, `ME54: Build + Flash hw_test`... Hoặc dùng các nút của extension nRF Connect: vào *Applications* → `Helmet` → **Add build configuration**, chọn SDK/Toolchain **v3.4.0** và board `me54bs62/nrf54l15/cpuapp` trong mục *Custom boards*.

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
- [.vscode/c_cpp_properties.json](.vscode/c_cpp_properties.json) chỉ cho extension C/C++ dùng **cùng lúc** file của cả ba app: `build/main/main/compile_commands.json`, `build/Helmet/Helmet/compile_commands.json` và `build/hw_test/hw_test/compile_commands.json`, không hỏi nRF Connect nữa. Mỗi file `.c` lấy đúng lệnh biên dịch của app chứa nó, mở file của app nào cũng đúng mà không phụ thuộc lần build gần nhất.
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
- **BLE kết nối xong bị ngắt ngay, `reason 0x05` (Authentication Failure)**: Ubuntu (BlueZ), Android và iOS thường **yêu cầu ghép đôi ngay khi kết nối**. Firmware không bật `CONFIG_BT_SMP` thì ghép đôi thất bại và máy kia tự ngắt sau khoảng 0.3 s. `hw_test` hiện đã bật SMP (Just Works) và lưu bond bằng settings + ZMS vào phân vùng `storage`. Các bước sau đã được thử với Bluetooth của laptop: ghép đôi, reset chip, kết nối lại có mã hóa luôn mà không cần ghép đôi lại.
  - Khi ghép đôi bằng `bluetoothctl`, phải đăng ký agent trước: `agent NoInputNoOutput`, rồi `default-agent`, rồi `pair D3:EE:24:55:47:E9`. Chạy riêng lệnh `bluetoothctl pair ...` không có agent sẽ báo `AuthenticationFailed`.
  - Nếu xóa bond ở một phía (`hw unpair` trên chip, hoặc *Forget/Remove* trên máy tính/điện thoại), phải xóa luôn ở phía kia rồi mới ghép đôi lại. Nếu không sẽ gặp lỗi khóa (`PIN or Key Missing`).
- **ModemManager** đang chạy trên máy và có thể chiếm `/dev/ttyACM*` của J-Link VCOM. Cài rule chặn nó:
  ```bash
  sudo cp tools/71-nrf-jlink-modemmanager-ignore.rules /etc/udev/rules.d/
  sudo udevadm control --reload-rules && sudo udevadm trigger
  ```
- **Tụ tải thạch anh** (`&lfxo`, `&hfxo` trong file dts của board) đang dùng giá trị của nRF54L15 DK. Nên hỏi Minew thông số CL của Y1/Y2 để chỉnh cho chính xác. Sai lệch nhỏ chỉ làm lệch tần số vài ppm.
- **Cảnh báo `Deprecated symbol NRF_PLATFORM_LUMOS`** khi build đến từ chính NCS 3.4.0 và xuất hiện với mọi board nRF54L, có thể bỏ qua.
- **Tạo app mới**: copy `apps/hw_test` thành `apps/<ten_app>`, sửa `prj.conf` và `src/`, rồi chạy `make APP=apps/<ten_app> build flash`. Thêm `${workspaceFolder}/build/<ten_app>/<ten_app>/compile_commands.json` vào danh sách `compileCommands` trong [.vscode/c_cpp_properties.json](.vscode/c_cpp_properties.json) để IntelliSense hiểu app mới. Nếu cần đổi chân cho riêng app, thêm file `apps/<ten_app>/boards/me54bs62_nrf54l15_cpuapp.overlay`.
# Helmet_ble6
