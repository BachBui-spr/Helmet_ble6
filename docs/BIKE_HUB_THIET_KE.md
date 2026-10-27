# Bike hub (apps/bike): trung tâm âm thanh trên xe

| | |
|---|---|
| Áp dụng cho | `apps/bike` (mới), board `me54bs62` (nRF54L15) dùng làm bộ trung tâm gắn trên xe. Mũ dùng `apps/main` (đã có phần thu Auracast) |
| Trạng thái | **Đã hiện thực và build sạch** ngày 20/10/2026: hub FLASH 27,8 % / RAM 39,5 %, mũ FLASH 41,4 % / RAM 95,7 %. Có **ghép mũ ↔ hub bằng nút, mã hoá tự động** (mục 8). **Chưa nạp, chưa chạy thử trên mạch** |
| Lý do ra đời | Kết quả phân tích ngày 20/10/2026 ([HELMET_AURACAST.md](HELMET_AURACAST.md)): tính năng "chia sẻ âm thanh" tới nhiều tai nghe phụ thuộc hãng điện thoại. Trên Xiaomi Pad 7 Pro (HyperOS 3) phần chia sẻ LE Audio của Android bị tắt; chia sẻ của Xiaomi chỉ dùng Bluetooth Classic, mà mũ không có Classic |
| Mã nguồn | Hub (`apps/bike/src/`): `app/main.c`, `app/ui.c` (nút, âm báo, nguồn; dùng `lib/hal/keys`, `lib/hal/power_hw`, `lib/services/prompt_player`), `app/app.h`, `features/phone/phone_sink.c`, `features/broadcast/bcast_source.c`, `features/broadcast/bhs.c`, `prj.conf`. Mũ: `apps/main/src/features/broadcast/hub_link.c`, `broadcast_sink.c`, `app/app_bt.c`. Giao thức BHS dùng chung: `lib/proto/include/me54/proto/bhs_proto.h` |
| Vấn đề đã gặp | Toàn bộ lỗi broadcast (mũ thu, ghép bằng nút, tranh radio CIS / BIG, dựng lại BIG) và cách sửa: [BROADCAST_VAN_DE_VA_GIAI_PHAP.md](BROADCAST_VAN_DE_VA_GIAI_PHAP.md) |

---

## 1. Ý tưởng

```
 Điện thoại ──LE Audio unicast (CIS, LC3 48 kHz)──►  BIKE HUB  ──Auracast (BIG 20 ms: 1 BIS stereo)──►  Mũ 1
 (chỉ cần nối 1 thiết bị                              (nRF54L15                                          Mũ 2
  như tai nghe bình thường)                            trên xe)                                          Mũ 3 ...
```

- **Điện thoại** chỉ làm việc nó vốn làm tốt: phát nhạc LE Audio tới **một** thiết bị ("Bike_Hub"). Không cần tính năng chia sẻ, không cần giao diện Auracast của hãng.
- **Hub** nhận khung LC3 từ điện thoại và **phát quảng bá lại nguyên vẹn** (Auracast "Bike Audio"). Hub **không giải mã, không mã hoá lại**, nên CPU gần như rảnh và không mất chất lượng.
- **Mũ** thu broadcast bằng phần Broadcast Sink đã làm trong `apps/main`. Số mũ **không giới hạn**.

## 2. Phía điện thoại (`phone_sink.c`)

| Thành phần | Thiết kế | Lý do |
|---|---|---|
| Vai trò | BAP Unicast Server + CAP Acceptor, **chỉ chiều nhận** (2 ASE sink, 0 ASE source) | Hub không có micro |
| PACS | LC3 **chỉ 48 kHz, chỉ 10 ms**, 1–2 kênh, **100–155 octet**, vị trí FL\|FR | BIG phát lại đúng cấu hình điện thoại chọn, không chuyển mã. Nhạc thường là 48_4 (120 B) |
| Ngữ cảnh | Unspecified, Media, Game, Instructional (chỉ đường) | **Không** có Conversational / Ringtone: cuộc gọi ở lại điện thoại |
| Âm lượng | **Không có VCS** | Điện thoại tự giảm âm lượng trong dữ liệu trước khi mã hoá (software volume), nên thanh âm lượng trên điện thoại vẫn điều khiển tiếng ở **mọi mũ** |
| Thứ tự GATT | Đăng ký PACS + ASCS **trước** `bt_enable()` | Như `apps/main`: Database Hash ổn định, tránh vòng lặp kết nối / ngắt |
| Quảng bá | Extended adv có thể kết nối: tên "Bike_Hub", appearance Generic Audio Sink, ASCS / CAS / PACS | Điện thoại nhận ra thiết bị LE Audio |
| Bảo mật | Just Works, yêu cầu mã hoá ngay khi kết nối; 30 s chưa mã hoá thì ngắt | Như `apps/main` |

