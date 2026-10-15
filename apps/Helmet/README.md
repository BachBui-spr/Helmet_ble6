# Helmet: mũ nghe nhạc qua Bluetooth LE Audio

Đây là chương trình chính của Helmet board (Minew ME54BS62 / nRF54L15). Mũ đóng vai trò **tai nghe**: điện thoại kết nối và phát nhạc, mũ giải mã rồi phát ra 2 loa qua 2 ampli MAX98357A.

## Vì sao là LE Audio mà không phải A2DP

**nRF54L15 chỉ có Bluetooth LE, không có Bluetooth Classic (BR/EDR).** A2DP và HFP là profile của Bluetooth Classic, nên chip này không thể chạy chúng bằng bất kỳ phần mềm nào. Nhạc vì vậy được truyền bằng **LE Audio** (Bluetooth 5.2 trở lên):

| | A2DP (Classic) | LE Audio (Helmet dùng) |
|---|---|---|
| Kênh vô tuyến | ACL Classic | CIS (Connected Isochronous Stream) trên BLE |
| Codec | SBC/AAC | **LC3** |
| Âm lượng | AVRCP | **VCP** |
| Play/pause | AVRCP | **MCP** |

**Yêu cầu với điện thoại:** phải hỗ trợ LE Audio (Unicast) và đã bật tính năng này. Nhiều máy Android 13 trở lên đời mới có hỗ trợ; trên một số máy phải bật *LE Audio* trong cài đặt Bluetooth hoặc trong *Tùy chọn nhà phát triển*. iPhone hiện chưa hỗ trợ rộng rãi với tai nghe LE Audio của bên thứ ba. Điện thoại chỉ có Bluetooth Classic sẽ **không** phát được nhạc tới Helmet.

## Vai trò Bluetooth

| Vai trò | Chức năng |
|---|---|
| CAP Acceptor + **BAP Unicast Server** | Nhận luồng âm thanh LC3 (2 ASE sink) |
| **TMAP Unicast Media Receiver** | Khai báo với điện thoại: "tôi là thiết bị nghe media" |
| **VCP Volume Renderer** | Điện thoại và nút Vol+/Vol− cùng chỉnh âm lượng của mũ |
| **MCP client** | Nút Call gửi PLAY/PAUSE tới trình phát nhạc trên điện thoại |
| PACS | LC3 16/24/32/48 kHz, khung 7.5/10 ms, 1–2 kênh, 26–155 B/khung, vị trí Trái + Phải |

## Cấu trúc

```
apps/Helmet/
├── CMakeLists.txt
├── prj.conf            Bluetooth, LE Audio, LC3, I2S, settings, log/shell
└── src/
    ├── helmet.h        khai báo chung
    ├── main.c          bt_enable, quảng bá, kết nối/ghép đôi, nút, shell "helmet", heartbeat
    ├── le_audio.c      PACS + BAP Unicast Server: nhận cấu hình và dữ liệu từ điện thoại
    ├── media_ctrl.c    VCP (âm lượng) + MCP (play/pause)
    ├── audio_out.c     LC3 → PCM → I2S20 → MAX98357A; ghép kênh L/R, âm lượng, bù lệch clock
    └── capture.c       chép gói LC3 nhận được ra RTT kênh 1 để nghe lại trên PC (J-Link)

scripts/helmet_audio.py  (trên PC) đọc RTT kênh 1, giải mã LC3, phát ra loa máy tính + lưu WAV
```

## Luồng âm thanh

```
Điện thoại ──CIS (LC3)──► BT RX thread ──hàng đợi──► audio thread (độ ưu tiên 2)
                                                      │ liblc3 giải mã (FPU); gói mất → PLC
                                                      │ ghép L/R (1 luồng stereo, hoặc 2 luồng mono L+R,
                                                      │          hoặc mono → phát cả 2 loa)
                                                      │ âm lượng VCP (hệ số số, đường cong bình phương)
                                                      ▼
                                     I2S20 (BCLK P1.12, LRCLK P1.10, DIN P1.15) ─► MAX98357A L/R ─► loa
```

