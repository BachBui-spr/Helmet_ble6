# Broadcast âm thanh: các vấn đề đã gặp và cách giải quyết

| | |
|---|---|
| Phạm vi | Toàn bộ phần **phát / thu âm thanh quảng bá (Auracast, BIG/BIS)** của dự án: mũ thu broadcast (`apps/main/src/features/broadcast/broadcast_sink.c`, `hub_link.c`, `lib/services/audio_out.c`) và bộ trung tâm trên xe (`apps/bike`) |
| Giai đoạn | 19/10/2026 – 23/10/2026, nhánh `feature/broadcast-sink` |
| Trạng thái | Commit `09aea4a` (23/10/2026): **broadcast ổn định với 1 xe + 2 mũ** (theo người thử trên mạch) |
| Tài liệu liên quan | Thiết kế mũ thu Auracast: [HELMET_AURACAST.md](HELMET_AURACAST.md). Thiết kế bộ trung tâm trên xe: [BIKE_HUB_THIET_KE.md](BIKE_HUB_THIET_KE.md) |
| Mục đích | Một chỗ duy nhất ghi lại **hiện tượng → nguyên nhân gốc → cách sửa → cách kiểm chứng** của mọi lỗi đã gặp, để người làm tiếp không phải dò lại từ log |

Ký hiệu trạng thái: ✅ đã sửa và kiểm chứng trên mạch · 🟡 đã sửa, chưa kiểm chứng đủ · 🔴 còn mở · ⚪ không phải lỗi firmware.

---

## Mục lục