**Tách kênh** (mỗi SDU nhận qua CIS):

| Điện thoại cấu hình | SDU | Thành cặp khung (trái, phải) |
|---|---|---|
| 1 ASE 2 kênh (FL\|FR), thường gặp (Pad 7 Pro) | `[khung trái][khung phải]` | trái, phải |
| 1 ASE 1 kênh (mono, hoặc chỉ một bên chạy) | 1 khung | cùng khung cho **cả hai** bên |
| 2 ASE 1 kênh (FL, FR riêng) | mỗi ASE 1 khung | ghép theo mốc thời gian SDU |

Cặp khung sang BIG: BIG 20 ms ghép lại thành **một SDU stereo** `[trái][phải]` trên 1 BIS; BIG 10 ms gửi trái → BIS 1, phải → BIS 2.

QoS hub đề nghị: unframed, 2M, RTN 2, **độ trễ tối đa 20 ms** (trước 10 ms), PD 20–40 ms. Điện thoại lấy đúng giá trị hub đề nghị (đo 23/10/2026). Độ trễ 20 ms cho phép controller điện thoại dùng **FT 2**: gói bị BIG chặn ở một sự kiện CIS được gửi lại ở sự kiện kế tiếp (mục 6). Lúc bắt đầu phát, hub log thông số CIS thật: `Phone CIS: ISO interval ..., NSE, BN, FT, PDU, subinterval ...`.

## 3. Phía mũ (`bcast_source.c`)

| Thành phần | Thiết kế |
|---|---|
| Vai trò | BAP Broadcast Source: 1 subgroup, Media |
| Kiểu BIG (`bike isoint`) | **20 ms (mặc định)**: **1 BIS stereo** (FL\|FR, SDU 240 B), chu kỳ ISO 20 ms, **BN 2** (2 SDU 10 ms mỗi sự kiện), tạo bằng *LE Create BIG Test* (`CONFIG_BT_ISO_TEST_PARAMS`). Cần 2 × octet ≤ 251 B (48_4: 240 B). **10 ms**: 2 BIS (FL, FR), Create BIG thường (kiểu cũ, để so sánh). Controller từ chối BIG 20 ms 2 lần liền thì hub tự dùng 10 ms. Lý do ở mục 6 |
| Cấu hình LC3 | Preset BAP **48_4_1**: 48 kHz, 10 ms, **120 B/khung**, unframed, 2M. Số octet tự đổi theo điện thoại (100–155) |
| Phát lại (RTN) | Mặc định **0** (preset là 4; 22/10 là 2, sáng 23/10 là 1). BIG 20 ms dùng tối đa RTN 1. Đổi bằng `bike rtn <0..4>` (giá trị đã lưu được ưu tiên hơn mặc định) |
| Tạo lại BIG | Workqueue **riêng** `bcast_wq` (stack 4 KB), bất đồng bộ: gọi stop rồi trả về, callback `stopped` mới xoá và tạo lại (dự kiến dưới 0,5 s, chưa đo). Controller tự kết thúc BIG (hoặc tạo BIG thất bại) thì hub cũng tự tạo lại |
| Tên / ID | Broadcast Name **"Bike Audio"**. Broadcast ID **ngẫu nhiên một lần, lưu vĩnh viễn** (`bike/bid`), để sau này mũ nhớ được "xe của mình" |
| Mã hoá | Mặc định **có mã hoá, mã 16 byte ngẫu nhiên** sinh lần đầu, lưu `bike/code`. **Không ai phải gõ**: mũ nhận mã khi ghép bằng nút (mục 8). `bike code new` (mã mới, mũ phải ghép lại), `bike code open` (không mã hoá), `bike code <chuỗi>` (thử nghiệm) |
| Địa chỉ bộ phát | Dùng **địa chỉ identity** (`BT_LE_ADV_OPT_USE_IDENTITY`), cố định, nên mũ đã ghép tạo đồng bộ PA thẳng theo địa chỉ + SID đã lưu, không cần quét |
| Khi điện thoại không phát | Hẹn giờ 10 ms đẩy **khung LC3 "im lặng"** (mã hoá sẵn một lần lúc khởi động bằng liblc3). Mũ luôn đồng bộ BIG và phát im lặng sạch; điện thoại bật nhạc là có tiếng ngay |

## 4. Chuyển tiếp khung (cốt lõi)