- Nạp sẵn 3 khung (khoảng 30 ms) rồi mới bắt đầu phát.
- Clock của điện thoại và của mũ lệch nhau vài chục ppm. Khi bộ đệm I2S có từ 5 khối trở lên thì bỏ 1 khung; khi còn từ 1 khối trở xuống thì lặp 1 khung (bộ đếm `sync -x/+y`).
- ⚠ **Tần số I2S thực tế lệch khoảng 0.8 %** (đã đo trên mạch ngày 14/10/2026). MCK của nRF54L15 chỉ chia nguyên từ 32 MHz, nên: 48 kHz → 47 619 Hz (MCK = 32 MHz/21, tỉ lệ 32), 24 kHz → 23 809 Hz, 16 kHz → 15 873 Hz, 32 kHz → 32 258 Hz. Hậu quả: cứ khoảng 1.26 s bộ bù clock lại bỏ (hoặc lặp) 1 khung 10 ms, sẽ nghe thấy tiếng "tách" khi có nhạc thật. **Cần sửa** bằng cách lấy mẫu lại (48 000 → 47 619 Hz, tỉ lệ đúng 126:125) trước khi đưa ra I2S.

## Build, nạp, xem log

```bash
cd ~/Project/Bluetooth_ME54BS62
make build      # Helmet là app mặc định của Makefile; thư mục build: build/Helmet
make flash
make rtt
```

Quay lại firmware kiểm tra phần cứng: `make APP=apps/hw_test build flash` (xem log: `make APP=apps/hw_test rtt`).

## Ghép đôi và thử nghe nhạc

1. Nạp firmware, mở `make rtt` để xem log.
2. Trên điện thoại hỗ trợ LE Audio: *Bluetooth → Ghép đôi thiết bị mới → "Helmet"*.
3. Log mong đợi:
   ```
   helmet: Connected: …
   helmet: Link encrypted (security level 2)
   helmet: Pairing complete (bonded, keys saved)
   media_ctrl: Media Control Service found on phone
   le_audio: Config ASE 0: LC3 48000 Hz, 10000 us, 2 ch (loc 0x3), … B/frame
   le_audio: QoS ASE 0: interval 10000 us, …
   le_audio: Streaming ASE 0: …
   audio_out: I2S configured: 48000 Hz, 480 samples/frame …
   audio_out: Stream 0 decoding: … -> L+R
   audio_out: I2S output running (48000 Hz)
   helmet: heartbeat: connected, STREAMING, SDU 1000 (bad 0), PLC 0, I2S on 48000 Hz q3, …
   ```
4. Mở nhạc trên điện thoại. Kể cả khi **chưa nối loa**, vẫn kiểm tra được đường dữ liệu bằng log: bộ đếm `SDU` tăng khoảng 100 mỗi giây (khung 10 ms) và I2S ở trạng thái `on`.

## Lệnh shell

| Lệnh | Tác dụng |
|---|---|
| `helmet status` | Kết nối, âm lượng, trạng thái từng ASE, I2S, thống kê SDU/PLC/bù clock |
| `helmet vol <0..255>` | Đặt âm lượng (điện thoại được thông báo lại) |
| `helmet play` / `helmet pause` | Gửi lệnh tới trình phát nhạc trên điện thoại |
| `helmet unpair` | Xóa mọi bond (sau đó xóa luôn "Helmet" trên điện thoại) |
| `helmet aclphy <1m\|2m\|coded>` | Thử nghiệm: đổi PHY của liên kết ACL (kiểm tra Helmet nhận 2M có ổn không) |
| `helmet aclint <ms>` | Thử nghiệm: xin đổi chu kỳ kết nối ACL (ví dụ 10 = bằng chu kỳ ISO). Dùng khi thiết bị phát chạy controller Zephyr `ll_sw` (Manufacturer *The Linux Foundation*) |
| `helmet cispref <1m\|2m> <rtn> <latency_ms>` | Thử nghiệm: QoS Helmet đề nghị cho kênh âm thanh CIS (PHY, số lần phát lại, độ trễ), áp dụng từ lần cấu hình luồng kế tiếp. Mặc định `2m 2 10` |
| `helmet capture [on\|off]` | Bật/tắt gửi gói LC3 nhận được về PC qua RTT kênh 1; không tham số thì in thống kê. `scripts/helmet_audio.py` tự bật/tắt lệnh này |

