# Helmet: thiết kế chi tiết các chức năng mới

| | |
|---|---|
| Áp dụng cho | `apps/Helmet` (firmware chính), board `me54bs62`, `scripts/helmet_audio.py` |
| Trạng thái | **Thiết kế, chưa hiện thực.** Chưa có dòng mã nào trong tài liệu này được viết vào dự án |
| Dựa trên | Mã nguồn ngày 15/10/2026 (xem [HELMET_GIAI_THICH_MA_NGUON.md](HELMET_GIAI_THICH_MA_NGUON.md)), NCS v3.4.0 / Zephyr 4.4 |
| Yêu cầu liên quan | FR-20 (đàm thoại), FR-21 (xử lý cuộc gọi bằng nút), FR-23 (nút Power, bật/tắt, ngủ), FR-22 (nhiệt độ hồng ngoại) trong [TAI_LIEU_THIET_KE_TEST.md](TAI_LIEU_THIET_KE_TEST.md) |

Quy ước nhãn:
- **[SDK]**: đã kiểm tra trực tiếp trong mã nguồn NCS v3.4.0 trên máy (tên hàm, Kconfig, hành vi).
- **[Đề xuất]**: lựa chọn thiết kế của nhóm firmware; chủ dự án cần duyệt.
- **[Cần đo]**: số liệu ước lượng, phải đo trên mạch trước khi chốt.
- **[TBD]**: chưa xác định, chờ trả lời câu hỏi mở (mục 11).

Cách đọc tài liệu:
- Mục 1–3: tổng quan thiết kế, kiến thức nền mới và các thay đổi cấu hình chung. Nên đọc trước.
- Mục 4–7: mỗi mục là một chức năng, cùng một khung trình bày: yêu cầu chi tiết → luồng dữ liệu → thiết kế từng module → trình tự hoạt động → xử lý lỗi → kiểm thử.
- Mục 8–12: tổng hợp thay đổi ở module cũ, chẩn đoán, kế hoạch kiểm thử, rủi ro, phụ lục.


> **Ghi chú 27/10/2026:** mã nguồn đã được tổ chức lại theo lớp. `helmet.h` / `main.c` nay tách thành `apps/main/src/app/` (lớp ứng dụng, `app/app.h`), `apps/main/src/features/<chức năng>/` và thư viện dùng chung `lib/` (`hal/`, `core/`, `services/`, `proto/`). Các tên file trong tài liệu này là tên tại thời điểm thiết kế; vị trí hiện tại xem [README.md](../README.md), mục *Kiến trúc mã nguồn*.

---

## Mục lục