```
bt_rx:      phone_sink ──► bcast_source_push_pair(L, R) ──► hàng đợi cặp khung (tối đa Q_MAX = 4)
                                                                  │  k_work_submit(tx_work)
system WQ:  tx_work: nếu mọi BIS còn chỗ (BIG 20 ms: ≤ 3 SDU trong controller; 10 ms: ≤ 2)
                     → lấy 1 cặp → BIG 20 ms: bt_bap_stream_send(BIS, [L][R])
                                   BIG 10 ms: bt_bap_stream_send(BIS L), bt_bap_stream_send(BIS R)
callback "sent" (controller đã phát 1 SDU) → inflight-- → tx_work
ISR hẹn giờ 10 ms (khi điện thoại không phát) → push_pair(im lặng, im lặng)
```

| Vấn đề | Cách xử lý |
|---|---|
| **Trái / phải lệch nhau** | Luôn đẩy và gửi **theo cặp**. Không BIS nào được gửi trước BIS kia |
| **Lệch đồng hồ** điện thoại ↔ hub (~20–50 ppm) | Điện thoại nhanh hơn: hàng đợi quá Q_MAX thì **bỏ cặp cũ nhất** (`dropped`). Điện thoại chậm hơn: controller hết SDU, phát PDU rỗng, mũ dùng PLC (`dry`). Với 50 ppm thì khoảng 3 phút mới một lần, gần như không nghe thấy |
| **Gói từ điện thoại bị mất** | Kênh đó **lặp lại khung trước một lần**, sau đó phát khung im lặng (`repeated`, `silence`). Không bỏ trống một BIS, vì như vậy sẽ làm lệch cặp |
| **Điện thoại đổi số octet** (vd 120 → 100) | Dừng BIG → tạo lại với số octet mới → BASE mới. Mũ mất BIG vài giây rồi tự đồng bộ lại (bản Auracast của mũ đã chờ được khi nguồn tạm dừng) |
| **Đồng hồ khung im lặng** | `k_timer` dùng GRTC, **cùng nguồn đồng hồ** với BIG, nên không trôi |
| **Độ trễ** | Hàng đợi hub ≤ 2 SDU trong controller + ≤ 4 cặp: thường 10–20 ms, xấu nhất 60 ms. Cộng CIS (PD 20–40 ms) + BIG (~20 ms) + bộ đệm mũ (~55 ms), tổng **≈ 100–130 ms**. Ổn cho nhạc; xem video có thể lệch tiếng (điện thoại chỉ biết PD của hub) |

Chỉ một thread (system workqueue) gọi `bt_bap_stream_send`. Việc tạo lại BIG chạy trên `bcast_wq`, chỉ khi `running = 0`. Hàng đợi dùng spinlock. Số "đang nằm trong controller" của từng BIS là biến atomic.

## 5. Cấu hình (`apps/bike/prj.conf`) và bộ nhớ

| Kconfig | Giá trị | Ghi chú |
|---|---|---|
| `BT_MAX_CONN`, `BT_CTLR_SDC_PERIPHERAL_COUNT` | 5, 5 (trước 3, 3) | Điện thoại + tối đa 2 mũ đang ghép cùng lúc (mũ đọc xong là ngắt) + dự phòng. Zephyr giữ sẵn 1 chỗ kết nối cho mỗi bộ quảng bá có thể kết nối đang chạy, nên 3 chỗ là hết |
| `BT_EXT_ADV_MAX_ADV_SET` | 3 | Quảng bá cho điện thoại + Auracast (kèm PA) + cửa sổ ghép mũ |
| `INPUT`, `I2S`, `POWEROFF`, `HWINFO` | y | Nút, âm báo, bật / tắt (giống mũ) |
| `BT_ASCS_MAX_ASE_SNK/SRC_COUNT` | 2 / 0 | Chỉ chiều nhận |
| `BT_BAP_BROADCAST_SOURCE`, `BROADCAST_SRC_STREAM_COUNT` | y, 2 | 1 subgroup: 1 BIS stereo (20 ms) hoặc 2 BIS (10 ms) |
| `BT_ISO_TEST_PARAMS` | y | BIG 20 ms, BN 2 (LE Create BIG Test). Mọi tham số test = 0 thì Zephyr dùng Create BIG thường |
| `BT_ISO_TX_MTU` | 251 (trước 155) | SDU stereo 2 × 120 B = 240 B |
| `BT_PERIPHERAL_PREF_MIN/MAX_INT`, `_TIMEOUT` | 27 / 27, 600 (trước 50 / 70, 400) | PPCP = 33,75 ms / 6 s. Điện thoại có đọc PPCP (đo 23/10/2026: nó chọn 75 ms, nằm trong dải cũ 62,5–87,5 ms) |
| `BT_ISO_MAX_CHAN` | 4 | 2 CIS + 2 BIS |
| `BT_ISO_TX_BUF_COUNT` = `BT_CTLR_SDC_ISO_TX_HCI_BUFFER_COUNT` | 6 | 2 BIS × (2 trong controller + dự phòng) |
| `BT_CTLR_ADV_ISO_STREAM_MAX/COUNT` | 2 | Tránh cấp thừa bộ nhớ controller |
| `LIBLC3` | y | Chỉ để tạo khung im lặng một lần |
| `BT_CTLR_SDC_BIG_RESERVED_TIME_US` | 5000 | Để trống khoảng 5 ms trong mỗi chu kỳ ISO 10 ms cho ACL, CIS và quảng bá. Mặc định 1600 làm việc ghép mũ bị treo |
| `NRF_RRAM_WRITE_BUFFER_SIZE` | 1 | Mặc định 32 khiến mỗi lần ghi RRAM phải xin **8,1 ms** radio trống liên tục (MPSL timeslot). BIG không bao giờ chừa đủ khoảng đó, nên việc lưu bond mới chặn thread BT RX tới 24 s và hub ngừng trả lời mũ. Với 1 dòng (16 B), mỗi lần ghi chỉ cần 0,6 ms. Mũ cũng đặt giá trị này để phòng khi lưu settings trong lúc nghe CIS / BIG |