1. [Hệ thống broadcast gồm những gì](#1-hệ-thống-broadcast-gồm-những-gì)
2. [Bảng tổng hợp](#2-bảng-tổng-hợp)
3. [Mũ thu Auracast](#3-mũ-thu-auracast)
4. [Chia sẻ âm thanh của Android: vì sao phải làm bộ trung tâm trên xe](#4-chia-sẻ-âm-thanh-của-android-vì-sao-phải-làm-bộ-trung-tâm-trên-xe)
5. [Ghép mũ với xe bằng nút](#5-ghép-mũ-với-xe-bằng-nút)
6. [Lưu settings trong lúc BIG chạy](#6-lưu-settings-trong-lúc-big-chạy)
7. [Tranh radio CIS / BIG trên xe (vấn đề lớn nhất)](#7-tranh-radio-cis--big-trên-xe-vấn-đề-lớn-nhất)
8. [Dựng lại BIG](#8-dựng-lại-big)
9. [Âm báo và khung im lặng trên xe](#9-âm-báo-và-khung-im-lặng-trên-xe)
10. [Hiện tượng không phải lỗi firmware](#10-hiện-tượng-không-phải-lỗi-firmware)
11. [Việc còn mở](#11-việc-còn-mở)
12. [Nguyên tắc rút ra](#12-nguyên-tắc-rút-ra)
13. [Chẩn đoán nhanh: triệu chứng → xem gì](#13-chẩn-đoán-nhanh-triệu-chứng--xem-gì)
14. [Cấu hình quan trọng](#14-cấu-hình-quan-trọng)
15. [Tham chiếu trong SDK](#15-tham-chiếu-trong-sdk)

---

## 1. Hệ thống broadcast gồm những gì

Có hai cách để nhiều mũ nghe chung nhạc:

```
Cách 1 – điện thoại tự phát Auracast (phụ thuộc hãng điện thoại):
  Điện thoại ──Auracast (BIG)──► Mũ 1, Mũ 2 …        (mũ: broadcast_sink.c, BASS)

Cách 2 – bộ trung tâm trên xe (dùng với mọi điện thoại LE Audio):
  Điện thoại ──LE Audio unicast (CIS)──► XE (apps/bike) ──Auracast (BIG)──► Mũ 1, Mũ 2 …
                                          phone_sink.c       bcast_source.c
  Ghép mũ ↔ xe bằng nút: bhs.c (xe) ↔ hub_link.c (mũ)
```

Cách 2 ra đời vì cách 1 không chạy được trên máy thử (mục 4). Xe dùng cùng module ME54BS62 (nRF54L15, chỉ có BLE, **một radio**). Phần lớn lỗi khó nằm ở chỗ một radio phải vừa nhận CIS từ điện thoại vừa phát BIG cho mũ (mục 7).

| Thành phần | File | Việc làm |
|---|---|---|
| Mũ: thu broadcast | `apps/main/src/features/broadcast/broadcast_sink.c` | Broadcast Sink + Scan Delegator (BASS), chọn BIS, đồng bộ PA / BIG |
| Mũ: vào xe | `apps/main/src/features/broadcast/hub_link.c` | Kết nối xe, đọc Join Info (ID, địa chỉ, mã), ngắt |
| Mũ: phát ra loa | `lib/services/audio_out.c` (+ `lib/core/pcm_ring.c`, `resampler.c`, `mixer.c`, `lib/hal/i2s_out.c`) | Giải mã LC3, ghép L/R, bù trôi, I2S |
| Xe: nhận từ điện thoại | `apps/bike/src/features/phone/phone_sink.c` | BAP Unicast Server, chỉ chiều nhận |
| Xe: phát cho mũ | `apps/bike/src/features/broadcast/bcast_source.c` | BAP Broadcast Source, chuyển nguyên khung LC3, khung im lặng, dựng lại BIG |
| Xe: ghép mũ | `apps/bike/src/features/broadcast/bhs.c` (giao thức: `lib/proto/include/me54/proto/bhs_proto.h`) | Bike Hub Service, cửa sổ ghép 60 s |
| Xe: kết nối, shell | `apps/bike/src/app/main.c` | Quảng bá cho điện thoại, bảo mật, chu kỳ ACL, `bike …` |

---

## 2. Bảng tổng hợp

| ID | Mảng | Hiện tượng | Nguyên nhân gốc | Cách giải quyết | TT |
|---|---|---|---|---|---|
| B-01 | Mũ | Có PA + BASE nhưng `no BIGInfo` | Android giữ PA + BASE mà không có BIG khi chưa phát nhạc | Đã có PA + BASE thì chờ không giới hạn | ✅ |
| B-02 | Mũ | `encrypted - set the code` | Broadcast có mã | `helmet bc code`, lưu `bcast/code`, dùng ngay | ✅ |
| B-03 | Mũ | Tiếng tạch, `Next buffers not supplied on time` 1–3 lần/s | Giải mã hết hàng đợi rồi mới nạp I2S; SDU broadcast đến dồn cục | Nạp I2S sau mỗi SDU, giữ 4 khối I2S | ✅ |
| B-04 | Mũ | Nguồn tạm dừng làm mũ rời broadcast | Coi BIG kết thúc `0x13` là lỗi | Về `PA_SYNCED`, chờ BIGInfo mới | ✅ |
| B-05 | Mũ | Mũ tự tắt khi đang nghe | Hẹn giờ tự tắt chỉ biết kết nối điện thoại | Chặn tự tắt khi `BC_STREAMING` | ✅ |
| B-06 | Mũ | Điện thoại phải ghép lại | Thêm BASS làm đổi DB GATT | Ghép lại một lần | ⚪ |
| B-07 | Android | "Couldn't connect to Helmet_2" khi chia sẻ | Hộp thoại là dual A2DP Classic, mũ không có Classic | Không sửa được ở mũ | ⚪ |
| B-08 | Android | Không có hộp thoại chia sẻ LE | HyperOS tắt AOSP LE Audio Sharing | Làm bộ trung tâm trên xe | ⚪ |
| B-09 | Mũ | Bản chẩn đoán còn trong firmware | `gatt_trace.c` + log debug BASS | Gỡ trước khi gộp | 🔴 |
| B-10 | Ghép | Ghép mũ ↔ xe treo 19 s | BIG chiếm gần hết chu kỳ 10 ms (`BIG_RESERVED_TIME` 1600 µs) | `BIG_RESERVED_TIME_US=5000` | ✅ |
| B-11 | Xe | Dựng lại BIG hỏng, broadcast tắt luôn | Xoá khi BIS chưa dừng hẳn (`-EBADMSG`) | Chờ `stopped`, thử lại (bản đầu); làm lại ở B-27 | ✅ |
| B-12 | Xe | Âm báo lỗi I2S | Thread âm báo bị chiếm CPU, ít khối đệm | 6 khối, ưu tiên 2 | ✅ |
| B-13 | Ghép | Mũ mang khoá cũ, ghép không được | Xe đã mất bond, mũ còn khoá | Mũ xoá bond với riêng xe đó rồi ghép lại | ✅ |
| B-14 | Ghép | MTU mất 12 s, ghép hết giờ | ACL 30–50 ms là bội số 10 ms, khoá pha với BIS | Mũ nối xe ở 11,25 ms | ✅ |
| B-15 | Ghép | Mũ thấy err 9 thay vì "key missing" | Xe ngắt ngay khi có lỗi bảo mật | Xe không ngắt khi cửa sổ ghép mở; mũ coi mọi lỗi mã hoá với xe đã bond là khoá cũ | ✅ |
| B-16 | Settings | Xe không trả lời 14 s sau khi ghép mới | Ghi RRAM xin 8,1 ms radio liên tục, BIG không bao giờ chừa đủ | `NRF_RRAM_WRITE_BUFFER_SIZE=1` (0,6 ms) | ✅ |
| B-17 | Settings | Xe mất bond / ID / mã | Rất có thể là hệ quả của B-16 | Sau B-16 ghép lại; cần thử tắt / bật | 🟡 |
| B-18 | Mũ | "Khoảng 1 phút lại ngắt" | Bấm nút "vào xe" lần nữa là rời (công tắc) | Không phải lỗi | ⚪ |
| B-19 | Log | Mốc thời gian nhảy về 0 | Bộ đếm 32 bit µs quay vòng sau 71 phút | Không phải lỗi | ⚪ |
| B-20 | Xe | Mũ `bad` ~0,2 % khi điện thoại im | `k_timer` 10 ms làm tròn thành 10,016 ms | Đề xuất: khung im lặng theo nhịp `sent` | 🔴 |
| B-21 | Radio | Mất gói 1–3 % theo đợt | Mất gói radio thật | Theo dõi | 🔴 |
| B-22 | Xe | RTN đã lưu khác mặc định | Lệnh `bike rtn` lưu vào settings | Mặc định mới là 0, trùng giá trị đã lưu | ✅ |
| B-23 | Radio | Nhạc chạy 20–40 s rồi rớt `0x08`, 26 % gói mất | BIG (ưu tiên 2) đè CIS / ACL (ưu tiên 3) | RTN 2 → 1, ACL 33,75 ms (bước 1) | ✅ → B-26 |
| B-24 | Xe | Điện thoại thấy hai "Bike_Hub" | Quảng bá ghép mũ có tên + cờ general | Bỏ tên và cờ general | ✅ |
| B-25 | Xe | Hết chỗ kết nối khi điện thoại + 2 mũ | `BT_MAX_CONN=3`, mỗi bộ quảng bá giữ sẵn 1 chỗ | 5 kết nối | ✅ |
| B-26 | Radio | 2 mũ nghe ~2 phút rồi chậm / rè, điện thoại pause | Sự kiện CIS trôi vào vùng chồng với BIG và mất **mọi** sự kiện | **BIG 20 ms, BN 2, 1 BIS stereo** + độ trễ CIS 20 ms (FT 2) | ✅ |
| B-27 | Xe | `bike rtn 0` làm xe treo rồi **tràn stack** | Dựng lại BIG trên sysworkq tự chặn chính nó, `lc3_encode` cần ~2,5 KB stack | Workqueue riêng 4 KB, dừng BIG bất đồng bộ | ✅ |
| B-28 | Radio | 4 lần rớt `0x08` trước khi phát | Điện thoại tự đổi ACL sang 30 ms, khoá pha với BIG | Xin 33,75 ms ngay; PPCP 33,75 ms | ✅ |
| B-29 | Xe | `sent` tăng ~3600 cặp/s khi điện thoại im | Chưa rõ | Heartbeat theo từng 10 s để bắt lại | 🔴 |

B-26, B-27, B-28 được đánh dấu ✅ theo commit `09aea4a` ("Broadcast is stable, 1 bike and 2 helmet"). Số liệu log của lần thử đó (đặc biệt giá trị **FT** của CIS) chưa được ghi lại vào đây; khi có, bổ sung vào mục 7.6.

---

## 3. Mũ thu Auracast

Thử ngày 19/10/2026 với Xiaomi Pad 7 Pro phát broadcast (có mã, 48 kHz, 2 BIS mono, PA 120–180 ms), mũ tự tìm.

### B-01. Có PA + BASE nhưng không có BIGInfo

- **Hiện tượng:** mũ đồng bộ được PA và đọc được BASE, nhưng sau 15 s vẫn `no BIGInfo` rồi bỏ.
- **Nguyên nhân:** khi chưa phát nhạc, Android giữ PA + BASE mà **không tạo BIG**. BIGInfo chỉ xuất hiện khi có nhạc.
- **Cách sửa:** đã có PA + BASE thì **chờ không giới hạn**.

### B-02. Broadcast có mã

- **Hiện tượng:** `encrypted - set the code`.
- **Cách sửa:** `helmet bc code <mã>`. Mã được lưu vào `bcast/code` và dùng ngay cả khi mũ đang chờ.
- **Lưu ý:** mũ chép chuỗi nguyên văn (byte 0 = ký tự đầu), giống sample Zephyr. Mã do điện thoại gửi qua BASS thì đã đúng định dạng.

### B-03. Tiếng tạch khi nghe broadcast

- **Hiện tượng:** nghe được nhưng có tiếng tạch; `Next buffers not supplied on time` / `I2S write failed` 1–3 lần/s. `rx` đủ, CPU còn rảnh 53 %. Giải mã trung bình 1,5 ms, lớn nhất 4,2 ms mỗi SDU.
- **Nguyên nhân:** `audio_thread` giải mã **hết hàng đợi** rồi mới nạp I2S. Với 3 khối, I2S chỉ chịu được ~10 ms chậm trễ. SDU broadcast (nhiều BIS, sau này BN 2) đến **dồn cục**, nên giải mã liên tiếp vượt quá 10 ms.
- **Cách sửa:** nạp I2S sau **mỗi** SDU (`pump()` trong vòng lặp); `I2S_TARGET_BLOCKS` 3 → 4 (chịu ~20 ms). Đổi lại, độ trễ mọi luồng tăng ~10 ms.

### B-04. Nguồn tạm dừng làm mũ rời broadcast

- **Hiện tượng:** điện thoại dừng nhạc, BIG kết thúc với `0x13` nhưng PA vẫn còn, và mũ rời hẳn broadcast.
- **Cách sửa:** quay về `PA_SYNCED`, chờ BIGInfo mới, không tính là lỗi.

### B-05. Mũ tự tắt khi đang nghe

- **Nguyên nhân:** hẹn giờ tự tắt chỉ xét kết nối với điện thoại. Khi mũ tự tìm broadcast thì không có kết nối nào.
- **Cách sửa:** chặn tự tắt khi đang phát (`BC_STREAMING`).

### B-06. Điện thoại phải ghép lại sau khi thêm BASS

Thêm dịch vụ BASS làm đổi DB GATT. Điện thoại thấy dữ liệu không khớp, nên cần quên mũ và ghép lại **một lần**. BASS được đăng ký trước `bt_enable()` (sau ASCS) để Database Hash ổn định giữa các lần khởi động.

---

## 4. Chia sẻ âm thanh của Android: vì sao phải làm bộ trung tâm trên xe

Phân tích bằng `adb logcat` + `dumpsys` ngày 19–20/10/2026, máy Xiaomi Pad 7 Pro (Android 16, HyperOS 3.0.303).

### B-07. "Couldn't connect to Helmet_2" khi chia sẻ

- **Hiện tượng:**
  - Buds3 kết nối trước, mũ sau: hộp thoại chia sẻ hiện ra nhưng báo lỗi, rồi tablet ngắt mũ.
  - Mũ trước, Buds3 sau: không có hộp thoại nào.
- **Nguyên nhân:** hộp thoại đó là **dual audio Classic** của HyperOS. Buds3 trên tablet này chạy A2DP (LE_AUDIO = 0), AirPods cũng vậy. Tablet cố mở A2DP tới mũ, mà nRF54L15 **chỉ có BLE**. Log mũ không có lệnh BASS nào; mũ bị ngắt đúng lúc bấm Share.

### B-08. Hai mũ kết nối nhưng không có hộp thoại chia sẻ LE

- **Nguyên nhân:**
  - Mũ đủ điều kiện cho AOSP LE Audio Sharing: BASS đã kết nối, có group id 1 và 2. Giả thuyết "thiếu CSIP" là **sai**.
  - Nhưng giao diện Bluetooth của HyperOS là của Xiaomi, không gắn `AudioSharingDialogHandler`.
  - `AudioSharingActivity` tự đóng sau chưa tới 1 s, tức `isAudioSharingUIAvailable()` = false. Cờ tính năng bị tắt trong bản HyperOS này; muốn đọc phải có root (`aflags`).
- **Thứ chạy được:**
  - Additional settings → "Broadcast audio" là giao diện Broadcast Assistant cho thiết bị đang phát; mũ nhận đúng lệnh BASS "Remote Scan Start/Stop".
  - Mũ cũng tự tìm được broadcast của tablet.
- **Kết luận:** chia sẻ âm thanh tới nhiều thiết bị LE **phụ thuộc hãng điện thoại**. Vì vậy dự án chuyển sang **bộ trung tâm trên xe** (cách 2): điện thoại chỉ cần phát LE Audio tới một thiết bị như tai nghe thường.

---

## 5. Ghép mũ với xe bằng nút

Cơ chế: trên xe mở cửa sổ ghép 60 s; trên mũ bấm Power ngắn rồi giữ Call 2 s. Mũ kết nối xe, mã hoá Just Works, đọc Join Info (broadcast ID, địa chỉ bộ phát, SID, mã 16 byte), rồi ngắt. Chi tiết ở [BIKE_HUB_THIET_KE.md](BIKE_HUB_THIET_KE.md) mục 8.

### B-10. Ghép treo 19 s khi BIG đang chạy

- **Hiện tượng:** ghép mũ ↔ xe treo 19 s rồi lỗi (err 9 / `0x13`, lần sau `0x08`).
- **Nguyên nhân:** `BT_CTLR_SDC_BIG_RESERVED_TIME_US` mặc định 1600 µs. Controller chọn tham số BIG sao cho BIG chiếm gần hết chu kỳ 10 ms, nên ACL của mũ đang ghép gần như không có lượt.
- **Kiểm chứng nguyên nhân:** tắt BIG (`bike rtn 0` ở bản lúc đó) thì ghép xong trong 1 s.
- **Cách sửa:** `BIG_RESERVED_TIME_US=5000`. BIG gọn trong ~5 ms, chừa ~5 ms cho ACL, CIS và quảng bá.

### B-13. Mũ mang khoá cũ

- **Hiện tượng:** mũ báo `err 2 PIN_OR_KEY_MISSING`, ghép không được.
- **Nguyên nhân:** xe đã mất bond với mũ (xem B-16, B-17), còn mũ vẫn giữ khoá cũ.
- **Cách sửa:** mũ xoá bond **chỉ với xe đó** rồi ghép lại một lần. `hub_link` chỉ báo `-EAGAIN` **sau khi liên kết đã ngắt hẳn**, để lần thử lại không gặp `-EBUSY`.

### B-14. MTU mất 12 s, ghép hết giờ

- **Hiện tượng:** trao đổi MTU mất 12 s, ghép hết giờ (`-116`), lệnh ngắt mất 4 s, có lỗi `0x22`.
- **Nguyên nhân:** chu kỳ kết nối mặc định 30–50 ms là **bội số** của chu kỳ ISO 10 ms. Mỗi sự kiện kết nối rơi đúng pha với BIS của xe, nên bị BIG đè liên tục.
- **Cách sửa:** mũ kết nối xe với chu kỳ cố định **11,25 ms** (không phải bội số 10 ms) và in chu kỳ thực tế khi kết nối. Sau sửa, MTU xong trong 90 ms.

### B-15. Mã lỗi bị che

- **Hiện tượng:** mũ thấy `9 UNSPECIFIED` thay vì "key missing", nên không biết là khoá cũ.
- **Nguyên nhân:** xe ngắt ngay bằng `0x05` khi có **bất kỳ** lỗi bảo mật nào.
- **Cách sửa:**
  - Xe: không ngắt khi cửa sổ ghép đang mở.
  - Mũ: nếu đã có bond với xe (`bt_foreach_bond`) mà mã hoá hỏng vì bất kỳ lý do gì thì coi là khoá cũ.
- **Kiểm chứng (20/10):** mũ ghép lại từ đầu, tổng ~1,3 s (encrypted 0,52 s → bonded +0,11 s → joined +0,08 s).

### B-24. Điện thoại thấy hai "Bike_Hub"

- **Nguyên nhân:** trong 60 s cửa sổ ghép, quảng bá ghép mũ (bộ quảng bá thứ 3, địa chỉ RPA riêng) mang tên "Bike_Hub" và cờ general. Điện thoại / máy tính bảng hiện thành hai thiết bị, kết nối nhầm thì bị ngắt ngay.
- **Cách sửa:** quảng bá ghép mũ chỉ có Flags (không general) + Service Data. Mũ nhận diện xe bằng Service Data, không cần tên.

### B-25. Hết chỗ kết nối

- **Nguyên nhân:** `BT_MAX_CONN=3`. Zephyr giữ sẵn 1 chỗ kết nối cho **mỗi bộ quảng bá có thể kết nối đang chạy**, nên điện thoại + 2 mũ đang ghép là hết.
- **Cách sửa:** `BT_MAX_CONN=5`, `BT_CTLR_SDC_PERIPHERAL_COUNT=5`. Đã chạy được 2 mũ + điện thoại cùng lúc.

---

## 6. Lưu settings trong lúc BIG chạy

### B-16. Xe không trả lời sau khi ghép mới

- **Hiện tượng:** ghép mới xong, xe không trả lời việc tìm Join Info suốt 14 s; lệnh ngắt mất 2 s.
- **Nguyên nhân:**
  - Lưu bond chạy ngay trong thread BT RX (`bt_keys_store`).
  - Ghi RRAM phải đi qua MPSL timeslot. Với `NRF_RRAM_WRITE_BUFFER_SIZE=32`, mỗi lần ghi xin **8,1 ms radio trống liên tục** (`FLASH_SLOT_WRITE` 8000 µs + 100 µs).
  - BIG 10 ms chiếm ~5 ms nên không bao giờ có khoảng trống đó. Mỗi thao tác ghi có thể chờ tới **24 s** (`FLASH_TIMEOUT_MS`), và trong lúc đó thread BT RX bị chặn.
- **Cách sửa:** `CONFIG_NRF_RRAM_WRITE_BUFFER_SIZE=1` trên **cả xe và mũ**. Mỗi lần ghi chỉ cần 0,6 ms, vừa khoảng trống của BIG.
- **Kiểm chứng:** xe lưu bond sau 0,11 s.

### B-17. Xe mất bond, broadcast ID và mã

- **Hiện tượng:** xe "quên" mũ; broadcast ID đổi `0xf0f566` → `0xd00371`. Mũ tự vào lại thì kẹt `PA_SYNCED` với `encrypt state 3` (sai mã).
- **Nguyên nhân (rất có thể):** hệ quả của B-16. Các lần ghi settings trong lúc BIG chạy đã hết giờ, nên xe mất bond, ID và mã.
- **Còn phải làm:** tắt / bật xe và mũ, xác nhận ID giữ nguyên và mũ tự vào lại sau ~5 s.

---

## 7. Tranh radio CIS / BIG trên xe (vấn đề lớn nhất)

Xe vừa **nhận CIS** từ điện thoại (điện thoại làm chủ lịch) vừa **phát BIG** cho mũ (xe làm chủ lịch) trên **cùng một radio**. Lỗi này đi qua ba lần sửa mới dứt điểm.

### 7.1 Quy tắc lập lịch của SoftDevice Controller

Từ `nrfxlib/softdevice_controller/doc/scheduling.rst`:

| Mức | Vai trò liên quan |
|---|---|
| 1 (cao nhất) | Kết nối peripheral / central **sắp hết hạn supervision** |
| 2 | **Isochronous Broadcaster (BIG): luôn ở mức này**; CIS khi đang thiết lập hoặc sắp hết hạn |
| 3 | Mọi vai trò khác: **CIS và ACL peripheral bình thường**, quảng bá |

Ngoài ra:
- Một timing-event **chạy trọn vẹn, không bị ngắt giữa chừng**. Mọi subevent của một nhóm ISO nằm trong **cùng một** timing-event.
- Vì vậy chỉ cần sự kiện CIS (NSE × subinterval) **chồng một phần** với BIG là CIS mất **cả** sự kiện.
- `sdc_hci_cmd_vs_set_role_priority` trong bản SDC này chỉ hỗ trợ initiator, **không** hạ được ưu tiên của BIG.

### 7.2 Lần 1 (B-23): nhạc chạy 20–40 s rồi rớt

Đo ngày 23/10/2026, RTN 2, Xiaomi Pad 7 Pro:

| Hiện tượng | Log |
|---|---|
| Nhạc chạy rồi rớt sau ~20–40 s | `ASE 0 stopped (reason 0x08, 3592 SDU, 919 bad)`: 26 % gói mất; `repeated` tăng ~20 khung/s; `Disconnected ... (0x08)` |
| Bắt tay chậm tuỳ thời điểm | `Connected` → `Link encrypted`: 0,28 s, 3 s, 9,4 s |
| Bấm phát là mất kết nối, lặp lại | `Enable ASE 0` → 0,7 s → `Release ASE 0` → `Failed to disconnect CIS: -11` → điện thoại ngắt `0x13` |

Sửa bước 1:
- RTN BIG 2 → 1 (BIG 4,1 → 2,8 ms mỗi 10 ms);
- xe xin điện thoại ACL **33,75 ms**, supervision 6 s;
- sửa luôn B-24 và B-25.

Kết quả: hai mũ kết nối và nghe được, nhưng vẫn chết sau ~2 phút (lần 2).

### 7.3 Lần 2 (B-26): nghe ~2 phút rồi chậm, rè, pause

Diễn biến tính từ lúc bắt đầu phát:

| Thời điểm | CIS mất (`repeated`/s) | BIG `sent` / 10 s |
|---|---|---|
| 0–45 s | ~12 | 1000 |
| 45–85 s | ~23 | 1000 |
| 85–115 s | giảm dần (chuỗi mất dài ra, chuyển sang im lặng) | 940 → 548 → **18** |
| 120 s | `ASE 0 stopped (reason 0x08, 11402 SDU, 4021 bad)` = **35 %** mất | |

Sau đó điện thoại gửi Enable lại, Release, `Failed to disconnect CIS -11`, rồi ngắt `0x13`. Người nghe thấy tiếng "chậm, không đồng bộ" (35 % khung là lặp lại hoặc im lặng), rồi nhạc pause.

**Cơ chế (mô hình trôi pha):**
1. CIS (đồng hồ điện thoại) và BIG (đồng hồ xe) đều có chu kỳ 10 ms nhưng lệch nhau vài chục ppm, nên pha tương đối **trôi chậm**.
2. Ước tính độ dài mỗi sự kiện:
   - CIS: SDU 240 B, RTN 2, NSE ~3, khoảng 4 ms;
   - BIG: 2 BIS, RTN 1, khoảng 2,8 ms.
3. Vùng chồng có độ rộng ≈ BIG + CIS ≈ **6,9 ms trên 10 ms**. Trong vùng đó **mọi** sự kiện CIS đều bị BIG đè, nên CIS chết sau 6 s supervision.
4. Vùng an toàn chỉ còn ~3 ms. Stream sống ~95 s trước khi vào vùng chồng, ứng với độ lệch đồng hồ ~30 ppm, mức bình thường của thạch anh.
5. Mô hình này giải thích luôn hiện tượng "bật tắt liên tục" trước đó: mỗi CIS mới có pha ngẫu nhiên, theo mô hình có ~70 % khả năng rơi ngay vào vùng chồng và chết sau ~6 s.
6. **RTN chỉ làm vùng chồng hẹp lại, không bao giờ xoá được.** Chừng nào hai chu kỳ bằng nhau và trôi pha, CIS chắc chắn sẽ có lúc rơi vào vùng chồng.

### 7.4 Lần 3: BIG 20 ms, BN 2, 1 BIS stereo

Ý tưởng là phá thế "cùng chu kỳ". BIG chỉ chạy **mỗi 20 ms**, mỗi sự kiện mang **2 SDU 10 ms** (BN 2). Khi đó:
- trong vùng chồng, BIG chỉ đè **1 trong 2** sự kiện CIS, không bao giờ đè liên tiếp;
- nếu điện thoại dùng **FT ≥ 2** (một gói được phép gửi lại ở sự kiện CIS kế tiếp), gói bị đè được gửi lại ngay ở sự kiện sau, sự kiện này không có BIG;
- điều kiện để BIG không đè cả hai sự kiện CIS: BIG + CIS < 10 ms.

| Kiểu BIG | Độ dài BIG mỗi sự kiện | Ghi chú |
|---|---|---|
| 10 ms, 2 BIS 120 B, RTN 2 / 1 / 0 | 4,1 / 2,8 / 1,4 ms | Kiểu cũ (`bike isoint 10`) |
| 20 ms, 1 BIS stereo 240 B, RTN 0 / 1 | 2,3 / 4,7 ms | Mặc định; tối đa RTN 1 để BIG + CIS < 10 ms |

Hiện thực:

| Thay đổi | Chi tiết |
|---|---|
| BIG 20 ms, BN 2, 1 BIS stereo | Tạo bằng **LE Create BIG Test** (`CONFIG_BT_ISO_TEST_PARAMS=y`): ISO interval 16 × 1,25 ms, BN 2, NSE = BN × IRC, PTO 0, PDU = SDU = 2 × octet. SDU stereo `[khung trái][khung phải]`. Cần 2 × octet ≤ 251 B (48_4: 240 B), nên `BT_ISO_TX_MTU=251` |
| Lệnh `bike isoint 10\|20` | Lưu `bike/isoint`; 10 = kiểu cũ 2 BIS để so sánh |
| Tự quay về 10 ms | Controller từ chối BIG 20 ms 2 lần liền thì xe dùng 10 ms (log `20 ms BIG not possible`) |
| RTN mặc định 0 | BIG 20 ms dùng tối đa RTN 1 |
| QoS xe đề nghị: độ trễ CIS **20 ms** (trước 10 ms) | Điện thoại lấy đúng giá trị xe đề nghị (đã thấy ở lần 1: RTN 2, latency 10 ms trùng đề nghị). Độ trễ 20 ms cho phép controller điện thoại dùng FT 2 |
| Log thông số CIS thật | `Phone CIS: ISO interval …, NSE, BN, FT, PDU, subinterval, transport latency`. Nếu FT 1 với BIG 20 ms thì xe cảnh báo |
| Mũ | **Không phải sửa**: mũ ưu tiên chọn BIS stereo (`select_bis()`), giải mã SDU 2 kênh, `BT_ISO_RX_MTU=251`, đã chịu được SDU dồn cục (B-03) |

Kết quả: commit `09aea4a`, broadcast ổn định với 1 xe + 2 mũ.

### 7.5 B-28: rớt kết nối khi điện thoại tự đổi chu kỳ ACL

- **Hiện tượng:** trước khi phát nhạc, 4 lần liền rớt `0x08`.
- **Phân tích log:** cả 4 lần, điện thoại tự đổi ACL 7,5 → **30 ms**, và liên kết hết hạn đúng ~4,97 s sau lúc đổi (supervision 5 s). Nghĩa là **liên kết chết ngay tại thời điểm đổi**. Yêu cầu 33,75 ms của xe gửi 3 s sau đó rơi vào liên kết đã chết, nên không phải nguyên nhân và cũng không cứu được.
- **Nguyên nhân:** 30 ms là bội số 10 ms, nên ACL khoá pha với BIG, như B-14. Ở 60 ms và 33,75 ms thì liên kết sống.
- **Phát hiện phụ:** điện thoại **có đọc PPCP**. Nó chọn 75 ms, nằm trong dải PPCP cũ 62,5–87,5 ms.
- **Cách sửa:**
  - PPCP = 33,75 ms / 6 s (`BT_PERIPHERAL_PREF_MIN/MAX_INT=27`, `TIMEOUT=600`);
  - điện thoại chọn bội số 10 ms thì xe xin 33,75 ms **ngay**, không chờ 3 s; tối đa 5 lần mỗi kết nối.

### 7.6 Nếu sau này lỗi radio quay lại

Kiểm tra theo thứ tự:
1. Log `Phone CIS: ... FT n`. Nếu **FT 1**, BIG 20 ms vẫn làm mất 1/2 sự kiện CIS trong vùng chồng. Thử điện thoại khác, hoặc so với `bike isoint 10`.
2. Heartbeat `phone CIS ASE 0 | 10 s: rx, lost, longest loss`. Bình thường `lost` nhỏ, `longest loss` vài SDU.
3. `bike status`: RTN và kiểu BIG đang dùng. Giá trị đã lưu (`bike/rtn`, `bike/isoint`) được ưu tiên hơn mặc định.

Giải pháp bền vững nếu firmware không đủ: **hai chip trên xe** (một nhận CIS, một phát BIG; nối I2S / UART), hoặc để điện thoại tự phát Auracast (cách 1). Trên một radio, firmware chỉ giảm được xác suất chồng lịch.

---

## 8. Dựng lại BIG

Xe phải dựng lại BIG khi điện thoại đổi số octet, khi đổi mã / RTN / chu kỳ, hoặc khi controller tự kết thúc BIG.

### B-11. Bản đầu: broadcast tắt luôn

- **Hiện tượng:** `delete failed (-77)`, broadcast tắt hẳn.
- **Nguyên nhân:** `bt_bap_broadcast_source_stop()` là bất đồng bộ. Xoá khi BIS chưa dừng hẳn thì trả `-EBADMSG`.
- **Sửa lúc đó:** chờ `stopped` tối đa 5 s, thử xoá lại khi `-EBADMSG`, lỗi thì thử lại sau 1 s. Preset QoS để `static` vì broadcast source giữ con trỏ tới nó. Bản này về sau gây ra B-27.

### B-27. `bike rtn 0` làm xe treo rồi tràn stack

- **Hiện tượng:**

```
BIS 0 stopped (reason 0x16)
Broadcast stop not confirmed after 5 s        <- 5 s sau
BIS 1 stopped (reason 0x16)                   <- 2 s sau nữa
Broadcast stopped (reason 0x16)
***** USAGE FAULT *****  Stack overflow  Current thread: sysworkq   <- 1 s sau
```

Tra địa chỉ lỗi bằng `addr2line`: PC nằm ở `lc3_mdct_forward` (mdct.c:433), LR ở `lc3_encode` → `analyze`.

- **Nguyên nhân 1 – tự chặn:** `restart_work` chạy trên **system workqueue** và đứng chờ callback `stopped` (5 s), rồi thử xoá 10 × 200 ms. Phần dọn dẹp của BIS 1 (còn SDU đang chờ) cũng cần system workqueue, nên chỉ chạy được sau khi hàm này nhả ra. Kết quả: mỗi lần dựng lại mất ~8 s không có âm thanh. Con số "controller mất ~2 s mới báo BIG dừng" đo ngày 20/10 thực ra cũng là do host tự chặn.
- **Nguyên nhân 2 – tràn stack:** lần chạy lại gọi `make_silence()` → `lc3_encode`. Riêng bộ đệm MDCT đã chiếm ~1,9 KB stack, trong khi sysworkq chỉ có 2560 B. Lúc khởi động không lỗi vì khung im lặng được mã hoá trên thread main (3072 B).
- **Cách sửa:**
  - workqueue **riêng** `bcast_wq` (stack 4 KB, preemptive ưu tiên 5);
  - dừng BIG **bất đồng bộ**: gọi stop rồi trả về ngay; callback `stopped` mới đẩy bước xoá → tạo BIG. Hẹn giờ 5 s chỉ để phòng khi không có callback;
  - yêu cầu dựng lại mới đến trong lúc đang dừng thì chờ tiếp, không gọi stop lần nữa;
  - khung im lặng chỉ mã hoá lại khi số octet đổi;
  - số octet điện thoại yêu cầu chỉ được lấy ra ở bước tạo BIG, để không mất khi đang chờ;
  - controller tự kết thúc BIG (hoặc tạo BIG thất bại) thì xe tự dựng lại sau 200 ms.
- **Kết quả:** dựng lại BIG không còn chặn system workqueue (nơi chạy `tx_work` và callback `sent`). Thời gian mỗi lần dựng lại dự kiến dưới 0,5 s nhưng **chưa đo**: lần sau đổi `bike isoint` hãy xem khoảng cách giữa `Restarting broadcast` và `Broadcast ... started`.

---

## 9. Âm báo và khung im lặng trên xe

### B-12. Âm báo lỗi I2S

- **Hiện tượng:** `Next buffers not supplied on time`, `Cannot write in state: 4` khi phát âm "đang tìm".
- **Nguyên nhân (nghi, chưa đo):** thread âm báo (preemptive 6) bị chiếm CPU trong khi chỉ có 4 khối đệm (40 ms).
- **Cách sửa:** 6 khối (60 ms), bắt đầu I2S sau 3 khối, thread ưu tiên 2; `i2s_trigger(DRAIN/DROP)` đúng thứ tự tham số. Lần thử sau không còn lỗi.

### B-20. Mũ báo `bad` ~0,2 % khi điện thoại im

- **Nguyên nhân:** khung im lặng do `k_timer` 10 ms tạo ra, mà tick là 32 µs nên bị làm tròn lên 313 tick = **10,016 ms**. Xe chỉ tạo 99,84 khung/s (đo `sent` ~998,5 mỗi 10 s). BIG cần 100 khung/s, thiếu ~1,6 khung/s, và mũ che bằng PLC.
- **Chỉ xảy ra khi điện thoại không phát.** Lúc có nhạc, nhịp do điện thoại quyết định.
- **Đề xuất (chưa làm):** tạo khung im lặng theo nhịp chính BIG (mỗi callback `sent` đẩy thêm một cặp).

---

## 10. Hiện tượng không phải lỗi firmware

| ID | Hiện tượng | Giải thích |
|---|---|---|
| B-06 | Điện thoại phải ghép lại sau khi cập nhật firmware có BASS | DB GATT đổi (mục 3) |
| B-07, B-08 | Chia sẻ âm thanh của tablet không đưa được mũ vào | Chính sách của HyperOS (mục 4) |
| B-18 | "Khoảng 1 phút lại ngắt" (commit `a75d769` ghi "error suddenly interrupt") | Log cho thấy `Gesture power short` → `Gesture call seqhold` → `leaving broadcast (button)`. Thao tác "vào xe" là **công tắc**, đang nghe mà bấm lại là rời. Lần thử sau không bấm nút, mũ nghe liên tục 11,5 phút |
| B-19 | Mốc thời gian log nhảy về `00:00:21` | Bộ đếm thời gian log 32 bit µs quay vòng sau 71 phút 35 giây; bộ đếm `rx`, `SDU` vẫn tăng tiếp |

---

## 11. Việc còn mở

| ID | Việc | Ưu tiên | Ghi chú |
|---|---|---|---|
| — | Ghi lại số liệu lần thử ổn định (`09aea4a`): FT của CIS, `lost`, `longest loss`, thời gian nghe liên tục | P1 | Để biết BIG 20 ms đứng vững nhờ FT 2 hay nhờ lý do khác |
| B-29 | Lúc 6–12 phút, điện thoại không phát, `sent` tăng ~3600 cặp/s (bình thường 100/s) | P2 | Muốn gửi thì phải có cặp khung được đẩy vào, mà khi điện thoại im chỉ có hẹn giờ 10 ms đẩy. Heartbeat mới in `pushed` / `sent` theo từng 10 s để bắt lại |
| B-17 | Tắt / bật xe và mũ: ID, mã, bond phải giữ nguyên | P2 | |
| B-09 | Gỡ bản chẩn đoán (`gatt_trace.c`, log debug BASS, `BT_GATT_AUTHORIZATION_CUSTOM`) | P2 | Trước khi gộp vào `main` |
| B-20 | Khung im lặng theo nhịp `sent` | P3 | |
| B-21 | Mất gói 1–3 % theo đợt | P3 | Theo dõi khi đi xe thật |
| — | Đồng bộ tiếng giữa hai mũ (presentation delay của BASE chưa áp dụng) | P3 | Hai mũ cùng firmware nên độ trễ gần nhau nhưng không khớp mẫu |
| — | Nhánh `feature/Sounding-Channel` có bản `apps/bike` **cũ**, chưa có các sửa ngày 23/10 (B-23 → B-28) | P1 khi gộp | Khi gộp nhánh, lấy `apps/bike` từ `feature/broadcast-sink`; chế độ Định vị trên nhánh đó tạm dừng BIG (`bcast_source_pause`) nên cần ghép lại cho khớp `bcast_wq` |
| — | Điện thoại có LE Audio Sharing thật (vd Pixel) làm Broadcast Assistant | P3 | Đường PAST + Set Broadcast Code của mũ chưa chạy với máy thật |

---

## 12. Nguyên tắc rút ra

1. **Radio là tài nguyên dùng chung.** BIG, CIS, ACL, PA, quảng bá và cả **ghi RRAM** đều tranh một radio. Luôn tính theo "khoảng trống liên tục trong mỗi chu kỳ ISO" (B-10, B-16).
2. **Sự kiện ISO là một khối.** Chồng một phần là mất cả sự kiện; ưu tiên của BIG cố định ở mức 2. Đọc `scheduling.rst` của SDC trước khi chọn tham số (B-26).
3. **Hai lịch tuần hoàn cùng chu kỳ nhưng khác đồng hồ thì chắc chắn sẽ có lúc chồng nhau.** Giảm độ dài chỉ làm hiếm đi. Muốn hết hẳn thì phải đổi chu kỳ để không chồng liên tiếp, kèm cơ chế gửi lại qua sự kiện sau (FT ≥ 2), hoặc dùng radio riêng (B-26).
4. **Tránh chu kỳ là bội số của ISO interval** cho liên kết phụ: 11,25 ms, 33,75 ms thay cho 30 / 40 / 60 ms (B-14, B-28).
5. **Không bao giờ đứng chờ callback Bluetooth trên system workqueue.** Callback hoặc phần dọn dẹp có thể cần chính hàng đợi đó. Dùng kiểu bất đồng bộ: gọi lệnh, trả về, bước tiếp theo do callback kích hoạt (B-27).
6. **LC3 cần stack lớn** (~2,5 KB cho `lc3_encode` ở 48 kHz). Chỉ gọi trong thread có stack đủ, và gọi càng ít càng tốt (B-27).
7. **Điện thoại nghe theo đề nghị của thiết bị.** Nó lấy RTN / độ trễ từ QoS preference của xe và đọc PPCP. Dùng hai kênh này để "lái" điện thoại thay vì xin đổi sau (B-26, B-28).
8. **Giá trị đã lưu trong settings đè giá trị mặc định.** Đổi mặc định trong code thì phải kiểm tra `bike status` xem thiết bị đang dùng giá trị nào (B-22).
9. **Đừng che mã lỗi.** Bên ngắt sớm làm bên kia mất thông tin chẩn đoán. Quy tắc phía khởi tạo ("đã có bond mà mã hoá hỏng thì ghép lại") bền hơn việc dựa vào mã lỗi của bên kia (B-15).
10. **Báo kết quả sau khi tài nguyên đã giải phóng.** Chỉ trả `-EAGAIN` khi liên kết đã ngắt hẳn, để lần thử lại không gặp `-EBUSY` (B-13).
11. **Đọc log theo thời gian chính xác.** Khoảng cách giữa hai dòng thường chỉ ra nguyên nhân: đổi ACL 30 ms → hết hạn đúng 4,97 s sau (B-28); dừng BIS 1 đúng lúc hàm chờ nhả ra (B-27).
12. **Heartbeat nên in số liệu theo từng khoảng (delta), không chỉ tổng tích luỹ.** Nhờ vậy thấy ngay lúc tỉ lệ thay đổi (B-26, B-29).

---

## 13. Chẩn đoán nhanh: triệu chứng → xem gì

| Triệu chứng | Xem log / lệnh nào | Nghi gì |
|---|---|---|
| Mũ đồng bộ PA nhưng không có tiếng | Log mũ: `no BIGInfo`, `encrypted` | Nguồn chưa phát (B-01); thiếu / sai mã (B-02, B-17) |
| Mũ có tiếng tạch | `I2S err`, `Next buffers not supplied` ở mũ | Nạp I2S không kịp (B-03) |
| Ghép mũ ↔ xe chậm / hết giờ | Chu kỳ kết nối in khi kết nối; thời gian MTU | ACL khoá pha với BIG (B-14); BIG reserved time (B-10); ghi RRAM (B-16) |
| Mũ báo khoá cũ | `PIN_OR_KEY_MISSING`, err 9 | B-13, B-15 |
| Điện thoại thấy hai "Bike_Hub" | Đang mở cửa sổ ghép? | B-24 |
| Nhạc từ điện thoại chập chờn, rồi pause | Heartbeat xe `phone CIS ... lost, longest loss`; `ASE 0 stopped (0x08, … bad)`; `Phone CIS: ... FT` | Tranh radio CIS / BIG (B-23, B-26) |
| Rớt kết nối điện thoại trước khi phát | `Phone ACL params: interval …` ngay trước `Disconnected (0x08)` | ACL là bội số 10 ms (B-28) |
| Đổi `bike rtn` / `code` / `isoint` mà broadcast tắt lâu | `Restarting broadcast`, `Broadcast stopped`, `stop not confirmed` | Đường dựng lại BIG (B-27) |
| Xe crash `Stack overflow` | `Current thread`; tra PC bằng `addr2line -e build/bike/bike/zephyr/zephyr.elf <pc>` | Hàm nặng (LC3) chạy trong thread stack nhỏ (B-27) |
| `sent` tăng bất thường | Heartbeat xe: `pushed` và `sent` theo từng 10 s | B-29 |

Heartbeat 10 s của xe (bản hiện tại):

```
heartbeat: phone connected, STREAMING
broadcast: running, BIG 20 ms, BN 2, 1 BIS stereo, RTN 0, phone streaming | 10 s: pushed 1000, sent 1000, dropped 0, dry ..., repeat 3, silence 0, no buf 0, err 0 | in ctrl 2, queue 0
phone CIS ASE 0 | 10 s: rx 1000, lost 3, longest loss 1 SDU (10 ms)
```

| Trường | Ý nghĩa |
|---|---|
| `pushed` | Cặp khung đưa vào hàng đợi (từ điện thoại hoặc khung im lặng) |
| `sent` | SDU đã giao cho controller trên BIS 0 |
| `dropped` | Bỏ cặp cũ nhất vì hàng đợi đầy (điện thoại nhanh hơn BIG, hoặc controller kẹt) |
| `dry` | Controller hết SDU trong lúc đang phát. Chỉ để tham khảo, nhất là với BN 2 |
| `repeat` / `silence` | Gói từ điện thoại mất: lặp khung trước một lần, sau đó chèn im lặng |
| `no buf` / `err` | Hết bộ đệm TX / `bt_bap_stream_send` lỗi |
| `in ctrl` | Số SDU đang nằm trong controller (BIG 20 ms tối đa 3; 10 ms tối đa 2 mỗi BIS) |
| `lost`, `longest loss` | SDU CIS từ điện thoại bị mất, và chuỗi mất liên tiếp dài nhất trong 10 s |

---

## 14. Cấu hình quan trọng

### Xe (`apps/bike/prj.conf`)

| Kconfig | Giá trị | Vì sao | Lỗi liên quan |
|---|---|---|---|
| `BT_CTLR_SDC_BIG_RESERVED_TIME_US` | 5000 | Chừa ~5 ms mỗi chu kỳ cho ACL, CIS, quảng bá | B-10 |
| `NRF_RRAM_WRITE_BUFFER_SIZE` | 1 | Mỗi lần ghi RRAM chỉ xin 0,6 ms radio | B-16 |
| `BT_MAX_CONN`, `BT_CTLR_SDC_PERIPHERAL_COUNT` | 5, 5 | Điện thoại + mũ đang ghép + chỗ giữ cho bộ quảng bá | B-25 |
| `BT_ISO_TEST_PARAMS` | y | BIG 20 ms, BN 2 (LE Create BIG Test) | B-26 |
| `BT_ISO_TX_MTU` | 251 | SDU stereo 240 B | B-26 |
| `BT_PERIPHERAL_PREF_MIN_INT` / `MAX_INT` / `TIMEOUT` | 27 / 27 / 600 | PPCP 33,75 ms / 6 s | B-28 |
| `SYSTEM_WORKQUEUE_STACK_SIZE` | 2560 | Không tăng: việc nặng đã chuyển sang `bcast_wq` (4 KB) | B-27 |

### Mũ (`apps/main/prj.conf`)

| Kconfig | Giá trị | Vì sao | Lỗi liên quan |
|---|---|---|---|
| `NRF_RRAM_WRITE_BUFFER_SIZE` | 1 | Như xe, phòng khi lưu settings lúc đang nghe CIS / BIG | B-16 |
| `BT_ISO_RX_MTU` | 251 (mặc định, không đặt trong `prj.conf`) | Nhận SDU stereo 240 B. Nếu sau này giảm thì BIS stereo của xe sẽ hỏng | B-26 |
| `BT_ISO_RX_BUF_COUNT` | 4 | 2 BIS + CIS cùng lúc | — |
| `BT_CTLR_SYNC_ISO_STREAM_MAX` / `COUNT` | 2 / 2 | Tối đa 2 BIS | — |

### Hằng số trong mã

| Ở đâu | Hằng số | Giá trị | Lỗi liên quan |
|---|---|---|---|
| `bcast_source.c` | `ISOINT_DEFAULT`, `RTN_DEFAULT`, `RTN_MAX_BN2` | 20 ms, 0, 1 | B-26 |
| `bcast_source.c` | `BCAST_WQ_STACK` | 4096 | B-27 |
| `phone_sink.c` | QoS preference | unframed, 2M, RTN 2, **20 ms**, PD 20–40 ms | B-26 |
| `main.c` (xe) | `PHONE_CONN_INT`, `PHONE_SUP_TIMEOUT`, `PHONE_PARAM_TRIES` | 27 (33,75 ms), 600 (6 s), 5 | B-23, B-28 |
| `hub_link.c` (mũ) | Chu kỳ kết nối tới xe | 11,25 ms | B-14 |
| `audio_out.c` (mũ) | `I2S_TARGET_BLOCKS` | 4 | B-03 |

---

## 15. Tham chiếu trong SDK

| Nội dung | File (NCS v3.4.0) |
|---|---|
| Bảng ưu tiên lập lịch SDC, quy tắc timing-event | `nrfxlib/softdevice_controller/doc/scheduling.rst` |
| Lệnh vendor của SDC (BIG / CIG reserved time, role priority) | `nrfxlib/softdevice_controller/include/sdc_hci_vs.h` |
| LE Create BIG Test trong host | `zephyr/subsys/bluetooth/host/iso.c` (`hci_le_create_big_test`, `is_advanced_big_param`) |
| Tham số test của BAP broadcast | `zephyr/include/zephyr/bluetooth/audio/bap.h` (`irc`, `pto`, `iso_interval`, `max_pdu`, `burst_number`, `num_subevents`) |
| Dừng / xoá broadcast source | `zephyr/subsys/bluetooth/audio/bap_broadcast_source.c` (`bt_bap_broadcast_source_stop/delete`, `big_stopped_cb`) |
| Bộ đệm ISO của SDC | `nrf/subsys/bluetooth/controller/Kconfig` (`BT_CTLR_SDC_ISO_*`) |
| Bộ đệm MDCT của LC3 (nguyên nhân tràn stack) | `modules/lib/liblc3/src/mdct.c` |
