# Helmet: hướng dẫn sử dụng và kiểm tra chức năng

| | |
|---|---|
| Firmware | `apps/main` (bản ngày 15/10/2026: đã sửa lỗi treo quảng bá, log tắt máy, bộ đệm ISO, chính sách mã hóa) |
| Board | Helmet board, module Minew ME54BS62 (nRF54L15), target `me54bs62/nrf54l15/cpuapp` |
| Tài liệu liên quan | Thiết kế: [HELMET_THIET_KE_CHUC_NANG_MOI.md](HELMET_THIET_KE_CHUC_NANG_MOI.md) · Giải thích mã cũ: [HELMET_GIAI_THICH_MA_NGUON.md](HELMET_GIAI_THICH_MA_NGUON.md) · Sơ đồ chân: [PINOUT.md](PINOUT.md) |

Mọi con số trong tài liệu (thời gian nhấn nút, âm báo, hẹn giờ) đều lấy trực tiếp từ mã nguồn `apps/main/src`. Đổi mã thì phải cập nhật lại tài liệu này.

Tài liệu gồm hai phần:
- **Phần A: Sử dụng** (mục 1–9). Mỗi nút làm gì, nhấn bao lâu, nghe âm báo nào, thiết bị phản ứng ra sao.
- **Phần B: Kiểm tra** (mục 10–14). Quy trình thử từng chức năng theo mức phần cứng đang có, kèm lệnh, kết quả mong đợi và tiêu chí đạt.

---

## Mục lục