Build ngày 20/10/2026 (có nút + ghép mũ): **FLASH 433 188 B (27,8 %), RAM 103 508 B (39,5 %)**, rất dư. Build 23/10/2026 (BIG 20 ms + `bcast_wq`): FLASH 438 832 B (28,1 %), RAM 115 856 B (44,2 %).

## 6. Rủi ro chính: lịch radio CIS + BIG

Hub vừa **nhận CIS** (điện thoại làm chủ lịch) vừa **phát BIG** (hub làm chủ lịch) trong mỗi chu kỳ 10 ms.

- CIS từ Pad 7 Pro (đo trên mũ): **NSE 5, subinterval 1,4 ms**, chiếm tới ~7 ms nếu dùng hết lượt phát lại (thường thành công ở lượt đầu nên ngắn hơn nhiều).
- BIG 2 BIS × (1 + RTN) lượt × ~0,6 ms: **RTN 4 ≈ 6 ms**, RTN 2 ≈ 3,6 ms.

Hai lịch chồng nhau thì controller (SDC) phải bỏ bớt lượt. Vì vậy mặc định **RTN = 2**: mũ ở gần (dưới 1–2 m) nên ít lượt phát lại là đủ. Nordic có công bố đã thử 1 CIS + 1 BIS chạy song song, còn **cấu hình này chưa đo**. Cách chỉnh trên mạch:
- xem `bike status` (`dropped`, `dry`) ở hub và tỉ lệ `bad` / PLC ở mũ;
- `bike rtn 1` hoặc `bike rtn 3` để thử.

**Đã đo ngày 23/10/2026 (RTN 2, Xiaomi Pad 7 Pro / điện thoại 68:4D:B6:A1:F6:F2):** rủi ro này xảy ra thật.

| Hiện tượng | Log |
|---|---|
| Nhạc chạy rồi rớt sau ~20–40 s | `ASE 0 stopped (reason 0x08, 3592 SDU, 919 bad)`: 26 % gói từ điện thoại mất; `repeated` tăng ~20 khung/s; `Disconnected ... (0x08 CONN_TIMEOUT)` |
| Bắt tay chậm tuỳ thời điểm | `Connected` → `Link encrypted`: 0,28 s, 3 s, 9,4 s |
| Bấm phát là mất kết nối, lặp lại | `Enable ASE 0` → 0,7 s → `Release ASE 0` → `Failed to disconnect CIS: -11` → điện thoại ngắt 0x13; không bao giờ có `Phone streaming` |

Cơ chế:
- Theo bảng ưu tiên của SDC (`nrfxlib/softdevice_controller/doc/scheduling.rst`), **Isochronous Broadcaster luôn ở mức 2**; CIS và ACL peripheral ở mức 3 (chỉ được nâng khi "sắp hết hạn"). Trùng giờ thì BIG thắng.
- CIS (SDU 240 B, RTN 2) và BIG cùng chu kỳ 10 ms nhưng theo **hai đồng hồ khác nhau**, nên vị trí tương đối trôi chậm (vài ms mỗi phút): lúc tốt chạy bình thường, lúc xấu kéo dài cả phút thì CIS mất gói / không thiết lập được và ACL hết hạn.
- Chu kỳ ACL điện thoại chọn là bội của 10 ms, nên ACL bị khoá đúng pha với BIG (như `hub_link` trước đây, B-14).
- Việc mũ vào xe **không** liên quan: mũ chỉ nối 0,1–0,5 s để lấy mã.

Đã sửa (23/10/2026):