1. [Tổng quan thiết kế](#1-tổng-quan-thiết-kế)
2. [Kiến thức nền mới](#2-kiến-thức-nền-mới)
3. [Thay đổi cấu hình và giao diện chung](#3-thay-đổi-cấu-hình-và-giao-diện-chung)
4. [Chức năng 1: Đàm thoại (FR-20)](#4-chức-năng-1-đàm-thoại-fr-20)
5. [Chức năng 2: Xử lý cuộc gọi và cử chỉ nút (FR-21)](#5-chức-năng-2-xử-lý-cuộc-gọi-và-cử-chỉ-nút-fr-21)
6. [Chức năng 3: Nguồn, bật/tắt, ngủ (FR-23)](#6-chức-năng-3-nguồn-bậttắt-ngủ-fr-23)
7. [Chức năng 4: Nhiệt độ hồng ngoại MLX90632 (FR-22)](#7-chức-năng-4-nhiệt-độ-hồng-ngoại-mlx90632-fr-22)
8. [Tổng hợp thay đổi ở các module hiện có](#8-tổng-hợp-thay-đổi-ở-các-module-hiện-có)
9. [Chẩn đoán: log, heartbeat, shell](#9-chẩn-đoán-log-heartbeat-shell)
10. [Kế hoạch kiểm thử và tiêu chí nghiệm thu](#10-kế-hoạch-kiểm-thử-và-tiêu-chí-nghiệm-thu)
11. [Rủi ro và câu hỏi mở](#11-rủi-ro-và-câu-hỏi-mở)
12. [Phụ lục](#12-phụ-lục)

---

## 1. Tổng quan thiết kế

### 1.1 Phạm vi

| Chức năng | Yêu cầu gốc | Kết quả mong muốn khi xong |
|---|---|---|
| **1. Đàm thoại** | FR-20 | Nghe và nói điện thoại qua LE Audio: thu micro PDM, khử vọng và giảm ồn, mã hóa LC3, gửi lên điện thoại qua CIS hai chiều; điện thoại tắt/bật được micro của mũ (MICP) |
| **2. Cuộc gọi bằng nút** | FR-21 | Nghe, từ chối, kết thúc cuộc gọi, tắt micro bằng nút; nút phân biệt nhấn ngắn, nhấn đúp, nhấn giữ, giữ rất lâu, tổ hợp hai nút |
| **3. Nguồn** | FR-23 | Bật/tắt bằng nút Power (System OFF), tự tắt khi không dùng, giảm quảng bá khi rảnh, âm báo trạng thái, lưu cài đặt qua lần tắt |
| **4. Nhiệt độ** | FR-22 | Đọc MLX90632 (driver tự viết theo thuật toán Melexis), lọc, cảnh báo ngưỡng, gửi lên điện thoại qua Health Thermometer Service |

Ngoài phạm vi (ghi nhận để không bỏ sót):
- DFU / MCUboot (FR-24).
- Đo % pin: phần cứng chưa đưa Bat+ về MCU. Mục 6.9 chỉ thiết kế cảnh báo "pin sắp hết" qua VDD.
- LED: mạch hở (ISS-01). Mọi phản hồi cho người dùng trong thiết kế này đi qua **âm báo**.
- Intercom giữa các mũ (Auracast).

### 1.2 Hai thành phần dùng chung phải làm trước

Phân tích bốn chức năng cho thấy hai thành phần nền mà chức năng nào cũng cần. Phải có chúng trước, nếu không các chức năng sau sẽ phải sửa lại:

| Thành phần | Vì sao cần | Dùng bởi |
|---|---|---|
| **A. `audio_out` thế hệ 2**: lấy mẫu lại theo tỉ lệ cố định + bù trôi clock từng mẫu + trộn âm báo + điểm lấy tín hiệu tham chiếu | Sửa lỗi I2S lệch 0.8 % (47 619 Hz). Bộ khử vọng **bắt buộc** tín hiệu tham chiếu liên tục, không bị bỏ/lặp nguyên khung 10 ms. Âm báo cần một đường trộn vào loa | Đàm thoại, cuộc gọi (chuông), nguồn (âm bật/tắt), nhiệt độ (cảnh báo) |
| **B. Bộ điều khiển ứng dụng**: một hàng đợi sự kiện + một máy trạng thái thiết bị, chạy trong **một** thread | Nút, cuộc gọi, kết nối, hẹn giờ tắt máy, cảnh báo nhiệt đến từ 5 thread khác nhau. Gom về một chỗ xử lý tuần tự thì không cần khóa và dễ kiểm thử | Mọi chức năng |

### 1.3 Kiến trúc sau thay đổi

```
┌──────────────────────────────────────── apps/Helmet ────────────────────────────────────────┐
│  main.c ── bộ điều khiển ứng dụng (hàng đợi sự kiện, máy trạng thái thiết bị, heartbeat)    │
│     ▲ sự kiện                                                                                │
│  buttons.c   call_ctrl.c   power.c   thermo.c + mlx90632.c                                   │
│  (cử chỉ)    (TBS client,  (System    (I2C bit-bang, công thức Melexis, HTS)                 │
│              MICP)         OFF, hẹn giờ)                                                     │
│                                                                                              │
│  le_audio.c ── ASE sink (nghe) + ASE source (nói), ngữ cảnh Conversational/Ringtone         │
│     │ SDU nhận                         ▲ SDU gửi (sent → gửi tiếp)                           │
│     ▼                                  │                                                     │
│  audio_out.c (v2): giải mã → trôi clock → trộn âm báo ─┬─► lấy mẫu lại cố định → I2S → loa  │
│                                                        │ tham chiếu (ref)                    │
│                                                        ▼                                     │
│  mic_in.c → voice_dsp.c (lọc, AEC, giảm ồn, AGC) → voice_tx.c (trôi clock, LC3 encode)       │
│  capture.c (+ bản ghi MIC/REF để chỉnh AEC trên PC)      media_ctrl.c (giữ nguyên)           │
├──────────────────────────────────────── Zephyr / NCS ───────────────────────────────────────┤
│ BAP sink+source · TBS client (CCP) · MICP Mic Device · VCP · MCC · HTS (GATT tự định nghĩa) │
│ liblc3 (decode + encode) · CMSIS-DSP · dmic (PDM) · I2S · gpio-i2c · poweroff · hwinfo ·    │
│ settings                                                                                     │
└──────────────────────────────────────────────────────────────────────────────────────────────┘
```

**Bảng file:**

| File | Trạng thái | Nội dung |
|---|---|---|
| `main.c` | Sửa lớn | Bộ điều khiển ứng dụng thay cho `on_input` trực tiếp; thứ tự khởi tạo mới; lệnh shell mới |
| `le_audio.c` | Sửa | Thêm ASE source, PACS source, ngữ cảnh mới, callback `sent` |
| `audio_out.c` | Sửa lớn (v2) | Bù trôi từng mẫu, lấy mẫu lại cố định, âm báo, tham chiếu AEC |
| `media_ctrl.c` | Sửa nhỏ | Khôi phục âm lượng đã lưu; lệnh bài kế tiếp |
| `capture.c` | Sửa nhỏ | Thêm bản ghi PCM micro và tham chiếu |
| `buttons.c` | **Mới** | Nhận dạng cử chỉ nút |
| `call_ctrl.c` | **Mới** | TBS client (CCP), máy trạng thái cuộc gọi, MICP |
| `mic_in.c` | **Mới** | Thu PDM 2 micro |
| `voice_dsp.c` | **Mới** | Lọc thông cao, khử vọng, giảm ồn, AGC (mã C thuần, chạy được trên PC) |
| `voice_tx.c` | **Mới** | Bù trôi clock chiều lên, mã hóa LC3, gửi SDU |
| `power.c` | **Mới** | Quyết định lúc khởi động, quy trình tắt máy, hẹn giờ tự tắt, chính sách quảng bá |
| `mlx90632.c` | **Mới** | Driver và công thức tính nhiệt (mã tính toán tách riêng, chạy được trên PC) |
| `thermo.c` | **Mới** | Lịch đo, lọc, ngưỡng cảnh báo, dịch vụ HTS |
| `helmet.h` | Sửa | Khai báo các API mới (mục 3.4) |

### 1.4 Thread sau thay đổi

| Thread | Ưu tiên | Stack | Việc | Thay đổi |
|---|---|---|---|---|
| `bt_rx` | coop 8 | 2 200 B | Callback Bluetooth; `stream_recv` chép SDU; **`sent` lấy SDU kế tiếp đã mã hóa sẵn và gửi** | `sent` mới, chỉ làm việc nhẹ |
| `audio_tid` | preempt 2 | 4 096 B → **6 144 B** [Cần đo] | Giải mã, trôi clock, âm báo, lấy mẫu lại, I2S | Thêm việc |
| `voice_tid` | **preempt 3** | **6 144 B** [Cần đo] | `dmic_read` → DSP → mã hóa LC3 → hàng SDU | **Mới** |
| `main` (bộ điều khiển) | preempt 0 → **preempt 4** [Đề xuất] | 2 048 B | Xử lý sự kiện, heartbeat | Hạ ưu tiên để không chen audio/voice khi ghi settings |
| `thermo_tid` | **preempt 10** | 2 048 B | Đọc MLX90632 qua I2C bit-bang, tính nhiệt (double) | **Mới**. Không dùng `sysworkq` (mục 7.4) |
| `sysworkq` | coop −1 | 2 048 B | Bật quảng bá, dò GATT | Không đổi |
| `input`, shell, logging | | | | Không đổi |

Lý do `voice_tid` ưu tiên thấp hơn `audio_tid`: thiếu một khung ở loa (tiếng tách) dễ nghe thấy hơn trễ vài ms ở chiều lên, vốn đã có bộ đệm.

### 1.5 Ngân sách RAM và CPU

**RAM** (hiện tại 149 KB / 256 KB = 56.8 %):

| Thành phần | Ước lượng | Cách tính |
|---|---|---|
| Bộ mã hóa LC3 32 kHz | 4.0 KB | `LC3_ENCODER_MEM_T(10000, 32000)` đo được 4 008 B (16 kHz: 2 608 B) [SDK] |
| Bộ đệm PDM | 3.8 KB | 6 khối × 640 B (16 kHz stereo 10 ms; micro luôn chạy 16 kHz, mục 4.5) |
| `voice_dsp`: AEC 256 hệ số + trạng thái, vòng tham chiếu 128 ms, bộ giảm ồn FFT 512 | ~20 KB | float 32 bit |
| `voice_tx`: vòng PCM 40 ms + 3 SDU | ~2 KB | |
| Stack `voice_tid` + tăng stack `audio_tid` + `thermo_tid` | ~10 KB | [Cần đo] bằng `kernel stacks` |
| `audio_out` v2: bộ lấy mẫu lại, bộ tạo âm báo | ~2 KB | |
| TBS client, MICP, HTS, bộ đệm ATT thêm | ~4 KB | |
| **Tổng thêm** | **~46 KB** | RAM dự kiến **~195 KB (76 %)** [Cần đo] |

Giảm RAM nếu thiếu:
- tắt `capture.c` trong bản phát hành (16 KB);
- giảm số bộ giải mã từ 4 xuống 2 (18 KB) nếu giới hạn tổng 2 kênh nhận.

**CPU** khi đang gọi (16 kHz hai chiều; mỗi 10 ms có 1.28 triệu chu kỳ ở 128 MHz):

| Khối | Ước lượng | Ghi chú |
|---|---|---|
| Giải mã LC3 mono 16 kHz | [Cần đo] | Nhẹ hơn nhiều so với nghe nhạc 48 kHz stereo đang chạy được |
| Mã hóa LC3 mono 16 kHz | [Cần đo] | Mã hóa nặng hơn giải mã |
| AEC NLMS 256 hệ số | ~10–13 % | 2 × 256 phép nhân-cộng × 16 000 mẫu/s ≈ 8.2 triệu MAC/s; giả định 1.5–2 chu kỳ/MAC với FPU |
| Giảm ồn (FFT/IFFT 512 điểm mỗi 10 ms) | ~4–5 % | CMSIS-DSP `arm_rfft_fast_f32` |
| Lọc thông cao, AGC, lấy mẫu lại, âm báo | < 2 % | |
| **Mục tiêu tổng phần ứng dụng** | **< 50 %** | Chừa chỗ cho Bluetooth controller và ngắt |

Cách đo: thêm lệnh `helmet perf` in số chu kỳ trung bình và lớn nhất của từng khối, dùng `k_cycle_get_32()`.

### 1.6 Máy trạng thái thiết bị

Mọi chức năng mới đều hỏi "thiết bị đang ở trạng thái nào". Ví dụ: nút Call nhấn ngắn là play/pause khi nghe nhạc, nhưng là nghe máy khi có cuộc gọi đến. Vì vậy trạng thái được định nghĩa **một lần**, trong bộ điều khiển ở `main.c`:

```
                       nút Power giữ 2 s
          ┌──────┐   (thức từ System OFF)    ┌────────┐
          │ OFF  │ ────────────────────────► │  BOOT  │── nhả sớm ──► OFF
          └──────┘                           └───┬────┘
              ▲                                  │ xác nhận bật / reset thường
              │                                  ▼
      ┌───────┴──────┐  hết giờ 10 phút    ┌──────────┐  điện thoại kết nối  ┌────────────┐
      │  SHUTDOWN    │ ◄────────────────── │  IDLE    │ ───────────────────► │ CONNECTED  │
      │ (quy trình   │ ◄── Power giữ 3 s ─ │(quảng bá)│ ◄─── ngắt kết nối ── │(nhạc/rảnh) │
      │  tắt máy)    │                     └──────────┘                      └─────┬──────┘
      └──────────────┘ ◄──────────── Power giữ 3 s (mọi trạng thái bật) ─────────┤
                                                       cuộc gọi đến (TBS)        │
                                                ┌──────────────┐ ◄───────────────┘
                                                │   RINGING    │
                                                └──┬────────┬──┘
                                   nghe máy        │        │ từ chối / người gọi bỏ
                                                   ▼        ▼
                                           ┌──────────┐   về CONNECTED
                                           │ IN_CALL  │── kết thúc ──► CONNECTED
                                           └──────────┘
```

Trạng thái phụ không làm đổi trạng thái chính: `streaming` (đang nhận nhạc), `mic_muted`, `temp_alarm`.

### 1.7 Thứ tự hiện thực đề xuất

| Mốc | Nội dung | Thử được khi nào | Phụ thuộc phần cứng |
|---|---|---|---|
| **M0** | `audio_out` v2 (lấy mẫu lại, bù trôi từng mẫu, âm báo, điểm tham chiếu) + bộ điều khiển ứng dụng | Ngay: kiểm qua heartbeat (`sync` phải về 0) và capture | Không (loa giúp nghe thử) |
| **M1** | `buttons.c` + `power.c` + âm báo | Ngay, nhờ lệnh shell giả lập nút (mục 9.3) | Board nút để thử thật |
| **M2** | `call_ctrl.c` (TBS client, MICP) + ASE source **gửi im lặng** | Điện thoại LE Audio có GTBS | Không cần micro |
| **M3** | `mic_in.c` + `voice_tx.c` (chưa có AEC) | Gọi thật, nghe được giọng | Board micro |
| **M4** | `voice_dsp.c`: AEC, giảm ồn, AGC; chỉnh tham số bằng dữ liệu ghi qua RTT | Gọi thật trong mũ | Micro + loa lắp trong mũ |
| **M5** | `mlx90632.c` + `thermo.c` + HTS | Khi gắn cảm biến | MLX90632 + điện trở kéo lên 4.7 kΩ |

Mỗi mốc kết thúc bằng các ca kiểm thử ở mục 10.

---

## 2. Kiến thức nền mới

### 2.1 Cuộc gọi qua LE Audio

**CIS hai chiều.** Một CIS chở dữ liệu theo **cả hai chiều** trong cùng một sự kiện 10 ms: điện thoại → mũ (giọng người bên kia) và mũ → điện thoại (giọng người đội mũ). Trong ASCS, mỗi chiều là một ASE riêng:

| ASE | Hướng | Helmet là | Hiện tại | Sau thay đổi |
|---|---|---|---|---|
| Sink | điện thoại → mũ | bên nhận | 2 ASE | 2 ASE |
| Source | mũ → điện thoại | bên gửi | 0 | **1 ASE** |

Điện thoại cấu hình ASE sink và ASE source rồi **gắn cả hai vào cùng một CIS**.

**Khác biệt quan trọng giữa ASE sink và ASE source** [SDK, `ascs.c`]:
- ASE **sink**: Helmet tự báo "sẵn sàng nhận" (đã có trong `stream_enabled`).
- ASE **source**: **điện thoại** gửi *Receiver Start Ready* khi nó sẵn sàng nhận. Lúc đó callback `start` của server (`lc3_start`) **được gọi**, rồi `stream_started`. Đây là chỗ bắt đầu gửi.

**Ngữ cảnh âm thanh** (bit trong *Audio Context*):

| Ngữ cảnh | Bit | Dùng khi | Hiện tại |
|---|---|---|---|
| Conversational | `0x0002` | Cuộc gọi | Chưa có |
| Ringtone | `0x0200` | Điện thoại phát chuông qua LE Audio | Chưa có |
| Media, Game… | | Nghe nhạc | Có |

Nếu Helmet không công bố Conversational, điện thoại sẽ không chuyển cuộc gọi sang mũ.

**Cấu hình codec cho đàm thoại** (theo bảng cấu hình chuẩn của BAP):

| Tên | Tần số | Khung | Octet/khung | Tốc độ | Ghi chú |
|---|---|---|---|---|---|
| 16_2 | 16 kHz | 10 ms | 40 | 32 kbps | BAP bắt buộc mọi thiết bị hỗ trợ |
| 32_2 | 32 kHz | 10 ms | 80 | 64 kbps | "Super wideband", thường dùng cho thiết bị TMAP |

**TMAP vai trò CT** (Call Terminal): thiết bị đầu cuối cuộc gọi. Ví dụ `samples/bluetooth/tmap_peripheral` của SDK đăng ký `BT_TMAP_ROLE_CT | BT_TMAP_ROLE_UMR` cùng ASE source và CCP [SDK]. Helmet đi theo mô hình này.

### 2.2 CCP và TBS: điều khiển cuộc gọi

- **TBS** (Telephone Bearer Service) chạy **trên điện thoại**. Nó công bố danh sách cuộc gọi và nhận lệnh.
  - **GTBS** (Generic TBS) là một bản gộp mọi ứng dụng gọi điện (SIM, WhatsApp…) thành một. Helmet chỉ cần GTBS.
- **CCP Call Control Client** chạy **trên mũ**. Trong NCS v3.4.0, lớp CCP (`bt_ccp_call_control_client_*`) mới lo tìm bearer. Các thao tác cuộc gọi dùng thẳng **TBS client** [SDK, `tbs.h`]:

| Hàm | Ý nghĩa |
|---|---|
| `bt_tbs_client_discover(conn)` | Dò TBS/GTBS trên điện thoại, đăng ký nhận thông báo |
| `bt_tbs_client_accept_call(conn, BT_TBS_GTBS_INDEX, call_index)` | Nghe máy |
| `bt_tbs_client_terminate_call(conn, BT_TBS_GTBS_INDEX, call_index)` | Từ chối (khi đang đổ chuông) hoặc kết thúc |
| `bt_tbs_client_hold_call` / `retrieve_call` | Giữ / lấy lại cuộc gọi (không dùng ở bản đầu) |
| Callback `call_state` | Danh sách cuộc gọi `{index, state, flags}` mỗi khi điện thoại thông báo |
| Callback `termination_reason` | Lý do kết thúc (người kia cúp, bận, không trả lời…) |
| Callback `remote_uri`, `friendly_name` | Số / tên người gọi (chỉ ghi log vì mũ không có màn hình) |

Trạng thái một cuộc gọi trong TBS: `INCOMING (0)`, `DIALING (1)`, `ALERTING (2)`, `ACTIVE (3)`, `LOCALLY_HELD (4)`, `REMOTELY_HELD (5)`, `LOCALLY_AND_REMOTELY_HELD (6)` [SDK].

TBS không có lệnh "gọi lại số cuối". `originate_call` cần một URI (`tel:…`). Vì vậy bản thiết kế này **không** có chức năng gọi lại.

### 2.3 MICP: điện thoại tắt/bật micro của mũ

**Microphone Control Service** chạy trên mũ, có một giá trị *Mute*: `0` = bật tiếng, `1` = tắt tiếng, `2` = vô hiệu hóa [SDK, `micp.h`].
- Điện thoại ghi giá trị này khi người dùng bấm nút tắt micro trên màn hình cuộc gọi.
- Mũ đổi giá trị này khi người dùng tắt micro bằng nút. Điện thoại được thông báo nên biểu tượng trên màn hình đổi theo.

### 2.4 Tiếng vọng và khử vọng (AEC)

```
 giọng người bên kia ──► loa trong mũ ──► (đường âm: vài cm, phản xạ trong mũ) ──► micro
                                                                                  │
 giọng người đội mũ ────────────────────────────────────────────────────────────►─┤
                                                                                  ▼
                                         micro thu = giọng mình + TIẾNG VỌNG + ồn (gió, xe)
```

Không khử thì người bên kia nghe lại chính giọng mình, trễ khoảng 100–300 ms qua mạng di động. Đó là tiếng vọng rất khó chịu.

**Nguyên lý bộ lọc thích nghi:**
- Mũ biết chính xác tín hiệu đã đưa ra loa (gọi là **tham chiếu** `x`).
- Một bộ lọc FIR `w` học **đường âm** từ loa tới micro. Lấy tín hiệu micro `d` trừ đi `w * x` (tiếng vọng ước lượng) thì còn lại giọng mình + ồn.
- **NLMS** (Normalized Least Mean Squares) cập nhật `w` sau mỗi mẫu:

  ```
  y[n] = Σ w[k]·x[n−k]           (tiếng vọng ước lượng, k = 0…L−1)
  e[n] = d[n] − y[n]             (tín hiệu gửi đi)
  w[k] += μ · e[n] · x[n−k] / (Σ x² + δ)
  ```

  `L` là số hệ số: phải phủ hết độ dài đường âm. Trong mũ, loa và micro gần nhau, đường âm ngắn: 256 hệ số ở 16 kHz = 16 ms [Đề xuất, cần đo đáp ứng xung thật].

**Ba điều kiện để AEC hoạt động:**
1. **Tham chiếu và micro phải chạy cùng một clock.** Chỉ lệch 0.01 % là tiếng vọng "trôi" khỏi bộ lọc. Mục 4.7 cho thấy Helmet đáp ứng được: PDM và I2S cùng lấy clock từ thạch anh 32 MHz.
2. **Độ trễ khối giữa tham chiếu và micro phải cố định.** Gồm thời gian chờ trong hàng đợi I2S, DMA, ampli, đường âm, bộ lọc của PDM. Nếu cơ chế bù clock **bỏ nguyên một khung** tham chiếu (như `audio_out` hiện tại), độ trễ nhảy 10 ms và bộ lọc phải học lại từ đầu. Đây là lý do phải làm `audio_out` v2 trước.
3. **Không cập nhật bộ lọc khi cả hai cùng nói** (double-talk), nếu không bộ lọc học nhầm giọng mình là tiếng vọng.

Sau bộ lọc luôn còn tiếng vọng dư (méo loa, rung vỏ mũ). Cần thêm khâu **NLP** (non-linear processing) giảm mạnh tín hiệu khi phần còn lại chủ yếu là tiếng vọng.

### 2.5 System OFF trên nRF54L15

- Chế độ ngủ sâu nhất: CPU, RAM (tùy chọn giữ), ngoại vi đều tắt. Dòng của chip dưới 1 µA.
- Thức dậy bằng **mức logic trên chân GPIO** đã cấu hình *sense*. Thức dậy = **reset toàn bộ**: chương trình chạy lại từ đầu `main()`.
- Nguyên nhân reset đọc được qua `hwinfo_get_reset_cause()`. Thức từ System OFF bằng GPIO cho cờ `RESET_LOW_POWER_WAKE` [SDK, ví dụ `samples/boards/nordic/system_off`].
- API: `sys_poweroff()` (Kconfig `CONFIG_POWEROFF`). Chân thức: `gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_LEVEL_ACTIVE)` trước khi tắt [SDK].
- ⚠ Khi J-Link đang gắn và phiên debug đang mở, Nordic chỉ **giả lập** System OFF: chip không thật sự tắt, dòng không giảm. Muốn đo dòng phải rút J-Link và reset nguồn.

### 2.6 MLX90632

Cảm biến nhiệt hồng ngoại không tiếp xúc, góc nhìn khoảng 50°, đo **nhiệt độ vật thể** (bề mặt nó nhìn vào) và **nhiệt độ môi trường** (của chính nó).
- Không có đầu ra °C sẵn. Cảm biến chỉ cho giá trị ADC thô; vi điều khiển phải tính bằng các **hệ số hiệu chuẩn riêng của từng con**, lưu trong EEPROM của cảm biến.
- Giao tiếp I2C, địa chỉ thanh ghi 16 bit, dữ liệu 16 bit, đều big-endian (giống `hw_test`).
- Melexis công bố thư viện C mã nguồn mở `mlx90632-library` (giấy phép Apache-2.0). Thiết kế ở mục 7 viết lại theo đúng thuật toán của thư viện này. Zephyr/NCS **không** có driver MLX90632 (chỉ có `mlx90394`) [SDK].

---

## 3. Thay đổi cấu hình và giao diện chung

### 3.1 `CMakeLists.txt`

Thêm 8 file vào `target_sources`: `buttons.c`, `call_ctrl.c`, `power.c`, `mic_in.c`, `voice_dsp.c`, `voice_tx.c`, `mlx90632.c`, `thermo.c`.

Sau khi build, IntelliSense tự nhận các file mới qua `build/Helmet/Helmet/compile_commands.json`.

### 3.2 `prj.conf`: phần thêm và phần đổi

**Đàm thoại**

| Cấu hình | Giá trị | Lý do |
|---|---|---|
| `CONFIG_BT_ASCS_MAX_ASE_SRC_COUNT` | 0 → **1** | 1 ASE source cho micro |
| `CONFIG_BT_PAC_SRC`, `CONFIG_BT_PAC_SRC_LOC` | n → **y** | Bản ghi PACS "source" và vị trí |
| `CONFIG_BT_ISO_MAX_CHAN` | 2 → **3** [Đề xuất] | Dự phòng điện thoại dùng CIS riêng cho chiều lên |
| `CONFIG_BT_ISO_TX_BUF_COUNT` | **3** | Hàng SDU gửi: 1 đang ở controller + 2 chờ |
| `CONFIG_BT_CTLR_SDC_ISO_TX_HCI_BUFFER_COUNT` | 1 → **2** | Controller giữ được 2 SDU gửi |
| `CONFIG_BT_MICP_MIC_DEV` | **y** | Dịch vụ tắt/bật micro |
| `CONFIG_AUDIO`, `CONFIG_AUDIO_DMIC` | **y** | Driver PDM (giống `hw_test`) |
| `CONFIG_CMSIS_DSP`, `CONFIG_CMSIS_DSP_FILTERING`, `CONFIG_CMSIS_DSP_TRANSFORM`, `CONFIG_CMSIS_DSP_FASTMATH`, `CONFIG_CMSIS_DSP_STATISTICS` | **y** | NLMS (`arm_lms_norm_f32`), biquad, FFT, `arm_sqrt_f32`, đo năng lượng |

**Cuộc gọi**

| Cấu hình | Giá trị | Lý do |
|---|---|---|
| `CONFIG_BT_CCP_CALL_CONTROL_CLIENT` | **y** | Profile CCP (giống ví dụ SDK) |
| `CONFIG_BT_TBS_CLIENT_GTBS` | **y** | Chỉ cần GTBS |
| `CONFIG_BT_TBS_CLIENT_TBS` | **n** | Không dò từng TBS riêng (tiết kiệm RAM) |
| `CONFIG_BT_TBS_CLIENT_ACCEPT_CALL`, `…_TERMINATE_CALL` | **y** (mặc định) | Nghe / từ chối / kết thúc |
| `CONFIG_BT_TBS_CLIENT_INCOMING_CALL`, `…_CALL_FRIENDLY_NAME` | **y** | Ghi log số và tên người gọi |
| `CONFIG_BT_TBS_CLIENT_MAX_CALLS` | 1 → **2** | Mặc định chỉ theo dõi 1 cuộc [SDK]; cần 2 để xử lý cuộc chờ |
| `CONFIG_BT_ATT_TX_COUNT` | 20 (giữ) | Ví dụ SDK ghi TBS client cần tới 12 bộ đệm |

**Nguồn**

| Cấu hình | Giá trị | Lý do |
|---|---|---|
| `CONFIG_POWEROFF` | **y** | `sys_poweroff()` |
| `CONFIG_HWINFO` | **y** | Đọc nguyên nhân reset |
| `CONFIG_PM_DEVICE` | **y** | Tạm dừng UART20 trước khi tắt (giảm dòng) |
| `CONFIG_ADC` | **y** (tùy chọn) | Đo VDD để cảnh báo pin sắp hết (mục 6.9) |
| `CONFIG_MAIN_THREAD_PRIORITY` | **4** | Mục 1.4 |

**Nhiệt độ**

| Cấu hình | Giá trị | Lý do |
|---|---|---|
| `CONFIG_I2C` | **y** | Bus `i2c-sensor` (gpio-i2c) đã có trong devicetree |

### 3.3 Devicetree

**Không cần sửa board.**
- Nút: đã có `gpio-keys`. Nhận dạng cử chỉ làm trong ứng dụng (mục 5.5 giải thích vì sao không dùng node `zephyr,input-longpress` / `zephyr,input-double-tap`).
- Chân thức System OFF: lấy bằng `GPIO_DT_SPEC_GET(DT_NODELABEL(btn_power), gpios)`.
- PDM: `pdm20` đã bật.
- I2C: alias `i2c-sensor` đã có. Địa chỉ 0x3A là hằng số trong driver (giống `hw_test`).

### 3.4 `helmet.h`: API mới [Đề xuất]

```c
/* ---- Ngữ cảnh: thêm Conversational + Ringtone cho chiều nhận, Conversational cho chiều gửi ---- */
#define HELMET_SINK_CONTEXTS   (… hiện có … | BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL | \
                                BT_AUDIO_CONTEXT_TYPE_RINGTONE)          /* 0x079F */
#define HELMET_SOURCE_CONTEXTS (BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL)    /* 0x0002 */
#define HELMET_SOURCE_LOCATION BT_AUDIO_LOCATION_FRONT_LEFT              /* [TBD] mục 4.3 */

/* ---- Bộ điều khiển ứng dụng (main.c) ---- */
enum helmet_state { HS_BOOT, HS_IDLE, HS_CONNECTED, HS_RINGING, HS_IN_CALL, HS_SHUTDOWN };

enum helmet_evt_type {
	EVT_KEY_RAW,      /* a = phím, b = 1 nhấn / 0 nhả                     (buttons.c) */
	EVT_KEY_TIMER,    /* a = phím: hết giờ chờ của bộ nhận dạng cử chỉ     (buttons.c) */
	EVT_GESTURE,      /* a = phím, b = cử chỉ                               (buttons.c) */
	EVT_CONNECTED, EVT_DISCONNECTED, EVT_SECURED,
	EVT_STREAM,       /* a = hướng, b = 1 bắt đầu / 0 dừng                  (le_audio.c) */
	EVT_CALL,         /* a = trạng thái cuộc gọi tổng hợp                   (call_ctrl.c) */
	EVT_MIC_MUTE,     /* a = 1 tắt / 0 bật                                  (call_ctrl.c) */
	EVT_IDLE_TIMEOUT, EVT_ADV_SLOWDOWN,                                    /* (power.c) */
	EVT_TEMP_ALARM,   /* a = 1 vượt ngưỡng / 0 trở lại                      (thermo.c) */
	EVT_LOW_BATTERY,
};
struct helmet_evt { uint8_t type; uint8_t a; uint8_t b; uint8_t pad; uint32_t v; };
void app_post(const struct helmet_evt *evt);   /* mọi thread / ISR, không bao giờ chờ */
enum helmet_state app_state(void);

/* ---- buttons.c ---- */
enum helmet_key { KEY_VOL_UP, KEY_VOL_DOWN, KEY_CALL, KEY_POWER, KEY_COUNT };
enum helmet_gesture { G_SHORT, G_DOUBLE, G_LONG, G_VERY_LONG, G_REPEAT, G_COMBO_VOL };
void buttons_handle(const struct helmet_evt *evt); /* chỉ gọi từ bộ điều khiển */
void buttons_set_double(enum helmet_key key, bool enabled);

/* ---- audio_out.c (v2): âm báo + tham chiếu AEC ---- */
enum helmet_prompt { PROMPT_POWER_ON, PROMPT_POWER_OFF, PROMPT_CONNECTED, PROMPT_DISCONNECTED,
		     PROMPT_RING, PROMPT_MIC_MUTED, PROMPT_MIC_UNMUTED, PROMPT_VOL_LIMIT,
		     PROMPT_PAIRING, PROMPT_TEMP_ALARM, PROMPT_LOW_BATTERY, PROMPT_STATUS_OK,
		     PROMPT_STATUS_NO_LINK, PROMPT_COUNT };
void audio_out_prompt(enum helmet_prompt p);   /* không chờ; RING lặp tới khi stop */
void audio_out_prompt_stop(void);
bool audio_out_prompt_busy(void);
/* Đọc tham chiếu ở miền 16 kHz, căn theo vị trí mẫu micro; trả số mẫu thật có */
int audio_out_ref_read(int16_t *dst, uint16_t n, uint32_t mic_sample_pos);

/* ---- mic_in.c / voice_dsp.c / voice_tx.c ---- */
struct helmet_voice_stats {
	uint32_t mic_blocks, mic_err, sdu_sent, sdu_underrun, slip_drop, slip_insert;
	int16_t mic_dbfs, out_dbfs;     /* mức micro và mức gửi đi, dBFS × 10 */
	int16_t erle_db;                /* độ giảm tiếng vọng, dB × 10 */
	bool muted, aec_on;
};
int voice_init(void);
void voice_start(struct bt_bap_stream *stream, const struct helmet_stream_cfg *cfg);
void voice_stop(void);
void voice_on_sent(void);           /* từ bt_rx: chỉ gửi SDU đã mã hóa sẵn */
void voice_set_mute(bool mute);
void voice_get_stats(struct helmet_voice_stats *st);

/* ---- call_ctrl.c ---- */
enum helmet_call { CALL_NONE, CALL_INCOMING, CALL_OUTGOING, CALL_ACTIVE, CALL_HELD };
int call_ctrl_init(void);
void call_ctrl_connected(struct bt_conn *conn);  /* bắt đầu dò GTBS */
void call_ctrl_disconnected(void);
int call_ctrl_accept(void);
int call_ctrl_reject(void);
int call_ctrl_hangup(void);
int call_ctrl_mic_mute(bool mute);
enum helmet_call call_ctrl_state(void);
void call_ctrl_print_status(const struct shell *sh);

/* ---- power.c ---- */
enum power_off_reason { OFF_BUTTON, OFF_IDLE, OFF_LOW_BATTERY, OFF_SHELL };
int power_boot_check(void);         /* gọi đầu main(); có thể không trả về (tắt lại) */
void power_off(enum power_off_reason why);  /* không trả về */
void power_activity(void);          /* có hoạt động → reset hẹn giờ tự tắt */
void power_print_status(const struct shell *sh);

/* ---- thermo.c ---- */
struct helmet_temp { int32_t object_mc, ambient_mc; uint32_t age_ms; bool valid; };
int thermo_init(void);
void thermo_get(struct helmet_temp *out);
void thermo_print_status(const struct shell *sh);
```

Ghi chú: nhiệt độ truyền giữa các module bằng **milli-°C kiểu số nguyên** (`36 750` = 36.750 °C), để không phải đưa `double` qua các thread và log.

---

## 4. Chức năng 1: Đàm thoại (FR-20)

### 4.1 Yêu cầu chi tiết

| ID | Yêu cầu | Chỉ tiêu [Đề xuất] |
|---|---|---|
| FR-20.1 | Nhận giọng người bên kia (16_2 và 32_2) và phát ra cả hai loa | Như nghe nhạc |
| FR-20.2 | Thu 2 micro PDM, gửi lên điện thoại 16_2 hoặc 32_2 | Không mất SDU khi sóng tốt |
| FR-20.3 | Khử vọng | ERLE ≥ 25 dB khi chỉ người bên kia nói [Cần đo] |
| FR-20.4 | Giảm ồn nền ổn định (gió đều, động cơ) | Giảm ≥ 10 dB ồn nền, không méo giọng rõ |
| FR-20.5 | Tắt/bật micro từ điện thoại (MICP) và từ nút | Có hiệu lực trong ≤ 20 ms |
| FR-20.6 | Độ trễ chiều lên trong mũ (micro → SDU rời mũ) | ≤ 40 ms |
| FR-20.7 | Mất micro hoặc lỗi DSP không làm rớt cuộc gọi | Gửi im lặng, ghi log |

### 4.2 Luồng dữ liệu tổng

```
CHIỀU XUỐNG (điện thoại → loa)                        CHIỀU LÊN (micro → điện thoại)
──────────────────────────────                        ──────────────────────────────
CIS (sink ASE) → stream_recv → audio_q                IM69D130 L+R → PDM20 (16 000 Hz đúng)
 → audio_thread: giải mã LC3 (16/32 kHz mono)          → mic_in: dmic_read 10 ms
 → [1] bù trôi clock từng mẫu (vòng PCM)                → voice_dsp:
 → [2] trộn âm báo (chuông, tắt micro…)                     gộp 2 micro → lọc thông cao 100 Hz
 → ┬─────────────────────────────── tham chiếu ───────►     → AEC (NLMS) ◄── ref (16 kHz)
   │                                 (vòng 128 ms)          → NLP → giảm ồn → AGC
 → [3] lấy mẫu lại cố định 126:125                          → (tắt micro → 0)
 → I2S 15 873 Hz → MAX98357A → loa                      → voice_tx: bù trôi clock từng mẫu
                                                          → (32 kHz: nâng tần số ×2)
                                                          → LC3 encode → hàng SDU (≤ 3)
                                                        → sent (bt_rx) → bt_bap_stream_send
                                                        → CIS (source ASE) → điện thoại
```

Điểm thiết kế quan trọng nhất: **tham chiếu lấy giữa [2] và [3]**. Lý do ở mục 4.7.

### 4.3 Thay đổi `le_audio.c`

**PACS chiều gửi** [Đề xuất]:

```c
static const struct bt_audio_codec_cap lc3_src_cap = BT_AUDIO_CODEC_CAP_LC3(
	BT_AUDIO_CODEC_CAP_FREQ_16KHZ | BT_AUDIO_CODEC_CAP_FREQ_32KHZ,
	BT_AUDIO_CODEC_CAP_DURATION_10,                 /* chỉ 10 ms: đơn giản hóa DSP */
	BT_AUDIO_CODEC_CAP_CHAN_COUNT_SUPPORT(1), 40u, 80u, 1u,
	BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL);
```

- Chỉ 1 kênh: hai micro được gộp thành một.
- Chỉ khung 10 ms: mọi khối DSP làm việc theo khung 10 ms. Thêm 7.5 ms thì phải viết lại phần đệm. Rủi ro nhỏ: một số điện thoại thích 7.5 ms nhưng đều có 10 ms (16_2 là bắt buộc).
- Vị trí chiều gửi: `BT_AUDIO_LOCATION_FRONT_LEFT`, theo ví dụ `tmap_peripheral`. Cần thử với điện thoại thật [TBD].

**Khởi tạo** (`le_audio_init`):
- Đăng ký thêm `bt_pacs_cap_register(BT_AUDIO_DIR_SOURCE, &src_cap)`.
- Đặt location và contexts cho chiều SOURCE.
- `param.src_cnt = 1`.

**Ngữ cảnh khả dụng thay đổi theo trạng thái** [Đề xuất]: khi micro hỏng hoặc chưa có (`mic_in` báo lỗi), đặt *available contexts* của SOURCE về 0 bằng `bt_pacs_set_available_contexts`. Điện thoại sẽ giữ cuộc gọi trên máy thay vì chuyển sang mũ không nói được.

**Callback ASCS:**

| Callback | Thay đổi |
|---|---|
| `lc3_config` | Bỏ đoạn từ chối `dir != SINK`. Với SOURCE: kiểm tra 1 kênh, tần số 16/32 kHz, khung 10 ms, octet 40–80. Cấp `src_stream` riêng (không dùng mảng `streams[]` của chiều nhận). Trả `qos_pref_src` |
| `lc3_reconfig` | Như trên theo hướng |
| `lc3_enable` | Kiểm tra metadata có ngữ cảnh Conversational thì báo `EVT_STREAM` cho bộ điều khiển (để đổi xử lý nút nếu cần) |
| `lc3_start` | **Có tác dụng với SOURCE**: điện thoại đã sẵn sàng nhận. Ghi log; việc chính nằm ở `stream_started` |
| `stream_started` | SOURCE: `voice_start(stream, &cfg)`. SINK: như cũ |
| `stream_stopped` / `released` | SOURCE: `voice_stop()` |
| **`sent`** (mới, trong `stream_ops`) | SOURCE: `voice_on_sent()` |

**QoS ưa thích chiều gửi:** `BT_BAP_QOS_CFG_PREF(true, 2M, RTN 2, latency 10 ms, PD 20–40 ms)`. Presentation delay của chiều gửi tính từ lúc thu tới lúc SDU sẵn sàng. Ước lượng 10 ms khối PDM + ≤ 5 ms xử lý + 10 ms chờ trong hàng ≈ 25 ms nên PD tối thiểu nên ≥ 25 ms [Cần đo, chỉnh sau M3].

### 4.4 `mic_in.c`: thu micro

**Cấu hình PDM** (dùng lại tham số đã chạy trong `hw_test`):

| Trường | Giá trị | Ghi chú |
|---|---|---|
| `pcm_rate` | **16 000** | Lõi DSP luôn chạy 16 kHz (mục 4.5) |
| `pcm_width` | 16 | |
| Kênh | 2 (L, R) | `dmic_build_channel_map(0,0,LEFT) \| (1,0,RIGHT)` như `hw_test` |
| Xung PDM cho phép | 1.0–3.25 MHz, duty 40–60 % | Như `hw_test` |
| `block_size` | 640 B | 160 mẫu × 2 kênh × 2 B = 10 ms |
| Khối nhớ | 6 khối (`K_MEM_SLAB`) | 60 ms dự phòng |

**Tần số PDM thật** (tính theo thuật toán `nrfx_pdm_prescalers_calc` [SDK] với các tỉ lệ của nRF54L15: 32, 48, 50, 64, 80, 96, 100, 128; clock gốc PCLK32M):

| Yêu cầu | Tỉ lệ | Chia | Xung PDM | Tần số thật |
|---|---|---|---|---|
| 16 000 Hz | 80 | 32 MHz / 25 | 1.28 MHz | **16 000 Hz, đúng tuyệt đối** |
| 32 000 Hz | 50 | 32 MHz / 20 | 1.6 MHz | **32 000 Hz, đúng tuyệt đối** |

So với I2S (chỉ có tỉ lệ cố định nên lệch 0.8 %), micro **không lệch**. Điều này được dùng ở mục 4.7.

**Vòng lặp của `voice_tid`:**

```
khi chưa có luồng gửi: chờ semaphore "start" (PDM tắt, micro ngủ vì không có xung)
voice_start: dmic_configure + START, bỏ 5 khối đầu (~50 ms bộ lọc PDM ổn định)
lặp:
    dmic_read(timeout 30 ms) → khối 160 × L/R
        lỗi / hết giờ → mic_err++, dùng khối 0 (vẫn giữ nhịp gửi)
    mic_pos += 160                     (đếm mẫu micro tuyệt đối, dùng để căn tham chiếu)
    voice_dsp_process(L, R, mic_pos) → 160 mẫu đã xử lý
    voice_tx_push(160 mẫu)             → có thể mã hóa 1 SDU
    k_mem_slab_free(khối)
voice_stop: dmic_trigger STOP, xả hàng SDU
```

**Gộp 2 micro** [Đề xuất, theo giai đoạn]:
- Giai đoạn 1: trung bình L và R. Với ồn không tương quan giữa hai micro (gió), SNR tăng tới 3 dB.
- Giai đoạn 2: mỗi băng tần chọn micro ít ồn hơn (giảm ồn gió hai micro). Chỉ làm khi đã biết vị trí lắp micro trong mũ [TBD].

### 4.5 `voice_dsp.c`: xử lý giọng

**Nguyên tắc:**
- Mã C thuần, không gọi API Zephyr. Biên dịch được trên PC (gcc + CMSIS-DSP bản PC hoặc hàm thay thế) để chạy offline trên dữ liệu ghi thật (mục 4.10).
- Lõi chạy **16 kHz**, khung 160 mẫu. Nếu điện thoại chọn 32_2, `voice_tx` nâng tần số ×2 sau DSP. Nội dung chỉ tới 8 kHz nhưng định dạng đúng.
  - Lý do: chạy AEC ở 32 kHz cần 512 hệ số × 32 000 mẫu/s, gấp 4 lần CPU (khoảng 40–50 %), không đáng với giọng nói trong mũ ồn.

**Chuỗi xử lý mỗi khung 10 ms:**

| # | Khâu | Thuật toán | Tham số đầu [Đề xuất] |
|---|---|---|---|
| 1 | Gộp micro | (L + R) / 2 | |
| 2 | Lọc thông cao | Biquad bậc 2 (`arm_biquad_cascade_df1_f32`) | 100 Hz, Q 0.707: cắt ồn gió/rung tần thấp |
| 3 | Đọc tham chiếu | `audio_out_ref_read(ref, 160, mic_pos − D)` | `D` = độ trễ khối (mục 4.7) |
| 4 | AEC | NLMS (`arm_lms_norm_f32`) | L = 256 (16 ms), μ = 0.1–0.3 |
| 5 | Phát hiện cả hai cùng nói | Geigel: nếu \|d\| > 0.5 · max\|x\| trong 16 ms → **ngừng cập nhật** 30 ms | |
| 6 | NLP | Ước lượng ERLE; khi năng lượng ra / năng lượng tham chiếu thấp và người bên kia đang nói → giảm 12–20 dB, chuyển mượt 10 ms | |
| 7 | Giảm ồn | Phổ: cửa sổ 20 ms, bước 10 ms, FFT 512 (`arm_rfft_fast_f32`); ước lượng ồn bằng *minimum statistics* 1.5 s; độ lợi Wiener, sàn −15 dB; làm mượt theo thời gian | |
| 8 | AGC | Mục tiêu −20 dBFS giọng; độ lợi tối đa +12 dB; tấn công 10 ms, nhả 300 ms; không tăng khi chỉ có ồn | |
| 9 | Tắt micro | Nhân 0 sau AGC. AEC vẫn chạy để không mất hội tụ | |
| 10 | Đo | `mic_dbfs`, `out_dbfs`, `erle_db` cho heartbeat | |

**Thứ tự triển khai** (giảm rủi ro):
- **Bước A (M3)**: chỉ khâu 1, 2, 8, 9, cộng một **bộ triệt vọng bán song công**: khi tham chiếu có tiếng mà micro không vượt tham chiếu → giảm micro 20 dB. Rẻ, chắc chắn hết vọng, nhưng nói chen thì bị nuốt chữ. Đủ để gọi được.
- **Bước B (M4)**: thêm khâu 3–7. Chỉnh tham số bằng dữ liệu thật ghi qua RTT.
- **Phương án dự phòng** nếu bước B không đạt: chuyển mã nguồn mở có sẵn (SpeexDSP: AEC MDF + preprocess, giấy phép BSD; hoặc WebRTC AECM bản dấu phẩy tĩnh, BSD). Phải đánh giá RAM/CPU trên PC trước khi chọn.

**Bộ nhớ** (float 32 bit):

| Mảng | Kích thước |
|---|---|
| Hệ số NLMS + trạng thái (256 + 256 + 160) | ~2.7 KB |
| Vòng tham chiếu 16 kHz, 128 ms (int16) | 4 KB |
| Giảm ồn: cửa sổ, FFT vào/ra, phổ ồn, độ lợi (512 + 512 + 257 × 3) | ~7 KB |
| Biquad, AGC, NLP | < 0.2 KB |
| Tạm cho khung | ~2 KB |

### 4.6 `voice_tx.c`: gửi lên điện thoại

**Vấn đề nhịp:** micro lấy mẫu theo **clock của mũ**, còn CIS gửi theo nhịp sự kiện ISO do **điện thoại** (central) quyết định. Hai clock lệch tối đa khoảng ±50 ppm. Ở 16 kHz, mỗi giây lệch 0.8 mẫu, tức khoảng 1.25 s lệch 1 mẫu.

**Thiết kế:**

```
voice_dsp ──160 mẫu/10 ms (clock mũ)──► VÒNG PCM (40 ms) ──160 ± 1 mẫu──► bộ mã hóa
                                             │ mức đầy
                                             ▼
                               mức > mục tiêu + 32 mẫu → khung này đọc 161 mẫu, nội suy về 160 (bỏ 1)
                               mức < mục tiêu − 32 mẫu → đọc 159 mẫu, nội suy về 160 (chèn 1)
LC3 encode (16 hoặc 32 kHz) ──► net_buf ──► HÀNG SDU (k_fifo, tối đa 3) ──► sent: gửi 1
```

- **Ai mã hóa**: `voice_tid`, ngay sau khi DSP xong một khung. Không mã hóa trong `bt_rx`.
- **Ai gửi**: `voice_on_sent()` chạy trong `bt_rx` mỗi khi controller báo đã gửi xong một SDU. Hàm lấy SDU kế tiếp khỏi hàng và gọi `bt_bap_stream_send(stream, buf, seq_num++)`. Việc này rất nhẹ.
- **Mồi**: khi `voice_start`, mã hóa sẵn 2 khung im lặng và gửi ngay 2 SDU, để controller luôn có dữ liệu. Sau đó mỗi `sent` gửi một SDU, nên nhịp gửi tự khóa theo nhịp CIS.
- **Thiếu dữ liệu** (hàng SDU rỗng khi `sent` gọi): gửi khung im lặng mã hóa sẵn, tăng `sdu_underrun`, giữ `seq_num` liên tục. Không bao giờ để controller không có gì gửi.
- **Thừa dữ liệu** (hàng đầy 3): bỏ SDU cũ nhất, tăng `slip_drop`. Lẽ ra không xảy ra nhờ cơ chế bù trôi trong vòng PCM.
- **Kho bộ đệm:** `NET_BUF_POOL_FIXED_DEFINE(tx_pool, 4, BT_ISO_SDU_BUF_SIZE(80), …)`, giống ví dụ `bap_unicast_server` [SDK].
- **32 kHz**: nâng tần số ×2 bằng bộ lọc nửa băng FIR 31 hệ số trước khi mã hóa.
- **Nội suy 161→160 / 159→160**: nội suy tuyến tính trên một khung. Với 1 mẫu mỗi 1.25 s, tai không phân biệt được.

**Xử lý lỗi:**

| Tình huống | Xử lý |
|---|---|
| `bt_bap_stream_send` trả lỗi | Trả buf về kho, tăng bộ đếm, lần `sent` sau thử lại. Lỗi liên tục quá 50 lần thì `voice_stop` và ghi log |
| Mất kết nối giữa chừng | `stream_stopped` → `voice_stop`: dừng PDM, xả hàng |
| Điện thoại đổi codec giữa cuộc gọi | `lc3_reconfig` → khi `started` lại thì `voice_start` khởi tạo lại bộ mã hóa |

### 4.7 `audio_out.c` v2: tiền đề cho đàm thoại

**Bài toán clock:**

| Tín hiệu | Clock | Tần số thật |
|---|---|---|
| Âm thanh từ điện thoại | Clock điện thoại | 16 000 × (1 ± 50 ppm) |
| I2S ra loa (nominal 16 kHz) | Thạch anh mũ / 63 / 32 | **15 873.016 Hz** = 16 000 × 125/126 |
| I2S (nominal 48 kHz) | Thạch anh mũ / 21 / 32 | **47 619.05 Hz** = 48 000 × 125/126 |
| I2S (nominal 32 kHz) | Thạch anh mũ / 31 / 32 | **32 258.06 Hz** = 32 000 × 125/124 |
| Micro | Thạch anh mũ | **16 000 Hz đúng** |

**Thiết kế ba tầng** (thay cho "bỏ/lặp nguyên khung" hiện tại):

```
giải mã ─► [1] VÒNG PCM + BÙ TRÔI TỪNG MẪU ─► [2] TRỘN ÂM BÁO ─► (điểm tham chiếu) ─► [3] LẤY MẪU LẠI CỐ ĐỊNH ─► I2S
           tốc độ điện thoại → tốc độ danh định       (âm lượng áp ở đây)                fs × 125/126 (hoặc 125/124)
           theo mức đầy, 1 mẫu mỗi lần                                                   bước cố định, chính xác tuyệt đối
```

1. **Tầng 1: bù trôi clock điện thoại.**
   - Giải mã ghi vào vòng PCM. Khi lấy ra một khung, so mức đầy với mục tiêu (2 khung).
   - Lệch quá ±0.2 khung thì khung này lấy thừa hoặc thiếu đúng 1 mẫu và nội suy về độ dài chuẩn.
   - Thay việc bỏ cả 10 ms (tiếng tách) bằng 1 mẫu (không nghe thấy).
2. **Tầng 2: trộn âm báo** (mục 6.8) và áp âm lượng.
3. **Tầng 3: lấy mẫu lại tỉ lệ cố định**, chỉ để bù việc I2S không ra đúng tần số.
   - Nội suy Hermite bậc 3, bộ đếm pha **số nguyên** theo phân số chính xác (ví dụ bước 126/125), nên không bao giờ tích lũy sai số.
   - Chi phí khoảng 10 phép tính mỗi mẫu mỗi kênh, dưới 1 % CPU ở 48 kHz stereo.

**Vì sao tham chiếu lấy giữa tầng 2 và tầng 3:**
- Từ điểm đó tới loa chỉ còn tầng 3, có tỉ lệ cố định **chính xác** và chạy cùng thạch anh với micro. Mẫu tham chiếu thứ `k` ra loa vào đúng thời điểm `t0 + k/16 000` theo clock mũ, và micro lấy mẫu thứ `m` vào `t0' + m/16 000` cũng theo clock mũ. Độ trễ giữa hai dòng là **hằng số** suốt cuộc gọi.
- Mọi sự "co giãn" do điện thoại lệch clock nằm ở tầng 1, **trước** điểm tham chiếu, nên AEC không bị ảnh hưởng.
- Âm báo (chuông, tiếng tắt micro) cũng đi ra loa nên cũng vào tham chiếu và được khử.

**Đo độ trễ khối `D`:**
- Ghi `ref_pos` (chỉ số mẫu tham chiếu) và `mic_pos` tại cùng một thời điểm (`k_cycle_get_32`) lúc I2S bắt đầu và lúc PDM bắt đầu. Cộng độ trễ cố định của hàng đợi I2S (số khối nạp trước × 160) và của PDM [Cần đo].
- Kiểm tra lại bằng tương quan chéo trong 2 s đầu cuộc gọi. AEC chỉ cần phủ phần dư (đường âm) bằng 256 hệ số.

**Khi điện thoại gửi 32 kHz xuống:** tham chiếu ở 32 kHz được hạ ×2 (lọc nửa băng) trước khi ghi vào vòng tham chiếu 16 kHz.

**Kết quả phụ:** sửa luôn lỗi `sync -8/+0` mỗi 10 s khi nghe nhạc. Heartbeat đổi `sync` thành `slip -x/+y` (số mẫu).

### 4.8 MICP: tắt/bật micro

- Khởi tạo: `bt_micp_mic_dev_register(&param)` với `param.cb->mute = mic_mute_cb` [SDK].
- `mic_mute_cb(mute)` được gọi cả khi điện thoại đổi lẫn khi mũ tự đổi. Callback chỉ đăng `EVT_MIC_MUTE` lên bộ điều khiển.
- Bộ điều khiển xử lý: `voice_set_mute(mute == BT_MICP_MUTE_MUTED)`, rồi phát `PROMPT_MIC_MUTED` hoặc `PROMPT_MIC_UNMUTED`.
- Nút (mục 5.6) gọi `call_ctrl_mic_mute(true/false)` → `bt_micp_mic_dev_mute()` / `unmute()`. Stack thông báo cho điện thoại và gọi lại `mic_mute_cb`, nên chỉ có **một đường xử lý**, giống cách VCP đang làm với âm lượng.
- Micro hỏng: `bt_micp_mic_dev_mute_disable()` để điện thoại biết không thể bật micro.

### 4.9 Trình tự: điện thoại chuyển cuộc gọi sang mũ

| # | Điện thoại | Helmet | Log dự kiến |
|---|---|---|---|
| 1 | (đang nghe nhạc 48 kHz) Disable + Release ASE sink nhạc | `stream_stopped` → `audio_out_stream_stop` | `ASE 0 stopped` |
| 2 | Config Codec ASE sink: 32_2, Conversational | `lc3_config` (SINK) | `Config ASE 0: LC3 32000 Hz, 10000 us, 1 ch …` |
| 3 | Config Codec ASE source: 32_2 | `lc3_config` (SOURCE) | `Config SRC: LC3 32000 Hz …` |
| 4 | Config QoS cả hai (cùng CIG, cùng CIS) | `lc3_qos` × 2 | |
| 5 | Enable cả hai, metadata Conversational | `lc3_enable` × 2 → `EVT_STREAM` | |
| 6 | Tạo CIS | SINK: tự sẵn sàng → `stream_started` → `audio_out_stream_start` (I2S 32 kHz → 32 258 Hz, tầng 3 = 125/124) | `Streaming ASE 0`, `I2S configured …` |
| 7 | Receiver Start Ready cho SOURCE | `lc3_start` → `stream_started` → `voice_start` | `Voice TX start: 32 kHz, 80 B, mic 16 kHz → ×2` |
| 8 | | `voice_tid`: PDM bắt đầu, bỏ 50 ms, mồi 2 SDU | |
| 9 | Nhận SDU mỗi 10 ms | `sent` → gửi SDU kế tiếp | heartbeat `voice: sent …` |
| 10 | Kết thúc cuộc gọi: Disable/Release cả hai | `voice_stop`, `audio_out_stream_stop` | |

### 4.10 Chỉnh tham số AEC bằng dữ liệu thật

Dùng lại hạ tầng RTT đã có:
- `capture.c` thêm hai loại bản ghi: `MIC (4)` = 160 mẫu micro sau gộp và `REF (5)` = 160 mẫu tham chiếu đã căn, kèm `mic_pos`. Bật bằng `helmet capture voice`.
- Thông lượng: 2 × 32 KB/s = 64 KB/s, gấp đôi mức capture nhạc lớn nhất đã chạy ổn (33 KB/s) [Cần đo]. Không đủ thì ghi micro + tham chiếu ở 8 kHz cho bước chỉnh thô.
- `scripts/helmet_audio.py --voice` lưu `mic.wav` + `ref.wav`. Một script mới `scripts/aec_lab.py` chạy chính `voice_dsp.c` (biên dịch thành thư viện `.so` cho PC, gọi qua `ctypes`) trên hai file đó, xuất `out.wav` và ERLE theo thời gian.
- Nhờ vậy chỉnh μ, L, ngưỡng NLP và giảm ồn trên PC rồi mới nạp mạch, không cần gọi điện thử hàng trăm lần.

### 4.11 Kiểm thử chức năng 1

| ID | Mục tiêu | Cách làm | Đạt khi |
|---|---|---|---|
| T-VOICE-01 | Clock micro | `helmet voice` khi đang thu: đếm khối trong 60 s | 6 000 ± 1 khối |
| T-VOICE-02 | Chiều lên không AEC | Gọi điện, nói trong phòng yên tĩnh | Người bên kia nghe rõ; `sdu_underrun` = 0 sau 5 phút |
| T-VOICE-03 | Bù trôi chiều lên | Gọi 30 phút | `slip` tăng đều, không có `sdu_underrun` / `slip_drop` |
| T-VOICE-04 | Tham chiếu cố định | Ghi MIC/REF, tương quan chéo đầu và cuối 10 phút | Đỉnh tương quan không dịch quá 1 mẫu |
| T-VOICE-05 | ERLE | Chỉ người bên kia nói 30 s | ERLE ≥ 25 dB sau 2 s hội tụ |
| T-VOICE-06 | Cả hai cùng nói | Hai bên đọc đoạn văn cùng lúc | Câu của người đội mũ không mất chữ (đánh giá nghe) |
| T-VOICE-07 | Giảm ồn | Quạt gió vào mũ, đo `out_dbfs` khi im lặng trước/sau bật giảm ồn | Giảm ≥ 10 dB |
| T-VOICE-08 | MICP | Bấm tắt micro trên điện thoại; rồi tắt bằng nút | Người kia không nghe; biểu tượng điện thoại đổi theo cả hai cách |
| T-VOICE-09 | CPU | `helmet perf` khi gọi | Tổng < 50 % |
| T-VOICE-10 | Không micro | Rút board micro, gọi | Cuộc gọi không rớt, `mic_err` tăng, điện thoại nhận im lặng |

---

## 5. Chức năng 2: Xử lý cuộc gọi và cử chỉ nút (FR-21)

### 5.1 Yêu cầu chi tiết

| ID | Yêu cầu |
|---|---|
| FR-21.1 | Biết có cuộc gọi đến, đổ chuông trong mũ (chuông của điện thoại qua LE Audio, hoặc chuông cục bộ) |
| FR-21.2 | Nghe máy, từ chối, kết thúc bằng nút Call |
| FR-21.3 | Tắt/bật micro bằng nút khi đang gọi |
| FR-21.4 | Vol+/Vol− chỉnh âm lượng cuộc gọi; giữ nút thì tăng/giảm liên tục |
| FR-21.5 | Nhận dạng: nhấn ngắn, nhấn đúp, giữ, giữ rất lâu, tổ hợp hai nút |
| FR-21.6 | Mọi cử chỉ có phản hồi âm thanh khi không có tác dụng hiển nhiên (tắt micro, âm lượng tối đa…) |

### 5.2 `call_ctrl.c`: thiết kế

**Dò dịch vụ sau khi mã hóa, theo thứ tự:**

```
security_changed → media_ctrl_connected (dò MCS) ──xong/thất bại──► call_ctrl_connected (dò GTBS)
```

Hai lần dò GATT được **nối tiếp**, không chạy song song: tránh hai client cùng chiếm bộ đệm ATT, và log dễ đọc. `media_ctrl` báo xong bằng một callback mới (`media_ctrl_set_done_cb`).

**Dữ liệu:**

```c
struct call_info { uint8_t index; uint8_t state; uint8_t flags; };
static struct call_info calls[2];        /* CONFIG_BT_TBS_CLIENT_MAX_CALLS = 2 (mặc định 1) */
static uint8_t call_count;
static enum helmet_call agg;             /* trạng thái tổng hợp */
static bool gtbs_ready;
```

**Callback** (chạy trong `bt_rx`, chỉ chép dữ liệu rồi đăng sự kiện):

| Callback TBS | Việc |
|---|---|
| `discover(conn, err, tbs_count, gtbs_found)` | `gtbs_ready = gtbs_found && !err`. Không có GTBS (ví dụ Radxa/BlueZ) → ghi cảnh báo, các cử chỉ cuộc gọi bị bỏ qua |
| `call_state(conn, err, inst, count, states[])` | Chép danh sách, tính `agg`, đổi thì `app_post(EVT_CALL, agg)` |
| `termination_reason(…, call_index, reason)` | Ghi log lý do (bảng 5.3) |
| `accept_call` / `terminate_call` (kết quả lệnh) | Lỗi → ghi log, phát âm báo lỗi |
| `remote_uri`, `friendly_name` | Ghi log `Incoming call from …` |

**Tính trạng thái tổng hợp** (ưu tiên từ trên xuống):

| Có ít nhất một cuộc ở trạng thái | `agg` |
|---|---|
| INCOMING | `CALL_INCOMING` |
| ACTIVE | `CALL_ACTIVE` |
| DIALING, ALERTING | `CALL_OUTGOING` |
| một trong các trạng thái HELD | `CALL_HELD` |
| (danh sách rỗng) | `CALL_NONE` |

**Chọn `call_index` cho lệnh:**
- `accept` / `reject`: cuộc ở trạng thái INCOMING.
- `hangup`: cuộc ACTIVE; không có thì cuộc OUTGOING (hủy cuộc đang quay số).
- Hai cuộc cùng lúc (cuộc chờ): nhấn ngắn khi có INCOMING + ACTIVE thì **nghe cuộc mới** (điện thoại tự giữ cuộc cũ). Đây là hành vi của đa số tai nghe [Đề xuất].

### 5.3 Máy trạng thái cuộc gọi trong bộ điều khiển

| Trạng thái thiết bị | Sự kiện | Hành động | Trạng thái mới |
|---|---|---|---|
| CONNECTED | `EVT_CALL(INCOMING)` | Bật cử chỉ đúp cho Call = tắt; sau 1 s nếu chưa có luồng Ringtone/Conversational nào thì `audio_out_prompt(PROMPT_RING)` (lặp) | RINGING |
| RINGING | Call nhấn ngắn | `call_ctrl_accept()` | (chờ TBS) |
| RINGING | Call giữ 1 s | `call_ctrl_reject()` | (chờ TBS) |
| RINGING | `EVT_CALL(ACTIVE)` | `audio_out_prompt_stop()`; bật cử chỉ đúp cho Call | IN_CALL |
| RINGING | `EVT_CALL(NONE)` | Dừng chuông (người gọi bỏ / đã từ chối) | CONNECTED |
| IN_CALL | Call nhấn ngắn | `call_ctrl_hangup()` | (chờ TBS) |
| IN_CALL | Call nhấn đúp | Đảo tắt/bật micro | IN_CALL |
| IN_CALL | `EVT_CALL(NONE)` | Bật lại micro nếu đang tắt (cuộc sau bắt đầu ở trạng thái bật) | CONNECTED |
| bất kỳ | `EVT_DISCONNECTED` | Xóa trạng thái cuộc gọi, dừng chuông | IDLE |

**Lý do kết thúc** (log):

| Mã | Ý nghĩa |
|---|---|
| `0x02` REMOTE_ENDED_CALL | Người kia cúp |
| `0x04` LINE_BUSY | Máy bận |
| `0x06` CLIENT_TERMINATED | Chính mũ kết thúc |
| `0x08` NO_ANSWER | Không trả lời |
| khác | In số |

**Chuông:**
- Khi có cuộc gọi đến, điện thoại có thể phát chuông qua LE Audio (ngữ cảnh Ringtone, đã thêm vào danh sách nhận).
- Nếu sau 1 s không có luồng nào được mở, mũ tự phát chuông cục bộ. Một số điện thoại chỉ báo cuộc gọi qua TBS mà không phát chuông [Cần đo trên điện thoại mục tiêu].

### 5.4 Trình tự: cuộc gọi đến, nghe máy, kết thúc

```
Điện thoại                              Helmet
──────────                              ──────
GTBS notify: call #1 INCOMING ───────► call_state → EVT_CALL(INCOMING) → RINGING
(có thể) mở ASE Ringtone ────────────► audio_out phát chuông của điện thoại
                                        hoặc sau 1 s: PROMPT_RING cục bộ
                                        người đội mũ nhấn Call (ngắn)
                                        → buttons → G_SHORT(KEY_CALL) → call_ctrl_accept()
◄──── GTBS CP write: Accept #1 ───────
GTBS notify: call #1 ACTIVE ─────────► EVT_CALL(ACTIVE) → dừng chuông → IN_CALL
Config/Enable ASE sink+source ───────► (mục 4.9) voice_start
... đang gọi ...                        nhấn Call (ngắn) → call_ctrl_hangup()
◄──── GTBS CP write: Terminate #1 ────
GTBS notify: danh sách rỗng ─────────► EVT_CALL(NONE) → CONNECTED
termination_reason 0x06 ─────────────► log "call ended by headset"
Disable/Release ASE ─────────────────► voice_stop
```

### 5.5 `buttons.c`: nhận dạng cử chỉ

**Vì sao không dùng node `zephyr,input-longpress` / `zephyr,input-double-tap`** [SDK, đã đọc binding]:
- Hai node này chỉ biến một mã phím thành mã khác, ngưỡng thời gian cố định trong devicetree. Không đổi được theo trạng thái: nhấn đúp chỉ nên chờ khi trạng thái hiện tại *có* hành động nhấn đúp, nếu không thì nhấn ngắn bị trễ vô ích 350 ms.
- Không nhận dạng được **tổ hợp hai nút**.
- Node double-tap chuyển tiếp sự kiện gốc, nên ứng dụng vẫn nhận "nhấn" của lần đầu và không phân biệt được nhấn đơn với nửa đầu của nhấn đúp nếu không tự chờ.

Vì vậy nhận dạng làm trong ứng dụng, còn `gpio-keys` vẫn lo chống dội.

**Luồng sự kiện:**

```
gpio-keys (chống dội 30 ms) → on_input (thread input): app_post(EVT_KEY_RAW{phím, nhấn/nhả})
k_timer của từng phím hết giờ (ISR)                      : app_post(EVT_KEY_TIMER{phím})
bộ điều khiển (main) → buttons_handle(evt) → máy trạng thái từng phím → app_post(EVT_GESTURE)
```

Mọi xử lý nằm trong **một thread** (bộ điều khiển), nên không cần khóa.

**Thông số** [Đề xuất]:

| Thông số | Giá trị | Ý nghĩa |
|---|---|---|
| `T_SHORT_MAX` | 600 ms | Nhả trước mốc này là nhấn ngắn |
| `T_DOUBLE_GAP` | 350 ms | Lần nhấn thứ hai phải bắt đầu trong khoảng này sau lần nhả đầu |
| `T_LONG` | 1 000 ms | Giữ tới mốc này: phát `G_LONG` **ngay lúc đó** (không chờ nhả) |
| `T_VERY_LONG` | 3 000 ms | Giữ tới mốc này: phát `G_VERY_LONG` |
| `T_REPEAT_START`, `T_REPEAT` | 500 ms, 250 ms | Vol+/Vol− giữ: lặp `G_REPEAT` |
| `T_COMBO` | 5 000 ms | Vol+ và Vol− cùng giữ |

**Máy trạng thái của một phím:**

```
 IDLE ──nhấn──► DOWN1 (hẹn T_LONG; Vol: hẹn T_REPEAT_START)
 DOWN1 ──nhả trước T_SHORT_MAX──► nếu phím có "đúp" trong trạng thái hiện tại:
                                      WAIT2 (hẹn T_DOUBLE_GAP)
                                  ngược lại: phát G_SHORT → IDLE
 DOWN1 ──hết giờ T_LONG──► phát G_LONG → HELD (hẹn T_VERY_LONG − T_LONG)
 HELD  ──hết giờ──► phát G_VERY_LONG → HELD2 (chờ nhả, không phát gì thêm)
 HELD/HELD2 ──nhả──► IDLE
 WAIT2 ──hết giờ──► phát G_SHORT → IDLE
 WAIT2 ──nhấn──► DOWN2 ──nhả──► phát G_DOUBLE → IDLE
 (Vol) DOWN1 ──hết giờ lặp──► phát G_REPEAT, hẹn T_REPEAT …
```

- Nhả trong khoảng `T_SHORT_MAX`…`T_LONG` (600–1 000 ms): không phát cử chỉ nào. Khoảng "mơ hồ" này tránh nhầm giữa nhấn ngắn và nhấn giữ.
- Vol+/Vol− không có `G_LONG`/`G_VERY_LONG`: giữ là lặp (`G_REPEAT`); đã lặp ít nhất một lần thì lúc nhả không phát `G_SHORT`.

**Tổ hợp:**
- Khi Vol+ và Vol− cùng ở trạng thái nhấn (khoảng cách hai lần nhấn ≤ 200 ms), chuyển cả hai sang chế độ COMBO, **không** phát cử chỉ đơn nào.
- Giữ đủ `T_COMBO` thì phát `G_COMBO_VOL`. Nhả sớm thì không phát gì.

**Kiểm thử không cần phần cứng:** máy trạng thái là hàm thuần nhận (sự kiện, thời điểm) và trả về cử chỉ. Có thể biên dịch và chạy kiểm thử đơn vị trên PC bằng `twister` (`native_sim`) hoặc một file test gcc đơn giản.

**Lưu ý phần cứng:** ISS-07, thứ tự chân connector giữa mạch chính và board nút khác nhau. Khi nối board nút, dùng log `KEY_RAW` để xác nhận từng nút trước khi thử cử chỉ.

### 5.6 Bảng hành động theo trạng thái [Đề xuất, chủ dự án cần duyệt]

| Phím / cử chỉ | IDLE (chưa kết nối) | CONNECTED (nhạc / rảnh) | RINGING | IN_CALL |
|---|---|---|---|---|
| **Call** ngắn | Âm "chưa kết nối" | Play / Pause (MCP) | **Nghe máy** | **Kết thúc** |
| **Call** đúp | — | Bài kế tiếp (MCP `NEXT_TRACK`) | — | **Tắt/bật micro** |
| **Call** giữ 1 s | — | (dự phòng) | **Từ chối** | — |
| **Vol+** ngắn / giữ | Tăng âm lượng | Tăng | Tăng chuông | Tăng |
| **Vol−** ngắn / giữ | Giảm | Giảm | Giảm chuông | Giảm |
| **Power** ngắn | Âm trạng thái (chưa kết nối) | Âm trạng thái (đã kết nối) | — | — |
| **Power** giữ 3 s | **Tắt máy** | **Tắt máy** | Từ chối rồi tắt | Kết thúc rồi tắt |
| **Vol+ + Vol−** giữ 5 s | **Xóa mọi ghép đôi** + âm ghép đôi | Ngắt kết nối, xóa ghép đôi | — | — |

- Âm lượng đã ở mức tối đa hoặc tối thiểu thì phát `PROMPT_VOL_LIMIT`.
- Mỗi cử chỉ hợp lệ gọi `power_activity()` để hẹn giờ tự tắt đếm lại.
- Bật máy (giữ Power 2 s khi đang OFF) do `power.c` xử lý lúc khởi động (mục 6.3), không qua `buttons.c`.

### 5.7 Kiểm thử chức năng 2

| ID | Mục tiêu | Cách làm | Đạt khi |
|---|---|---|---|
| T-BTN-01 | Phân biệt cử chỉ | Mỗi cử chỉ 20 lần trên mỗi nút | 100 % đúng loại, log `GESTURE` |
| T-BTN-02 | Không trễ thừa | Vol+ ngắn khi CONNECTED | Âm lượng đổi < 100 ms sau khi nhả |
| T-BTN-03 | Lặp | Giữ Vol+ 3 s | Khoảng 11 bước (1 sau 500 ms + 1 mỗi 250 ms) |
| T-BTN-04 | Tổ hợp | Vol+ + Vol− giữ 5 s, rồi giữ 3 s | Lần đầu xóa ghép đôi, lần sau không làm gì |
| T-CALL-01 | Nhận biết cuộc gọi đến | Gọi tới điện thoại | `EVT_CALL INCOMING`, nghe chuông trong ≤ 1.5 s |
| T-CALL-02 | Nghe máy | Call ngắn khi đổ chuông | Cuộc gọi kết nối, có âm hai chiều |
| T-CALL-03 | Từ chối | Call giữ 1 s | Người gọi bị ngắt / chuyển hộp thư |
| T-CALL-04 | Kết thúc | Call ngắn khi đang gọi | Cuộc gọi kết thúc, log lý do 0x06 |
| T-CALL-05 | Người gọi bỏ | Người gọi cúp khi đang đổ chuông | Chuông dừng, về CONNECTED |
| T-CALL-06 | Cuộc chờ | Có cuộc thứ hai khi đang gọi, Call ngắn | Nghe cuộc mới |
| T-CALL-07 | Điện thoại không có GTBS | Kết nối với Radxa | Log cảnh báo, nút Call vẫn play/pause |

---

## 6. Chức năng 3: Nguồn, bật/tắt, ngủ (FR-23)

### 6.1 Ràng buộc phần cứng

Từ `docs/PCB charger schematic.pdf`:
- **AP2112K-3.3: chân EN nối thẳng VIN.** LDO luôn bật khi có pin, không có công tắc nguồn và không có mạch tự giữ nguồn.
- **TP4056: CHRG, STDBY chỉ nối LED.** MCU không biết đang sạc hay đầy.
- **Bat+ không về MCU.** Không đo được điện áp pin trực tiếp.

Hệ quả:
- "Tắt máy" = **System OFF** của nRF54L15. Chip gần như không tiêu thụ, nhưng LDO và các IC khác vẫn được cấp điện.
- Không có sự kiện "cắm sạc" để thức dậy.

### 6.2 Máy trạng thái nguồn

Dùng chung máy trạng thái thiết bị ở mục 1.6. `power.c` lo bốn việc:
1. quyết định lúc khởi động (mục 6.3);
2. quy trình tắt (mục 6.4);
3. hai bộ hẹn giờ: tự tắt và giảm quảng bá (mục 6.5);
4. lưu/khôi phục cài đặt (mục 6.6).

### 6.3 Quyết định lúc khởi động: `power_boot_check()`

Gọi **đầu tiên** trong `main()`, trước `audio_out_init` và `bt_enable`, để nếu phải tắt lại thì không tốn thời gian và điện bật Bluetooth.

| Nguyên nhân reset (`hwinfo`) | Ý nghĩa | Hành động [Đề xuất] |
|---|---|---|
| `RESET_LOW_POWER_WAKE` | Thức từ System OFF do nút Power | Đọc nút mỗi 20 ms: còn giữ đủ **2 s** (tính từ lúc reset) thì bật; nhả sớm thì `power_off` im lặng (không âm báo). Tránh bật nhầm do va chạm trong túi |
| `RESET_POR` | Vừa lắp pin / nguồn lên lần đầu | Bật (thuận tiện khi sản xuất và nạp firmware) |
| `RESET_PIN`, `RESET_SOFTWARE`, `RESET_DEBUG` | Reset bằng nút, phần mềm, J-Link | Bật |
| `RESET_WATCHDOG`, `RESET_CPU_LOCKUP` | Lỗi | Bật, ghi log cảnh báo |

Sau khi quyết định bật:
- `hwinfo_clear_reset_cause()`, để lần reset sau đọc đúng.
- Phát `PROMPT_POWER_ON` khi `audio_out` sẵn sàng.
- **Bỏ qua phím Power cho tới lần nhả đầu tiên.** Người dùng vẫn đang giữ nút để bật; nếu không chặn, `buttons.c` sẽ đếm tiếp và phát `G_VERY_LONG` ở mốc 3 s, tức là **tắt máy ngay sau khi bật**.

### 6.4 Quy trình tắt: `power_off(why)`

Chạy trong bộ điều khiển. Hàm **không trả về**.

| # | Bước | Chi tiết | Giới hạn thời gian |
|---|---|---|---|
| 1 | Chuyển SHUTDOWN | Bỏ qua mọi cử chỉ nút sau thời điểm này | |
| 2 | Kết thúc cuộc gọi | Nếu IN_CALL: `call_ctrl_hangup()`; RINGING: `call_ctrl_reject()` | chờ ≤ 500 ms |
| 3 | Âm báo | `PROMPT_POWER_OFF` (3 nốt đi xuống). Chờ `audio_out_prompt_busy()` về false. Riêng tắt do hẹn giờ: vẫn phát | ≤ 800 ms |
| 4 | Ngắt kết nối | `bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_POWER_OFF)` (`0x15`: điện thoại hiểu là mũ tắt nguồn, không cố kết nối lại ngay); chờ `EVT_DISCONNECTED` | ≤ 1 s |
| 5 | Dừng quảng bá | `bt_le_ext_adv_stop(adv)` | |
| 6 | Dừng ngoại vi âm thanh | `voice_stop`, `audio_out_shutdown()`: dừng I2S và PDM, ampli và micro không còn xung clock (dòng còn lại: mục 6.7) | |
| 7 | Dừng cảm biến | `thermo` đưa MLX90632 về chế độ ngủ (mục 7.3) | |
| 8 | Lưu cài đặt | `settings_save_one` cho các khóa ở mục 6.6 nếu đã thay đổi | |
| 9 | Chờ nhả nút Power | Nếu tắt bằng nút: chờ chân P0.03 về mức nhả. Lý do: chân thức cấu hình theo **mức** "nhấn"; còn đang giữ thì chip thức ngay lập tức | ≤ 10 s, quá thì vẫn tắt |
| 10 | Cấu hình chân thức | `gpio_pin_interrupt_configure_dt(&power_btn, GPIO_INT_LEVEL_ACTIVE)` | |
| 11 | Tạm dừng UART | `pm_device_action_run(uart20, PM_DEVICE_ACTION_SUSPEND)` | |
| 12 | Xóa nguyên nhân reset | `hwinfo_clear_reset_cause()` | |
| 13 | Tắt | `sys_poweroff()` | |

Phác thảo mã [Đề xuất]:

```c
static const struct gpio_dt_spec power_btn = GPIO_DT_SPEC_GET(DT_NODELABEL(btn_power), gpios);

FUNC_NORETURN void power_off(enum power_off_reason why)
{
	LOG_INF("Power off (%s)", reason_str(why));
	app_set_state(HS_SHUTDOWN);
	call_ctrl_end_any(K_MSEC(500));
	audio_out_prompt(PROMPT_POWER_OFF);
	wait_until(!audio_out_prompt_busy(), K_MSEC(800));
	bt_disconnect_and_wait(BT_HCI_ERR_REMOTE_POWER_OFF, K_SECONDS(1));
	advertising_stop();
	voice_stop();
	audio_out_shutdown();
	thermo_sleep();
	settings_save_dirty();
	if (why == OFF_BUTTON) {
		wait_until(gpio_pin_get_dt(&power_btn) == 0, K_SECONDS(10));
	}
	gpio_pin_interrupt_configure_dt(&power_btn, GPIO_INT_LEVEL_ACTIVE);
	pm_device_action_run(DEVICE_DT_GET(DT_NODELABEL(uart20)), PM_DEVICE_ACTION_SUSPEND);
	hwinfo_clear_reset_cause();
	sys_poweroff();
}
```

### 6.5 Tự tắt và chính sách quảng bá

| Hẹn giờ | Bắt đầu đếm | Dừng / đếm lại | Hết giờ |
|---|---|---|---|
| **Giảm quảng bá** 30 s | Bắt đầu quảng bá (khởi động hoặc sau ngắt kết nối) | Có kết nối | Đổi tham số sang `BT_BAP_ADV_PARAM_CONN_REDUCED` (150 ms) [SDK] |
| **Tự tắt** 10 phút [Đề xuất] | Vào IDLE (không kết nối) | Có kết nối; mọi cử chỉ nút (`power_activity`) | `power_off(OFF_IDLE)` |
| (Không tắt khi đang kết nối) | | | Đang kết nối nhưng rảnh thì giữ nguyên: điện thoại có thể mở luồng bất cứ lúc nào |

Đổi tham số quảng bá: `bt_le_ext_adv_stop` → `bt_le_ext_adv_update_param(adv, BT_BAP_ADV_PARAM_CONN_REDUCED)` → `bt_le_ext_adv_start`.

So sánh tần suất:
- 20–30 ms: điện thoại tìm thấy trong khoảng 1 s.
- 150 ms: dòng trung bình của radio giảm khoảng 5–7 lần [Cần đo]; thời gian tìm thấy vẫn dưới vài giây.

Thời gian tự tắt chỉnh được bằng lệnh `helmet autooff <phút>` (0 = không bao giờ), lưu trong settings.

### 6.6 Cài đặt lưu qua lần tắt

Dùng `settings` (đã có cho bond), khóa gốc `helmet/`. Một `SETTINGS_STATIC_HANDLER_DEFINE(helmet, "helmet", …)` nạp lại các giá trị khi `settings_load()`.

| Khóa | Kiểu | Mặc định | Dùng ở |
|---|---|---|---|
| `helmet/vol` | u8 | 128 | `media_ctrl_init` truyền vào `bt_vcp_vol_rend_register` thay cho hằng 128 |
| `helmet/autooff` | u16 (phút) | 10 | `power.c` |
| `helmet/temp/emis` | u16 (‰) | 980 | `thermo.c` (độ phát xạ 0.98) |
| `helmet/temp/alarm` | i32 (m°C) | 38 000 | `thermo.c` |

Chỉ ghi khi giá trị đổi **và** lúc tắt máy (bước 8, mục 6.4), không ghi mỗi lần nhấn nút, để giảm số lần ghi RRAM.

### 6.7 Dòng tiêu thụ và thời gian chờ

**Khi "tắt"** (System OFF), số liệu theo datasheet [Cần đo]:

| Linh kiện | Dòng |
|---|---|
| AP2112K (dòng tĩnh) | ~55 µA (điển hình) |
| nRF54L15 System OFF | < 1 µA |
| 2 × MAX98357A không có BCLK | [Cần đo]. Chân SD_MODE nối cố định qua điện trở nên không đưa ampli vào shutdown bằng chân được |
| 2 × IM69D130 không có xung | [Cần đo] (vào chế độ ngủ) |
| TP4056 + DW01 | vài µA |
| **Giả định tổng** | **~60 µA** |

Thời gian để "tắt" trong tủ tới khi cạn pin (với giả định 60 µA):

| Dung lượng pin [TBD] | Thời gian |
|---|---|
| 300 mAh | ~208 ngày |
| 500 mAh | ~347 ngày |
| 1 000 mAh | ~694 ngày |

**Đề xuất cho PCB bản sau:**
- LDO có chân EN điều khiển được, cộng mạch tự giữ nguồn bằng nút Power: tắt thật, dòng gần 0.
- Hoặc tối thiểu: đưa SD_MODE của ampli về GPIO.

**Đo:** đồng hồ µA (hoặc Nordic PPK2) nối tiếp với pin, **đã rút J-Link** (mục 2.5). Đo ở 6 trạng thái: OFF, IDLE quảng bá nhanh, IDLE quảng bá chậm, CONNECTED rảnh, nghe nhạc, đang gọi.

### 6.8 Âm báo (`audio_out` v2)

Vì LED không dùng được, âm báo là **kênh phản hồi duy nhất** cho người đội mũ.

**Thiết kế bộ tạo âm:**
- Mỗi âm báo là một danh sách đoạn `{tần số Hz, thời lượng ms, khoảng lặng ms}`.
- Sóng sin tạo bằng bộ tích lũy pha 32 bit + bảng 256 mẫu (`int16`), nội suy tuyến tính. Rất rẻ.
- Biên độ cố định −18 dBFS, **không** phụ thuộc âm lượng, để luôn nghe được kể cả khi âm lượng nhỏ [Đề xuất]. Ngoại lệ: chuông lấy theo âm lượng hiện tại, tối thiểu −30 dBFS.
- Mỗi đoạn có dốc lên/xuống 5 ms để không kêu "lụp bụp".
- Khi đang phát nhạc: nhạc giảm 10 dB trong lúc có âm báo (ducking), âm báo cộng vào, bão hòa ở ±32 767.
- Khi không có luồng nào: `audio_out` tự cấu hình I2S ở 16 kHz, phát xong 200 ms thì dừng I2S.
- Chạy trong `audio_tid`, tầng 2 (mục 4.7). Âm báo vì vậy cũng có trong tham chiếu AEC.

**Danh sách âm báo** [Đề xuất]:

| Âm | Mẫu | Khi nào |
|---|---|---|
| `POWER_ON` | 880 → 1175 → 1568 Hz, mỗi nốt 80 ms | Bật máy |
| `POWER_OFF` | 1568 → 1175 → 880 Hz | Tắt máy |
| `CONNECTED` | 2 × 1319 Hz 60 ms | Kết nối + mã hóa xong |
| `DISCONNECTED` | 659 Hz 200 ms | Mất kết nối |
| `RING` | (1000 Hz 400 ms, nghỉ 200 ms) × 2, rồi nghỉ 2 s; lặp | Cuộc gọi đến (chuông cục bộ) |
| `MIC_MUTED` | 1047 → 523 Hz | Tắt micro |
| `MIC_UNMUTED` | 523 → 1047 Hz | Bật micro |
| `VOL_LIMIT` | 2 093 Hz 30 ms | Âm lượng chạm trần/sàn |
| `PAIRING` | 4 × 1568 Hz 50 ms | Đã xóa ghép đôi |
| `TEMP_ALARM` | 3 × 2 637 Hz 150 ms | Nhiệt độ vượt ngưỡng |
| `LOW_BATTERY` | 2 × 440 Hz 150 ms | VDD thấp (mục 6.9) |
| `STATUS_OK` / `STATUS_NO_LINK` | 1 / 2 tiếng 1 319 Hz | Power nhấn ngắn |

Khi có cuộc gọi, chỉ phát âm báo ngắn (≤ 300 ms), trừ chuông.

### 6.9 Tùy chọn: cảnh báo pin sắp hết qua VDD

Không có Bat+, nhưng khi pin xuống thấp thì LDO không còn đủ điện áp vào để giữ 3.3 V. Khi đó **VDD của chip tụt theo pin**.
- Đo VDD bằng SAADC, kênh nội `NRF_SAADC_VDD` có trong dt-bindings [SDK]. Cần xác nhận nRF54L15 hỗ trợ kênh này trong datasheet mục SAADC.
- Mỗi 60 s đo một lần. VDD < 3.20 V liên tục 3 lần thì gửi `EVT_LOW_BATTERY`: phát âm báo mỗi 5 phút.
- VDD < 3.00 V thì `power_off(OFF_LOW_BATTERY)`. Mạch bảo vệ DW01 cắt ở khoảng 2.4 V, quá thấp để radio và âm thanh hoạt động ổn định.
- Giới hạn: chỉ biết "sắp hết", không ra %. Ngưỡng phụ thuộc độ sụt áp của LDO theo dòng tải [Cần đo].

### 6.10 Kiểm thử chức năng 3

| ID | Mục tiêu | Cách làm | Đạt khi |
|---|---|---|---|
| T-PWR-01 | Tắt bằng nút | Giữ Power 3 s | Âm tắt, điện thoại báo mất kết nối, dòng về mức OFF |
| T-PWR-02 | Bật bằng nút | Khi OFF, giữ Power 2 s | Âm bật, quảng bá, điện thoại tự kết nối lại |
| T-PWR-03 | Chống bật nhầm | Khi OFF, nhấn Power 0.5 s | Không âm, dòng về mức OFF trong < 100 ms |
| T-PWR-04 | Tự tắt | Không kết nối 10 phút | Tự tắt, có âm |
| T-PWR-05 | Giảm quảng bá | Đo dòng IDLE trước/sau 30 s | Dòng giảm rõ [Cần đo] |
| T-PWR-06 | Lưu âm lượng | Đổi âm lượng, tắt, bật | Âm lượng giữ nguyên |
| T-PWR-07 | Dòng OFF | PPK2, rút J-Link | Ghi số liệu, so với 60 µA giả định |
| T-PWR-08 | Tắt khi đang gọi | Giữ Power 3 s khi đang gọi | Cuộc gọi kết thúc trên điện thoại, rồi tắt |
| T-PRM-01 | Âm báo khi nghe nhạc | Đổi âm lượng tới trần khi đang phát nhạc | Nghe `VOL_LIMIT` rõ, nhạc giảm rồi trở lại |

---

## 7. Chức năng 4: Nhiệt độ hồng ngoại MLX90632 (FR-22)

### 7.1 Mục đích: chưa chốt [TBD]

Tài liệu gốc chưa nói đo nhiệt để làm gì. Thiết kế dưới đây **chung cho cả hai hướng**; chỉ khác tham số:

| | Hướng A: thân nhiệt người đội | Hướng B: nhiệt trong mũ / môi trường |
|---|---|---|
| Cảm biến nhìn vào | Trán / thái dương, cách 1–3 cm | Không gian trong mũ |
| Chế độ đo | Medical (độ chính xác cao nhất ở 35–42 °C) | Medical hoặc Extended |
| Độ phát xạ | 0.98 (da) | 0.95 (vải/xốp) |
| Ngưỡng cảnh báo mặc định | ≥ 38.0 °C | ≥ 40 °C (nóng, nguy cơ say nắng) |
| Dịch vụ BLE | HTS, loại "Body (general)" | HTS hoặc dịch vụ riêng |

Lưu ý hướng A: nhiệt độ bề mặt da trán trong mũ kín khác thân nhiệt lõi (mồ hôi, gió, tóc), nên chỉ dùng được để **theo dõi xu hướng**, không thay nhiệt kế y tế.

### 7.2 Điều kiện phần cứng

| Việc | Trạng thái |
|---|---|
| Gắn MLX90632 | Chưa gắn |
| Điện trở kéo lên 4.7 kΩ cho SDA, SCL (ISS-03) | Chưa có. Kéo lên nội ~13 kΩ chỉ đủ ở 100 kHz, dây ngắn |
| SDA P1.13 / SCL P0.04 ở hai miền nguồn (C-04) | Phải dùng I2C bit-bang (đã có trong devicetree) |
| Thử nhận diện | `hw_test` đã có lệnh `hw mlx` đọc `EE_VERSION` |

### 7.3 `mlx90632.c`: driver

Tách làm hai lớp để kiểm thử được trên PC:
- **Lớp I/O** (chỉ chạy trên chip): đọc/ghi thanh ghi qua `i2c_write_read` / `i2c_write`.
- **Lớp tính toán** (C thuần, hàm thuần): đổi số thô ra °C.

**Giao tiếp I2C** (giống `hw_test`):

| Thao tác | Byte trên bus |
|---|---|
| Đọc 16 bit | ghi `[addr_hi, addr_lo]`, đọc `[data_hi, data_lo]` |
| Ghi 16 bit | ghi `[addr_hi, addr_lo, data_hi, data_lo]` |
| Đọc 32 bit EEPROM | 2 lần đọc 16 bit: **word thấp ở `addr`, word cao ở `addr + 1`** (theo driver Linux `mlx90632`; cần đối chiếu datasheet) |

**Bản đồ thanh ghi** (theo `mlx90632.h` của Melexis):

| Tên | Địa chỉ | Cỡ | Hệ số chia | Ghi chú |
|---|---|---|---|---|
| `EE_VERSION` | 0x240B | 16 | | Byte thấp = phiên bản DSP; thư viện hỗ trợ `0x05` (DSPv5) |
| `P_R` | 0x240C | 32 | 2^8 | Hiệu chuẩn môi trường |
| `P_G` | 0x240E | 32 | 2^20 | |
| `P_T` | 0x2410 | 32 | 2^44 | |
| `P_O` | 0x2412 | 32 | 2^8 | |
| `Ea` | 0x2424 | 32 | 2^16 | |
| `Eb` | 0x2426 | 32 | 2^8 | |
| `Fa` | 0x2428 | 32 | 2^46 | |
| `Fb` | 0x242A | 32 | 2^36 | |
| `Ga` | 0x242C | 32 | 2^36 | |
| `Gb` | 0x242E | 16 | 2^10 | |
| `Ka` | 0x242F | 16 | 2^10 | |
| `Ha` | 0x2481 | 16 | 2^14 | |
| `Hb` | 0x2482 | 16 | 2^10 | |
| `EE_MEDICAL_MEAS1/2` | 0x24E1 / 0x24E2 | 16 | | Bit 10:8 = tần số đo (0 = 0.5 Hz … 7 = 64 Hz) |
| `REG_CTRL` | 0x3001 | 16 | | Bit 2:1 chế độ (0 halt, 1 sleeping step, 2 step, 3 liên tục); bit 3 SOC (bắt đầu đo); bit 8:4 loại đo (0x00 medical, 0x11 extended) |
| `REG_STATUS` | 0x3FFF | 16 | | Bit 0 có dữ liệu mới; bit 6:2 vị trí chu kỳ; bit 9 EEPROM bận; bit 10 thiết bị bận |
| `RAM_1(n)`, `RAM_2(n)`, `RAM_3(n)` | 0x4000 + 3n, +1, +2 | 16 | | n = 1, 2. Datasheet gọi là RAM_4…RAM_9 |

Các giá trị 16 bit là số **có dấu** (`int16_t`).

**Khởi tạo** (`mlx90632_init`):
1. Đọc `EE_VERSION`. Không trả lời thì báo "không có cảm biến" và tắt chức năng, **không** làm hỏng khởi động.
2. Kiểm tra byte thấp = `0x05`; khác thì ghi cảnh báo nhưng vẫn chạy.
3. Đọc toàn bộ hệ số vào `struct mlx90632_cal` (9 giá trị 32 bit + 4 giá trị 16 bit). Khoảng 22 lần đọc ≈ 15 ms ở 100 kHz.
4. Ghi `REG_CTRL`: loại đo medical, chế độ **sleeping step**: cảm biến ngủ cho tới khi được ra lệnh đo, tiết kiệm điện.

**Một lần đo** (`mlx90632_measure`), theo `mlx90632_start_measurement` và `read_temp_raw` của Melexis:

```
1. Đọc STATUS, ghi lại STATUS với bit 0 = 0 (xóa cờ dữ liệu mới)
2. Ghi CTRL: giữ chế độ sleeping step, đặt SOC = 1 (bắt đầu đo)
3. Lặp tối đa ~1 s: ngủ 10 ms, đọc STATUS; bit 0 = 1 thì dừng
       quá giờ → -ETIMEDOUT
4. cycle_pos = (STATUS >> 2) & 0x1F
       cycle_pos = 1: kênh mới = 1, kênh cũ = 2
       cycle_pos = 2: kênh mới = 2, kênh cũ = 1
       khác: -EINVAL
5. object_new = (RAM_2(mới) + RAM_1(mới)) / 2     object_old = (RAM_2(cũ) + RAM_1(cũ)) / 2
   ambient_new = RAM_3(1)                         ambient_old = RAM_3(2)
```

**Tính nhiệt độ** (viết lại từ `mlx90632_calc_temp_ambient` / `_object` của Melexis):

```
Tiền xử lý môi trường:
    VR_Ta = ambient_old + (Gb / 2^10) · (ambient_new / 12)
    AMB   = ((ambient_new / 12) / VR_Ta) · 2^19

Nhiệt độ môi trường:
    B  = AMB − P_R / 2^8
    Ta = P_O / 2^8  +  B / (P_G / 2^20)  +  (P_T / 2^44) · B²

Tiền xử lý vật thể:
    VR_IR = ambient_old + (Ka / 2^10) · (ambient_new / 12)
    S     = (((object_new + object_old) / 2) / 12 / VR_IR) · 2^19

Nhiệt độ vật thể (lặp 5 lần, To bắt đầu = 25):
    TAdut = (AMB − Eb / 2^8) / (Ea / 2^16) + 25
    lặp:
        α  = (Fa / 2^46) · (Ha / 2^14) · (1 + Ga · (To − 25) / 2^36 + Fb · (TAdut − 25) / 2^36)
        To = ( S / (ε · α) + (TAdut + 273.15)^4 )^(1/4) − 273.15 − Hb / 2^10
```

- ε là độ phát xạ (mặc định của thư viện là 1.0; thiết kế này lấy từ settings).
- Tính bằng **`double`**. Cortex-M33 chỉ có FPU đơn, nên `double` chạy bằng phần mềm: chậm nhưng chỉ 1–2 lần/giây, không đáng kể. Các hằng chia 2^44 và 2^46 cần độ chính xác kép.

**Kiểm thử lớp tính toán:** kho `mlx90632-library` có bộ kiểm thử đơn vị với bộ số thô và kết quả mong đợi. Chép các bộ số đó sang một chương trình kiểm thử chạy trên PC. Kết quả phải khớp tới 0.01 °C trước khi chạy trên chip.

**Đưa cảm biến về ngủ** (`mlx90632_sleep`): ghi `REG_CTRL` chế độ halt hoặc sleeping step. Gọi khi tắt máy.

### 7.4 `thermo.c`: lịch đo, lọc, cảnh báo

**Thread riêng `thermo_tid` (preempt 10)**, không dùng `sysworkq`:
- I2C bit-bang là **vòng chờ bận** của CPU: mỗi lần đọc 16 bit mất khoảng 0.5 ms, một lần đo đọc 8 thanh ghi mất khoảng 4 ms liền.
- `sysworkq` là thread coop −1, không bị chiếm quyền. Chạy bit-bang trong đó sẽ chặn `audio_tid` (preempt 2) tới 4 ms, có thể gây thiếu dữ liệu I2S.
- Trong thread preempt 10, audio và voice chen vào bất cứ lúc nào. Bit-bang I2C chịu được vì master tự giữ xung clock, chỉ chậm lại chứ không sai.

**Vòng lặp:**

```
thermo_init: mlx90632_init (thất bại → thread ngủ vĩnh viễn, thermo_get trả valid = false)
lặp mỗi T_MEAS (2 s) [Đề xuất]:
    mlx90632_measure → (Ta, To)
    kiểm tra hợp lệ: −20 ≤ Ta ≤ 85, −20 ≤ To ≤ 100 (°C); sai → bỏ, đếm lỗi
    lọc: trung vị 5 mẫu gần nhất (loại gai), rồi trung bình trượt mũ α = 0.3
    cập nhật giá trị chia sẻ (atomic theo khối, hoặc k_spinlock ngắn)
    so ngưỡng có trễ: vượt ngưỡng 3 lần liên tiếp → EVT_TEMP_ALARM(1);
                      xuống dưới (ngưỡng − 0.5 °C) 3 lần → EVT_TEMP_ALARM(0)
    thiết bị OFF / SHUTDOWN → mlx90632_sleep, thread dừng
```

**Bộ điều khiển xử lý `EVT_TEMP_ALARM(1)`:** phát `PROMPT_TEMP_ALARM` (không phát khi đang gọi; để dành phát khi cuộc gọi kết thúc), gửi chỉ thị HTS ngay.

**Dòng tiêu thụ:** cảm biến chỉ đo khi được ra lệnh (sleeping step), còn lại ngủ. Dòng trung bình phụ thuộc chu kỳ đo [Cần đo].

### 7.5 Dịch vụ BLE: Health Thermometer Service

Theo ví dụ `samples/bluetooth/peripheral_ht` [SDK]:

| Thành phần | UUID | Thuộc tính | Nội dung |
|---|---|---|---|
| Health Thermometer Service | 0x1809 | | |
| Temperature Measurement | 0x2A1C | Indicate | Cờ + nhiệt độ (IEEE-11073 FLOAT 32 bit) + (tùy chọn) loại |
| Temperature Type | 0x2A1D | Read | `0x02` = Body (general) cho hướng A [Đề xuất] |
| Measurement Interval | 0x2A21 | Read (tùy chọn) | 2 (giây) |

**Định dạng gói Temperature Measurement:**
- Byte 0: cờ. Bit 0 = 0 nghĩa là °C; bit 1 = có timestamp (không dùng); bit 2 = có loại (không dùng vì đã có đặc tính 0x2A1D).
- Byte 1–4: FLOAT gồm phần định trị 24 bit có dấu (little-endian) và số mũ cơ số 10, 8 bit.
- Ví dụ 36.75 °C = 3 675 × 10⁻²: định trị `0x000E5B`, số mũ `−2 = 0xFE`. Gói: `00 5B 0E 00 FE`.

**Khi nào gửi:**
- Mỗi 10 s nếu điện thoại đã bật indicate [Đề xuất].
- Gửi ngay khi có cảnh báo.
- Indicate cần xác nhận, nên chỉ gửi gói mới sau khi gói trước được xác nhận (giống ví dụ SDK).

Nhiệt độ môi trường (Ta) không có chỗ trong HTS. Nếu cần, thêm một đặc tính riêng (UUID 128 bit) trong một dịch vụ riêng ở bước sau.

### 7.6 Lệnh shell

| Lệnh | Tác dụng |
|---|---|
| `helmet temp` | Ta, To đã lọc, tuổi dữ liệu, số lỗi, trạng thái cảnh báo |
| `helmet temp raw` | Đo một lần, in số thô (`ambient_new/old`, `object_new/old`, `cycle_pos`) và kết quả chưa lọc |
| `helmet temp cal` | In toàn bộ hệ số EEPROM (đối chiếu khi gỡ lỗi) |
| `helmet temp emis <0.10–1.00>` | Đổi độ phát xạ, lưu settings |
| `helmet temp alarm <°C>` | Đổi ngưỡng cảnh báo, lưu settings |

### 7.7 Kiểm thử chức năng 4

| ID | Mục tiêu | Cách làm | Đạt khi |
|---|---|---|---|
| T-TEMP-01 | Công thức | Chạy bộ số kiểm thử của Melexis trên PC | Sai khác ≤ 0.01 °C |
| T-TEMP-02 | Nhận diện | `helmet temp cal` | `EE_VERSION` byte thấp `0x05`, hệ số khác 0 |
| T-TEMP-03 | Môi trường | So Ta với nhiệt kế tham chiếu trong phòng | Lệch ≤ 1 °C |
| T-TEMP-04 | Vật thể | Nhắm vào cốc nước 37 °C (đo bằng nhiệt kế chuẩn), cách 2 cm, ε = 0.95 | Lệch ≤ 0.5 °C [Cần đo] |
| T-TEMP-05 | Không có cảm biến | Chạy khi chưa gắn MLX90632 | Khởi động bình thường, `temp` báo "not present" |
| T-TEMP-06 | Không chặn audio | Nghe nhạc trong khi đo mỗi 2 s, 10 phút | Không tăng `slip`/lỗi I2S so với khi tắt đo |
| T-TEMP-07 | HTS | App nRF Connect bật indicate | Nhận gói mỗi 10 s, giá trị khớp `helmet temp` |
| T-TEMP-08 | Cảnh báo | Hạ ngưỡng xuống dưới giá trị đang đo | Âm cảnh báo + gói HTS ngay; nâng ngưỡng thì hết cảnh báo |

---

## 8. Tổng hợp thay đổi ở các module hiện có

| File | Thay đổi | Mục |
|---|---|---|
| `main.c` | `power_boot_check()` đầu `main()`. Hàng đợi `app_q` + máy trạng thái thiết bị; vòng `main` thành `k_msgq_get(&app_q, &evt, đến hạn heartbeat)`. `on_input` chỉ đăng `EVT_KEY_RAW`. Callback kết nối đăng sự kiện. Quảng bá: thêm đổi tham số. Dữ liệu quảng bá: TMAS role `0x000A` (`05 16 55 18 0A 00`); ASCS sink `0x079F`, source `0x0002` (`09 16 4E 18 00 9F 07 02 00 00`). `bt_tmap_register(UMR \| CT)`. Lệnh shell mới (mục 9.3) | 1.6, 5.5, 6 |
| `le_audio.c` | ASE source, PACS source, `lc3_config`/`reconfig` theo hướng, `lc3_start` cho source, `sent` → `voice_on_sent`, ngữ cảnh khả dụng của source theo tình trạng micro | 4.3 |
| `audio_out.c` | Vòng PCM + bù trôi từng mẫu, trộn âm báo + ducking, điểm tham chiếu + vòng 128 ms, lấy mẫu lại Hermite theo phân số chính xác, I2S tự bật cho âm báo khi không có luồng, `audio_out_shutdown()` | 4.7, 6.8 |
| `media_ctrl.c` | Âm lượng ban đầu từ settings; lưu khi đổi (đánh dấu "bẩn"); `media_ctrl_next_track()`; báo "dò xong" để nối tiếp dò GTBS; phát `VOL_LIMIT` khi chạm trần/sàn | 5.2, 5.6, 6.6 |
| `capture.c` | Bản ghi `MIC (4)`, `REF (5)`; lệnh `helmet capture voice` | 4.10 |
| `helmet.h` | Mục 3.4 | 3.4 |
| `prj.conf` | Mục 3.2 | 3.2 |
| `CMakeLists.txt` | 8 file mới | 3.1 |
| `scripts/helmet_audio.py` | Đọc bản ghi MIC/REF, `--voice` | 4.10 |
| `scripts/aec_lab.py` | **Mới**: chạy `voice_dsp.c` offline trên WAV | 4.10 |

---

## 9. Chẩn đoán: log, heartbeat, shell

### 9.1 Heartbeat mở rộng

```
heartbeat: CONNECTED, STREAMING, SDU 5230 (bad 3), PLC 3, I2S on 48000→47619 Hz q2, slip -12/+0, err 0
voice: IN_CALL 32k, mic -38.5 dBFS, out -21.0 dBFS, ERLE 27.4 dB, sent 6000, underrun 0, slip +4/-0, muted 0
power: state CONNECTED, autooff 10 min (paused), adv fast
temp: To 34.62 °C, Ta 29.10 °C, age 1.2 s, alarm 0
```

- Dòng `voice:` chỉ in khi có luồng gửi.
- Dòng `temp:` chỉ in khi có cảm biến.
- `I2S on 48000→47619 Hz` in **cả hai** tần số: yêu cầu và thật (sửa nhầm lẫn hiện tại).

### 9.2 Log sự kiện mới

| Module | Log |
|---|---|
| `buttons` | `KEY_RAW Call down`, `GESTURE Call SHORT (state RINGING)` |
| `call_ctrl` | `GTBS found`, `Incoming call #1 from tel:…`, `Call #1 ACTIVE`, `Call ended (reason 0x02 remote)` |
| `voice` | `Voice TX start: 32 kHz, 80 B, mic 16 kHz ×2`, `AEC delay D = 1123 samples`, `Mic error -11 (block lost)` |
| `power` | `Boot: wake by Power button, held 2.0 s → ON`, `Power off (button)`, `Adv → reduced 150 ms` |
| `thermo` | `MLX90632 DSPv5 found`, `Temp alarm ON 38.4 °C` |

### 9.3 Lệnh shell mới

| Lệnh | Tác dụng |
|---|---|
| `helmet btn <volup\|voldown\|call\|power> <short\|double\|long\|vlong>` | **Giả lập cử chỉ** (đăng `EVT_GESTURE`): thử mọi logic khi chưa có board nút |
| `helmet call [accept\|reject\|end]` | Không tham số: in trạng thái cuộc gọi; có tham số: gửi lệnh |
| `helmet mic [mute\|unmute]` | Tắt/bật micro qua MICP |
| `helmet voice` | Thống kê chiều lên, tham số DSP |
| `helmet aec <on\|off\|reset>` | Bật/tắt AEC, khởi động lại bộ lọc (so sánh khi thử) |
| `helmet prompt <tên>` | Phát thử âm báo |
| `helmet power off` | Tắt máy (như giữ Power 3 s) |
| `helmet autooff <phút>` | Đặt thời gian tự tắt (0 = tắt tính năng) |
| `helmet perf` | Chu kỳ CPU trung bình/lớn nhất của giải mã, mã hóa, AEC, giảm ồn, lấy mẫu lại |
| `helmet temp …` | Mục 7.6 |

---

## 10. Kế hoạch kiểm thử và tiêu chí nghiệm thu

### 10.1 Mức kiểm thử

| Mức | Đối tượng | Công cụ |
|---|---|---|
| Đơn vị trên PC | Máy trạng thái nút, công thức MLX90632, `voice_dsp`, bộ lấy mẫu lại, bù trôi | gcc + chương trình kiểm thử, hoặc Zephyr `twister` trên `native_sim` |
| Offline trên dữ liệu thật | AEC, giảm ồn | Bản ghi MIC/REF qua RTT + `aec_lab.py` |
| Tích hợp trên mạch, không cần điện thoại | Nút (lệnh giả lập), âm báo, nguồn, nhiệt độ | Shell + heartbeat |
| Hệ thống với điện thoại LE Audio | Cuộc gọi, MICP, chuông, tương thích | Điện thoại mục tiêu [TBD] |

### 10.2 Các ca kiểm thử

Chi tiết ở mục 4.11 (T-VOICE), 5.7 (T-BTN, T-CALL), 6.10 (T-PWR, T-PRM), 7.7 (T-TEMP). Bổ sung kiểm thử hồi quy:

| ID | Mục tiêu | Đạt khi |
|---|---|---|
| T-REG-01 | Nghe nhạc sau khi có `audio_out` v2 | 1 giờ nghe: `slip` tăng đều, **không còn** `sync -8` mỗi 10 s, không tiếng tách |
| T-REG-02 | Capture nhạc vẫn chạy | `make audio` như trước |
| T-REG-03 | RAM / CPU | RAM ≤ 80 %; `kernel stacks` mỗi thread còn ≥ 25 % stack |

### 10.3 Tiêu chí nghiệm thu [Đề xuất]

| # | Tiêu chí |
|---|---|
| AC2-1 | Gọi 10 cuộc liên tiếp với điện thoại mục tiêu: nghe, từ chối, kết thúc bằng nút đều đúng; không cuộc nào rớt do mũ |
| AC2-2 | Cuộc gọi 30 phút: âm hai chiều liên tục, người bên kia không nghe tiếng vọng (đánh giá chủ quan, 3 người) |
| AC2-3 | Mọi ca T-BTN, T-PWR đạt; dòng OFF đo được và ghi vào tài liệu |
| AC2-4 | Khi đã gắn cảm biến: T-TEMP-01…08 đạt |
| AC2-5 | Không ca hồi quy nào hỏng |

---

## 11. Rủi ro và câu hỏi mở

### 11.1 Rủi ro

| ID | Rủi ro | Mức | Giảm thiểu |
|---|---|---|---|
| R-01 | AEC tự viết không đạt chất lượng | Cao | Bước A (bán song công) làm trước để luôn gọi được; ghi dữ liệu thật và chỉnh offline; phương án SpeexDSP / WebRTC AECM |
| R-02 | **Ồn gió khi chạy xe** lấn giọng nói | Cao | Vị trí micro + mút chắn gió (cơ khí); lọc thông cao; giảm ồn hai micro ở giai đoạn 2 |
| R-03 | CPU không đủ khi gọi | Trung bình | Lõi DSP 16 kHz; đo `helmet perf` sớm (M3); giảm L của AEC hoặc tắt giảm ồn phổ |
| R-04 | RAM vượt 80 % | Trung bình | Tắt capture trong bản phát hành; giảm số bộ giải mã |
| R-05 | Điện thoại không chuyển cuộc gọi sang LE Audio, hoặc không có GTBS | Trung bình | Thử sớm (M2) với điện thoại mục tiêu; bản ghi tương thích |
| R-06 | Dòng OFF cao do LDO luôn bật | Trung bình | Đo (T-PWR-07); đề xuất mạch tự giữ nguồn ở PCB sau |
| R-07 | Độ chính xác nhiệt trong mũ (mồ hôi, khoảng cách, tóc) | Trung bình | Chốt mục đích (Q-T1); chỉ báo xu hướng; hiệu chuẩn bù độ lệch |
| R-08 | I2C bit-bang không ổn định với kéo lên nội | Thấp | Thêm 4.7 kΩ (ISS-03); hạ tốc độ nếu cần |
| R-09 | Đổi cấu hình nhạc ↔ gọi liên tục làm I2S khởi động lại nhiều lần | Thấp | `handle_start` chỉ cấu hình lại khi tần số đổi; đo thời gian chuyển |

### 11.2 Câu hỏi mở cần chủ dự án trả lời

| ID | Câu hỏi | Ảnh hưởng |
|---|---|---|
| Q-C1 | Điện thoại mục tiêu cụ thể (hãng, đời, Android) | Kiểm thử tương thích, cấu hình codec ưu tiên |
| Q-C2 | Duyệt bảng hành động nút (mục 5.6) và thông số thời gian (mục 5.5) | `buttons.c` |
| Q-C3 | Vị trí lắp 2 micro trong mũ; có mút chắn gió không | `voice_dsp` giai đoạn 2, R-02 |
| Q-P1 | Dung lượng pin | Ước lượng thời gian chờ, ngưỡng pin yếu |
| Q-P2 | Thời gian tự tắt mặc định (đề xuất 10 phút) | `power.c` |
| Q-P3 | Có làm mạch tự giữ nguồn / đưa SD_MODE về GPIO ở PCB sau không | Dòng OFF |
| Q-T1 | Đo nhiệt để làm gì (hướng A hay B, mục 7.1)? Có cần báo về ứng dụng riêng không? | Chế độ đo, ngưỡng, dịch vụ BLE |
| Q-T2 | Vị trí và khoảng cách cảm biến trong mũ | Độ phát xạ, độ chính xác |

---

## 12. Phụ lục

### 12.1 Tham chiếu SDK đã kiểm tra (NCS v3.4.0)

| Nội dung | Vị trí |
|---|---|
| Gửi SDU: `bt_bap_stream_send(stream, buf, seq_num)`, callback `sent` | `zephyr/include/zephyr/bluetooth/audio/bap.h` |
| Khởi động ASE source qua `lc3_start` (Receiver Start Ready) | `zephyr/subsys/bluetooth/audio/ascs.c` (`ase_start`) |
| Kho bộ đệm gửi ISO | `samples/bluetooth/bap_unicast_server/src/main.c` (`NET_BUF_POOL_FIXED_DEFINE(tx_pool, …)`) |
| TBS client: accept, terminate, call_state, termination_reason, `BT_TBS_GTBS_INDEX` | `zephyr/include/zephyr/bluetooth/audio/tbs.h` |
| CCP client + vai trò TMAP CT \| UMR | `samples/bluetooth/tmap_peripheral/` (`ccp_call_ctrl.c`, `main.c`) |
| MICP Mic Device: `bt_micp_mic_dev_register`, `mute`/`unmute`/`mute_disable`, callback `mute` | `zephyr/include/zephyr/bluetooth/audio/micp.h` |
| Node `zephyr,input-longpress`, `zephyr,input-double-tap` | `zephyr/dts/bindings/input/` |
| Quảng bá giảm tần suất `BT_BAP_ADV_PARAM_CONN_REDUCED` (150 ms) | `bap.h` |
| System OFF: `sys_poweroff`, `GPIO_INT_LEVEL_ACTIVE`, `RESET_LOW_POWER_WAKE` | `samples/boards/nordic/system_off/src/main.c` |
| Tỉ lệ PDM nRF54L15 (32/48/50/64/80/96/100/128), thuật toán chọn bộ chia | `modules/hal/nordic/nrfx/bsp/stable/mdk/nrf54l15_types.h`, `drivers/src/nrfx_pdm.c` |
| Bộ chia I2S (giải thích 47 619 Hz) | `modules/hal/nordic/nrfx/drivers/src/nrfx_i2s.c` (`nrfx_i2s_prescalers_calc`) |
| Kích thước bộ nhớ LC3 | `modules/lib/liblc3/include/lc3.h` (`LC3_ENCODER_MEM_T`, `LC3_DECODER_MEM_T`) |
| CMSIS-DSP | `modules/lib/cmsis-dsp`, Kconfig `zephyr/modules/cmsis-dsp/Kconfig` |
| Health Thermometer Service | `samples/bluetooth/peripheral_ht/src/hts.c` |
| Kênh SAADC nội `NRF_SAADC_VDD` | `zephyr/include/zephyr/dt-bindings/adc/nrf-saadc.h` |
| MLX90632: thanh ghi, công thức | `melexis/mlx90632-library` trên GitHub (`inc/mlx90632.h`, `src/mlx90632.c`), Apache-2.0 |

### 12.2 Số liệu tính toán dùng trong tài liệu

| Đại lượng | Giá trị | Cách tính |
|---|---|---|
| I2S thật ở 48 / 24 / 16 kHz | 47 619 / 23 810 / 15 873 Hz | 32 MHz / (21, 42, 63) / 32, tức × 125/126 |
| I2S thật ở 32 kHz | 32 258 Hz | 32 MHz / 31 / 32, tức × 125/124 |
| PDM 16 kHz | đúng | 32 MHz / 25 = 1.28 MHz, tỉ lệ 80 |
| PDM 32 kHz | đúng | 32 MHz / 20 = 1.6 MHz, tỉ lệ 50 |
| Lệch clock điện thoại 50 ppm ở 16 kHz | 0.8 mẫu/s | 16 000 × 50 × 10⁻⁶ |
| LC3 encoder 16 / 32 / 48 kHz | 2 608 / 4 008 / 5 408 B | `sizeof(LC3_ENCODER_MEM_T(10000, fs))` |
| LC3 decoder 16 / 32 / 48 kHz | 3 152 / 6 112 / 9 072 B | `sizeof(LC3_DECODER_MEM_T(10000, fs))` |
| NLMS 256 hệ số ở 16 kHz | 8.2 triệu MAC/s | 2 × 256 × 16 000 |
| Gói HTS 36.75 °C | `00 5B 0E 00 FE` | 3 675 × 10⁻² |
| ASCS khi có Conversational + Ringtone | sink `0x079F`, source `0x0002` | `0x059D \| 0x0002 \| 0x0200` |
| TMAP UMR + CT | `0x000A` | `BIT(3) \| BIT(1)` |