**Phần A: Sử dụng**
1. [Tổng quan và hiện trạng kiểm chứng](#1-tổng-quan-và-hiện-trạng-kiểm-chứng)
2. [Nút bấm: nhận dạng cử chỉ và thời gian](#2-nút-bấm-nhận-dạng-cử-chỉ-và-thời-gian)
3. [Bảng hành động theo trạng thái](#3-bảng-hành-động-theo-trạng-thái)
4. [Âm báo](#4-âm-báo)
5. [Bật, tắt, tự tắt, pin](#5-bật-tắt-tự-tắt-pin)
6. [Ghép đôi và kết nối điện thoại](#6-ghép-đôi-và-kết-nối-điện-thoại)
7. [Nghe nhạc](#7-nghe-nhạc)
8. [Gọi điện](#8-gọi-điện)
9. [Đo nhiệt độ](#9-đo-nhiệt-độ)

**Phần B: Kiểm tra**

10. [Chuẩn bị: nạp firmware, mở log và shell](#10-chuẩn-bị-nạp-firmware-mở-log-và-shell)
11. [Đọc log và heartbeat](#11-đọc-log-và-heartbeat)
12. [Lệnh shell đầy đủ](#12-lệnh-shell-đầy-đủ)
13. [Quy trình kiểm tra theo mức phần cứng](#13-quy-trình-kiểm-tra-theo-mức-phần-cứng)
14. [Sự cố thường gặp](#14-sự-cố-thường-gặp)
15. [Giới hạn hiện tại](#15-giới-hạn-hiện-tại)

---

# PHẦN A: SỬ DỤNG

## 1. Tổng quan và hiện trạng kiểm chứng

Helmet là tai nghe **LE Audio** gắn trên mũ bảo hiểm: hai loa (trái/phải), hai micro, bốn nút, cảm biến nhiệt hồng ngoại. Chip nRF54L15 chỉ có Bluetooth LE nên **chỉ dùng được với điện thoại hỗ trợ LE Audio** (xem mục 6.1).

| Chức năng | Phần cứng cần có | Hiện trạng kiểm chứng (15/10/2026) |
|---|---|---|
| Khởi động, quảng bá "Helmet", dịch vụ GATT | Mạch chính | ✅ Đã thử trên mạch |
| Quảng bá nhanh 30 s rồi chậm, tự quảng bá lại khi mất kết nối | Mạch chính | ✅ Đã thử trên mạch |
| Ngắt máy không ghép đôi được (chính sách mã hóa) | Mạch chính | ✅ Nhánh "từ chối ghép đôi" đã thử; ⏳ nhánh "im lặng 30 s" chưa tạo lại được |
| Tắt máy (System OFF), tự tắt khi rảnh, lưu cài đặt | Mạch chính | ✅ Đã thử trên mạch |
| Bật máy bằng nút Power | Board nút | ⏳ Chưa thử: chưa nối board nút |
| Nhận dạng cử chỉ nút | Board nút | ✅ 12/12 kịch bản thử trên PC; ✅ hành động thử qua lệnh giả lập; ⏳ chưa thử với nút thật |
| Âm báo, đường ra I2S, lấy mẫu lại theo tần số I2S thật | Mạch chính (loa để nghe) | ✅ I2S đo được 15 873.6 Hz, đúng tính toán; ⏳ chưa nghe bằng loa |
| Nghe nhạc LE Audio | Loa + nguồn LE Audio | ✅ Phần nhận đã chạy ở `apps/Helmet` (cùng mã); ⏳ `apps/main` chưa thử với luồng thật |
| Gọi điện: micro, khử vọng, giảm ồn | Micro + loa + điện thoại có GTBS | ✅ Thuật toán thử trên PC; ⏳ chưa thử trên mạch |
| Điều khiển cuộc gọi bằng nút | Board nút + điện thoại có GTBS | ⏳ Chưa thử |
| Đo nhiệt độ MLX90632 | Cảm biến + điện trở kéo lên | ✅ Công thức khớp 17/17 bộ số chuẩn Melexis; ⏳ chưa gắn cảm biến |
| Cảnh báo pin yếu (đo VDD) | Mạch chính | ✅ Đọc VDD 3 276 mV; ⏳ chưa thử với pin yếu thật |

**Không có đèn báo.** LED Bluetooth trên mạch đang hở (ISS-01), nên **âm báo là phản hồi duy nhất** cho người dùng.

### 1.1 Trạng thái của thiết bị

Thiết bị luôn ở **đúng một** trong các trạng thái dưới đây. Hành động của nút phụ thuộc vào trạng thái (mục 3).

| Trạng thái | Ý nghĩa | Vào khi | Ra khi |
|---|---|---|---|
| `OFF` | Tắt (System OFF), chip gần như không tiêu thụ | Giữ Power 3 s, tự tắt, lệnh `helmet power off` | Giữ Power 2 s (hoặc reset, lắp pin) |
| `BOOT` | Đang khởi động (khoảng 30 ms) | Bật máy | Khởi tạo xong → `IDLE` |
| `IDLE` | Bật, đang quảng bá, chưa có điện thoại | Khởi động xong, mất kết nối | Điện thoại kết nối → `CONNECTED` |
| `CONNECTED` | Đã kết nối (đang nghe nhạc hoặc rảnh) | Điện thoại kết nối | Mất kết nối → `IDLE`; có cuộc gọi đến → `RINGING`; gọi đi → `IN_CALL` |
| `RINGING` | Có cuộc gọi đến | Điện thoại báo cuộc gọi đến | Nghe máy → `IN_CALL`; từ chối / người gọi bỏ → `CONNECTED` |
| `IN_CALL` | Đang gọi (cả lúc đang quay số đi, đang giữ máy) | Nghe máy, gọi đi | Hết cuộc gọi → `CONNECTED` |
| `SHUTDOWN` | Đang chạy quy trình tắt (dưới 1 s, hoặc tới lúc nhả nút Power) | Lệnh tắt | → `OFF` |

Xem trạng thái hiện tại: lệnh `helmet status` (dòng `State`) hoặc dòng `heartbeat` mỗi 10 s.

---

## 2. Nút bấm: nhận dạng cử chỉ và thời gian

### 2.1 Bốn nút

| Nút | Chân MCU | Mã sự kiện |
|---|---|---|
| **Vol+** | P0.02 (pad E1) | `INPUT_KEY_VOLUMEUP` |
| **Vol−** | P0.01 (pad D1) | `INPUT_KEY_VOLUMEDOWN` |
| **Call** | P0.00 (pad C1) | `INPUT_KEY_PLAYPAUSE` |
| **Power** | P0.03 (pad F1) | `INPUT_KEY_POWER` |

- Nút nối xuống GND khi nhấn (active low), có điện trở kéo lên.
- Driver chống dội **30 ms**: nhấp nhả nhanh hơn 30 ms không được tính.

⚠ **Thứ tự connector khác nhau giữa mạch chính và board nút** (ISS-07):
- Mạch chính: B1..B4 = E1, D1, C1, F1.
- Board nút: B1..B4 = C1, D1, E1, F1.

Nút in chữ "Power" trên vỏ chưa chắc nối vào P0.03. Khi lắp board nút lần đầu, làm bài thử 13.2.1 để biết nút vật lý nào là nút nào. **Chỉ nút nào nối vào P0.03 mới bật được máy từ OFF.**

### 2.2 Các cử chỉ và mốc thời gian

Thời gian tính **từ lúc nhấn xuống**.

| Cử chỉ | Cách làm | Thời điểm thiết bị nhận | Áp dụng cho |
|---|---|---|---|
| **Nhấn ngắn** | Nhấn rồi nhả **trước 600 ms** | Ngay lúc nhả. Riêng nút đang bật "chờ nhấn đúp" (xem dưới) thì **350 ms sau khi nhả** | Cả 4 nút |
| **Nhấn đúp** | Nhấn ngắn (nhả trước 600 ms), rồi **nhấn lần hai trong vòng 350 ms** sau lần nhả đầu | Lúc nhả lần hai, giữ lần hai bao lâu cũng được | Chỉ Call, và chỉ ở `CONNECTED`, `IN_CALL` |
| **Giữ** | Giữ **từ 1 s** | **Đúng mốc 1.0 s, khi vẫn đang giữ** (không chờ nhả) | Call, Power |
| **Giữ lâu** | Giữ **từ 3 s** | **Đúng mốc 3.0 s, khi vẫn đang giữ**. Đã có "Giữ" ở mốc 1 s trước đó | Call, Power |
| **Giữ lặp** | Giữ Vol+ hoặc Vol− | Lần 1 ở mốc **500 ms**, sau đó cứ **250 ms** một lần cho tới khi nhả | Vol+, Vol− |
| **Tổ hợp** | Nhấn **Vol+ và Vol− gần như cùng lúc** (lần nhấn thứ hai **trong vòng 200 ms** sau lần đầu), giữ cả hai | **5.0 s tính từ lần nhấn thứ hai**, khi vẫn đang giữ | Vol+ cùng Vol− |

**Các quy tắc phụ:**
- **Vùng bỏ qua 600–1 000 ms** (Call, Power): nhả trong khoảng này thì **không có cử chỉ nào**. Vùng này tránh nhầm giữa nhấn ngắn và giữ.
- **"Chờ nhấn đúp"** chỉ bật cho nút Call ở `CONNECTED` và `IN_CALL`. Ở hai trạng thái này, nhấn ngắn Call có độ trễ 350 ms. Ở `RINGING` và `IDLE` thì nhấn ngắn Call có tác dụng ngay khi nhả, để nghe máy không bị chậm.
- **Vol+/Vol− không có "Giữ" và "Giữ lâu"**: giữ là lặp. Giữ quá 500 ms thì lúc nhả không tính thêm nhấn ngắn.
- **Tổ hợp:** khi đã vào chế độ tổ hợp, Vol+/Vol− **không** tăng/giảm âm lượng. Nhả một nút trước 5 s thì hủy tổ hợp và không làm gì; nút còn lại bị bỏ qua cho tới khi nhả. Hai nút nhấn cách nhau hơn 200 ms thì là hai lần nhấn riêng, không phải tổ hợp.
- **Ngay sau khi bật máy bằng Power**, nút Power bị bỏ qua cho tới lần nhả đầu tiên. Nhờ vậy giữ tiếp sau khi máy đã bật cũng không tắt máy.
- Ở `BOOT` và `SHUTDOWN`, mọi cử chỉ bị bỏ qua.
- Mỗi cử chỉ ở `IDLE` **đặt lại hẹn giờ tự tắt** (mục 5.3).

### 2.3 Ví dụ cụ thể theo thời gian

| Thao tác | Kết quả |
|---|---|
| Vol+: nhấn 120 ms rồi nhả | 1 lần tăng âm lượng, ngay lúc nhả |
| Vol−: giữ 1.6 s | Giảm 5 lần, ở các mốc 500, 750, 1 000, 1 250, 1 500 ms |
| Vol+: giữ 3 s | Tăng 11 lần (mốc 500 ms + 10 lần mỗi 250 ms) |
| Vol+: giữ khoảng 4.3 s từ âm lượng 0 | Lên mức tối đa 255 (16 bước) |
| Call ở `CONNECTED`: nhấn 200 ms rồi nhả | Nhấn ngắn được tính ở mốc 550 ms (200 + 350 ms chờ đúp) |
| Call ở `CONNECTED`: nhấn 150 ms, nhả, nhấn lại ở mốc 400 ms, nhả ở 500 ms | Nhấn đúp, tính ở mốc 500 ms |
| Call: giữ 1.5 s | "Giữ" ở mốc 1 000 ms |
| Call: nhấn 800 ms rồi nhả | Không có gì (vùng bỏ qua) |
| Power: giữ 3.5 s | "Giữ" ở 1 000 ms (không có hành động), "Giữ lâu" ở 3 000 ms (tắt máy) |
| Vol+ nhấn ở 0 ms, Vol− nhấn ở 100 ms, giữ cả hai | Tổ hợp ở mốc 5 100 ms |
| Vol+ nhấn ở 0 ms, Vol− nhấn ở 300 ms, nhả nhanh cả hai | Hai lần nhấn ngắn riêng, không phải tổ hợp |

Tất cả ví dụ trên đều nằm trong 12 kịch bản đã chạy trên PC với đúng mã `buttons.c` hiện tại.

---

## 3. Bảng hành động theo trạng thái

"—" nghĩa là không làm gì.

| Nút / cử chỉ | `IDLE` (chưa kết nối) | `CONNECTED` (nhạc / rảnh) | `RINGING` (cuộc gọi đến) | `IN_CALL` (đang gọi) |
|---|---|---|---|---|
| **Call** nhấn ngắn | Âm "chưa kết nối" (2 tiếng bíp) | **Phát / tạm dừng** nhạc trên điện thoại | **Nghe máy** | **Kết thúc cuộc gọi** (cả khi đang quay số đi) |
| **Call** nhấn đúp | — (không chờ đúp) | **Bài kế tiếp** | — (không chờ đúp) | **Tắt / bật micro** |
| **Call** giữ 1 s | — | — (dự phòng) | **Từ chối** cuộc gọi | — |
| **Vol+** ngắn / giữ lặp | Tăng âm lượng | Tăng âm lượng | Tăng âm lượng (chuông to hơn) | Tăng âm lượng cuộc gọi |
| **Vol−** ngắn / giữ lặp | Giảm âm lượng | Giảm âm lượng | Giảm âm lượng | Giảm âm lượng cuộc gọi |
| **Power** nhấn ngắn | Âm "chưa kết nối" (2 bíp) | Âm "đã kết nối" (1 bíp) | 1 bíp ⚠ (xem mục 15) | 1 bíp |
| **Power** giữ 3 s | **Tắt máy** | **Tắt máy** | **Từ chối rồi tắt** | **Kết thúc cuộc gọi rồi tắt** |
| **Vol+ + Vol−** giữ 5 s | **Xóa mọi ghép đôi** | **Ngắt kết nối + xóa mọi ghép đôi** | — | — |

**Chi tiết âm lượng:**
- Thang 0–255, mặc định **128** ở lần bật đầu tiên. Mỗi bước **16**, nên từ nhỏ nhất tới lớn nhất là 16 bước.
- Âm lượng trên mũ và trên điện thoại luôn đồng bộ: nút trên mũ đổi thì thanh âm lượng trên điện thoại đổi theo, và ngược lại.
- **Vol+ luôn bỏ tắt tiếng.** Nếu điện thoại đang tắt tiếng mũ, nhấn Vol+ là có tiếng lại.
- Đang ở 255 mà nhấn Vol+ (không tắt tiếng), hoặc đang ở 0 mà nhấn Vol−, thì nghe tiếng "tíc" báo đã chạm giới hạn.
- Âm lượng **được lưu**: 10 s sau lần đổi cuối, và lúc tắt máy. Lần bật sau dùng lại mức đã lưu.
- Âm lượng đổi được cả khi chưa kết nối.

**Chi tiết "Xóa mọi ghép đôi":**
- Xóa toàn bộ khóa ghép đôi (tối đa 4 thiết bị) trong bộ nhớ.
- Sau đó phải **xóa cả "Helmet" trong danh sách Bluetooth của điện thoại**, rồi ghép đôi lại từ đầu (mục 6.2).
- Nghe âm "ghép đôi": 4 tiếng bíp cao, nhanh.

---

## 4. Âm báo

### 4.1 Danh sách

| Tên (dùng với `helmet prompt`) | Nghe như thế nào | Thời lượng | Khi nào |
|---|---|---|---|
| `power_on` | 3 nốt **đi lên**: 880 → 1 175 → 1 568 Hz | 320 ms | Bật máy xong |
| `power_off` | 3 nốt **đi xuống**: 1 568 → 1 175 → 880 Hz | 360 ms | Bắt đầu tắt máy |
| `connected` | 2 bíp 1 319 Hz | 180 ms | Điện thoại kết nối **và mã hóa xong** |
| `disconnected` | 1 tiếng trầm 659 Hz | 200 ms | Mất kết nối |
| `ring` | 2 hồi 1 000 Hz (mỗi hồi 400 ms, cách 200 ms), rồi nghỉ 2 s; **lặp** | chu kỳ 3 s | Cuộc gọi đến mà điện thoại không tự phát chuông (mục 8.2) |
| `mic_muted` | Cao xuống thấp: 1 047 → 523 Hz | 210 ms | Tắt micro khi đang gọi |
| `mic_unmuted` | Thấp lên cao: 523 → 1 047 Hz | 210 ms | Bật micro khi đang gọi |
| `vol_limit` | 1 tiếng "tíc" rất ngắn 2 093 Hz | 30 ms | Âm lượng chạm 0 hoặc 255; lệnh nghe/từ chối/kết thúc gọi bị điện thoại báo lỗi |
| `pairing` | 4 bíp nhanh 1 568 Hz | 350 ms | Đã xóa mọi ghép đôi |
| `temp_alarm` | 3 bíp rất cao 2 637 Hz | 650 ms | Nhiệt độ vượt ngưỡng (đang gọi thì để dành, phát khi kết thúc cuộc gọi) |
| `low_battery` | 2 tiếng trầm 440 Hz | 400 ms | Pin sắp hết (mục 5.4) |
| `status_ok` | 1 bíp 1 319 Hz | 80 ms | Power nhấn ngắn khi **đã kết nối** |
| `status_no_link` | 2 bíp 1 319 Hz | 240 ms | Power hoặc Call nhấn ngắn khi **chưa kết nối** |

### 4.2 Âm lượng âm báo và cách trộn

- Âm báo phát ở mức cố định **−18 dBFS**, không phụ thuộc âm lượng, để luôn nghe được.
- **Riêng chuông** đi theo âm lượng hiện tại, nhưng không nhỏ hơn −30 dBFS.
- Đang nghe nhạc mà có âm báo thì **nhạc nhỏ đi 10 dB** trong lúc âm báo phát, xong thì trở lại.
- Không có luồng nhạc nào thì thiết bị tự bật I2S ở 16 kHz để phát âm báo, phát xong khoảng 30 ms thì tắt.
- ⚠ Mỗi lúc chỉ có **một** âm báo: âm báo mới **thay thế** âm báo đang phát (xem mục 15 về chuông).

---

## 5. Bật, tắt, tự tắt, pin

### 5.1 Bật máy

Mạch không có công tắc nguồn: LDO luôn cấp 3.3 V khi có pin. "Tắt" là chế độ **System OFF** của chip.

| Cách | Thao tác | Kết quả |
|---|---|---|
| **Nút Power** (cách bình thường) | Khi đang OFF: nhấn và **giữ Power ít nhất 2 s** | Nghe `power_on` (3 nốt đi lên), quảng bá "Helmet", điện thoại đã ghép đôi tự kết nối lại. Có thể nhả ngay sau khi nghe âm báo; giữ tiếp cũng không sao |
| Nhấn Power dưới 2 s | | **Máy tắt lại ngay, không có âm báo**: chống bật nhầm do va chạm trong túi |
| Lắp pin / cấp nguồn lần đầu | | **Bật luôn**, không cần nút |
| Reset (nút RESET trên mạch, `make reset`, J-Link) | | Bật luôn |

- Chỉ nút trên **P0.03** đánh thức được chip. Cắm sạc USB **không** đánh thức máy: mạch sạc không nối về MCU.
- Mốc 2 s tính từ lúc chip thức dậy, tức gần như từ lúc nhấn.
- Khi chưa có board nút mà máy đang OFF: dùng `make APP=apps/main reset` (qua J-Link) hoặc tháo và lắp lại pin.

### 5.2 Tắt máy

| Cách | Thao tác |
|---|---|
| **Nút Power** | **Giữ Power 3 s** (ở mọi trạng thái trừ `BOOT`/`SHUTDOWN`) |
| Lệnh shell | `helmet power off` |
| Tự tắt | Mục 5.3 |
| Pin quá yếu | Hiện **chưa bật** (mục 5.4) |

**Diễn biến khi giữ Power 3 s:**
1. Tới mốc 3.0 s (vẫn đang giữ), quy trình tắt bắt đầu.
2. Đang có cuộc gọi đến thì **từ chối**; đang gọi thì **kết thúc**; chờ thêm 300 ms.
3. Phát `power_off` (3 nốt đi xuống) và chờ phát xong (tối đa 0.8 s).
4. Ngắt kết nối điện thoại với lý do "thiết bị tắt nguồn" (0x15), để điện thoại không cố kết nối lại ngay. Chờ tối đa 1 s. Dừng quảng bá.
5. Dừng micro, I2S và cảm biến nhiệt; **lưu cài đặt** (âm lượng, thời gian tự tắt, độ phát xạ, ngưỡng nhiệt).
6. **Chờ bạn nhả nút Power** (tối đa 10 s): nếu còn giữ thì chip sẽ thức dậy ngay sau khi tắt.
7. Chip vào System OFF.

Cả quy trình mất khoảng 0.5 s nếu không có cuộc gọi (đo được 443 ms). Sau đó thêm thời gian chờ nhả nút.

### 5.3 Tự tắt và quảng bá

| Hẹn giờ | Chạy khi | Hết giờ thì |
|---|---|---|
| **Quảng bá nhanh 30 s** | Mỗi lần vào `IDLE` (khởi động, mất kết nối) | Chuyển quảng bá từ **20–30 ms** sang **150 ms** để tiết kiệm pin. Điện thoại vẫn tìm thấy, chỉ chậm hơn một chút |
| **Tự tắt**, mặc định **10 phút** | Chỉ ở `IDLE`; mỗi cử chỉ nút đặt lại từ đầu | Tắt máy như mục 5.2 (lý do `idle timeout`) |

- Đã kết nối thì **không tự tắt**, kể cả khi không phát gì. Ngắt kết nối thì hẹn giờ đếm lại từ đầu.
- Đổi thời gian: `helmet autooff <phút>`, từ 0 đến 240; **0 = không bao giờ tự tắt**. Giá trị được lưu.
- 💡 Khi thử trên bàn mà chưa có board nút, nên đặt `helmet autooff 0`. Nếu không, sau 10 phút máy tự tắt và phải `make reset` mới bật lại được.

### 5.4 Pin

- MCU **không đo được % pin**: chân pin không nối về MCU. Mạch sạc chỉ báo bằng LED trên mạch nguồn.
- Thiết bị đo **điện áp nguồn của chip (VDD)**: lần đầu 20 s sau khi bật, sau đó mỗi 60 s.
  - Pin còn khỏe: LDO giữ VDD khoảng 3.3 V (đo được 3 276 mV).
  - Pin yếu: LDO không giữ được nữa và VDD tụt theo pin.
- **Cảnh báo pin yếu:** VDD < **3 200 mV** ở **3 lần đo liên tiếp** thì phát `low_battery`. Sau đó cứ khoảng 6 phút phát lại, trừ khi đang gọi hoặc đổ chuông.
- **Tự tắt khi VDD < 3 000 mV đang tắt** (`LOWBATT_POWER_OFF 0` trong `power.c`), cho tới khi kiểm chứng ngưỡng với pin thật.
- Xem VDD lúc nào cũng được: `helmet vdd`.
- Khi "tắt", pin vẫn tốn khoảng 60 µA (ước lượng, chủ yếu do LDO). Pin 500 mAh để "tắt" được khoảng 1 năm. Con số thật phải đo (mục 13.6).

---

## 6. Ghép đôi và kết nối điện thoại

### 6.1 Điện thoại dùng được

- Phải hỗ trợ **Bluetooth LE Audio** (Unicast). Thường là Android 13 trở lên đời mới; trên một số máy phải bật *LE Audio* trong cài đặt Bluetooth hoặc trong Tùy chọn nhà phát triển.
- iPhone và điện thoại chỉ có Bluetooth Classic **không** phát nhạc hay gọi qua Helmet được.
- Muốn **điều khiển cuộc gọi bằng nút**, điện thoại phải có dịch vụ cuộc gọi chung (GTBS). Muốn **play/pause, bài kế tiếp**, điện thoại phải có dịch vụ media (MCS). Thiếu thì các nút đó không có tác dụng (log ghi "not found"), còn âm thanh vẫn chạy.

### 6.2 Ghép đôi lần đầu

1. Bật Helmet. Trong 30 s đầu máy quảng bá nhanh nên điện thoại tìm thấy rất nhanh; sau đó vẫn tìm thấy, chỉ chậm hơn.
2. Trên điện thoại: *Bluetooth → Ghép đôi thiết bị mới → **Helmet*** (biểu tượng tai nghe).
3. Ghép đôi kiểu *Just Works*: không có mã số. Android có thể hỏi "Ghép đôi với Helmet?": bấm **Ghép đôi** **trong vòng 30 s**, nếu không Helmet sẽ ngắt (mục 6.4).
4. Ghép đôi và mã hóa xong thì nghe `connected` (2 bíp).

### 6.3 Những lần sau

- Bật Helmet là điện thoại đã ghép đôi **tự kết nối lại**, không cần thao tác. Nghe `connected` khi xong.
- Helmet nhớ tối đa **4 điện thoại**. Ghép đôi máy thứ 5 thì máy cũ nhất bị quên.
- **Mỗi lúc chỉ kết nối 1 điện thoại.** Đang có kết nối thì Helmet ngừng quảng bá, máy khác không thấy nó. Muốn chuyển sang máy khác: ngắt Bluetooth trên máy đang kết nối.
- Helmet dùng địa chỉ Bluetooth ngẫu nhiên, đổi định kỳ (*privacy*). Chỉ máy đã ghép đôi mới nhận ra; ứng dụng quét BLE sẽ thấy địa chỉ thay đổi, đó là bình thường.

### 6.4 Chính sách bảo mật: khi nào Helmet tự ngắt

Mọi chức năng (nhạc, gọi, âm lượng, micro, nhiệt độ) đều **bắt buộc liên kết mã hóa**. Vì chỉ có 1 ô kết nối, Helmet không giữ những kết nối không dùng được:

| Tình huống | Helmet làm gì | Thời gian |
|---|---|---|
| Máy kia **từ chối ghép đôi**, hoặc mã hóa bằng **khóa cũ** mà Helmet đã xóa | Ngắt ngay (lý do 0x05), quảng bá nhanh lại | Đo được 325 ms sau khi kết nối |
| Máy kia kết nối rồi **im lặng**: không ghép đôi, không từ chối | Ngắt sau **30 s** nếu vẫn chưa mã hóa | 30 s |

Ví dụ thường gặp: bạn đã xóa ghép đôi trên mũ (Vol+ + Vol− 5 s) nhưng **chưa xóa "Helmet" trên điện thoại**. Điện thoại sẽ cố kết nối bằng khóa cũ và bị ngắt liên tục. Cách xử lý: xóa "Helmet" trên điện thoại rồi ghép đôi lại.

### 6.5 Kết nối và mất kết nối

| Sự kiện | Âm báo | Thiết bị |
|---|---|---|
| Điện thoại kết nối và mã hóa xong | `connected` | `CONNECTED`, ngừng quảng bá, tìm dịch vụ media rồi dịch vụ cuộc gọi trên điện thoại |
| Mất kết nối (ra xa, tắt Bluetooth) | `disconnected` | `IDLE`, quảng bá nhanh 30 s rồi chậm; hẹn giờ tự tắt đếm lại; micro tự bật lại nếu đang tắt |

---

## 7. Nghe nhạc

- Mở nhạc trên điện thoại khi đã kết nối. Điện thoại tự mở luồng LE Audio tới Helmet.
- **Định dạng nhận được:** LC3 16 / 24 / 32 / 48 kHz, khung 7.5 hoặc 10 ms, 1 hoặc 2 kênh, 26–155 byte mỗi khung. Nhạc thường là 48 kHz.
  - Điện thoại gửi **stereo**: kênh trái ra loa trái, kênh phải ra loa phải.
  - Gửi **mono**, hoặc chỉ một bên: cùng âm thanh ra cả hai loa.
  - Gửi 2 luồng riêng trái/phải: Helmet ghép lại theo thời gian.
- **Điều khiển:** Call ngắn = phát/tạm dừng; Call đúp = bài kế tiếp; Vol+/Vol− = âm lượng. Bấm phát/tạm dừng trên điện thoại thì Helmet cũng biết, nên lần nhấn Call sau vẫn đúng ý.
- **Chất lượng:**
  - Gói mất trên sóng được nội suy (PLC) nên không nghe tiếng tách.
  - Lệch clock giữa điện thoại và mũ được bù **từng mẫu**, không nghe thấy.
  - Tần số I2S thật của chip lệch 0.8 % so với danh định (48 kHz → 47 619 Hz). Phần này đã được lấy mẫu lại chính xác nên không còn tiếng tách định kỳ như bản `apps/Helmet` cũ.
- **Độ trễ trong mũ:** khoảng 55 ms (bộ đệm 1.5 khung + 4 khối I2S; từ 19/10/2026 giữ 4 khối thay vì 3 để không bị tách khi gói đến dồn cục).
- **Hai mũ nghe chung một điện thoại (Auracast):**
  - Bật "Chia sẻ âm thanh" trên điện thoại (nếu máy hỗ trợ), hoặc trên mũ nhấn **Power ngắn → giữ Call 2 s** để tự tìm broadcast.
  - Chi tiết, âm báo và cách kiểm thử: [HELMET_AURACAST.md](HELMET_AURACAST.md).
  - Cuộc gọi (unicast) luôn được ưu tiên hơn broadcast.

---

## 8. Gọi điện

### 8.1 Gọi đi

Quay số trên điện thoại. Helmet chuyển sang `IN_CALL` ngay khi điện thoại bắt đầu quay số. Nhấn Call ngắn để **hủy** cuộc đang quay số.

### 8.2 Cuộc gọi đến

1. Điện thoại báo có cuộc gọi đến. Helmet chuyển sang `RINGING`.
   - Điện thoại tự phát chuông qua LE Audio thì nghe chuông của điện thoại.
   - Sau **1 s** mà điện thoại không phát chuông, Helmet tự phát `ring` (lặp chu kỳ 3 s).
2. **Nghe máy:** nhấn **Call ngắn** (có tác dụng ngay khi nhả).
3. **Từ chối:** **giữ Call 1 s** (có tác dụng ở mốc 1 s, khi vẫn giữ).
4. Người gọi bỏ máy trước: chuông dừng, về `CONNECTED`.
5. Lệnh bị điện thoại báo lỗi: nghe tiếng "tíc" (`vol_limit`).

### 8.3 Trong cuộc gọi

| Muốn | Thao tác | Ghi chú |
|---|---|---|
| Kết thúc | **Call ngắn** | Tác dụng 350 ms sau khi nhả (chờ xem có nhấn đúp không) |
| Tắt / bật micro | **Call đúp** | Nghe `mic_muted` / `mic_unmuted`. Biểu tượng micro trên điện thoại đổi theo |
| Chỉnh âm lượng | Vol+ / Vol− | |
| Tắt micro từ điện thoại | Bấm nút micro trên màn hình cuộc gọi | Helmet tắt micro và phát `mic_muted` |

- Kết thúc cuộc gọi thì micro **tự bật lại** cho cuộc sau.
- **Xử lý giọng nói:** lọc thông cao 100 Hz, khử vọng, giảm ồn (FFT thực CMSIS-DSP), tự động chỉnh mức về −20 dBFS. Giọng được xử lý và gửi ở 16 kHz: Helmet chỉ khai báo 16 kHz cho chiều gửi, vì 32 kHz làm CPU cuộc gọi lên khoảng 98 % (đo ngày 16/10/2026). Nếu điện thoại vẫn chọn 32 kHz (do còn nhớ bản PAC cũ), Helmet vẫn nhận và nâng tần số trước khi gửi, kèm cảnh báo `Phone chose 32 kHz uplink (stale PAC?)`. Khi thấy cảnh báo này thì ghép lại điện thoại một lần.
- **Micro lỗi:** nếu driver micro (PDM) không khởi động được hoặc đọc lỗi, cuộc gọi **không rớt**: Helmet gửi im lặng và báo cho điện thoại là micro không dùng được (log `Microphone not available - sending silence`).
- ⚠ **Board micro chưa nối:** Helmet **không tự phát hiện được**. Bộ PDM vẫn chạy và đọc đường DATA đang thả nổi, nên người bên kia nghe im lặng hoặc tiếng ồn lạ. Kiểm tra micro trước bằng `hw_test` (mục 13.4).

---

## 9. Đo nhiệt độ

Chỉ hoạt động khi đã gắn **MLX90632**. Cần thêm điện trở kéo lên 4.7 kΩ cho SDA (P1.13) và SCL (P0.04) (ISS-03). Chưa có cảm biến thì máy vẫn chạy bình thường và thử dò lại mỗi 30 s.

| Thông số | Giá trị |
|---|---|
| Chu kỳ đo | Khoảng **2 s**: mỗi lần đo mất khoảng 1 s, sau đó cảm biến ngủ |
| Lọc | Trung vị 5 lần đo gần nhất, rồi trung bình trượt (α = 0.3). Phản ứng sau khoảng 10 s |
| Giá trị hợp lệ | Môi trường −20…85 °C, vật thể −20…100 °C; ngoài khoảng bị loại |
| Độ phát xạ | Mặc định **0.98** (da người); đổi bằng `helmet temp emis <‰>` |
| **Cảnh báo** | Vật thể ≥ **38.0 °C** ở **3 lần liên tiếp** (khoảng 6 s): phát `temp_alarm` |
| Hết cảnh báo | Xuống dưới ngưỡng − 0.5 °C ở 3 lần liên tiếp |
| Đổi ngưỡng | `helmet temp alarm <°C>` (20–60), được lưu |

**Xem trên điện thoại** (Health Thermometer Service):
- Dùng ứng dụng **nRF Connect** (hoặc ứng dụng sức khỏe hỗ trợ HTS) trên **điện thoại đã ghép đôi**. Dịch vụ này bắt buộc mã hóa.
- Mở dịch vụ *Health Thermometer* → *Temperature Measurement* → bật **Indicate**.
- Nhận giá trị nhiệt độ vật thể **mỗi 10 s**, và **ngay lập tức** khi có cảnh báo. *Temperature Type* = Body (general).

Nhiệt độ bề mặt trán trong mũ khác thân nhiệt thật (mồ hôi, gió, tóc). Chỉ nên dùng để **theo dõi xu hướng**.

---

# PHẦN B: KIỂM TRA

## 10. Chuẩn bị: nạp firmware, mở log và shell

```bash
cd ~/Project/Bluetooth_ME54BS62
make APP=apps/main build          # thư mục build: build/main
make APP=apps/main flash          # nạp qua J-Link (SWD)
make APP=apps/main rtt            # log + shell qua RTT (Ctrl+C để thoát)
make APP=apps/main reset          # reset / đánh thức khi máy đang OFF
make APP=apps/main audio          # (thay cho rtt) nghe lại âm thanh Helmet nhận, lưu WAV
```

- `Makefile` mặc định là `apps/main` (từ 27/10/2026), nên `make build`, `make flash`, `make rtt` dùng luôn cho mũ; xe cần `APP=apps/bike`.
- Khung dây J-Link: xem [PINOUT.md](PINOUT.md). Mạch phải được cấp nguồn riêng (pin hoặc USB-C).
- Mỗi lúc chỉ mở **một** phiên RTT: `make rtt` và `make audio` không chạy cùng lúc.
- ⚠ Khi J-Link đang gắn, chip chỉ **giả lập** System OFF. Muốn đo dòng tắt máy thật phải rút J-Link.

---

## 11. Đọc log và heartbeat

### 11.1 Log khởi động bình thường

```
<inf> power: Boot: reset cause 0x00000002
<inf> helmet: === Helmet (apps/main): music + calls + gestures + power + temperature ===
<inf> audio_out: Audio out ready: LC3 -> clock slip -> prompts -> fixed resampler -> I2S20
<inf> media_ctrl: Volume restored: 128/255
<inf> le_audio: BAP unicast server: 2 sink ASE (LC3 16-48 kHz, 1-2 ch) + 1 source ASE (LC3 16 kHz mono)
<inf> helmet: State BOOT -> IDLE
<inf> helmet: Advertising as "Helmet" (fast 20-30 ms)
<inf> audio_out: I2S 16000 Hz -> actual 15873.015 Hz (MCK 32 MHz/63, ratio 32), resample 126/125
<inf> thermo: MLX90632 not present - retry every 30 s
```

Bình thường log khởi động có **0 dòng `<wrn>` / `<err>`**.

**Nguyên nhân reset** (bit trong `reset cause`):

| Bit | Ý nghĩa |
|---|---|
| `0x01` | Chân RESET |
| `0x02` | Phần mềm hoặc J-Link |
| `0x08` | Cấp nguồn lần đầu |
| `0x80` | Thức từ OFF bằng nút Power |
| `0x800` | Thức từ OFF do clock; thấy khi reset bằng J-Link lúc máy đang OFF |

### 11.2 Heartbeat (mỗi 10 s)

```
heartbeat: CONNECTED, STREAMING, SDU 5230 (bad 3), PLC 3, I2S on 48000->47619.047 Hz q3 ring 720, slip -12/+0, underrun 0, err 0
link: ISO rx ok 5227, CRC 0, lost 3, empty 0; call none
ISO LQ ASE 0 (goi vo tuyen): CRC error 1, unreceived 3, duplicate 0, tx flushed 0
voice: 16000 Hz, mic -38 dBFS, out -21 dBFS, ERLE 27 dB, sent 6000, underrun 0, slip -1/+0, unmuted
temp: object 34.62 C, ambient 29.10 C, age 1200 ms
```

| Trường | Ý nghĩa | Bình thường |
|---|---|---|
| Trạng thái (`IDLE`/`CONNECTED`/…) | Mục 1.1 | |
| `STREAMING` / `idle` | Có luồng nhạc/giọng tới hay không | |
| `SDU` / `bad` | Gói âm thanh nhận được / gói lỗi hoặc mất | `bad` < 1 % |
| `PLC` | Số khung phải nội suy | Tăng chậm |
| `I2S on 48000->47619.047 Hz` | Tần số danh định → tần số I2S thật | |
| `q3` | Số khối đang chờ phát | 2–3 |
| `ring` | Số mẫu chờ trong vòng đệm | Ổn định |
| `slip -x/+y` | Số **mẫu** bỏ/chèn để bù lệch clock điện thoại | Tăng chậm, vài mẫu mỗi giây |
| `underrun` | Khung phải phát im lặng vì hết dữ liệu | 0 khi sóng tốt |
| `err` | Lỗi I2S + lỗi giải mã + hàng đợi đầy | 0 |
| Dòng `link:` | Chỉ có khi đã kết nối: gói tốt / sai CRC / mất / rỗng; trạng thái cuộc gọi | |
| `ISO LQ` | Bộ đếm gói vô tuyến của controller | `unreceived` tăng theo mọi khung là phía phát không gửi |
| Dòng `voice:` | Chỉ có khi đang gọi: mức micro, mức gửi đi, độ giảm vọng (ERLE), SDU đã gửi | `underrun` = 0; ERLE ≥ 20 dB khi có tiếng vọng |
| Dòng `temp:` | Chỉ có khi có cảm biến | |

---

## 12. Lệnh shell đầy đủ

Gõ trong phiên `make APP=apps/main rtt`. Mọi lệnh bắt đầu bằng `helmet`.

| Lệnh | Tác dụng | Ví dụ / ghi chú |
|---|---|---|
| `helmet status` | Tổng quan: trạng thái, kết nối, âm lượng, cuộc gọi, các ASE, I2S, giọng nói, nguồn | |
| `helmet vol <0..255>` | Đặt âm lượng (điện thoại được báo lại) | `helmet vol 160` |
| `helmet play` / `pause` / `next` | Lệnh media tới điện thoại | Cần điện thoại có MCS |
| `helmet unpair` | Xóa mọi ghép đôi | Giống tổ hợp Vol+ + Vol− |
| `helmet btn <nút> <cử chỉ>` | **Giả lập cử chỉ** (không cần board nút) | Nút: `volup` `voldown` `call` `power`; cử chỉ: `short` `double` `long` `vlong` `repeat` `combo`. Ví dụ `helmet btn call short`. Lệnh này bỏ qua phần đo thời gian của nút thật, đưa cử chỉ thẳng vào bảng hành động (mục 3) |
| `helmet call` | Trạng thái cuộc gọi | |
| `helmet call accept` / `reject` / `end` | Nghe / từ chối / kết thúc | |
| `helmet mic` / `mic mute` / `mic unmute` | Trạng thái / tắt / bật micro (qua MICP, điện thoại được báo) | |
| `helmet voice` | Thống kê chiều gửi: mức micro, ERLE, SDU đã gửi, lỗi | |
| `helmet dsp` / `dsp <aec\|ns\|agc> <on\|off>` | Xem / bật tắt từng khâu xử lý giọng | So sánh có và không có khử vọng |
| `helmet prompt <tên>` / `prompt stop` | Phát thử âm báo (tên ở mục 4.1) / dừng | `ring` lặp cho tới khi `stop` |
| `helmet power` / `power off` | Trạng thái nguồn (nguyên nhân reset, thời gian tự tắt còn lại, VDD) / tắt máy | |
| `helmet autooff <0..240>` | Thời gian tự tắt (phút), 0 = không bao giờ | Được lưu |
| `helmet vdd` | Đo VDD ngay | Bình thường khoảng 3 270–3 300 mV |
| `helmet temp` | Nhiệt độ đã lọc, số lần đo, lỗi, cảnh báo | |
| `helmet temp raw` / `temp cal` | Số thô lần đo cuối / hệ số hiệu chuẩn EEPROM | |
| `helmet temp emis <100..1000>` / `temp alarm <20..60>` | Độ phát xạ (‰) / ngưỡng cảnh báo (°C) | Được lưu |
| `helmet perf` | Thời gian CPU trung bình/lớn nhất: giải mã, khối ra I2S, DSP giọng, mã hóa | |
| `helmet capture [on\|off]` | Gửi gói nhạc nhận được về PC (`make audio` tự bật) | |
| `helmet cispref <1m\|2m> <rtn> <ms>` | Thử nghiệm: QoS đề nghị cho kênh âm thanh | |
| `helmet bc` / `bc scan [tên]` / `bc stop` / `bc code <mật khẩu>\|clear` | Auracast: trạng thái / tự tìm broadcast / rời / mã broadcast cho cách tự tìm | Xem [HELMET_AURACAST.md](HELMET_AURACAST.md) |

Lệnh Zephyr có sẵn: `kernel uptime`, `kernel thread list`, `kernel thread stacks` (mức dùng stack từng thread).

---

## 13. Quy trình kiểm tra theo mức phần cứng

Làm theo thứ tự. Mỗi mức chỉ cần phần cứng của mức đó và các mức trước.

### 13.1 Mức 0: chỉ mạch chính + J-Link

Đây là cấu hình hiện tại. Mọi bài dưới đây **đã chạy đạt ngày 15/10/2026**.

| ID | Mục tiêu | Các bước | Đạt khi |
|---|---|---|---|
| M0-01 | Khởi động sạch | `make APP=apps/main reset`, xem log | Đủ các dòng ở 11.1, **0 `<wrn>`/`<err>`** |
| M0-02 | Đường âm báo, tần số I2S thật | `helmet prompt ring` → `kernel uptime` + `helmet perf` (ghi số `n` của *Out block*) → chờ 20 s → lặp lại hai lệnh → `helmet prompt stop` | Tần số = Δn × 160 / Δt ≈ **15 873 Hz** (không phải 16 000). Kết quả đo: 2 046 khối / 20.623 s = 15 873.6 Hz. Khi đang phát `helmet status` ghi `I2S running … 3 blocks`; dừng xong ghi `stopped` |
| M0-03 | Cử chỉ và hành động ở IDLE | `helmet btn volup short` ×3, `helmet btn voldown repeat`, `helmet btn call short`, `helmet btn power short` | Log `Volume 144`, `160`, `176`, rồi `160`; *Auto off* trong `helmet power` được đặt lại gần 600 s |
| M0-04 | Lưu cài đặt | `helmet vol 176`, `helmet autooff 7`, `helmet power off`, rồi `make APP=apps/main reset` | Log `Volume restored: 176/255`; `helmet power` ghi `Auto off: 7 min`. **Trả lại:** `helmet vol 128`, `helmet autooff 10` |
| M0-05 | Tắt máy, máy giữ trạng thái tắt | `helmet power off` | Log đủ `Power off (shell)` → `State … -> SHUTDOWN` → `System OFF - hold Power 2 s to turn on`. Ứng dụng quét BLE (điện thoại hoặc `bluetoothctl scan le`) **không còn thấy "Helmet"** |
| M0-06 | Quảng bá chậm, không treo | Reset, để yên 75 s | Ở giây 30: **một** dòng `Advertising as "Helmet" (reduced 150 ms)`; heartbeat đều 10 s; shell trả lời; `helmet status` ghi `advertising slow` |
| M0-07 | Tự tắt | `helmet autooff 1`, chờ 65 s | Đúng 60 s sau: `Power off (idle timeout)`, quảng bá biến mất. Trả lại `autooff 10` |
| M0-08 | Quảng bá đúng nội dung | Quét bằng nRF Connect hoặc `bluetoothctl scan le` + `bluetoothctl info <địa chỉ>` | Tên `Helmet`, Appearance `0x0942`; Service Data TMAS `0a 00`; ASCS `00 9f 07 03 00 00`; CAS `00` |
| M0-09 | Dịch vụ GATT | Kết nối bằng nRF Connect (chưa cần ghép đôi), xem danh sách dịch vụ | Có HTS 0x1809, VCS 0x1844, MICS 0x184D, ASCS 0x184E, PACS 0x1850, CAS 0x1853, TMAS 0x1855 (cộng GAP, GATT) |
| M0-10 | Chính sách mã hóa: từ chối | Kết nối từ máy **không cho ghép đôi** (laptop: `bluetoothctl pairable off` rồi `connect`) | Log `Security failed … - disconnecting (0x05)` và `Disconnected … 0x16` **dưới 1 s**; quảng bá nhanh lại |
| M0-11 | Chính sách mã hóa: im lặng | nRF Connect trên điện thoại: *Connect*, **không** bấm *Bond* | Khoảng 30 s sau: log `Not encrypted after 30 s - disconnecting (0x05)`. ⏳ Chưa làm |
| M0-12 | Đo VDD | `helmet vdd` | Khoảng 3 270–3 300 mV |
| M0-13 | Stack và CPU | `kernel thread stacks`, `helmet perf` sau các bài trên | Mỗi thread còn ≥ 25 % stack (đo được: cao nhất là `main` 47 %); *Out block* khoảng 420 µs (4 % ở 16 kHz) |

### 13.2 Mức 1: thêm board nút

| ID | Mục tiêu | Các bước | Đạt khi |
|---|---|---|---|
| M1-01 | **Xác định nút** (làm đầu tiên) | Nhấn ngắn **lần lượt từng nút vật lý**, ghi lại log | Mỗi lần có một dòng `Gesture <nút> short`. Lập bảng *nút in trên vỏ → tên trong log*. Sai khác là do ISS-07: sửa dây hoặc đổi devicetree |
| M1-02 | Nhấn ngắn / vùng bỏ qua | Mỗi nút: nhấn khoảng 0.2 s, 20 lần; rồi nhấn khoảng 0.8 s, 5 lần | 20/20 `short`; 0/5 cử chỉ ở 0.8 s (Call, Power) |
| M1-03 | Nhấn đúp | Kết nối điện thoại (`CONNECTED`), Call: nhấn–nhả–nhấn nhanh, 20 lần | 20/20 `call double` |
| M1-04 | Giữ / giữ lâu | Call giữ 1.5 s; Power giữ 3.5 s (**sẽ tắt máy**) | `call long`; `power long` rồi `power vlong` và máy tắt |
| M1-05 | Lặp âm lượng | Giữ Vol+ 3 s | 11 lần `volup repeat`; âm lượng tăng 11 bước (hoặc tới 255) |
| M1-06 | Tổ hợp | Nhấn Vol+ và Vol− cùng lúc, giữ 6 s | **Một** `volup combo` ở khoảng 5 s; log `delete all bonds`; nghe `pairing` nếu có loa. ⚠ Xóa thật mọi ghép đôi |
| M1-07 | Tổ hợp nhả sớm | Như trên nhưng nhả ở 2 s | Không có cử chỉ nào, không xóa |
| M1-08 | **Bật bằng nút** | `helmet power off`, **rút J-Link**, rồi giữ Power 2.5 s | Máy bật: quảng bá trở lại (ứng dụng quét thấy "Helmet"); nghe `power_on` nếu có loa. Gắn lại J-Link: `reset cause` có bit `0x80` |
| M1-09 | Chống bật nhầm | Khi OFF, nhấn Power 0.5 s | Máy **không** bật (ứng dụng quét không thấy "Helmet") |
| M1-10 | Giữ Power lúc bật | Khi OFF, giữ Power 5 s liền | Máy bật ở khoảng 2 s và **không tắt lại** ở mốc 3 s |
| M1-11 | Tắt bằng nút khi vẫn giữ | Khi ON, giữ Power 6 s | Ở 3 s nghe `power_off`; máy chỉ thật sự tắt **sau khi nhả** |

### 13.3 Mức 2: thêm loa

| ID | Mục tiêu | Các bước | Đạt khi |
|---|---|---|---|
| M2-01 | Nghe đủ âm báo | `helmet prompt <tên>` cho cả 13 tên ở mục 4.1 | Mỗi âm nghe đúng mô tả; cả **hai loa** cùng kêu; không có tiếng lụp bụp ở đầu và cuối |
| M2-02 | Âm báo không phụ thuộc âm lượng | `helmet vol 0` rồi `helmet prompt status_ok`; `helmet vol 255` rồi lặp lại | Hai lần nghe to như nhau |
| M2-03 | Chuông theo âm lượng | `helmet prompt ring`; đổi `helmet vol 50` ↔ `255` | Chuông to/nhỏ theo âm lượng nhưng vẫn nghe được ở mức thấp |
| M2-04 | Tiếng ồn nền | Không phát gì, áp tai vào loa | Không có tiếng rè (I2S tắt khi rảnh) |

### 13.4 Mức 3: thêm board micro

1. **Kiểm tra micro bằng `hw_test` trước**, vì `apps/main` chỉ bật micro khi đang có cuộc gọi. `apps/hw_test` đã gỡ khỏi cây mã (27/10/2026); lấy lại từ lịch sử git vào một thư mục tạm rồi build:
   ```bash
   git worktree add /tmp/hw_test a1ab029 && make -C /tmp/hw_test APP=apps/hw_test build flash
   make -C /tmp/hw_test APP=apps/hw_test rtt
   hw mic 1000        # phòng yên tĩnh, rồi vỗ tay
   ```
   Đạt khi: `L != R` gần 100 % số mẫu; AC khoảng −60…−75 dBFS lúc yên tĩnh và tăng khi vỗ tay.
2. Nạp lại `apps/main` rồi kiểm tra trong cuộc gọi thật (mức 4).

### 13.5 Mức 4: điện thoại LE Audio

| ID | Mục tiêu | Các bước | Đạt khi |
|---|---|---|---|
| M4-01 | Ghép đôi | Mục 6.2 | `Pairing complete (bonded, keys saved)`, `Link encrypted (security level 2)`, nghe `connected` |
| M4-02 | Tự kết nối lại | Tắt rồi bật Helmet | Kết nối lại không cần thao tác, không hỏi ghép đôi |
| M4-03 | Dịch vụ trên điện thoại | Xem log sau kết nối | `Media Control Service found`, `GTBS found …` (thiếu thì chức năng tương ứng không có) |
| M4-04 | Nghe nhạc | Mở nhạc 5 phút | `Streaming ASE …`, `I2S on 48000->47619.047 Hz`; `bad` < 1 %; `underrun` không tăng; `slip` tăng chậm; nghe không tách |
| M4-05 | Nút media | Call ngắn, Call đúp, Vol± | Tạm dừng / phát / bài kế tiếp; thanh âm lượng trên điện thoại đổi theo |
| M4-06 | Âm lượng từ điện thoại | Kéo thanh âm lượng điện thoại | Log `Volume …/255 (phone)` |
| M4-07 | Nghe nhạc qua PC | `make APP=apps/main audio` khi đang phát | Nghe lại được trên loa máy tính, file WAV trong `build/audio` |
| M4-08 | Cuộc gọi đến, nghe máy | Gọi tới điện thoại, Call ngắn | `RINGING` → `IN_CALL`; có chuông trong ≤ 1.5 s; hai chiều nghe rõ |
| M4-09 | Từ chối | Gọi tới, giữ Call 1 s | Người gọi bị ngắt; log `Call #… ended …` |
| M4-10 | Kết thúc | Đang gọi, Call ngắn | Cuộc gọi kết thúc; lý do `ended by headset` |
| M4-11 | Tắt micro | Đang gọi, Call đúp; rồi bấm nút micro trên điện thoại | Người kia không nghe; nghe `mic_muted`/`mic_unmuted`; biểu tượng trên điện thoại đổi theo |
| M4-12 | Khử vọng | Người kia nói liên tục 30 s, người đội mũ im | Người kia **không nghe lại giọng mình**; `voice: … ERLE` ≥ 20 dB. So với `helmet dsp aec off` |
| M4-13 | Cả hai cùng nói | Hai bên đọc cùng lúc | Giọng người đội mũ không mất chữ |
| M4-14 | Giảm ồn | Quạt thổi vào mũ khi gọi | Người kia nghe ồn ít hơn so với `helmet dsp ns off` |
| M4-15 | CPU khi gọi | `helmet perf` trong cuộc gọi | Tổng giải mã + khối ra + DSP + mã hóa < 50 % của 10 ms |
| M4-16 | Gọi lâu | Cuộc gọi 30 phút | Không rớt; `voice: … underrun` không tăng |

### 13.6 Mức 5: cảm biến nhiệt và đo dòng

| ID | Mục tiêu | Các bước | Đạt khi |
|---|---|---|---|
| M5-01 | Nhận cảm biến | Gắn MLX90632, reset | `MLX90632 found: EE_VERSION 0x..05`; `helmet temp cal` có hệ số khác 0 |
| M5-02 | Nhiệt độ môi trường | So `helmet temp` với nhiệt kế phòng | Lệch ≤ 1 °C |
| M5-03 | Nhiệt độ vật thể | Nhắm vào cốc nước khoảng 37 °C cách 2 cm, `helmet temp emis 950` | Lệch ≤ 0.5 °C so với nhiệt kế chuẩn |
| M5-04 | Cảnh báo | `helmet temp alarm 25` (thấp hơn giá trị đang đo) | Khoảng 6 s sau: `Temperature alarm ON`, nghe `temp_alarm`. Đặt lại `alarm 38` thì hết cảnh báo |
| M5-05 | HTS | nRF Connect (đã ghép đôi): bật Indicate | Nhận giá trị mỗi 10 s, khớp `helmet temp` |
| M5-06 | Đo nhiệt không làm rè nhạc | Nghe nhạc 10 phút có cảm biến | `slip`, `err` không khác so với khi không có cảm biến |
| M5-07 | **Dòng khi tắt** | Rút J-Link, PPK2 hoặc đồng hồ µA nối tiếp với pin, `helmet power off` (trước khi rút) | Ghi lại số đo; dự kiến khoảng 60 µA |
| M5-08 | Dòng các trạng thái | Như trên ở: IDLE quảng bá nhanh, IDLE chậm, CONNECTED rảnh, nghe nhạc, đang gọi | Ghi vào bảng; quảng bá chậm phải thấp hơn rõ so với nhanh |

---

## 14. Sự cố thường gặp

| Hiện tượng | Nguyên nhân | Cách xử lý |
|---|---|---|
| Không thấy log, `make rtt` không vào | Máy đang OFF (tự tắt sau 10 phút), hoặc dây J-Link | `make APP=apps/main reset`; đặt `helmet autooff 0` khi thử trên bàn |
| Nhấn Power không bật máy | Nhấn chưa đủ 2 s; nút đó không nối vào P0.03 (ISS-07) | Giữ ≥ 2.5 s; làm bài M1-01 |
| Máy tắt khi đang để trên bàn | Tự tắt sau 10 phút không kết nối | `helmet autooff 0` (hoặc tăng thời gian) |
| Điện thoại không thấy "Helmet" | Máy đang kết nối với thiết bị khác (chỉ 1 kết nối); máy OFF | Ngắt Bluetooth trên máy đang kết nối; bật mũ |
| Điện thoại kết nối rồi bị ngắt liên tục | Mũ đã xóa ghép đôi nhưng điện thoại còn khóa cũ | Xóa "Helmet" trên điện thoại, ghép đôi lại |
| Ghép đôi bị ngắt giữa chừng | Không bấm xác nhận trên điện thoại trong 30 s | Ghép lại, bấm "Ghép đôi" nhanh hơn |
| Nút Call không phát/dừng nhạc | Điện thoại không có dịch vụ media (log `Media Control Service not found`) | Hạn chế của điện thoại; dùng điều khiển trên điện thoại |
| Nút Call không nghe/kết thúc cuộc gọi | Điện thoại không có GTBS (log `GTBS not found`) | Như trên |
| Có kết nối nhưng không có tiếng | Điện thoại chưa mở luồng; nguồn phát không gửi dữ liệu | `helmet status`: ASE `STREAMING`? `ISO LQ unreceived` tăng theo mọi khung là phía phát không gửi |
| Nhạc bị tách đều đặn | Không còn xảy ra với `apps/main`. Nếu thấy, xem `slip`, `underrun` trong heartbeat | Ghi log gửi lại |
| Không có nhiệt độ | Chưa gắn MLX90632 hoặc thiếu điện trở kéo lên | `helmet temp`; kiểm tra SDA P1.13 / SCL P0.04 |

---

## 15. Giới hạn hiện tại

| # | Giới hạn | Ảnh hưởng |
|---|---|---|
| 1 | **Không có LED** (mạch hở, ISS-01) | Chỉ có âm báo; chưa có loa thì chỉ xem được qua log |
| 2 | **Cuộc gọi chờ:** đang gọi mà có cuộc thứ hai, nhấn **Call ngắn sẽ kết thúc cuộc đang nói** (không nghe cuộc mới). Sau đó cuộc chờ chưa nghe được bằng nút | Khác với tài liệu thiết kế (mục 5.2). Hiện phải nghe cuộc chờ trên điện thoại |
| 3 | **Âm báo đè chuông:** khi đang phát chuông cục bộ, âm báo khác (Power ngắn, âm lượng chạm giới hạn) **thay thế** chuông, và chuông **không phát lại** | Đổ chuông cục bộ nên tránh nhấn Power ngắn hoặc chỉnh âm lượng tới giới hạn |
| 4 | Không đo được % pin; chưa bật tự tắt khi pin cạn | Chỉ có cảnh báo "pin sắp hết" theo VDD |
| 5 | Micro chỉ chạy khi có cuộc gọi; không có lệnh thử micro riêng trong `apps/main`; **không phát hiện được board micro chưa nối** | Thử micro bằng `apps/hw_test` lấy từ lịch sử git (mục 13.4) |
| 6 | Không có gọi lại số cuối, không có trợ lý giọng nói | Giới hạn của chuẩn TBS và của thiết kế |
| 7 | Chiều gửi chỉ hỗ trợ khung 10 ms, 16 kHz (16_1, 16_2) | Điện thoại nào chỉ dùng 7.5 ms cho cuộc gọi sẽ không chuyển cuộc gọi sang mũ |
| 8 | Chưa ghi được tín hiệu micro và tham chiếu qua RTT để chỉnh khử vọng | Cần sửa `scripts/helmet_audio.py` |
| 9 | Khi "tắt", pin vẫn tốn khoảng 60 µA (LDO luôn bật) | PCB bản sau nên có mạch tự giữ nguồn |
| 10 | Nhánh "ngắt sau 30 s chưa mã hóa" chưa được tạo lại trên mạch | Bài M0-11 |