| Sửa | Chi tiết |
|---|---|
| RTN mặc định 2 → **1** | Mỗi lượt BIS (120 B, mã hoá, 2M) ~0,69 ms: BIG từ 4,1 ms còn **2,8 ms** mỗi 10 ms, không còn che hết các lượt gửi lại của CIS. `BIG_RESERVED_TIME_US` giữ 5000: nếu nâng lên 6500 thì BIG chỉ còn 3,5 ms, không đủ cho `bike rtn 2` |
| Xin điện thoại **chu kỳ ACL 33,75 ms, supervision 6 s** | 1 s sau khi mã hoá, và 3 s sau mỗi lần điện thoại tự đổi lại (tối đa 3 lần mỗi kết nối). 33,75 ms lệch 3,75 ms mỗi sự kiện so với BIG nên ACL không bị khoá pha. Log: `Phone ACL ... -> asking for 33.75 ms / 6 s`, `Phone ACL params: interval ...` |
| Quảng bá cửa sổ ghép **không tên, không cờ general** | Hết cảnh thấy hai "Bike_Hub" trong 60 s ghép mũ |
| 5 kết nối | Mục 5 |

**Đo lần 2 ngày 23/10/2026 (sau các sửa trên, 2 mũ):** nghe được ~2 phút rồi tiếng chậm / rè, sau đó điện thoại pause.

| Thời điểm (từ lúc phát) | CIS mất (`repeated`/s) | BIG `sent` / 10 s |
|---|---|---|
| 0–45 s | ~12 | 1000 |
| 45–85 s | ~23 | 1000 |
| 85–115 s | giảm dần (chuỗi mất dài ra, chuyển sang `silence`) | 940 → 548 → **18** |
| 120 s | `ASE 0 stopped (reason 0x08, 11402 SDU, 4021 bad)`, 35 % gói mất | |

Cơ chế:
- Sự kiện CIS (NSE × subinterval, ~4 ms) là **một khối lập lịch**, ưu tiên mức 3. Chồng **bất kỳ phần nào** với BIG (mức 2) là mất cả sự kiện.
- Hai chu kỳ 10 ms trôi pha. Vùng chồng rộng khoảng BIG + CIS ≈ 6,9 ms trên 10 ms. Khi pha trôi vào vùng này, CIS mất **mọi** sự kiện → chết 0x08 sau 6 s → điện thoại dừng stream (pause). Đổi RTN chỉ làm vùng này hẹp lại, **không bao giờ hết**.
- Cùng cơ chế giải thích việc "bật tắt liên tục" trước đó: CIS mới có pha ngẫu nhiên, rơi vào vùng chồng thì chết sau ~6 s.
- 4 lần rớt 0x08 trước khi phát: điện thoại tự đổi ACL sang **30 ms** (bội số 10 ms, khoá pha với BIG), liên kết chết **ngay lúc đổi**, hết hạn đúng 5 s sau. Yêu cầu 33,75 ms của hub gửi 3 s sau thì đã muộn.

Đã sửa (23/10/2026, lần 2):

| Sửa | Chi tiết |
|---|---|
| **BIG 20 ms, BN 2, 1 BIS stereo** | BIG chỉ có ở 1 trong 2 sự kiện CIS → trong vùng chồng chỉ mất 1/2 sự kiện CIS, và với FT 2 gói đó được gửi lại ở sự kiện kế tiếp. Điều kiện: BIG (RTN 0: 2,3 ms; RTN 1: 4,7 ms) + CIS (~4 ms) < 10 ms, nên BIG 20 ms dùng tối đa RTN 1 |
| QoS hub đề nghị **độ trễ 20 ms** | Để điện thoại dùng FT ≥ 2 (mục 2). Nếu log `Phone CIS: ... FT 1` thì hub cảnh báo: với FT 1, mỗi gói bị chặn là mất |
| ACL bội số 10 ms → xin 33,75 ms **ngay** | Không chờ 3 s; tối đa 5 lần mỗi kết nối. PPCP = 33,75 ms / 6 s |
| Tạo lại BIG trên `bcast_wq` (stack 4 KB), bất đồng bộ | Trước đó chạy trên system workqueue và **chờ** callback `stopped` tới 5 s, trong khi phần dọn BIS cũng cần system workqueue → tự chặn ~8 s. Lần chạy lại thì `lc3_encode` (bộ đệm MDCT ~1,9 KB) làm **tràn stack 2560 B** (USAGE FAULT khi gõ `bike rtn 0`). Con số "controller mất ~2 s mới báo BIG dừng" đo ngày 20/10 thực ra cũng do host tự chặn |
| Heartbeat theo **từng 10 s** | `pushed / sent / dropped / dry / repeat / silence / no buf / err`, số SDU trong controller; `phone CIS ASE 0 \| 10 s: rx, lost, longest loss` |

Còn chưa rõ: trong lần đo 2, lúc 6–12 phút `sent` tăng ~3600 cặp/s khi điện thoại không phát (bình thường 100/s). Heartbeat mới in `pushed` và `sent` theo từng 10 s để bắt lại.

