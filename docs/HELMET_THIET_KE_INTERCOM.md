# Helmet: thiết kế intercom mũ–mũ qua Bluetooth LE

| | |
|---|---|
| Áp dụng cho | `apps/main` (firmware đầy đủ), board `me54bs62` (nRF54L15) |
| Trạng thái | **Thiết kế, chưa hiện thực.** Chưa có dòng mã nào trong tài liệu này được viết vào dự án |
| Dựa trên | Mã nguồn `apps/main` ngày 16/10/2026, NCS v3.4.0 / Zephyr 4.4, số đo cuộc gọi thật trên mạch ngày 16/10/2026 |
| Yêu cầu liên quan | **FR-25 (mới, đề xuất)**: đàm thoại trực tiếp giữa hai mũ, không cần điện thoại |
| Tài liệu liên quan | [HELMET_THIET_KE_CHUC_NANG_MOI.md](HELMET_THIET_KE_CHUC_NANG_MOI.md) (đàm thoại, nút, nguồn), [HELMET_HUONG_DAN_SU_DUNG_VA_KIEM_TRA.md](HELMET_HUONG_DAN_SU_DUNG_VA_KIEM_TRA.md) (cách dùng hiện tại) |

Quy ước nhãn:
- **[SDK]**: đã kiểm tra trực tiếp trong mã nguồn NCS v3.4.0 trên máy (tên hàm, Kconfig, hành vi). Vị trí ghi ở phụ lục 14.1.
- **[Đo]**: số đo trên mạch thật.
- **[Đề xuất]**: lựa chọn thiết kế, chủ dự án cần duyệt.
- **[Cần đo]**: số liệu ước lượng, phải đo trên mạch trước khi chốt.
- **[TBD]**: chưa xác định, chờ trả lời câu hỏi mở (mục 13.2).

Cách đọc:
- Mục 1–3: tổng quan, kiến thức nền và thay đổi cấu hình chung. Nên đọc trước.
- Mục 4–8: ghép đôi, liên kết, phiên đàm thoại, ưu tiên với điện thoại, nút và âm báo.
- Mục 9–14: tổng hợp thay đổi, chẩn đoán, kiểm thử, mở rộng, rủi ro, phụ lục.


> **Ghi chú 27/10/2026:** mã nguồn đã được tổ chức lại theo lớp. `helmet.h` / `main.c` nay tách thành `apps/main/src/app/` (lớp ứng dụng, `app/app.h`), `apps/main/src/features/<chức năng>/` và thư viện dùng chung `lib/` (`hal/`, `core/`, `services/`, `proto/`). Các tên file trong tài liệu này là tên tại thời điểm thiết kế; vị trí hiện tại xem [README.md](../README.md), mục *Kiến trúc mã nguồn*.

---

## Mục lục