## Nghe lại âm thanh Helmet nhận được trên máy tính (qua J-Link)

Không cần loa: Helmet chép **nguyên các gói LC3 nhận từ điện thoại** (chưa giải mã, trước âm lượng) ra RTT kênh 1. Script trên PC đọc qua J-Link, giải mã bằng liblc3 (cùng thư viện với firmware), **phát ra loa máy tính** và **lưu file WAV**.

```
Thiết bị phát ──LE Audio (BLE)──► Helmet ──RTT kênh 1 (J-Link, SWD)──► PC: scripts/helmet_audio.py
                                                                         ├─ loa máy tính (paplay)
                                                                         └─ build/audio/helmet_<thời gian>_48k.wav
```

```bash
make build flash                       # firmware có capture.c
make audio                             # = scripts/helmet_audio.py; đóng "make rtt" trước, script hiện luôn log + nhận lệnh shell
make audio AUDIO_OPTS="--no-play"      # truyền thêm tùy chọn của script (bảng dưới)
```

| Tùy chọn | Tác dụng |
|---|---|
| `--no-play` | Chỉ lưu WAV, không phát loa |
| `--sink <tên>` | Chọn loa phát (`pactl list short sinks`). **Nếu chính laptop là thiết bị phát LE Audio**, phải chọn loa của laptop, nếu không âm thanh sẽ quay vòng về Helmet |
| `--raw cap.bin` | Lưu thêm dữ liệu thô; giải mã lại sau bằng `--replay cap.bin` |
| `--selftest` | Tự kiểm tra phần tách gói + giải mã LC3 trên PC, không cần mạch |

- Mỗi 10 s script in: số khung, SDU, gói lỗi, PLC, **"mất qua RTT"** (bản ghi bị bỏ vì J-Link đọc không kịp) và tốc độ KB/s. Gói mất được nội suy bằng PLC nên file WAV vẫn liền mạch.
- Băng thông lớn nhất: 48 kHz stereo khoảng 33 KB/s. Bộ đệm RTT kênh 1 là 16 KB (khoảng 0.5 s).
- Khi không có J-Link đọc, bộ đệm đầy thì firmware bỏ bản ghi (chế độ SKIP), **không bao giờ** làm chậm luồng nhạc. Mặc định capture **tắt**.
- Định dạng bản ghi: xem đầu file `src/capture.c`.

## Nút (khi board nút được nối)

| Nút | Chức năng |
|---|---|
| Vol+ / Vol− | Tăng/giảm âm lượng 16 nấc; Vol+ đồng thời bỏ tắt tiếng |
| Call | Play/pause trên điện thoại |
| Power | Chưa gán chức năng |

## Ý nghĩa heartbeat (mỗi 10 s)

```
heartbeat: connected, STREAMING, SDU 5230 (bad 3), PLC 3, I2S on 48000 Hz q3, sync -0/+1, err 0
```

| Trường | Ý nghĩa | Bình thường |
|---|---|---|
| `advertising/connected` | Trạng thái liên kết | |
| `STREAMING/idle` | Có luồng âm thanh hay không | |
| `SDU` / `bad` | Gói nhận được / gói lỗi hoặc mất | `bad` dưới 1 % |
| `PLC` | Số khung phải nội suy vì mất gói | Tăng chậm |
| `I2S on … q3` | Đang phát; số khối đang chờ | `q` từ 2 đến 4 |
| `sync -x/+y` | Số khung bị bỏ / lặp để bù lệch clock | Tăng khoảng 1 lần mỗi vài phút |
| `err` | Lỗi I2S, lỗi giải mã, hàng đợi đầy | 0 |