Nếu BIG 20 ms vẫn không giữ được CIS (điện thoại không chịu FT ≥ 2): giải pháp bền vững là **2 chip trên xe** (một nhận CIS từ điện thoại, một phát BIG; nối I2S / UART), hoặc để điện thoại tự phát Auracast. Firmware trên một radio chỉ giảm được tần suất lỗi.

## 7. Cách dùng và kiểm thử

### 7.1 Build và nạp

```
make APP=apps/bike build                       # build/bike
make APP=apps/bike JLINK_SN=<sn board hub> flash
scripts/rtt.sh build/bike                      # log + shell của hub
```

> Nạp `apps/bike` lên board nào thì board đó **không còn là mũ** nữa. Cần 1 board làm hub và ít nhất 1 board làm mũ (`apps/main`).

### 7.2 Thao tác

1. **Ghép điện thoại với "Bike_Hub"** như một tai nghe LE Audio. Log hub: `Connected`, `Pairing complete`, `Link encrypted`, âm `connected`.
2. **Ghép mũ bằng nút (mỗi mũ một lần):**
   - trên hub: **Power ngắn → giữ Vol+ 2 s** (âm "đang tìm" lặp);
   - trên mỗi mũ, trong 60 s: **Power ngắn → giữ Call 2 s**.

   Chi tiết ở mục 8.
3. **Phát nhạc trên điện thoại** (đầu ra là Bike_Hub). Log hub: `Config ASE 0: LC3 48 kHz, 2 ch (loc 0x3), 120 B/frame` → `Phone streaming ASE 0 ... -> broadcast L+R`. **Mọi mũ có tiếng cùng lúc.**
4. **Chỉnh âm lượng trên điện thoại:** mọi mũ to / nhỏ theo. Nút Vol trên từng mũ vẫn chỉnh riêng mũ đó.
5. **Hằng ngày:** bật hub và mũ, không bấm gì. Mũ tự vào lại broadcast của hub sau ~5 s (mục 8.3).

### 7.3 Lệnh shell của hub

| Lệnh | Việc làm |
|---|---|
| `bike status` | Điện thoại, ASE, broadcast (ID, octet, RTN, mã hoá), thống kê chuyển tiếp: `pushed`, `dropped`, `dry`, `repeated`, `silence`, `send err` |
| `bike pair` | Mở cửa sổ ghép mũ 60 s (như Power ngắn → giữ Vol+ 2 s) |
| `bike code new` / `open` / `<chuỗi>` | Mã ngẫu nhiên mới / không mã hoá / mã tự đặt (lưu, tạo lại BIG; mũ đã ghép phải ghép lại) |
| `bike rtn <0..4>` | Số lần phát lại mỗi PDU của BIS (lưu, tạo lại BIG). Mặc định 0; BIG 20 ms dùng tối đa 1; càng cao càng tranh radio với nhạc từ điện thoại (mục 6) |
| `bike isoint <10\|20>` | Kiểu BIG (lưu, tạo lại BIG): 20 = 1 BIS stereo, BN 2 (mặc định); 10 = 2 BIS trái / phải (kiểu cũ) |
| `bike unpair` | Xoá mọi ghép đôi (điện thoại + mũ). Mũ vẫn giữ thông tin broadcast nên vẫn tự vào được |

Heartbeat 10 s (số liệu **trong 10 s vừa qua**):
```
heartbeat: phone connected, STREAMING
broadcast: running, BIG 20 ms, BN 2, 1 BIS stereo, RTN 0, phone streaming | 10 s: pushed 1000, sent 1000, dropped 0, dry ..., repeat 3, silence 0, no buf 0, err 0 | in ctrl 2, queue 0
phone CIS ASE 0 | 10 s: rx 1000, lost 3, longest loss 1 SDU (10 ms)
```

### 7.4 Tiêu chí đạt

| Kiểm tra | Đạt khi |
|---|---|
| Khởi động hub | `BIS 0 started (...): ISO interval 20000 us, NSE 2, BN 2, IRC 1, ...` và `Broadcast "Bike Audio" ID 0x...... started: LC3 48 kHz, 10 ms, 120 B, BIG 20 ms, BN 2, 1 BIS stereo, RTN 0, encrypted` |
| Bắt đầu phát | `Phone CIS: ISO interval 10000 us, NSE ., BN 1, FT ≥ 2, ...` (FT 1: hub cảnh báo) |
| Nghe lâu | Nghe liên tục > 10 phút không pause; `phone CIS ... lost` nhỏ, `longest loss` vài SDU |
| Mũ vào khi điện thoại chưa phát | Mũ `STREAMING`, im lặng, `bad` gần 0 |
| Nhạc | Hub: `dropped` và `dry` tăng rất chậm (vài lần / phút trở xuống). Mũ: tiếng liên tục, không tạch, `I2S err` không tăng |
| 2 mũ cùng lúc | Cả hai có tiếng. Đặt cạnh nhau thì lệch không đáng kể |
| Điện thoại dừng / phát lại | Mũ im lặng rồi có tiếng lại, không phải vào lại |