1. [Tổng quan](#1-tổng-quan)
2. [Kiến thức nền mới](#2-kiến-thức-nền-mới)
3. [Thay đổi cấu hình và giao diện chung](#3-thay-đổi-cấu-hình-và-giao-diện-chung)
4. [Ghép đôi hai mũ](#4-ghép-đôi-hai-mũ)
5. [Liên kết thường trực và tự kết nối lại](#5-liên-kết-thường-trực-và-tự-kết-nối-lại)
6. [Phiên intercom](#6-phiên-intercom)
7. [Ưu tiên với điện thoại](#7-ưu-tiên-với-điện-thoại)
8. [Nút, âm báo, trạng thái thiết bị](#8-nút-âm-báo-trạng-thái-thiết-bị)
9. [Tổng hợp thay đổi ở các module](#9-tổng-hợp-thay-đổi-ở-các-module)
10. [Chẩn đoán: log, heartbeat, shell](#10-chẩn-đoán-log-heartbeat-shell)
11. [Kiểm thử và tiêu chí nghiệm thu](#11-kiểm-thử-và-tiêu-chí-nghiệm-thu)
12. [Mở rộng sau bản đầu](#12-mở-rộng-sau-bản-đầu)
13. [Rủi ro và câu hỏi mở](#13-rủi-ro-và-câu-hỏi-mở)
14. [Phụ lục](#14-phụ-lục)

---

## 1. Tổng quan

### 1.1 Mục tiêu

- Hai mũ **ghép đôi với nhau chỉ bằng nút**, không cần điện thoại hay app.
- Khi cả hai bật và trong tầm: **tự kết nối**. Người dùng mở intercom bằng nút từ **mũ bất kỳ**, hai người nói và nghe **cùng lúc** (song công).
- Mỗi mũ vẫn dùng điện thoại của mình (nhạc, cuộc gọi). **Cuộc gọi điện thoại được ưu tiên** hơn intercom.

### 1.2 Phạm vi

| Trong phạm vi bản đầu | Ngoài phạm vi (xem mục 12) |
|---|---|
| 2 mũ, mỗi mũ lưu 1 bạn đồng hành | Nhóm từ 3 mũ trở lên |
| Ghép đôi, liên kết thường trực, tự kết nối lại | Nghe nhạc nền trong lúc intercom |
| Phiên intercom hai chiều 16 kHz | Chia sẻ cuộc gọi điện thoại cho bạn (hội nghị 3 bên) |
| Ưu tiên với nhạc và cuộc gọi điện thoại | Kết nối với intercom hãng khác (Sena, Cardo…): họ dùng Bluetooth Classic hoặc radio riêng, nRF54L15 không có Classic |
| Nút, âm báo, lệnh shell, kiểm thử | Chế độ tầm xa Coded PHY (mốc tùy chọn I5) |

### 1.3 Ràng buộc nền tảng (đã kiểm tra)

| Ràng buộc | Hệ quả cho thiết kế |
|---|---|
| nRF54L15 chỉ có Bluetooth LE, không có BR/EDR | Không dùng HFP hay intercom Classic. Bắt buộc dùng LE Audio (kênh đẳng thời ISO) |
| Zephyr chỉ cho **một** ISO server. ASCS đã đăng ký nó. `ascs_iso_accept()` chỉ nhận CIS khớp một ASE đã cấu hình (đúng `cig_id`/`cis_id`, trạng thái Enabling hoặc QoS Configured) [SDK] | Mũ peripheral **không thể** nhận một CIS "trần". Luồng giữa hai mũ phải đi qua BAP, nên mũ còn lại phải là **BAP Unicast Client** |
| Một radio. Ở ISO interval 10 ms, controller dành sẵn 1.3 ms mỗi chu kỳ cho vai trò khác (`BT_CTLR_SDC_CIG_RESERVED_TIME_US`) [SDK] | Giới hạn số lần gửi lại theo PHY (mục 2.5) |
| CPU: cuộc gọi 32 kHz hiện dùng **98 % CPU** (`helmet cpu`, 16/10/2026) [Đo] | Intercom chạy 16 kHz mono hai chiều. Phải làm các tối ưu CPU còn lại **trước** (mốc I0) |
| RAM đã dùng 82.9 % (217 KB / 256 KB) [Đo] | Ngân sách RAM rất chặt (mục 1.7) |
| Không có màn hình, bàn phím; chỉ có 4 nút | Ghép đôi kiểu Just Works, điều khiển bằng cử chỉ nút |
| Anten trên module, công suất phát hiện 0 dBm (`CONFIG_BT_CTLR_TX_PWR_0`) [SDK] | Tầm xa hạn chế (mục 2.5) |

### 1.4 Các phương án đã cân nhắc

| | Phương án | Ưu điểm | Nhược điểm | Kết luận |
|---|---|---|---|---|
| **A** | **LE Audio BAP unicast**: một mũ là central + BAP Unicast Client, mũ kia dùng ASCS sẵn có | Theo chuẩn. Phía nhận **đã có và đã chạy** với điện thoại. Dùng lại nguyên đường âm thanh của cuộc gọi. CIS được mã hóa | Thêm vai trò central và BAP client (tốn flash, RAM). Hai mũ có vai trò khác nhau | **Chọn** |
| B | CIS "trần" (`bt_iso_chan_connect`) + GATT riêng | Ít mã | **Không làm được**: ISO server đã thuộc ASCS (mục 1.3) | Loại |
| C | Auracast hai chiều: mỗi mũ phát một BIG và đồng bộ BIG của mũ kia | Không cần kết nối, mở rộng được nhóm | Mỗi mũ vừa phát vừa thu BIG, cộng periodic advertising sync. Không có xác nhận gói, độ trễ cao hơn, bảo mật chỉ bằng broadcast code | Để dành cho nhóm > 2 mũ (mục 12) |
| D | Gửi khung LC3 qua GATT notification hoặc L2CAP CoC | Đơn giản, không cần ISO | Không có nhịp đồng bộ, jitter lớn, phải tự làm hàng đợi, tốn air time | Loại |
| E | Mượn TBS/CCP: mũ khởi tạo đóng vai "điện thoại" để mũ kia đổ chuông qua `call_ctrl.c` sẵn có | Dùng lại giao diện cuộc gọi | Không đối xứng (mũ kia không gọi ngược lại được). TBS server tốn RAM | Loại. Thay bằng dịch vụ điều khiển riêng (mục 6.2) |

### 1.5 Kiến trúc

**Sơ đồ kết nối** (bản đầu: mỗi lúc chỉ có một luồng âm thanh trên mỗi mũ, mục 1.8):

```
        Điện thoại A                                       Điện thoại B
     (central, BAP client)                              (central, BAP client)
              │ ACL + CIS (nhạc / cuộc gọi)                      │ ACL + CIS
              ▼                                                  ▼
    ┌───────────────────────┐   ACL: HIS + BAP (ASCS/PACS)   ┌───────────────────────┐
    │ Mũ A = INITIATOR      │ ─────────────────────────────► │ Mũ B = ACCEPTOR       │
    │ central với mũ B,     │   CIS hai chiều, LC3 16_2      │ peripheral với cả     │
    │ BAP Unicast Client,   │ ◄────────────────────────────► │ điện thoại B và mũ A, │
    │ peripheral với ĐT A   │                                │ ASCS sẵn có           │
    └───────────────────────┘                                └───────────────────────┘
```

**Trong firmware** (phần mới in đậm):

```
┌──────────────────────────────────────── apps/main ────────────────────────────────────────┐
│ main.c: bộ điều khiển (hàng đợi sự kiện, trạng thái thiết bị, phone_conn / ic_conn)       │
│    ▲ sự kiện EVT_IC_*                                                                     │
│ **intercom.c**: phiên, ghép đôi, quét/kết nối lại, chính sách ưu tiên, settings           │
│ **ic_service.c**: Helmet Intercom Service (server ở mọi mũ, client ở initiator)           │
│ **ic_unicast.c**: BAP Unicast Client (chỉ chạy khi mũ là initiator)                       │
│ le_audio.c: ASCS server, 2 kho stream (điện thoại / mũ bạn), kiểm ngữ cảnh theo kết nối   │
│ voice_tx.c ─► stream gửi (ASE source cho điện thoại, hoặc stream client tới mũ bạn)       │
│ audio_out.c ◄─ stream nhận (giống nhau cho điện thoại và mũ bạn)                          │
│ buttons.c (thêm "giữ rồi nhả"), prompts.c (+9 âm báo), power.c, call_ctrl.c, media_ctrl.c │
├──────────────────────────────────────── Zephyr / NCS ─────────────────────────────────────┤
│ BAP Unicast Server + **BAP Unicast Client** · **central + quét** · **Filter Accept List** │
│ **2 bộ quảng bá extended** · ISO central + peripheral · SMP · GATT client · liblc3        │
└───────────────────────────────────────────────────────────────────────────────────────────┘
```

### 1.6 Vai trò của hai mũ

Hai mũ chạy **cùng một firmware**. Vai trò được chốt **một lần lúc ghép đôi** (mục 4.3) và lưu lại:

| | INITIATOR | ACCEPTOR |
|---|---|---|
| Liên kết ACL với mũ bạn | Central: tự kết nối khi thấy bạn | Peripheral: quảng bá cho riêng bạn |
| LE Audio | BAP Unicast Client: dò PACS/ASCS của bạn, cấu hình codec, QoS, Enable, mở CIS | ASCS sẵn có, giống khi điện thoại cấu hình |
| HIS (dịch vụ điều khiển) | Client: ghi Control Point, nhận Event | Server: nhận Control Point, gửi Event |
| Người dùng thấy gì | **Giống hệt nhau**: mũ nào cũng bấm mở, đóng, nghe, từ chối được |

### 1.7 Ngân sách RAM, flash, CPU

**CPU**: dùng số đo cuộc gọi thật ngày 16/10/2026 làm gốc:

| Khối | Cuộc gọi điện thoại 32 kHz [Đo] | Intercom 16 kHz mono hai chiều [Cần đo] |
|---|---|---|
| Giải mã LC3 (`audio_tid`) | 2.25 ms/khung (32 kHz **stereo**) | khoảng 0.6 ms (mono 16 kHz, khoảng ¼ khối lượng) |
| Khối xuất (`audio_tid`) | 0.72 ms | khoảng 0.5 ms (I2S 16 kHz) |
| Voice DSP (luôn chạy 16 kHz) | khoảng 3.8 ms CPU | khoảng 3.8 ms (không đổi) |
| Mã hóa LC3 | khoảng 3.1 ms (32 kHz + nâng mẫu ×2) | khoảng 1.5 ms (16 kHz) |
| Bluetooth (MPSL, RX, TX) | khoảng 2.4 % | khoảng 3–4 % (2 liên kết + quét) |
| **Tổng (`helmet cpu`, TOTAL non-idle)** | **98.1 %** | **khoảng 65–70 %** |

Kết luận:
- Intercom **khả thi về CPU** nhưng biên dư mỏng.
- Phải làm **trước** các tối ưu còn lại của cuộc gọi: micro chỉ 16 kHz, FFT thực CMSIS-DSP cho giảm ồn, và AEC 128 hệ số nếu đo được đường vọng đủ ngắn. Các tối ưu này nằm trong mốc I0.
- Mục tiêu khi intercom: `TOTAL` ≤ 75 %.

**RAM** (hiện 217 KB / 256 KB):

| Thành phần thêm | Ước lượng | Cách tính |
|---|---|---|
| Controller: +1 liên kết central | 1.0 KB | `SDC_MEM_PER_CENTRAL_LINK(27,69,3,2)` = 1013 B [SDK] |
| Controller: +1 liên kết peripheral | 1.1 KB | `SDC_MEM_PER_PERIPHERAL_LINK(27,69,3,2)` = 1133 B [SDK] |
| Controller: 1 CIG + 1 CIS | 0.7 KB | `SDC_MEM_PER_CIG(1)` = 176 B, mỗi CIS thêm 547 B [SDK] |
| Controller: quét extended (3 bộ đệm) | 1.4 KB | `SDC_MEM_SCAN_EXT(3)` = 1368 B [SDK] |
| Controller: bộ quảng bá thứ 2 | 0.6–0.9 KB | `SDC_MEM_PER_ADV_SET(31…191)` = 574–862 B [SDK] |
| Host: +2 `bt_conn`, ATT/L2CAP/SMP mỗi kết nối | 2–3 KB | [Cần đo] |
| Host: bản sao theo kết nối (MCC, TBS client, ASE của ASCS, PACS) | 3–5 KB | [Cần đo] |
| BAP Unicast Client (2 endpoint, 2 stream, 1 group) | khoảng 1.5 KB | [Cần đo] |
| App: `intercom.c`, `ic_service.c`, `ic_unicast.c`, 2 stream | khoảng 1.5 KB | |
| **Tổng thêm** | **khoảng 13–17 KB** | RAM dự kiến **230–234 KB (90–91 %)** [Cần đo] |

Nguồn giảm RAM nếu thiếu, theo thứ tự ít ảnh hưởng nhất trước:
- Tắt `capture.c` trong bản phát hành.
- Tắt các mục đọc tùy chọn của MCC (tên bài, vị trí, tốc độ phát…).
- `CONFIG_LOG_BUFFER_SIZE` 4096 → 2048, `CONFIG_BT_ATT_TX_COUNT` 20 → 12.
- `MIC_BLOCK_COUNT` 12 → 8 sau khi CPU không còn quá tải.

**Flash**: hiện 495 KB (31.7 %). Phần central, quét, BAP client và GATT client discovery thêm khoảng 40–60 KB, lên khoảng 35 % [Cần đo].

### 1.8 Mức hoạt động đồng thời với điện thoại

| Mức | Nội dung | Bản đầu |
|---|---|---|
| **0 (bản đầu)** | **Mỗi mũ chỉ một luồng LE Audio tại một thời điểm**: hoặc luồng điện thoại (nhạc/gọi), hoặc intercom. Liên kết ACL với điện thoại vẫn giữ để nhận thông báo cuộc gọi và điều khiển nhạc | **Có** |
| 1 | Intercom + nhạc nền (nhạc giảm khoảng −15 dB) | Không. Cần 2 bộ giải mã (48 kHz + 16 kHz), CIS của điện thoại và mũ bạn chạy cùng lúc. Một thiết bị vừa là CIS central vừa là CIS peripheral **không nằm trong cấu hình Nordic đã thử** [SDK] |
| 2 | Chia sẻ cuộc gọi điện thoại cho bạn | Không. Cần trộn và mã hóa lại, tốn thêm CPU |

Lý do chọn mức 0: tại mỗi thời điểm, mỗi mũ chỉ có một CIG/CIS hoạt động. Như vậy tránh được cấu hình controller chưa được thử, và CPU giống hệt một cuộc gọi.

### 1.9 Thứ tự hiện thực đề xuất

| Mốc | Nội dung | Kiểm được khi nào | Phần cứng cần |
|---|---|---|---|
| **I0** | Nền. Tối ưu CPU còn lại (micro 16 kHz, FFT CMSIS-DSP, AEC 128 hệ số nếu đo cho phép). Bật `CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE`. `pdm20` dùng `PCLK32M_HFXO`. Tách `phone_conn` / `ic_conn` trong `main.c`. `MAX_CONN=3`, bộ quảng bá thứ 2. Âm báo lặp được nối lại sau âm báo ngắn. Nút "giữ rồi nhả" | Chạy lại toàn bộ kiểm thử điện thoại (M0–M5 trong hướng dẫn sử dụng). `helmet cpu` khi gọi ≤ 70 % | 1 mũ + điện thoại |
| **I1** | Ghép đôi, liên kết thường trực, tự kết nối lại, HIS (HELLO), chưa có âm thanh | Lệnh `helmet ic …` trên 2 mạch | **2 mạch** |
| **I2** | BAP Unicast Client: mở luồng với **âm thử** thay micro (`helmet ic tone`) | Heartbeat hai bên: SDU, PLC, underrun | 2 mạch, chưa cần micro |
| **I3** | Máy trạng thái phiên, nút, âm báo, ưu tiên điện thoại | Kịch bản mục 11 | 2 mạch + 1–2 điện thoại |
| **I4** | Micro và loa thật: AEC trong intercom, đo độ trễ, đo tầm, chọn PHY và công suất phát | Đo ngoài trời | 2 mũ hoàn chỉnh |
| I5 (tùy chọn) | Chế độ tầm xa Coded S2. Khởi động nhanh (giữ ASE ở QoS Configured giữa các phiên) | So sánh tầm xa và thời gian mở | 2 mũ |

---

## 2. Kiến thức nền mới

### 2.1 Hai mũ "nói chuyện" qua BAP như thế nào

Trên liên kết mũ–mũ, **initiator làm đúng việc của điện thoại**: cấu hình hai ASE của acceptor. ASE sink chở giọng của initiator tới loa acceptor. ASE source chở giọng của acceptor tới initiator. Hai ASE được ghép vào **một CIS hai chiều**.

```
Initiator (BAP client)                           Acceptor (ASCS)
  dò PACS + ASCS (1 lần mỗi kết nối) ──────────► trả bản ghi PAC, danh sách ASE
  Config Codec (sink: 16 kHz mono 40 B) ───────► ASE sink   : Idle → Codec Configured
  Config Codec (source: 16 kHz mono 40 B) ─────► ASE source : Idle → Codec Configured
  Config QoS (10 ms, RTN, latency, PD) ────────► cả hai     : → QoS Configured
  Enable (metadata: Conversational) ───────────► cả hai     : → Enabling
  tạo CIS (LE Create CIS) ─────────────────────► ascs_iso_accept: nhận vì khớp ASE
                                                 ASE sink: acceptor tự báo Receiver Start Ready
  Receiver Start Ready cho ASE source ─────────► ASE source : → Streaming
  ◄═════════ SDU 40 B mỗi 10 ms, cả hai chiều trên cùng CIS ═════════►
Kết thúc:
  Disable ─────────────────────────────────────► sink → QoS Configured, source → Disabling
  Receiver Stop Ready (source) ────────────────► source → QoS Configured, CIS đóng
  Release ─────────────────────────────────────► → Releasing → Idle
```

Phía acceptor, toàn bộ chuỗi này đã chạy được với điện thoại. Chỉ cần thêm phân biệt kết nối "điện thoại" và "mũ bạn" (mục 6.5).

### 2.2 Nhiều kết nối cùng lúc: vai trò và số ô `bt_conn`

Theo hai bài học trước:
- Quảng bá connectable **cấp sẵn một `bt_conn`** cho mỗi bộ quảng bá.
- `recycled()` nghĩa là "có một ô trống", không phải "đã ngắt kết nối" (bài học livelock).

Với `CONFIG_BT_MAX_CONN=3` và `CONFIG_BT_CTLR_SDC_PERIPHERAL_COUNT=2`, controller cho phép 2 liên kết peripheral và 3 − 2 = **1 liên kết central** [SDK]. Mặc định chỉ có 1 peripheral khi bật cả central lẫn peripheral, nên **phải đặt** giá trị này.

| Tình huống | INITIATOR dùng ô | ACCEPTOR dùng ô |
|---|---|---|
| Chưa có kết nối nào | set 0 cấp sẵn + tự kết nối (auto-connect) = 2 | set 0 cấp sẵn + set 1 cấp sẵn = 2 |
| Chỉ có điện thoại | điện thoại + tự kết nối = 2 | điện thoại + set 1 = 2 |
| Chỉ có mũ bạn | mũ bạn + set 0 = 2 | mũ bạn + set 0 = 2 |
| Cả hai | điện thoại + mũ bạn = 2 (set 0 dừng như hiện nay) | điện thoại + mũ bạn = 2 |
| Đang ghép đôi | set 0 + set 1 → dừng set 1 rồi mới tạo kết nối = 2–3 | set 0 + set 1 = 2 |

Ô thứ 3 là phần dự phòng cho lúc một `bt_conn` cũ chưa kịp trả về. Bộ quảng bá set 0 (cho điện thoại) **giữ nguyên logic chống livelock** hiện có (`adv_on`, `adv_on_slow`). Set 1 dùng cùng mẫu đó.

### 2.3 Riêng tư (RPA), Filter Accept List và tự kết nối

- Mũ bật `CONFIG_BT_PRIVACY`, nên quảng bá bằng địa chỉ ngẫu nhiên đổi định kỳ (RPA). Sau khi ghép đôi, hai mũ có IRK của nhau, và controller tự giải RPA nhờ danh sách resolving list do Zephyr nạp từ bond.
- **Filter Accept List** (FAL, `CONFIG_BT_FILTER_ACCEPT_LIST`, 8 mục, `CONFIG_BT_CTLR_FAL_SIZE=8`) [SDK] chứa **địa chỉ định danh** của bạn:
  - Initiator gọi `bt_conn_le_create_auto()`. Controller tự quét và kết nối khi thấy bạn, CPU không cần làm gì.
  - Acceptor quảng bá set 1 với `BT_LE_ADV_OPT_FILTER_CONN`, nên **chỉ bạn kết nối được**. Điện thoại lạ thấy cũng không kết nối được.
- Set 1 **không có AD Flags** (non-discoverable), nên điện thoại không hiện nó trong danh sách thiết bị.

### 2.4 Ghép lại khi một bên đã xóa bond (bài học ngày 16/10/2026)

Khi một bên còn bond cũ, Zephyr **từ chối** lần ghép không xác thực mới từ cùng địa chỉ: `update_keys_check()` trả `false` và SMP gửi *Authentication Requirements* [SDK]. Điện thoại báo lỗi này là "incorrect PIN or passkey". Với hai mũ, chuyện này xảy ra khi một mũ bị xóa bond (giữ Vol+ cùng Vol− 5 s) còn mũ kia vẫn giữ.

Thiết kế bật **`CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE=y`** (mốc I0). Bản sửa này cũng giải quyết việc ghép lại điện thoại sau khi "quên thiết bị". Đánh đổi: ai sao chép được địa chỉ định danh của một thiết bị đã ghép thì có thể ghép đè. Mọi bond của mũ vốn đã là Just Works (không chống MITM), nên rủi ro tăng thêm nhỏ.

### 2.5 PHY, công suất phát và tầm xa

**Air time mỗi chu kỳ 10 ms** cho SDU 40 B. Gói gồm 40 B + MIC 4 B + header 2 B + CRC 3 B, và mỗi sự kiện con gồm 2 gói cộng 2 khoảng nghỉ 150 µs:

| PHY | 1 gói (µs) | 1 sự kiện con hai chiều (µs) | Sự kiện con tối đa trong 8.7 ms | RTN tối đa (FT=1) | Độ nhạy thu (datasheet, phải kiểm lại) | Tầm tương đối, cùng công suất |
|---|---|---|---|---|---|---|
| 2M | 220 | khoảng 740 | 11 | 10 | khoảng −93 dBm | 1.0× |
| 1M | 432 | khoảng 1160 | 7 | 6 | khoảng −96 dBm | khoảng 1.4× |
| Coded S2 | khoảng 1170 | khoảng 2630 | 3 | 2 | khoảng −100 dBm | khoảng 2.2× |
| Coded S8 | khoảng 3540 | khoảng 7370 | 1 | 0 | khoảng −104 dBm | khoảng 3.5× (**không dùng được** ở 10 ms) |

- Công suất phát +8 dBm so với 0 dBm hiện nay cho thêm 8 dB, tức khoảng ×2.5 tầm trong không gian tự do. `CONFIG_BT_CTLR_TX_PWR_PLUS_8` có trên nRF54L [SDK]. Đổi lại dòng tiêu thụ khi phát tăng [Cần đo].
- Tài liệu SoftDevice Controller ghi CIS hai chiều trên 1M và Coded đã được thử, với ISO interval dài hơn [SDK].
- **Bản đầu**: 1M PHY, +8 dBm, RTN 4. So với 2M ở 0 dBm, link budget hơn khoảng 11 dB, tức khoảng ×3.5 tầm.
- **Tầm thực tế phải đo** (mốc I4). Đầu người, mũ và thân xe làm suy hao thêm 10–20 dB. Với anten trên module, ước lượng thô là **vài chục tới hơn trăm mét khi nhìn thẳng**. Như vậy phù hợp người ngồi sau hoặc các xe đi gần nhau, **không** so được với intercom thương mại tầm 1–2 km (họ dùng Classic công suất cao, radio riêng hoặc anten ngoài).

### 2.6 Đồng hồ của hai mũ và bù trôi

- Nhịp CIS do controller của initiator quyết định. Điểm neo của nó bám theo LFXO, mỗi mũ có sai số vài chục ppm.
- Micro (PDM) và loa (I2S) chạy theo HFXO của từng mũ.
- Cơ chế bù trôi hiện có (lấy thêm hoặc bớt 1 mẫu mỗi khung theo mức đầy trung bình) đã chạy ở **cả hai chiều**: `audio_out.c` chiều nghe, `voice_tx.c` chiều nói. Vì vậy dùng được nguyên cho intercom.
- Bài học ngày 16/10/2026: bù trôi chỉ đúng khi **CPU không quá tải**. Khi quá tải, SDU im lặng bị chèn vào và cơ chế bù trôi phải bỏ hàng nghìn mẫu (`slip -5557`) [Đo].
- `pdm20` đang khai `clock-source = "PCLK32M"`, nên driver không tự giữ HFXO [SDK]. Trong cuộc gọi, I2S (mặc định `PCLK32M_HFXO`) giữ HFXO hộ. Mốc I0 đổi `pdm20` sang `PCLK32M_HFXO` để micro luôn đúng clock, kể cả khi loa tắt.

---

## 3. Thay đổi cấu hình và giao diện chung

### 3.1 `prj.conf`

```
# ---- Nhieu ket noi: dien thoai + mu ban ----
CONFIG_BT_CENTRAL=y                       # chon luon BT_OBSERVER (quet)
CONFIG_BT_MAX_CONN=3
CONFIG_BT_CTLR_SDC_PERIPHERAL_COUNT=2     # 2 peripheral + 1 central (muc 2.2)
CONFIG_BT_FILTER_ACCEPT_LIST=y
CONFIG_BT_EXT_ADV_MAX_ADV_SET=2           # set 0: dien thoai, set 1: mu ban
CONFIG_BT_SMP_APP_PAIRING_ACCEPT=y        # chi cho ghep qua set 1 khi dang o che do ghep doi
CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE=y    # muc 2.4
CONFIG_BT_MAX_PAIRED=5                    # 4 dien thoai + 1 mu ban

# ---- BAP Unicast Client (chi dung khi la initiator) ----
CONFIG_BT_ISO_CENTRAL=y
CONFIG_BT_BAP_UNICAST_CLIENT=y            # phu thuoc GATT_CLIENT, AUTO_DISCOVER_CCC, AUTO_UPDATE_MTU,
CONFIG_BT_GATT_AUTO_UPDATE_MTU=y          #   CENTRAL, ISO_CENTRAL, BONDABLE [SDK]
CONFIG_BT_BAP_UNICAST_CLIENT_ASE_SNK_COUNT=1
CONFIG_BT_BAP_UNICAST_CLIENT_ASE_SRC_COUNT=1
CONFIG_BT_BAP_UNICAST_CLIENT_GROUP_COUNT=1
CONFIG_BT_BAP_UNICAST_CLIENT_GROUP_STREAM_COUNT=2

# ---- Doi gia tri hien co ----
CONFIG_BT_ISO_MAX_CHAN=4                  # 3 (dien thoai) + 1 CIS hai chieu (mu ban)
CONFIG_BT_ASCS_MAX_ACTIVE_ASES=5          # dien thoai 3 + mu ban 2
# CONFIG_BT_ISO_TX_BUF_COUNT giu 3 = CONFIG_BT_CTLR_SDC_ISO_TX_HCI_BUFFER_COUNT
#   (moi luc chi 1 luong gui; hai gia tri PHAI bang nhau - bai hoc cu)
# CONFIG_BT_AUDIO_CODEC_CFG_MAX_METADATA_SIZE=32 da co (sua ngay 16/10)

# ---- Vo tuyen ----
CONFIG_BT_CTLR_TX_PWR_PLUS_8=y            # [De xuat], do dong tieu thu o moc I4
# CONFIG_BT_CTLR_PHY_CODED=y              # chi o moc I5 (che do tam xa)
```

### 3.2 `CMakeLists.txt`

Thêm `src/intercom.c`, `src/ic_service.c`, `src/ic_unicast.c`. Giữ cờ `-O3` hiện có cho các file âm thanh.

**Lưu ý:** mỗi khi thêm một file overlay devicetree mới, phải build sạch (`make APP=apps/main pristine`). Lý do là CMake lưu sẵn `DTC_OVERLAY_FILE` rỗng từ lần cấu hình đầu (đã gặp ngày 16/10/2026).

### 3.3 `helmet.h`: API mới [Đề xuất]

```c
/* ---------------- intercom.c ---------------- */
enum ic_role { IC_ROLE_NONE, IC_ROLE_INITIATOR, IC_ROLE_ACCEPTOR };
enum ic_state {
	IC_NO_PEER,      /* chua ghep ban */
	IC_PAIRING,      /* dang ghep doi (60 s) */
	IC_DISCONNECTED, /* co ban, chua co lien ket (dang tu ket noi lai) */
	IC_LINKED,       /* ACL + ma hoa + HELLO xong, chua co phien */
	IC_OUTGOING,     /* da gui START_REQ, cho ban tra loi */
	IC_INCOMING,     /* ban goi, dang do chuong (tu tra loi tat) */
	IC_SETUP,        /* dang dung luong BAP */
	IC_ACTIVE,       /* dang dam thoai */
	IC_SUSPENDED,    /* tam dung vi cuoc goi dien thoai (o mu nay hoac mu ban) */
	IC_TEARDOWN,     /* dang dong luong */
};

int intercom_init(void);
void intercom_handle(const struct helmet_evt *evt);  /* EVT_IC, chay trong thread bo dieu khien */
bool intercom_is_peer_conn(const struct bt_conn *conn);
void intercom_conn_connected(struct bt_conn *conn, bool via_ic_adv);
void intercom_conn_secured(struct bt_conn *conn);   /* sau khi ma hoa: xac dinh ban hay dien thoai */
void intercom_conn_disconnected(struct bt_conn *conn, uint8_t reason);
int intercom_start(void);
int intercom_stop(uint8_t reason);
int intercom_accept(void);
int intercom_reject(void);
int intercom_pair_start(void);
void intercom_pair_stop(void);
int intercom_unpair(void);
enum ic_state intercom_state(void);
bool intercom_session_active(void);                 /* OUTGOING..TEARDOWN */
void intercom_phone_call(bool active);              /* tu call_ctrl qua bo dieu khien */
void intercom_print_status(const struct shell *sh);

/* ---------------- ic_unicast.c (initiator) ---------------- */
int ic_unicast_discover(struct bt_conn *conn);      /* 1 lan moi ket noi */
int ic_unicast_start(const struct bt_bap_qos_cfg *qos);
int ic_unicast_stop(void);

/* ---------------- ic_service.c ---------------- */
struct ic_msg { uint8_t op; uint8_t session; uint8_t arg; uint8_t arg2; };
int ic_svc_send(struct bt_conn *conn, const struct ic_msg *m);  /* CP (client) hoac Event (server) */
int ic_svc_discover(struct bt_conn *conn);                       /* initiator */
```

Sự kiện mới: một loại `EVT_IC`. Trong đó `a` là loại con (`IC_EV_ADV_FOUND`, `IC_EV_CONNECTED`, `IC_EV_SECURED`, `IC_EV_DISCOVERED`, `IC_EV_MSG`, `IC_EV_STREAM`, `IC_EV_TIMER`, `IC_EV_DISCONNECTED`). Tin nhắn HIS gói gọn trong `b` và `v` (opcode, session, tham số), nên **không cần bộ đệm riêng** cho hàng sự kiện.

Trạng thái thiết bị thêm `HS_INTERCOM` (mục 8.1).

### 3.4 Cài đặt lưu (settings)

| Khóa | Nội dung | Ghi chú |
|---|---|---|
| `helmet/ic/peer` | `{ bt_addr_le_t id; uint8_t role; uint8_t ver; uint16_t caps; }` (12 B) | Ghi lúc ghép đôi xong, xóa khi bỏ ghép |
| `helmet/cfg` (thêm trường) | `ic_autoanswer` (mặc định 1), `ic_autoresume` (1), `ic_phy` (1M) | Các trường **nối vào cuối** struct. `cfg_set` phải nhận được bản lưu cũ ngắn hơn (giữ giá trị mặc định cho trường mới) |

Lúc khởi động, kiểm tra `peer.id` còn trong danh sách bond (`bt_foreach_bond`). Nếu bond đã mất, ví dụ bị xóa bằng Vol+ cùng Vol−, thì xóa luôn `helmet/ic/peer` và ghi log.

---

## 4. Ghép đôi hai mũ

### 4.1 Yêu cầu

| ID | Yêu cầu |
|---|---|
| FR-25.1 | Ghép đôi chỉ bằng nút: hai người cùng **giữ Call 3 s**, đặt hai mũ **sát nhau** (dưới khoảng 50 cm). Ghép xong ≤ 15 s |
| FR-25.1a | Không ghép nhầm với mũ khác ở xa (đi theo đoàn): chỉ nhận mũ có suy hao đường truyền ≤ 55 dB [Cần đo] |
| FR-25.1b | Ghép bạn mới thì **thay** bạn cũ. Bond của bạn cũ chỉ bị xóa **sau khi** ghép mới thành công |

### 4.2 Quảng bá intercom (set 1)

Bộ quảng bá extended, connectable, không có AD Flags. Dữ liệu gồm một AD *Service Data – 128-bit UUID* (kiểu 0x21):

| Byte | Trường | Ý nghĩa |
|---|---|---|
| 0–15 | UUID dịch vụ HIS | `0eed0001-080c-4225-a3d0-f91ce86a80c2` [Đề xuất] |
| 16 | `ver` | Phiên bản giao thức = 1 |
| 17 | `flags` | bit 0 PAIRING, bit 1 hỗ trợ Coded PHY, bit 2 tự trả lời đang bật, các bit còn lại để dành |
| 18–21 | `nonce` | Số ngẫu nhiên 32 bit, chỉ có nghĩa khi PAIRING, tạo mới mỗi lần vào chế độ ghép |
| 22 | `tx_dbm` | Công suất phát của gói quảng bá (int8), dùng để tính suy hao `tx_dbm − RSSI` |

Tổng cộng 25 B, vừa trong một gói AUX_ADV_IND.

| Chế độ | Bộ lọc kết nối | Chu kỳ quảng bá |
|---|---|---|
| Ghép đôi (cả hai mũ) | Không lọc (bạn chưa có trong FAL) | 100 ms |
| Thường (chỉ acceptor) | FAL: chỉ bạn | 100 ms trong 30 s đầu sau khi bật / mất liên kết / bấm mở, sau đó 500 ms [Đề xuất] |
| Initiator khi không ghép đôi | Không quảng bá set 1 | |

### 4.3 Quy trình ghép đôi

```
 Mũ A (nonce 0x9C…)                                     Mũ B (nonce 0x31…)
 giữ Call 3 s → IC_PAIRING, âm "ic_pairing" lặp         giữ Call 3 s → IC_PAIRING
 quảng bá set 1 (PAIRING, nonce) + quét 60/30 ms        quảng bá set 1 (PAIRING, nonce) + quét
        ◄────────── thấy nhau, suy hao ≤ 55 dB, cùng ver ──────────►
 nonce A > nonce B → A là INITIATOR                     nonce B < nonce A → B là ACCEPTOR:
                                                          dừng quét, giữ set 1, chờ 10 s
 dừng quét + dừng set 1, bt_conn_le_create(B) ────────► connected qua set 1 (callback
                                                          bt_le_ext_adv_cb.connected)
 bt_conn_set_security(L2) ── SMP LESC Just Works ─────► pairing_accept: cho phép vì đang
                                                          IC_PAIRING và kết nối đến qua set 1
        ◄─────────── bond: LTK, IRK, địa chỉ định danh ─────────────►
 dò HIS, đăng ký Event; ghi CP HELLO(ver, caps) ──────► kiểm tra ver
        ◄──────────────────────────────── Event HELLO(ver, caps)
 ghi CP PAIR_CONFIRM ─────────────────────────────────► lưu peer {id A, role ACCEPTOR}
        ◄──────────────────────────────── Event PAIR_ACK
 lưu peer {id B, role INITIATOR}                        xóa bond bạn cũ (nếu khác A)
 xóa bond bạn cũ (nếu khác B)
 âm "ic_paired", vào IC_LINKED                          âm "ic_paired", vào IC_LINKED
 tự mở một phiên thử 10 s (cả hai nghe nhau) [Đề xuất]
```

Các trường hợp đặc biệt:
- **Hai nonce bằng nhau** (xác suất 1/2³²): cả hai tạo nonce mới và đổi dữ liệu quảng bá.
- **Thấy nhiều mũ đang ghép:** chọn mũ có suy hao nhỏ nhất. Hai mũ khác nhau đều phải thấy nhau, nên ghép chéo nhầm là rất khó.
- **Hết 60 s** chưa ghép được: âm "ic_unavailable", quay về trạng thái trước đó.

### 4.4 Lưu, thay, xóa bạn đồng hành

| Việc | Cách |
|---|---|
| Ghép bạn mới | Như 4.3. Sau khi lưu `peer` mới, gọi `bt_unpair(BT_ID_DEFAULT, &old.id)` nếu khác. Bạn cũ sẽ không kết nối được nữa: không còn trong FAL, không còn khóa |
| Bỏ ghép riêng intercom | `helmet ic unpair` (shell). Không thêm cử chỉ nút để tránh bấm nhầm |
| Xóa toàn bộ bond (Vol+ cùng Vol− 5 s, như hiện nay) | Xóa luôn `helmet/ic/peer` |
| Bond của bạn bị đẩy ra vì `MAX_PAIRED` | `CONFIG_BT_KEYS_OVERWRITE_OLDEST` đẩy bond **ít dùng nhất**. Bạn đồng hành thường xuyên kết nối nên khó bị đẩy. Tăng `MAX_PAIRED` lên 5. Lúc khởi động kiểm tra lại như ở mục 3.4 |

### 4.5 Bảo mật

| Mối đe dọa | Biện pháp |
|---|---|
| Nghe lén thụ động | LESC (ECDH): kẻ nghe lén không suy ra được khóa, kể cả khi bắt được lúc ghép. CIS được mã hóa bằng khóa phiên của ACL |
| MITM lúc ghép (Just Works) | Cả hai người phải bấm, cửa sổ chỉ 60 s, chặn theo suy hao ≤ 55 dB (hai mũ phải sát nhau). Mức này tương đương intercom thương mại không có màn hình |
| Thiết bị lạ kết nối set 1 | FAL. Ngoài chế độ ghép, `pairing_accept` từ chối mọi lần ghép qua set 1 |
| Thiết bị lạ kết nối qua set 0 (cho điện thoại) rồi ghi HIS | Mọi đặc tính HIS yêu cầu mã hóa, **và** app chỉ nhận khi `conn == ic_conn` (địa chỉ định danh khớp `peer.id`) |
| Ghép đè bond (`ALLOW_UNAUTH_OVERWRITE`) | Đã chấp nhận ở mục 2.4 |

### 4.6 Lỗi và xử lý

| Lỗi | Xử lý |
|---|---|
| Ghép SMP thất bại | Ngắt kết nối (chính sách hiện có). Cả hai quay lại quét/quảng bá trong phần thời gian còn lại của 60 s |
| `ver` không tương thích | REJECT(VERSION). Âm "ic_unavailable". Log ghi rõ hai phiên bản |
| Mất kết nối giữa chừng (trước PAIR_ACK) | Không lưu gì. Bond (nếu đã tạo) được xóa bằng `bt_unpair` |

---

## 5. Liên kết thường trực và tự kết nối lại

### 5.1 Chính sách

**Giữ ACL thường trực** khi hai mũ cùng bật và trong tầm, còn âm thanh chỉ chạy khi có phiên [Đề xuất].
- Lý do: mở intercom nhanh (≤ 1.5 s), và mũ bạn gọi tới được.
- Cái giá: ACL rảnh ở 60 ms tiêu thụ chỉ vài chục µA, rất nhỏ so với ampli loa.

### 5.2 INITIATOR: tự kết nối bằng FAL

- `bt_le_filter_accept_list_add(&peer.id)`, sau đó `bt_conn_le_create_auto(create_param, conn_param)`.
- Quét **nhanh** 60 ms / 30 ms trong 30 s sau khi bật, mất liên kết, hoặc người dùng bấm mở intercom. Sau đó chuyển sang quét **chậm** 1280 ms / 30 ms (khoảng 2 % thời gian thu) [Đề xuất].
- Muốn đổi tham số quét thì gọi `bt_conn_create_auto_stop()` rồi bắt đầu lại.
- Tự kết nối dừng trong lúc ghép đôi (chế độ đó dùng quét thường).

### 5.3 ACCEPTOR: set 1 lọc theo FAL

Như mục 4.2, chế độ "thường". Set 1 dừng khi đã có `ic_conn`.

### 5.4 Tham số ACL với mũ bạn

| Tham số | Giá trị [Đề xuất] | Lý do |
|---|---|---|
| Chu kỳ | 60 ms (48 × 1.25 ms) | BAP khuyên 60/67.5/70 ms khi có ISO. Không tranh chỗ với CIS 10 ms |
| Peripheral latency | 0 | Đơn giản. Thời gian thiết lập BAP không bị kéo dài |
| Supervision timeout | 4 s | Phát hiện mất sóng đủ nhanh mà không ngắt oan khi bị che sóng ngắn |

### 5.5 Khóa cũ hoặc mất bond

- Mã hóa thất bại (`BT_SECURITY_ERR_PIN_OR_KEY_MISSING`) với `peer.id`: tăng bộ đếm.
- Sau 3 lần liên tiếp: đánh dấu `peer` "hỏng", dừng tự kết nối để khỏi tốn pin. Lần bấm mở intercom kế tiếp phát âm "ic_unavailable" và log gợi ý ghép lại.

### 5.6 Mất sóng, ra khỏi tầm

```
 CIS mất / ACL timeout 4 s ──► hai mũ: âm "ic_lost", trạng thái IC_DISCONNECTED (nhớ "phiên đang mở")
 initiator: quét nhanh 30 s ; acceptor: set 1 nhanh 30 s
 kết nối lại trong 60 s và ic_autoresume = 1 ──► tự mở lại phiên, âm "ic_start" hai bên
 quá 60 s ──► bỏ phiên (vẫn tự kết nối ACL khi gặp lại, nhưng không tự mở âm thanh)
```

---

## 6. Phiên intercom

### 6.1 Yêu cầu chi tiết

| ID | Yêu cầu | Tiêu chí |
|---|---|---|
| FR-25.2 | Tự kết nối lại khi cả hai bật và trong tầm | ≤ 5 s |
| FR-25.3 | Mở hoặc đóng intercom từ mũ bất kỳ | Từ lúc nhả nút tới khi nghe được ≤ 1.5 s (đã có liên kết) |
| FR-25.4 | Song công 16 kHz, khử vọng và giảm ồn như cuộc gọi | Hai người nói cùng lúc vẫn nghe được nhau |
| FR-25.5 | Độ trễ miệng → tai | ≤ 100 ms |
| FR-25.6 | Chất lượng đường truyền | PLC ≤ 2 % ở khoảng cách mục tiêu [TBD Q2] |
| FR-25.7 | Chạy được khi không có điện thoại. Khi có, nhạc và cuộc gọi vẫn dùng được | |
| FR-25.8 | Cuộc gọi điện thoại ưu tiên hơn intercom | Mục 7 |
| FR-25.9 | Mất sóng thì báo bằng âm, tự mở lại phiên trong 60 s | Mục 5.6 |
| FR-25.10 | Một mũ tắt thì mũ kia được báo | END(POWER_OFF) |
| FR-25.11 | Chỉ mũ đã ghép mới kết nối và mở được intercom. Liên kết luôn mã hóa | Mục 4.5 |
| FR-25.12 | Tắt/bật micro trong intercom | Có hiệu lực ≤ 20 ms |

### 6.2 Helmet Intercom Service (HIS)

Dịch vụ GATT riêng, có ở **mọi** mũ. Phía acceptor là server, phía initiator là client. UUID dùng chung phần đuôi `-080c-4225-a3d0-f91ce86a80c2` [Đề xuất]:

| Đặc tính | UUID (32 bit đầu) | Thuộc tính | Nội dung |
|---|---|---|---|
| Dịch vụ HIS | `0eed0001` | | |
| Info | `0eed0002` | Read, cần mã hóa | `ver` u8, `caps` u16 (bit 0: 16 kHz, bit 1: 32 kHz, bit 2: Coded, bit 3: tự trả lời bật), `role` u8, `name[12]` |
| Control Point | `0eed0003` | Write có phản hồi, cần mã hóa. App chỉ nhận từ `ic_conn` | Tin nhắn initiator → acceptor |
| Event | `0eed0004` | Indicate, cần mã hóa | Tin nhắn acceptor → initiator |

**Tin nhắn**: 4 byte cố định `[op][session][arg][arg2]`. `session` là bộ đếm 8 bit do **bên khởi xướng** tạo. Mọi trả lời lặp lại đúng `session` đó.

| op | Tên | Hướng | `arg` |
|---|---|---|---|
| 0x01 | HELLO | hai chiều | `ver` (và `arg2` = `caps` byte thấp) |
| 0x02 | PAIR_CONFIRM | I → A | |
| 0x03 | PAIR_ACK | A → I | |
| 0x10 | START_REQ | hai chiều | |
| 0x11 | ACCEPT | hai chiều | |
| 0x12 | REJECT | hai chiều | lý do |
| 0x13 | CANCEL | hai chiều | |
| 0x14 | END | hai chiều | lý do |
| 0x15 | SUSPEND | hai chiều | lý do |
| 0x16 | RESUME | hai chiều | |
| 0x20 | STATE | hai chiều | bit 0 đang tắt micro, bit 1 đang gọi điện thoại, bit 2 pin yếu |

| Mã lý do | Ý nghĩa |
|---|---|
| 0x00 | Người dùng |
| 0x01 | Đang có cuộc gọi điện thoại |
| 0x02 | Tắt máy |
| 0x03 | Pin yếu |
| 0x04 | Hết thời gian chờ |
| 0x05 | Lỗi thiết lập luồng |
| 0x06 | Chưa ghép |
| 0x07 | Phiên bản không tương thích |
| 0x08 | Bận (đang trong phiên khác) |

### 6.3 Máy trạng thái phiên (`intercom.c`)

```
                      ghép xong / kết nối + HELLO
  IC_NO_PEER ──────────────────────────────────────────────► IC_LINKED ◄──────────────┐
      ▲                                    mất liên kết │      │  ▲                    │
      │ bỏ ghép                                         ▼      │  │                    │
      │                                     IC_DISCONNECTED ───┘  │ kết thúc phiên    │
      │                                                            │                    │
      │        bấm mở (cục bộ)                 START_REQ từ bạn    │                    │
      │     IC_LINKED ──► IC_OUTGOING       IC_LINKED ──► IC_INCOMING                   │
      │                    │ ACCEPT          tự trả lời / bấm nghe │                    │
      │                    ▼                                       ▼                    │
      │                  IC_SETUP (initiator chạy BAP, acceptor chờ ASE) ──lỗi/5 s──────┤
      │                    │ cả hai stream đã Streaming                                 │
      │                    ▼                                                            │
      │                  IC_ACTIVE ──bấm đóng / END──► IC_TEARDOWN ─────────────────────┘
      │                    │  ▲
      │   cuộc gọi ĐT ở mũ │  │ cúp máy + tự nối lại
      │   này hoặc mũ bạn  ▼  │
      │                  IC_SUSPENDED
```

| Sự kiện | Trong trạng thái | Hành động | Trạng thái mới |
|---|---|---|---|
| Bấm mở | LINKED | Gửi START_REQ(s), âm "ic_calling" lặp, hẹn 20 s | OUTGOING |
| Bấm mở | DISCONNECTED | Quét/quảng bá nhanh 10 s. Có liên kết thì gửi START_REQ. Không có thì âm "ic_unavailable" | OUTGOING / như cũ |
| START_REQ | LINKED | Tự trả lời bật: ACCEPT, âm "ic_start". Tắt: âm "ic_ring" lặp, hẹn 20 s | SETUP / INCOMING |
| START_REQ | OUTGOING (hai bên cùng bấm) | Coi như đồng ý. Initiator chạy BAP | SETUP |
| START_REQ | đang gọi điện thoại | REJECT(PHONE_CALL) | như cũ |
| ACCEPT | OUTGOING | Dừng âm chờ. Initiator chạy BAP | SETUP |
| REJECT / hết 20 s | OUTGOING | Âm "ic_unavailable" (REJECT có lý do 0x01 thì là "ic_busy") | LINKED |
| Bấm nghe | INCOMING | ACCEPT | SETUP |
| Bấm từ chối / hết 20 s | INCOMING | REJECT(USER/TIMEOUT) | LINKED |
| Cả hai stream Streaming | SETUP | Âm "ic_start" | ACTIVE |
| Lỗi BAP hoặc hết 5 s | SETUP | END(ERROR), dọn luồng, âm "ic_unavailable" | LINKED |
| Bấm đóng / END | ACTIVE | Gửi END(USER) nếu bấm cục bộ. Initiator: Disable + Release | TEARDOWN → LINKED, âm "ic_end" |
| Cuộc gọi điện thoại được nghe | ACTIVE | Mục 7.3 | SUSPENDED |
| RESUME / cúp máy | SUSPENDED | Nếu `ic_autoresume`: chạy lại BAP | SETUP |
| Mất ACL | mọi trạng thái | Âm "ic_lost" nếu đang trong phiên | DISCONNECTED |

Các hẹn giờ:

| Tên | Giá trị |
|---|---|
| `T_IC_PAIR` | 60 s |
| `T_IC_RING` | 20 s |
| `T_IC_SETUP` | 5 s |
| `T_IC_REACH` | 10 s |
| `T_IC_RESUME` | 60 s |
| `T_IC_HELLO` | 3 s |

### 6.4 Thiết lập luồng BAP (`ic_unicast.c`, phía initiator)

**Lúc có liên kết** (một lần mỗi kết nối):
1. `bt_bap_unicast_client_discover(conn, BT_AUDIO_DIR_SINK)`, rồi `…SOURCE`.
2. Lưu endpoint sink đầu tiên và endpoint source đầu tiên. Acceptor có 2 sink và 1 source.

**Mở phiên**:
1. `bt_bap_stream_config(conn, &ic_tx, sink_ep, &cfg_16_2_mono)` và `bt_bap_stream_config(conn, &ic_rx, src_ep, &cfg_16_2_mono)`. Ngữ cảnh luồng là Conversational. ASE sink dùng Channel Allocation 0 (mono, phát ra cả hai tai). ASE source dùng FL như acceptor khai báo.
2. Lần đầu: `bt_bap_unicast_group_create()` với **một cặp** `{ rx_param = ic_rx, tx_param = ic_tx }`, nên hai chiều nằm trên một CIS [SDK: `bt_bap_unicast_group_stream_pair_param`].
3. `bt_bap_stream_qos(conn, group)`.
4. `bt_bap_stream_enable(ic_tx, meta)` và `…(ic_rx, meta)`. `meta` là Streaming Audio Contexts = Conversational (4 B).
5. `bt_bap_stream_connect(ic_tx)`. Đây là lúc tạo CIS.
6. `bt_bap_stream_start(ic_rx)`, tức initiator là bên nhận của ASE source nên gửi Receiver Start Ready. `ic_tx` bắt đầu khi acceptor tự báo Receiver Start Ready cho ASE sink.
7. Callback `started`: `ic_tx` thì gọi `voice_start(ic_tx, cfg)`, `ic_rx` thì gọi `audio_out_stream_start(0, cfg)`.

**Đóng phiên**:
1. `bt_bap_stream_disable()` cho cả hai stream.
2. `bt_bap_stream_stop(ic_rx)` (Receiver Stop Ready).
3. `bt_bap_stream_release()` cho cả hai.
4. Giữ `group` tới khi mất ACL. Khi đó `bt_bap_unicast_group_delete()`.

**QoS**:

| Tham số | Bản đầu [Đề xuất] | Chế độ tầm xa (I5) | Preset 16_2_1 để so sánh [SDK] |
|---|---|---|---|
| SDU interval | 10 000 µs | 10 000 µs | 10 000 µs |
| Framing | unframed | unframed | unframed |
| Max SDU | 40 B | 40 B | 40 B |
| PHY | **1M** | Coded (S2) | 2M |
| RTN | **4** | 2 | 2 |
| Max transport latency | **20 ms** | 30 ms | 10 ms |
| Presentation delay | min của acceptor (sink 20 ms, source 25 ms) | như bên trái | 40 ms |

Air time ở 1M với RTN 4: tối đa 5 sự kiện con × 1.16 ms ≈ 5.8 ms mỗi 10 ms khi phải gửi lại hết. Thường thì ít hơn nhiều, vì gói được xác nhận sớm thì các sự kiện con sau được bỏ qua. Như vậy còn ≥ 2.9 ms cho ACL của điện thoại và mũ bạn.

### 6.5 Phía acceptor: `le_audio.c` theo từng kết nối

| Callback | Thay đổi |
|---|---|
| `lc3_config` | `conn` là mũ bạn thì cấp stream từ **kho intercom** (1 sink + 1 source), chỉ nhận LC3 16/32 kHz, 10 ms, mono, và trả QoS pref riêng cho intercom (1M, RTN 4, latency 20 ms). Ngược lại cấp từ kho điện thoại như hiện nay |
| `lc3_enable`, `lc3_metadata` | **ASCS đã tự kiểm ngữ cảnh**: khi bật `CONFIG_BT_CAP_ACCEPTOR` (mũ đang bật), ASCS từ chối metadata có Streaming Context không nằm trong available contexts **của đúng kết nối đó**, bằng `METADATA_REJECTED` (`ascs.c:2172–2181`, `is_context_available()`) [SDK]. Vì vậy chỉ cần đặt ngữ cảnh theo kết nối (dòng dưới). Kiểm tra thêm trong app chỉ cần cho trường hợp riêng: mũ bạn xin luồng trong lúc mũ đang gọi điện thoại thì từ chối |
| `stream_started` | Stream intercom: sink thì gọi `audio_out_stream_start(0, cfg)`, source thì gọi `voice_start(stream, cfg)`, sau đó gửi `EVT_IC` (IC_EV_STREAM) |
| Ngữ cảnh theo kết nối | `bt_pacs_conn_set_available_contexts_for_conn()` [SDK]. `ic_conn` luôn chỉ có Conversational (sink và source). `phone_conn` có NONE trong lúc có phiên intercom, và trả về mặc định (`NULL`) khi phiên kết thúc. Giá trị này tự trở về mặc định khi ACL ngắt [SDK] |

Phía initiator, `le_audio.c` (server) không đổi. Điện thoại của mũ initiator vẫn cấu hình ASE như hiện nay.

### 6.6 Đường âm thanh: dùng lại nguyên đường cuộc gọi

```
 Mũ X                                                              Mũ Y
 micro PDM 16 kHz → voice_dsp (HPF, AEC, NS, AGC) → voice_tx (bù trôi, LC3 16 kHz 40 B)
        → stream gửi (X là initiator: ic_tx ; X là acceptor: ASE source của kết nối mũ bạn)
        ═══════════════════════ CIS hai chiều ═══════════════════════►
                                         audio_out (giải mã LC3, bù trôi, trộn âm báo)
                                         → tham chiếu AEC → lấy mẫu lại cố định → I2S → loa
 ◄═══════════════════════ chiều ngược lại giống hệt ═══════════════════════
```

- `voice_tx.c` đã nhận `struct bt_bap_stream *` bất kỳ. Stream client tới ASE sink của bạn cũng gửi bằng `bt_bap_stream_send()` và nhận callback `sent` y như ASE source của server. Chỉ cần đăng ký `.sent` trong `stream_ops` của client.
- `audio_out.c` không đổi: luồng mono phát ra cả hai tai.
- AEC **bắt buộc** ở cả hai mũ. Tiếng vọng còn sót sẽ quay về người nói sau khoảng 2 × độ trễ một chiều (khoảng 160–200 ms), rất dễ nghe thấy. Mục tiêu ERLE ≥ 25 dB (giống FR-20.3).
- Nguồn **âm thử** (`helmet ic tone <Hz>`) thay micro trong `voice_tx.c` để kiểm mốc I2 khi chưa có micro.

### 6.7 Độ trễ miệng → tai

| Khối | ms | Nguồn số liệu |
|---|---|---|
| Gom khối PDM | 10 | `mic_in.c`: khối 10 ms |
| Lọc decimation của PDM | khoảng 1 | [Cần đo] |
| Giảm ồn WOLA (cửa sổ 320, bước 160) | 10 | `voice_dsp.c`: `NS_WIN` |
| Vòng PCM + hàng SDU (mồi 2 SDU) | 10–20 | `voice_tx.c`: `PRIME_SDUS 2` [Cần đo] |
| LC3 look-ahead | 2.5 | Khung LC3 10 ms |
| Truyền CIS | 5–20 | Max transport latency 20 ms |
| Hàng nhận + vòng PCM phía nghe | khoảng 10 | `audio_out.c` [Cần đo] |
| Hàng I2S (mục tiêu 3 khối) | 30 | `audio_out.c`: `I2S_TARGET_BLOCKS 3` |
| Lấy mẫu lại, DMA | khoảng 1 | |
| **Tổng** | **khoảng 80–105** | |

Tối ưu riêng cho intercom (đổi theo chế độ, không ảnh hưởng nghe nhạc):
- `I2S_TARGET_BLOCKS` 3 → 2.
- `PRIME_SDUS` 2 → 1.

Hai thay đổi này bớt khoảng 20 ms, tổng còn **khoảng 60–85 ms**. ITU-T G.114 coi một chiều dưới 150 ms là tốt cho hội thoại.

### 6.8 Kết thúc, tạm dừng, nối lại

| Tình huống | Bên khởi xướng | Bên kia |
|---|---|---|
| Bấm đóng | END(USER), âm "ic_end", dọn luồng | Âm "ic_end" |
| Tắt máy | Trong `power_off()`, **trước** `app_bt_shutdown()`: END(POWER_OFF), chờ phản hồi ATT tối đa 300 ms | Âm "ic_end" |
| Pin yếu tự tắt | END(LOW_BATTERY) | Âm "low_battery" |
| Cuộc gọi điện thoại | SUSPEND(PHONE_CALL), mục 7.3 | Âm "ic_busy", SUSPENDED |
| Cúp máy | RESUME | Nếu tự nối lại bật thì mở lại phiên |

### 6.9 Xử lý lỗi

| Lỗi | Phát hiện | Xử lý |
|---|---|---|
| `bt_bap_*` trả lỗi, hoặc ASE bị từ chối | Callback / mã trả về | END(ERROR), dọn luồng, âm "ic_unavailable", log đủ mã lỗi |
| Thiết lập quá 5 s | `T_IC_SETUP` | Như trên |
| CIS rớt nhưng ACL còn | `stream_stopped` / ISO disconnected | Initiator thử mở lại luồng 1 lần, thất bại thì END(ERROR) |
| Micro lỗi trong phiên | `voice_tx.c` (đã có: tự khởi động lại sau 3 lần đọc lỗi) | Không cần xử lý thêm |
| Hàng HIS ghi lỗi (ATT) | Mã trả về của `bt_gatt_write` / `bt_gatt_indicate` | Thử lại 1 lần, rồi coi như mất liên kết |

---

## 7. Ưu tiên với điện thoại

### 7.1 Bảng ưu tiên (mức 0)

| Đang có | Sự kiện mới | Kết quả |
|---|---|---|
| Nhạc từ điện thoại | Mở intercom | **Intercom thắng**: MCP Pause (nhớ để Play lại), chặn ngữ cảnh của điện thoại, nếu sau 1 s luồng điện thoại vẫn chạy thì mũ tự Disable/Release ASE của điện thoại |
| Intercom | Cuộc gọi đến | **Chuông cục bộ trộn vào tiếng intercom**. Nghe máy thì intercom tạm dừng |
| Intercom | Gọi đi từ điện thoại / VoIP | Mũ thấy `CALL_OUTGOING` qua TBS, tự tạm dừng intercom và mở ngữ cảnh. Điện thoại có chuyển tiếng sang mũ hay không tùy hãng [Cần đo] |
| Cuộc gọi điện thoại | Bạn mở intercom | REJECT(PHONE_CALL), mũ bạn phát "ic_busy" |
| Cuộc gọi điện thoại | Bấm mở intercom cục bộ | Không làm gì, phát "status_no_link" (ngắn) |

### 7.2 Chặn nhạc khi intercom

1. `media_ctrl_pause()` nếu đang phát, đặt `resume_music = true`.
2. `bt_pacs_conn_set_available_contexts_for_conn(phone_conn, SINK/SOURCE, NONE)`. Điện thoại nhận thông báo PACS.
3. Luồng mới của điện thoại xin ngữ cảnh đã bị chặn sẽ bị **ASCS tự từ chối** (`METADATA_REJECTED`, mục 6.5).
4. Hết 1 s mà ASE của điện thoại vẫn Streaming thì gọi `bt_bap_stream_disable()` rồi `bt_bap_stream_release()`. Server được phép tự làm việc này [SDK].
5. Khi phiên kết thúc: trả ngữ cảnh về mặc định (`NULL`), và gọi `media_ctrl_play()` nếu `resume_music`.

### 7.3 Cuộc gọi đến trong lúc intercom (ví dụ ở mũ B)

```
 Điện thoại B ──TBS: cuộc gọi đến──► Mũ B (IC_ACTIVE): chuông "ring" trộn vào tiếng intercom
 Người dùng B: Call ngắn (nghe máy)
 Mũ B ── SUSPEND(PHONE_CALL) ─────► Mũ A: âm "ic_busy", IC_SUSPENDED,
                                          bỏ chặn nhạc (nhạc của A được phát lại)
 Initiator (A hoặc B): Disable + Release luồng intercom ──► CIS đóng
 Mũ B: trả ngữ cảnh cho điện thoại B, chờ 200 ms (để điện thoại kịp nhận thông báo PACS),
       rồi mới call_ctrl_accept()
 Điện thoại B cấu hình ASE Conversational → cuộc gọi chạy trên mũ B (HS_IN_CALL)
 … cúp máy (CALL_NONE) …
 Mũ B ── RESUME ──► Mũ A: nếu ic_autoresume: chặn nhạc lại; initiator chạy BAP → IC_ACTIVE,
                     âm "ic_start" ở cả hai mũ
```

- Nếu **cả hai** mũ cùng có cuộc gọi: phiên chỉ được nối lại khi cuộc gọi **sau cùng** kết thúc. Mỗi mũ theo dõi "bạn đang gọi" qua STATE bit 1.
- Thứ tự "trả ngữ cảnh → chờ → Accept" là để điện thoại thấy mũ sẵn sàng **trước khi** chọn đường âm thanh cho cuộc gọi. Hành vi này khác nhau giữa các hãng [Cần đo].

### 7.4 Mũ bạn đang gọi điện thoại

- Mũ nhận SUSPEND(PHONE_CALL) chuyển sang SUSPENDED và **bỏ chặn nhạc** của chính nó, để người đó nghe nhạc trong lúc chờ.
- Bấm "đóng intercom" trong SUSPENDED thì gửi END(USER) và bỏ hẳn phiên.

---

## 8. Nút, âm báo, trạng thái thiết bị

### 8.1 Máy trạng thái thiết bị mở rộng

```
          ┌──────────┐  có liên kết (điện thoại và/hoặc mũ bạn)   ┌────────────┐
          │  IDLE    │ ─────────────────────────────────────────► │ CONNECTED  │
          │(không có │ ◄──────────────── mất hết liên kết ─────── │            │
          │ liên kết)│                                            └──┬──────▲──┘
          └──────────┘                       phiên intercom bắt đầu │      │ phiên kết thúc
                                                                    ▼      │
                                              cuộc gọi đến     ┌────────────┐
                    ┌─────────┐ ◄──────────────────────────────│ INTERCOM   │ (mới)
                    │ RINGING │  (intercom vẫn chạy, chuông    └────────────┘
                    └────┬────┘   cục bộ trộn vào)                  ▲
                 nghe máy│                       cúp máy + tự nối lại│
                         ▼                                          │
                    ┌─────────┐ ───────────────────────────────────┘
                    │ IN_CALL │  (intercom SUSPENDED)
                    └─────────┘ ── cúp máy, không có phiên ──► CONNECTED
```

- **IDLE** đổi nghĩa thành "không có liên kết nào, cả điện thoại lẫn mũ bạn". Hẹn giờ tự tắt chỉ chạy trong IDLE như hiện nay.
- Bổ sung [Đề xuất]: chỉ có liên kết mũ bạn mà không có phiên trong 3 × `autooff_min` (mặc định 30 phút) thì cũng tự tắt, tránh hai mũ giữ nhau bật mãi khi để quên.
- Ghép đôi không phải trạng thái thiết bị riêng. Nó là `ic_state = IC_PAIRING` trong lúc thiết bị ở IDLE hoặc CONNECTED.

### 8.2 `buttons.c`: chế độ "giữ rồi nhả"

Hiện tại `G_LONG` phát **ngay** khi giữ đủ 1 s, nên giữ 3 s sẽ ra cả `G_LONG` (lúc 1 s) lẫn `G_VERY_LONG` (lúc 3 s). Intercom cần tách hai cử chỉ này:

- Thêm `buttons_set_long_release(enum helmet_key key, bool enabled)`. Khi bật:
  - nhả trong khoảng 1.0–3.0 s → `G_LONG` (phát **lúc nhả**);
  - giữ tới 3 s → chỉ `G_VERY_LONG`, không có `G_LONG`.
- Bộ điều khiển bật chế độ này cho Call ở IDLE, CONNECTED, INTERCOM, và tắt ở RINGING, IN_CALL. Nhờ vậy giữ 1 s để từ chối cuộc gọi vẫn phản hồi ngay lúc 1 s như hiện nay.

### 8.3 Bảng hành động theo trạng thái [Đề xuất, chủ dự án cần duyệt]

| Trạng thái | Call ngắn | Call đúp | Call giữ 1–3 s rồi nhả | Call giữ 3 s |
|---|---|---|---|---|
| IDLE | Âm "no link" (như hiện nay) | | Mở intercom: tìm bạn 10 s rồi mở. Chưa ghép thì "ic_unavailable" | **Ghép đôi intercom** |
| CONNECTED, có điện thoại | Play/Pause | Bài kế tiếp | Mở intercom | Ghép đôi |
| CONNECTED, chỉ có mũ bạn | Âm trạng thái | | Mở intercom | Ghép đôi |
| Đang ghép đôi | Hủy ghép | | | Hủy ghép |
| IC_OUTGOING (đang gọi bạn) | Hủy | | Hủy | |
| IC_INCOMING (bạn gọi, tự trả lời tắt) | **Nghe** | | **Từ chối** | |
| IC_ACTIVE | **Đóng intercom** | Tắt/bật micro | Đóng intercom | |
| IC_SUSPENDED (bạn đang gọi điện) | | | Đóng hẳn intercom | |
| RINGING (điện thoại, có hoặc không có intercom) | Nghe máy (tạm dừng intercom) | | Từ chối (phát ngay lúc 1 s, như hiện nay) | |
| IN_CALL | Cúp máy | Tắt/bật micro | | |

Các nút khác:
- **Vol+ / Vol−**: chỉnh âm lượng ở mọi trạng thái (một mức âm lượng chung, VCP vẫn báo cho điện thoại).
- **Power ngắn**: âm trạng thái. Thêm một tiếng cao nếu đang có liên kết với mũ bạn.
- **Power giữ 3 s**: tắt máy, gửi END(POWER_OFF) trước.
- Quy ước giống cuộc gọi điện thoại để dễ nhớ: "ngắn = nghe hoặc kết thúc", "đúp = micro".

### 8.4 Âm báo mới (`prompts.c`)

| Tên | Đoạn {Hz, ms, lặng ms} | Lặp | Khi nào |
|---|---|---|---|
| `ic_pairing` | {784,60,60} {988,60,60} {1175,60,600} | Có | Đang ghép đôi |
| `ic_paired` | {1175,80,20} {1568,80,20} {2093,160,0} | | Ghép xong |
| `ic_start` | {1568,60,40} {2093,100,0} | | Intercom mở |
| `ic_end` | {2093,60,40} {1568,100,0} | | Intercom đóng |
| `ic_ring` | {1397,150,100} {1760,150,1200} | Có | Bạn gọi (khi tự trả lời tắt) |
| `ic_calling` | {1175,100,1900} | Có | Đang chờ bạn trả lời |
| `ic_lost` | {784,150,50} {587,250,0} | | Mất liên kết mũ bạn |
| `ic_unavailable` | {587,100,80} {587,100,0} | | Chưa ghép, không tìm thấy, lỗi |
| `ic_busy` | {1047,100,100} ×3 | | Bạn đang gọi điện thoại |

Cần sửa trước (mốc I0): hiện một âm báo ngắn **ghi đè** âm báo lặp (chuông) mà không phát lại chuông. Đây là hạn chế đã ghi trong hướng dẫn sử dụng. Với `ic_ring` và `ic_calling`, âm báo lặp phải được **nối lại** sau âm báo ngắn.

---

## 9. Tổng hợp thay đổi ở các module

| File | Mức | Nội dung |
|---|---|---|
| `main.c` | Sửa lớn | Thay `active_conn` bằng `phone_conn` và `ic_conn`. Phân loại kết nối: vai trò central thì là mũ bạn; đến qua set 1 thì là ứng viên mũ bạn; còn lại tạm coi là điện thoại cho tới khi mã hóa xong và địa chỉ định danh cho biết chắc. Chỉ gọi `media_ctrl_connected` / `call_ctrl_connected` cho điện thoại. Set 0 giữ logic chống livelock. Thêm `HS_INTERCOM`, bảng nút mới, lệnh `helmet ic` |
| `le_audio.c` | Sửa | Hai kho stream. QoS pref intercom. Kiểm ngữ cảnh trong `lc3_enable` / `lc3_metadata`. Ngữ cảnh theo kết nối |
| `voice_tx.c` | Sửa nhỏ | Nguồn âm thử. `PRIME_SDUS` theo chế độ |
| `audio_out.c` | Sửa nhỏ | `I2S_TARGET_BLOCKS` theo chế độ. Âm báo lặp được nối lại |
| `buttons.c` | Sửa nhỏ | `buttons_set_long_release()` |
| `prompts.c` | Sửa nhỏ | 9 âm báo mới |
| `power.c` | Sửa nhỏ | Nghĩa mới của IDLE. END(POWER_OFF) khi tắt. Tự tắt khi chỉ có liên kết mũ bạn |
| `call_ctrl.c`, `media_ctrl.c` | Sửa nhỏ | Chỉ gắn với `phone_conn`. Báo trạng thái cuộc gọi cho `intercom.c` |
| `intercom.c` | **Mới** | Máy trạng thái phiên, ghép đôi, quét và kết nối lại, chính sách ưu tiên, settings `helmet/ic` |
| `ic_service.c` | **Mới** | HIS: server (mọi mũ) và client (initiator) |
| `ic_unicast.c` | **Mới** | BAP Unicast Client |
| `helmet.h` | Sửa | Mục 3.3 |
| `prj.conf`, `CMakeLists.txt` | Sửa | Mục 3.1, 3.2 |
| `boards/me54bs62_nrf54l15_cpuapp.overlay` | Sửa nhỏ | `pdm20`: `clock-source = "PCLK32M_HFXO"` |

---

## 10. Chẩn đoán: log, heartbeat, shell

### 10.1 Heartbeat (thêm một dòng mỗi 10 s khi có bạn)

```
ic: ACTIVE role=INITIATOR peer=E0:FE:…(id) rssi -61 dBm, PHY 1M, rx SDU 6000 (lost 12, PLC 24),
    tx 6000 (underrun 0), cpu 68 %
```

### 10.2 Log sự kiện

- `IC pairing start (nonce 0x…)`
- `IC candidate <addr> path loss 41 dB`
- `IC role: INITIATOR/ACCEPTOR`
- `IC paired with <id>`
- `IC linked (<id>, HELLO v1 caps 0x…)`
- `IC session <n>: START_REQ/ACCEPT/REJECT(<lý do>)/END(<lý do>)`
- `IC BAP: config/qos/enable/connect/start <kết quả>`
- `IC lost (reason 0x..)`, `IC resume`

### 10.3 Lệnh shell `helmet ic`

| Lệnh | Việc |
|---|---|
| `helmet ic` | Trạng thái: vai trò, bạn, liên kết, phiên, QoS, bộ đếm |
| `helmet ic pair` / `helmet ic pair stop` | Vào hoặc thoát chế độ ghép đôi |
| `helmet ic start` / `stop` / `accept` / `reject` | Giả lập nút |
| `helmet ic unpair` | Bỏ ghép bạn (chỉ intercom) |
| `helmet ic auto on\|off` | Tự trả lời |
| `helmet ic resume on\|off` | Tự nối lại sau cuộc gọi hoặc mất sóng |
| `helmet ic phy 1m\|2m\|coded` | PHY cho phiên kế tiếp |
| `helmet ic qos <rtn> <latency_ms>` | QoS cho phiên kế tiếp |
| `helmet ic tone <Hz>\|off` | Âm thử thay micro |
| `helmet ic link` | RSSI, thông số CIS, chất lượng ISO (như `le_audio_print_link_quality`) |

Kèm theo: `helmet cpu` (đã có) để đo CPU từng luồng trong lúc intercom.

---

## 11. Kiểm thử và tiêu chí nghiệm thu

### 11.1 Thiết bị cần có

- **2 mạch** chạy `apps/main`: 2 bo ME54BS62, hoặc 1 bo cùng 1 DK nRF54L15 có overlay chân phù hợp [TBD Q1].
- 2 J-Link (hoặc 1, cắm luân phiên), RTT cho từng mạch (`JLINK_SN=… make rtt`).
- 1–2 điện thoại có LE Audio. Lưu ý: hai phiên RTT **trên cùng một mạch** sẽ chia nhau mất log.

### 11.2 Các ca kiểm thử

| ID | Nội dung | Cách làm | Đạt khi |
|---|---|---|---|
| IC-01 | Ghép đôi | `helmet ic pair` trên cả hai mạch, hai mạch sát nhau | Vai trò đúng theo nonce, cả hai lưu `peer`, ≤ 15 s |
| IC-02 | Chặn theo suy hao | Một mạch cách 5 m | Không ghép |
| IC-03 | Ghép lại sau khi một bên xóa bond | Xóa bond mạch A, ghép lại | Thành công (`ALLOW_UNAUTH_OVERWRITE`) |
| IC-04 | Tự kết nối sau khi bật lại | Tắt/bật mạch acceptor | Liên kết lại ≤ 5 s |
| IC-05 | Thiết bị lạ không vào được set 1 | Điện thoại / laptop quét và thử kết nối set 1 | Không kết nối được |
| IC-06 | HIS chỉ nhận từ bạn | Điện thoại ghi Control Point | Bị từ chối |
| IC-07 | Mở từ initiator, tự trả lời bật | `helmet ic start` | Âm thử nghe được ở acceptor ≤ 1.5 s |
| IC-08 | Mở từ acceptor | `helmet ic start` trên acceptor | Như IC-07 |
| IC-09 | Tự trả lời tắt: nghe / từ chối / hết giờ | `helmet ic auto off` | Đúng bảng 6.3 |
| IC-10 | Hai bên cùng bấm mở | Bấm gần như đồng thời | Một phiên duy nhất |
| IC-11 | Chất lượng 30 phút | Âm thử hai chiều | PLC ≤ 2 % ở 1 m, `mic restart` = 0, không có lỗi trong log |
| IC-12 | CPU | `helmet cpu` trong phiên | TOTAL ≤ 75 % |
| IC-13 | Nhạc bị tạm dừng rồi phát lại | Đang nghe nhạc thì mở intercom | Pause khi mở, Play khi đóng |
| IC-14 | Cuộc gọi đến ở acceptor | Gọi tới điện thoại B | Chuông trộn vào intercom. Nghe máy thì tạm dừng. Cúp máy thì nối lại |
| IC-15 | Cuộc gọi đến ở initiator | Như IC-14 với A | Như trên |
| IC-16 | Gọi đi từ điện thoại khi đang intercom | Bấm gọi trên điện thoại | Intercom tạm dừng. Ghi lại điện thoại có chuyển tiếng sang mũ không |
| IC-17 | Mất sóng | Che hoặc mang mạch đi xa rồi quay lại trong 60 s | "ic_lost", sau đó phiên tự nối lại |
| IC-18 | Tắt máy một bên | Power giữ 3 s | Bên kia nghe "ic_end" |
| IC-19 | Bỏ ghép | `helmet ic unpair` | Không tự kết nối nữa, bond đã xóa |
| IC-20 | Độ trễ | Mục 11.3 | ≤ 100 ms |
| IC-21 | Tầm xa | Mục 11.3 | Ghi bảng PLC theo khoảng cách cho 2M / 1M / +8 dBm |
| IC-22 | Hồi quy chức năng điện thoại | Kiểm thử M0–M5 trong hướng dẫn sử dụng | Không có gì hỏng |

### 11.3 Đo độ trễ và tầm xa

- **Độ trễ (mạch)**: A phát một tiếng "tách" bằng âm thử và bật một GPIO đúng lúc gửi khung. Đo trên máy hiện sóng / logic analyzer từ GPIO của A tới tín hiệu I2S (hoặc loa) của B.
- **Độ trễ (âm học)**: một micro PC đặt ở loa B, một micro ở miệng người nói vào A. Ghi cả hai kênh, tính độ lệch bằng tương quan chéo.
- **Tầm xa**: hai người đứng trên đường thẳng, mỗi 10 m ghi `helmet ic link` và PLC trong 30 s. Làm lần lượt cho 2M/0 dBm, 1M/+8 dBm (và Coded S2 ở mốc I5). Ghi thêm dòng tiêu thụ ở mỗi cấu hình công suất.

### 11.4 Tiêu chí nghiệm thu [Đề xuất]

- Ghép đôi thành công ≥ 9/10 lần, mỗi lần ≤ 15 s.
- Tự kết nối lại ≤ 5 s. Mở intercom ≤ 1.5 s khi đã có liên kết, ≤ 6 s khi phải kết nối lại.
- Độ trễ miệng → tai ≤ 100 ms. ERLE ≥ 25 dB ở cả hai mũ.
- PLC ≤ 2 % ở khoảng cách mục tiêu [TBD Q2]. Chạy 30 phút không rớt, không có tiếng tách nghe được.
- `helmet cpu` TOTAL ≤ 75 % trong phiên. RAM ≤ 92 %, các stack còn ≥ 20 % trống.
- Cuộc gọi điện thoại chen ngang và nối lại đúng với cả hai vai trò. Toàn bộ kiểm thử điện thoại cũ vẫn đạt.

---

## 12. Mở rộng sau bản đầu

| Mở rộng | Hướng làm | Điều kiện |
|---|---|---|
| Chế độ tầm xa | Coded S2 cho ACL và CIS. Set 1 quảng bá trên Coded (`BT_LE_ADV_OPT_CODED`), quét Coded, `CONFIG_BT_CTLR_PHY_CODED=y`, RTN 2 | Đo xong mốc I4 |
| Khởi động nhanh | Giữ ASE ở QoS Configured giữa các phiên. Mở phiên chỉ cần Enable + CIS + Start (khoảng 0.5 s) | CIG vẫn được giữ sẵn trong controller |
| Nhạc nền trong intercom (mức 1) | Hai bộ giải mã, I2S 48 kHz, nâng mẫu giọng ×3, tham chiếu AEC hạ về 16 kHz | Đo CPU/RAM. Thử cấu hình CIS central + peripheral cùng lúc trên một thiết bị |
| Chia sẻ cuộc gọi (mức 2) | Mũ đang gọi trộn giọng bạn vào chiều lên điện thoại, và trộn giọng điện thoại vào chiều tới bạn | CPU: thêm mã hóa và trộn |
| Nhóm 3–4 mũ | (a) Một mũ làm "trung tâm": 1 CIG với nhiều CIS, mũ trung tâm trộn tiếng. (b) Auracast: mỗi người phát một BIG khi nói (nhấn để nói), mọi người đồng bộ các BIG | (a) CPU của mũ trung tâm. (b) Thiết kế lại, chấp nhận độ trễ cao hơn |
| Tự bật micro theo giọng (VOX) | Chỉ gửi khi phát hiện giọng, giữ ACL | Bộ phát hiện giọng chống gió |

---

## 13. Rủi ro và câu hỏi mở

### 13.1 Rủi ro

| ID | Rủi ro | Mức | Giảm thiểu |
|---|---|---|---|
| R1 | RAM vượt 90 % | Cao | Danh sách cắt giảm ở mục 1.7. Đo ngay sau mốc I0 |
| R2 | CPU không đủ (cuộc gọi hiện 98 %) | Cao | Mốc I0 tối ưu trước. Intercom cố định 16 kHz mono |
| R3 | Tầm xa thực tế ngắn | Trung bình | 1M + 8 dBm. Coded S2 (I5). Cân nhắc anten tốt hơn ở bản PCB sau |
| R4 | Điện thoại không chuyển tiếng cuộc gọi sang mũ sau khi ngữ cảnh đổi | Trung bình | Thứ tự "trả ngữ cảnh → chờ → Accept". Thử với từng dòng máy |
| R5 | Acceptor có 2 ACL peripheral từ 2 đồng hồ khác nhau, cộng CIS: va lịch, mất gói | Trung bình | RTN 4. Theo dõi PLC và ISO link quality |
| R6 | Tiếng vọng vòng về người nói (khoảng 160–200 ms) | Trung bình | AEC ở cả hai mũ, ERLE ≥ 25 dB, chỉnh bằng dữ liệu ghi |
| R7 | Bond của bạn bị đẩy ra | Thấp | `MAX_PAIRED` 5, kiểm tra lúc khởi động |
| R8 | Ghép nhầm mũ khác khi đi theo đoàn | Thấp | Chặn theo suy hao + nonce + hai bên cùng bấm |
| R9 | BAP client và server trong cùng một image ít được dùng | Thấp | Chính bộ test bsim audio của Zephyr bật cả hai với `MAX_CONN=3` [SDK] |
| R10 | Tương tác giữa 2 bộ quảng bá và pool kết nối gây vòng lặp (như livelock cũ) | Trung bình | Mẫu `adv_on` cho cả hai set. Kiểm thử chuyển chậm/nhanh và mất kết nối liên tục |
| R11 | Không ghép lại được khi một bên còn bond cũ | Đã biết | `CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE` (mục 2.4) |

### 13.2 Câu hỏi mở cần chủ dự án trả lời

| ID | Câu hỏi | Ảnh hưởng |
|---|---|---|
| Q1 | Có đủ 2 mạch để thử không (2 bo ME54BS62, hoặc 1 bo + 1 DK nRF54L15)? | Mốc I1 trở đi |
| Q2 | Khoảng cách mục tiêu: người ngồi sau cùng xe (khoảng 1 m) hay hai xe cách nhau 50–200 m? | PHY, công suất, mốc I5, tiêu chí PLC |
| Q3 | Tự trả lời mặc định bật hay tắt? | Trải nghiệm, quyền riêng tư |
| Q4 | Có cần nghe nhạc nền trong lúc intercom (mức 1) không? | Kiến trúc âm thanh, CPU, RAM |
| Q5 | Một bạn hay nhiều bạn? Có cần nhóm > 2 mũ không? | Settings, giao thức, mục 12 |
| Q6 | Chấp nhận +8 dBm (dòng tăng) không? Bản PCB sau có cân nhắc anten ngoài không? | Tầm xa, pin |
| Q7 | Duyệt bảng nút mục 8.3? | `buttons.c`, hướng dẫn sử dụng |
| Q8 | Duyệt `CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE` (cũng sửa lỗi ghép lại điện thoại)? | Mốc I0 |
| Q9 | Chấp nhận micro cuộc gọi chỉ 16 kHz (tối ưu CPU 4a)? | CPU của cuộc gọi và intercom |

---

## 14. Phụ lục

### 14.1 Tham chiếu SDK đã kiểm tra (NCS v3.4.0)

| Nội dung | Vị trí |
|---|---|
| ASCS đăng ký ISO server duy nhất | `zephyr/subsys/bluetooth/audio/ascs.c:3215` (`bt_iso_server_register(&iso_server)`) |
| Chỉ nhận CIS khớp ASE ở Enabling/QoS Configured | `ascs.c:831–850` (`ascs_iso_accept`) |
| Chỉ một ISO server | `zephyr/subsys/bluetooth/host/iso.c:1434` (`bt_iso_server_register`) |
| ASCS tự từ chối Streaming Context không available theo kết nối (khi bật CAP Acceptor) | `ascs.c:2132` (`is_context_available`), `2172–2181` |
| ASCS chỉ cảnh báo, không từ chối, khi CCID List lạ; Zephyr chưa kiểm CCID của MCS ("TODO: check mcs") | `ascs.c:2194–2201`, `cap_acceptor.c:97–107` |
| Metadata tối đa `CONFIG_BT_AUDIO_CODEC_CFG_MAX_METADATA_SIZE` (mặc định 4) | `Kconfig.bap:45`, kiểm tra ở `ascs.c:2253` |
| Phụ thuộc của `BT_BAP_UNICAST_CLIENT` | `Kconfig.bap:25–32` (GATT_CLIENT, AUTO_DISCOVER_CCC, AUTO_UPDATE_MTU, CENTRAL, ISO_CENTRAL, BONDABLE) |
| Server được tự Disable/Release stream | `include/zephyr/bluetooth/audio/bap.h` (`bt_bap_stream_disable`, `bt_bap_stream_release`: "unicast client or unicast server") |
| Ghép một cặp stream thành một CIS hai chiều | `bap.h:1634` (`bt_bap_unicast_group_stream_pair_param`) |
| Ngữ cảnh theo kết nối, tự về mặc định khi ngắt | `include/zephyr/bluetooth/audio/pacs.h:188–219` |
| Preset 16_2_1: 16 kHz, 10 ms, 40 B, RTN 2, 10 ms, PD 40 ms | `bap_lc3_preset.h:97–101` |
| Số vai trò peripheral, central = MAX_CONN − PERIPHERAL_COUNT | `nrf/subsys/bluetooth/controller/Kconfig:128–138` |
| CIG reserved time 1300 µs | `nrf/subsys/bluetooth/controller/Kconfig:527` |
| Bộ nhớ controller theo liên kết, CIG, CIS, quét, quảng bá | `nrfxlib/softdevice_controller/include/sdc.h:180–364` (đã tính bằng chính các macro này) |
| CIS hai chiều trên 1M và Coded đã được Nordic thử. Số vai trò đồng thời chỉ giới hạn bởi bộ nhớ và air time | `nrfxlib/softdevice_controller/doc/isochronous_channels.rst:45, 60–65` |
| +8 dBm trên nRF54L | `zephyr/subsys/bluetooth/controller/Kconfig:364–366` |
| Từ chối ghép đè bond không xác thực | `zephyr/subsys/bluetooth/host/smp.c:642–685` (`update_keys_check`), `host/Kconfig:682` |
| Kết nối tự động, FAL | `include/zephyr/bluetooth/conn.h:1938`, `bluetooth.h:2698` |
| Callback `connected` theo bộ quảng bá; `BT_LE_ADV_OPT_FILTER_CONN`, `_CODED` | `bluetooth.h:238`, `766`, `833` |
| Driver PDM tự dừng khi hàng đợi đầy hoặc hết khối | `zephyr/drivers/audio/dmic_nrfx_pdm.c:113–207` |
| `clock-source` mặc định của PDM và I2S là `PCLK32M_HFXO` | `dts/bindings/audio/nordic,nrf-pdm.yaml:23`, `dts/bindings/i2s/nordic,nrf-i2s.yaml:23` |
| BAP client + server cùng image, `MAX_CONN=3` | `zephyr/tests/bsim/bluetooth/audio/prj.conf` |

### 14.2 Số liệu tính toán dùng trong tài liệu

**Air time một gói** (SDU 40 B, có mã hóa):
- PDU = header 2 + payload 40 + MIC 4 = 46 B. Cộng preamble và địa chỉ truy cập 4 B, CRC 3 B.
- 2M: (2 + 4 + 46 + 3) B × 4 µs = 220 µs.
- 1M: (1 + 4 + 46 + 3) B × 8 µs = 432 µs.
- Coded S2: preamble 80 µs + AA 256 µs + CI 16 µs + TERM1 24 µs + (46 + 3) B × 8 bit × 2 µs = 784 µs, + TERM2 khoảng 6 µs, tổng khoảng 1170 µs.
- Coded S8: tương tự với 8 µs/bit, tổng khoảng 3540 µs.
- Một sự kiện con hai chiều = 2 gói + T_IFS 150 µs + T_MSS 150 µs.

**Tầm tương đối** (không gian tự do): hệ số = 10^(ΔdB / 20).
- +3 dB → 1.4×
- +7 dB → 2.2×
- +8 dB → 2.5×
- +11 dB → 3.5×

**CPU cuộc gọi đo ngày 16/10/2026** (32 kHz, sau `-O3`):
- `voice_tid` 70.0 %, `audio_tid` 24.4 %, MPSL 1.3 %, các luồng khác < 1 %. TOTAL 98.1 %.
- `helmet perf` đo thời gian thực (kể cả lúc bị chen ngang): giải mã 2254 µs, khối xuất 718 µs, DSP 5006 µs, mã hóa 4123 µs.
- Ước tính phần CPU tự thân: DSP khoảng 3.8 ms, mã hóa khoảng 3.1 ms (chia 7.0 ms của `voice_tid` theo tỉ lệ thời gian thực).