Khi đang kết nối, ngay sau heartbeat có thêm một dòng chẩn đoán sóng:

```
link: RSSI -58 dBm, ACL PHY tx 2M / rx 2M; ISO rx ok 5230, CRC 2, lost 1, empty 0
```

| Trường | Ý nghĩa |
|---|---|
| `RSSI` | Cường độ tín hiệu của liên kết ACL: −40…−70 dBm là tốt, dưới −85 dBm là yếu |
| `ACL PHY` | PHY của liên kết điều khiển (1M / 2M / Coded) |
| `ok / CRC / lost / empty` | Gói âm thanh tốt / nhận được nhưng sai CRC / không nhận được gì / độ dài 0 |

Lúc bắt đầu luồng còn có dòng `CIS ASE n: ... C->P PHY 2M, BN, FT, NSE, max PDU ...`, cho biết thông số kênh âm thanh mà thiết bị phát đã chọn.

## Đã kiểm tra trên mạch (13/10/2026)

| Kiểm tra | Kết quả |
|---|---|
| Build | ✅ không có cảnh báo của project; RRAM 25.0 %, RAM 56.8 % (gồm 16 KB bộ đệm RTT cho capture) |
| Khởi động | ✅ LC3 + I2S sẵn sàng, BAP server 2 ASE sink |
| Quảng bá | ✅ Laptop thấy "Helmet", appearance Headset (0x0942), UUID ASCS / CAS / TMAS, địa chỉ RPA riêng tư |
| Ghép đôi | ✅ Just Works, mã hóa level 2, bond lưu vào RRAM; laptop giải được địa chỉ RPA ra địa chỉ thật D3:EE:24:55:47:E9 |
| Dịch vụ GATT | ✅ PACS, ASCS, VCS, CAS, TMAS |
| Ngắt kết nối | ✅ Tự quảng bá lại |
| MCP khi máy kia không có MCS | ✅ Báo "not found", tắt play/pause, không lỗi |
| **Phát nhạc thật** | ⏳ **Chưa thử được**: cần điện thoại hỗ trợ LE Audio, và loa chưa được nối |
| Capture qua J-Link | ✅ `--selftest` đạt (stereo 1 CIS, 2 CIS L/R, mất 12 gói liên tiếp, mono 16 kHz 7.5 ms); ⏳ chưa thử với luồng thật |

## Giới hạn và việc tiếp theo

- **Chưa có cuộc gọi**: micro chưa nối, nên chưa khai báo ASE source (ngữ cảnh Conversational). Cuộc gọi qua LE Audio cần thêm PDM → LC3 encoder → ASE source và CCP/TBS.
- **Presentation delay** (độ trễ trình diễn) không được căn chính xác, chỉ dùng bộ đệm cố định khoảng 30 ms. Như vậy là đủ cho 1 thiết bị có 2 loa; nếu ghép thành cặp tai nghe riêng biệt thì phải đồng bộ theo thời điểm (timestamp).
- **Bù lệch clock** đang làm theo khối (bỏ hoặc lặp 1 khung, khoảng vài phút một lần). Có thể nâng cấp lên chèn/bỏ từng mẫu, hoặc lấy mẫu lại (resample), để không nghe thấy.
- **RAM**: 4 bộ giải mã LC3 48 kHz chiếm khoảng 36 KB. Có thể giảm còn 2 nếu giới hạn tổng số kênh bằng 2.
- LED P2.07 không dùng (mạch đang hở, xem `docs/PINOUT.md`). Nút Power chưa có chức năng (dự kiến: bật/tắt, ngủ).
- Bond cũ do `hw_test` lưu không tương thích với Helmet (khác cấu hình privacy). Ở lần khởi động đầu, log có thể báo lỗi `bt_keys`; chỉ cần ghép đôi lại.