## 8. Nút và ghép mũ ↔ hub bằng nút (không gõ mã)

### 8.1 Bảng nút (hub dùng đúng board mũ)

| Thao tác | **Hub** (`hub_ui.c`) | **Mũ** (`apps/main`) |
|---|---|---|
| Giữ Power 2 s | Bật | Bật |
| Giữ Power 3 s | Tắt (System OFF) | Tắt |
| Power ngắn | Âm trạng thái: 1 tiếng = có điện thoại, 2 tiếng = chưa. Đang mở cửa sổ ghép thì **đóng** cửa sổ | Âm trạng thái |
| **Power ngắn → giữ Vol+ 2 s** | **Mở cửa sổ ghép mũ 60 s** (gọi lại: gia hạn) | Chọn master intercom |
| **Power ngắn → giữ Call 2 s** | (để dành) | **Vào xe**: tự tìm. Thấy hub đang mở cửa sổ ghép thì ghép với hub (ưu tiên). Không có thì vào broadcast mạnh nhất. Đang nghe thì **rời** |
| Giữ Vol+ và Vol− 5 s | Xoá mọi ghép đôi | Xoá ghép đôi |
| Call ngắn | (để dành: play / pause) | Như cũ |

Âm báo hub: `power_on`; `connected` / `disconnected` (điện thoại); `pair_search` (lặp, cửa sổ đang mở); `helmet_joined` (mỗi mũ vào); `pair_end` (đóng, có mũ vào); `fail` (đóng, không có mũ nào); `unpair`; `power_off`.

### 8.2 Cơ chế ghép (lần đầu)

```
Hub: Power → giữ Vol+ 2 s      → bộ quảng bá thứ 3 (có thể kết nối), Service Data 128 bit
                                  {BHS UUID 0eed0101-..., ver 1, cờ "đang ghép"}, 60 s
Mũ:  Power → giữ Call 2 s      → quét; thấy cờ "đang ghép" → hub_link.c:
       kết nối (mũ là central) → mã hoá Just Works (lưu bond) → tìm đặc tính Join Info
       (0eed0102-..., chỉ đọc được khi đã MÃ HOÁ và cửa sổ ĐANG MỞ) → đọc 29 byte:
       broadcast ID, địa chỉ identity + SID bộ phát broadcast, mã 16 byte → ngắt
     → lưu "bcast/hub" → đồng bộ PA theo địa chỉ + SID → BASE → BIG (mã đã có) → `bc_joined`
Hub: kết nối nào đọc Join Info là mũ (không phải điện thoại) → âm `helmet_joined`, bật lại
     quảng bá cho mũ kế tiếp; mũ chưa tự ngắt sau 3 s thì hub ngắt
```

Ghép được nhiều mũ trong cùng 60 s, lần lượt hoặc tối đa 2 mũ cùng lúc.

**Khoá ghép đôi cũ bị lệch.** Trường hợp này xảy ra khi hub đã xoá ghép đôi (`bike unpair`, giữ Vol+ + Vol− 5 s, nạp lại flash), nhưng mũ vẫn còn khoá cũ, hoặc ngược lại. Khi đó mã hoá sẽ hỏng. Hai phía xử lý như sau:
- **Hub:** khi cửa sổ ghép đang mở, lỗi bảo mật **không** làm hub ngắt liên kết, để mũ nhận đúng mã lỗi `key missing`. Ngoài cửa sổ ghép, hub vẫn ngắt bằng `0x05` như trước. Điện thoại kết nối mà không mã hoá được vẫn bị ngắt sau 30 s.
- **Mũ:** coi là khoá cũ nếu lỗi là `key missing` / `key rejected` / `auth fail`, **hoặc** nếu mũ đã có ghép đôi với hub này mà mã hoá hỏng vì bất kỳ lý do gì, kể cả khi liên kết bị ngắt giữa chừng. Nhờ vậy mũ không phụ thuộc vào việc hub trả mã lỗi nào. Mũ tự xử lý như sau:
1. `hub_link.c` ghi lại địa chỉ hub, ngắt liên kết, chờ ngắt **xong hẳn** rồi mới báo `-EAGAIN`.
2. `broadcast_sink.c` chạy `bt_unpair` **chỉ với hub đó** (các ghép đôi điện thoại / intercom giữ nguyên), rồi quét lại.
3. Cửa sổ ghép trên hub vẫn mở, nên mũ thấy hub và ghép mới bằng Just Works.

Mỗi lần bấm "vào xe" chỉ thử lại **một** lần. Lần thứ hai vẫn hỏng thì mũ kêu `bc_fail`. Log mũ: `Stale bond with bike hub ... - deleting it and pairing again`.

> **Khác với đề xuất ban đầu:** đề xuất là hub làm Broadcast Assistant, chủ động kết nối tới mũ và ghi BASS. Bản hiện thực đảo chiều: **mũ chủ động kết nối hub và đọc thông tin**. Thao tác nút và kết quả cho người dùng không đổi. Lý do: hub không cần vai central / Assistant; mũ đã có sẵn vai central (intercom) để dùng lại; và mũ tự lưu thông tin nên ngày sau tự vào lại mà không cần hub đi tìm từng mũ.

### 8.3 Hằng ngày (không bấm gì)

- Mũ đã ghép: **5 s sau khi bật**, mũ tự đồng bộ PA theo địa chỉ + SID đã lưu và vào BIG bằng mã đã lưu, kêu `bc_joined`.
- Hub chưa bật hoặc ở ngoài tầm: mũ thử khoảng 40 s rồi nghỉ 30 s và thử lại, **im lặng** (không kêu `bc_fail`).
- Broadcast mất giữa chừng (tắt xe / hub, đi xa): mũ tự thử lại theo chu kỳ trên.
- Không tự vào khi đang gọi, đang intercom, hoặc đang nghe nhạc unicast từ điện thoại (tránh tranh radio).
- **Rời bằng nút** (Power → giữ Call khi đang nghe): mũ thôi tự vào lại cho tới lần ghép hoặc lần bật sau.
- `helmet bc forget`: quên hub. `helmet bc`: có dòng `Bike hub : paired, broadcast 0x..., auto-join on/off`.
- Bấm "vào xe" khi hub **không** mở cửa sổ ghép: mũ tự tìm. Nếu thấy broadcast của hub đã ghép thì dùng mã đã lưu, nên vẫn vào được.

### 8.4 Bảo mật

- Mã broadcast chỉ rời hub qua liên kết **đã mã hoá**, và chỉ trong cửa sổ 60 s do người dùng bật bằng nút trên hub.
- Hàng xóm có hub khác: mã khác, broadcast ID khác, nên không nghe được và mũ không vào nhầm.
- Mã nằm trong flash của hub và mũ (như khoá ghép đôi Bluetooth).

### 8.5 Kiểm thử ghép bằng nút

| Bước | Kỳ vọng (log) |
|---|---|
| Hub: Power ngắn → giữ Vol+ 2 s | Hub: `Helmet pairing window OPEN for 60 s`, âm lặp |
| Mũ: Power ngắn → giữ Call 2 s | Mũ: `Bike hub ... is pairing` → `Connecting to bike hub` → `Hub link encrypted` → `Paired with bike hub: broadcast 0x..., SID .., encrypted - saved` → `PA synced` → `BIGInfo: encrypted` → `Receiving broadcast "Bike Audio"`, âm `bc_joined`. Hub: `Helmet ... joined (1 in this window)`, âm `helmet_joined` |
| Lặp với mũ thứ hai | Hub đếm `2 in this window` |
| Chờ hết 60 s | Hub: `pairing window closed: 2 helmet(s) joined`, âm `pair_end` |
| Tắt / bật mũ | Mũ: `Paired bike hub: broadcast 0x... - auto-join in 5 s` → `Auto-joining ...` → `Receiving broadcast`, âm `bc_joined` |
| Tắt hub khi mũ đang nghe | Mũ mất BIG / PA → thử lại im lặng. Bật hub lại thì mũ tự vào lại |
| `bike unpair` trên hub, rồi ghép lại mũ đã ghép trước đó | Mũ: `Hub link security failed (2 ...)` → `Stale bond with bike hub ... - deleting it and pairing again` → `Hub link encrypted` → `Paired with bike hub` |

## 9. Giới hạn và giai đoạn sau

| # | Hiện tại | Hướng mở rộng |
|---|---|---|
| 1 | Cuộc gọi không qua hub (không có micro, không Conversational) | Mũ vẫn ghép trực tiếp với điện thoại cho cuộc gọi, hoặc hub thêm chiều gửi (micro trên mũ → hub → điện thoại) |
| 2 | Lịch CIS + BIG chưa đo | Mục 6 |
| 3 | Một mũ vừa ghép với điện thoại vừa ở gần hub có thể bị điện thoại chọn làm đầu ra thay hub | Trên điện thoại chọn đầu ra là Bike_Hub. Hoặc chỉ ghép điện thoại với hub |
| 4 | Call trên hub chưa dùng | Play / pause qua MCP (như mũ) |
| 5 | Đổi mã trên hub (`bike code new`) thì mũ đã ghép phải ghép lại | Hub có thể giữ mã cũ song song (chưa cần) |
