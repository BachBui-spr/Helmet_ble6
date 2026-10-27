# Chế độ Định vị (NAV): khoảng cách giữa hai mũ và xe

| | |
|---|---|
| Áp dụng cho | Mũ: `apps/main`, xe: `apps/bike` (bike hub, cùng module ME54BS62 / nRF54L15). Nhánh `test/Broadcast-Sounding` = `feature/Broadcast-Sink` + Định vị bản 2 chuyển từ `feature/Sounding-Channel` (a3b0e35) |
| Mục tiêu | Xe và hai mũ cùng bật Định vị → **mỗi mũ in log mỗi 2 s, luôn đủ** ba khoảng cách: tới xe, tới mũ kia, và từ mũ kia tới xe |
| Trạng thái | **Bản 2 (đơn giản)** ngày 22/10/2026, đã chạy trên mạch ở nhánh Sounding (mục 13). 27/10/2026: chuyển sang cấu trúc phân lớp trên nhánh test, Định vị là **chức năng riêng** (mục 1.1). Build sạch: mũ FLASH 46,1 % / RAM 99,0 %, xe FLASH 31,6 % / RAM 50,6 %. **Chưa nạp** |
| Mã nguồn | Chung: `lib/proto/include/me54/proto/nav_proto.h` (định dạng quảng bá), `his_proto.h` (`IC_OP_NAV_*`), `lib/core/prompts.c` (`nav_on` / `nav_off`). Mũ: `apps/main/src/features/nav/nav.c` (+ móc nối trong `app/app_bt.c`, `app_ctrl.c`, `app_gesture.c`, `app_shell.c`, `features/intercom/ic_service.c`, `intercom.c`, `features/broadcast/broadcast_sink.c`). Xe: `apps/bike/src/features/nav/nav_hub.c` (+ `app/main.c`, `app/ui.c`, `features/broadcast/bcast_source.c`) |
| Không chuyển | `ranging.c` của nhánh Sounding (đo khoảng cách mũ–mũ **trong lúc** intercom, chưa từng thử): trái với nguyên tắc chức năng riêng ở mục 1.1 |

---

## 1. Kết quả mong muốn

Bật Định vị trên xe và trên cả hai mũ. Sau vài giây, **mỗi mũ** in **mỗi 2 s**:

```
NAV: bike 4.21 m | helmet C6:C3:C4:37:C4:34 (random) 2.10 m | that helmet -> bike 3.80 m (4 s)
```

Mỗi số là **lần đo gần nhất** của cạnh đó, giữ tối đa 30 s. Số cũ hơn 2,5 s thì in kèm tuổi trong ngoặc, ví dụ `(4 s)`. Tuổi tính từ lúc **đo** chứ không phải lúc nhận, vì mũ kia gửi kèm tuổi. Vì vậy khi mỗi cạnh đã đo được ít nhất một lần, dòng log luôn có đủ 3 số. Mỗi 10 s có thêm một dòng thống kê thủ tục CS của các liên kết mũ này đo (`nav: bike link ..., N procedures ok, M aborted ...`).

| Phần | Nghĩa | Ai đo |
|---|---|---|
| `bike 4.21 m` | Mũ này → xe | Chính mũ này |
| `helmet XX 2.10 m` | Mũ này → mũ kia | Mũ có **mã nhỏ hơn** đo, gửi sang mũ còn lại |
| `that helmet -> bike 3.80 m` | Mũ kia → xe | Mũ kia đo, gửi sang |

Khi cạnh nào chưa từng có số, phần đó in trạng thái, ví dụ `bike (searching)`, `(securing)`, `(CS config)`, `(measuring)`, `(waiting)`.

Ngoài phạm vi: góc, hướng, bíp dẫn đường, chạy song song với intercom (mục 12).

### 1.1 Định vị là chức năng riêng

Mũ có đủ các chức năng: nghe / gọi điện thoại, nghe chung nhạc của xe (Auracast), intercom, Định vị. **Định vị không chạy cùng lúc với intercom hay Auracast**: khi cần tìm nhau thì bật Định vị, xong thì tắt, bình thường vẫn nghe / gọi như cũ.

| Lúc | Mũ | Xe |
|---|---|---|
| Bật Định vị | Kết thúc intercom (nếu có), rời broadcast (`bcast_leave`), quảng bá + quét Định vị | Tạm dừng BIG (`bcast_source_pause`), quảng bá Định vị, phản xạ CS |
| Đang Định vị | Không chọn được vai trò intercom, không tự tìm / tự vào lại Auracast, không nhận lệnh BASS của điện thoại. Cuộc gọi điện thoại vẫn ưu tiên | Điện thoại vẫn kết nối; nhạc tới xe không phát ra BIG |
| Tắt (nút, lệnh, hoặc tự tắt sau 10 phút) | Ngắt các liên kết Định vị; sau 3 s tự vào lại broadcast của xe đã ghép (`bcast_rejoin_soon`), nếu trước đó chưa rời bằng nút | Ngắt các mũ, tạo lại BIG |

---

## 2. Vì sao thiết kế lại

Bản 1 (ngày 22/10, đã bỏ) theo sát tài liệu mẫu `Kien_truc_Bluetooth_Channel_Sounding_mu_bao_hiem.docx`: xe là CS initiator và là trung tâm, gộp ba cạnh thành tam giác, gửi kết quả về mũ; khoảng cách mũ–mũ lấy từ intercom; có bíp dẫn đường, đọc số bằng tiếng bíp, danh sách ghép. Thử trên mạch lộ ra:
- mũ đã ghép đi vào xe qua **bộ quảng bá của điện thoại** (cùng địa chỉ identity), nên xe không nhận ra mũ;
- kiểm tra IPT theo trường `t_ip2_ipt_times_supported` **chặn nhầm** (trường này bằng 0 khi controller gửi sự kiện phiên bản 1);
- cần intercom mới có khoảng cách mũ–mũ;
- quá nhiều bước so với mục tiêu "đọc được 3 khoảng cách".

Những gì giữ lại từ tài liệu mẫu:
- ba cạnh xe–H1, xe–H2, H1–H2;
- không dùng Direction Finding;
- số đo có dấu thời gian, quá 3 s thì không dùng;
- callback ngắn, xử lý trong thread.

Những gì bỏ:
- gửi dữ liệu CS thô: dùng IPT, bên đo tự tính;
- xe là trung tâm tính toán: ở bản này **bên kết nối tự đo**;
- epoch tuần tự, tam giác / góc.

---

## 3. Kiến trúc

```
                 XE (chỉ phản xạ CS, không tính gì)
                ↗                          ↖
   mũ A kết nối + đo                  mũ B kết nối + đo
   d(A, xe)                           d(B, xe)
             A ───────────────────────► B        A có mã nhỏ hơn: A kết nối + đo d(A, B)
                 HIS: A gửi d(A,B), d(A,xe) cho B; B gửi d(B,xe) cho A
```

### 3.1 Nguyên tắc: bên kết nối (central) là bên đo (CS initiator)

| Liên kết | Central = bên đo và tự tính | Bên kia (chỉ phản xạ) |
|---|---|---|
| Mũ ↔ xe | Mũ | Xe |
| Mũ ↔ mũ | Mũ có **mã nhỏ hơn**. Mã = 4 byte thấp của địa chỉ identity, nằm trong quảng bá | Mũ còn lại |

Lý do:
- **CS Security chỉ central được bật**: controller trả 0x0C nếu peripheral gửi lệnh này. Đo và bật bảo mật cùng một bên thì toàn bộ thủ tục nằm ở một phía, đúng như mẫu `ipt_initiator` / `ipt_reflector` của Nordic.
- **IPT**: bên phản xạ chỉnh pha khi gửi trả, nên bên đo tự tính được khoảng cách, không cần nhận số đo của bên kia.
- **Xe không phải nhận diện mũ** bằng giao thức riêng. Xe chỉ bật vai trò phản xạ cho mọi kết nối đến khi đang Định vị. Mũ tạo cấu hình CS với xe thì xe biết đó là mũ (sự kiện `LE CS Config Complete`), lúc đó xe gỡ kết nối khỏi luồng điện thoại.

### 3.2 Trao đổi số liệu: chỉ giữa hai mũ, qua HIS có sẵn

Hai mũ dùng **Helmet Intercom Service** (Control Point / Event, bản tin 4 byte `[op][tuổi ×100 ms][cm thấp][cm cao]`; byte thứ 2 intercom luôn để 0), thêm 3 mã lệnh:

| Op | Tên | Hướng | Nội dung |
|---|---|---|---|
| 0x40 | `IC_OP_NAV_HELLO` | central → peripheral | "Đây là liên kết Định vị", arg = phiên bản |
| 0x41 | `IC_OP_NAV_DIST_PEER` | central → peripheral | Khoảng cách mũ–mũ central vừa đo (cm) |
| 0x42 | `IC_OP_NAV_DIST_BIKE` | hai chiều | Khoảng cách từ mũ gửi tới xe (cm) |

Mỗi mũ gửi mỗi 0,5 s, xen kẽ hai loại (central gửi cả hai, peripheral gửi `DIST_BIKE`), chỉ gửi số đo trong 10 s gần nhất. Xe không có dịch vụ GATT nào cho Định vị, nên cơ sở dữ liệu GATT của xe giống bản gốc nhánh broadcast. Chỉ cần cắm J-Link vào **một mũ bất kỳ** là thấy đủ 3 khoảng cách.

---

## 4. Trình tự

### 4.1 Bật

| Thiết bị | Thao tác | Kết quả |
|---|---|---|
| Xe | **Call ngắn** hoặc `bike nav on` | Âm 3 nốt đi lên; phát quảng bá Định vị (legacy, có thể kết nối, 100–150 ms); **tạm dừng Auracast (BIG)** để nhường radio, phát lại khi tắt (`bike nav big keep` để giữ) |
| Mỗi mũ | **Giữ Call 1 s** hoặc `helmet nav on` | Âm 3 nốt đi lên; kết thúc intercom nếu đang có; thêm Service Data Định vị vào quảng bá (150 ms) và quét 30 ms / 90 ms |

Thứ tự bật không quan trọng. Chế độ tự tắt sau 10 phút trên mũ; bấm lại để tắt sớm.

### 4.2 Mũ → xe

```
Mũ thấy quảng bá Định vị loại "xe"   → kết nối (ACL 33,75 ms, giám sát 4 s)
Xe: kết nối đến khi đang Định vị     → bật vai trò CS phản xạ cho kết nối đó
Mũ: mã hoá (bond cũ, hoặc Just Works) → "NAV bike: linked ... this helmet measures"
Mũ: đặt vai trò initiator → đọc khả năng CS của xe → tạo cấu hình 0 (mode 2, IPT)
Xe: nhận cấu hình → "NAV: helmet ... is measuring its distance to the bike"
Mũ: bật CS Security → đặt lịch (500 ms) → bật đo lặp → "NAV bike: MEASURING"
Mỗi lượt: pha 72 kênh → IFFT → trung vị 5 lượt → d(mũ, xe)
```

### 4.3 Mũ ↔ mũ

```
Mũ A (mã nhỏ hơn) thấy quảng bá loại "mũ" của B (mã lớn hơn) → kết nối
Mũ B: kết nối đến khi đang Định vị = ỨNG VIÊN → bật vai trò phản xạ, chờ NAV_HELLO
Mũ A: mã hoá → tìm HIS trên B, đăng ký Event → gửi NAV_HELLO
Mũ B: NAV_HELLO → "NAV peer helmet: linked ... the other helmet measures"
Mũ A: đo như mục 4.2 → d(A, B), gửi B
Hai mũ gửi nhau d(mình, xe)
```

Ứng viên không gửi `NAV_HELLO` trong 6 s được giao cho luồng điện thoại (giống ứng viên của intercom). Vì vậy điện thoại nối vào lúc đang Định vị chỉ chậm 6 s.

### 4.4 Mất liên kết, lỗi

| Tình huống | Xử lý |
|---|---|
| Mất liên kết | Quét lại sau 1 s, tự nối lại |
| Kết nối quá 10 s / mã hoá + bắt tay quá 15 s | Bỏ, quét lại sau 3 s |
| Mã hoá hỏng do khoá cũ (bên kia đã xoá ghép đôi) | Xoá bond phía mình, lần sau ghép Just Works |
| Bước CS nào đó lỗi | Ngắt, nối lại sau 5 s |
| Bên kia không hỗ trợ IPT | Bước tạo cấu hình báo lỗi → như trên, log `config create (IPT) failed` |

---

## 5. Định dạng quảng bá (`lib/proto/include/me54/proto/nav_proto.h`)

Service Data 128 bit, 22 byte: `[0..15] UUID 0eed0201-080c-4225-a3d0-f91ce86a80c2`, `[16] phiên bản = 2`, `[17] loại (0 = xe, 1 = mũ)`, `[18..21] mã (LE)`.

- Xe: quảng bá legacy riêng, chỉ có Flags + Service Data (27 B).
- Mũ: thêm mục này vào **quảng bá chính** (extended), vẫn giữ các mục LE Audio.

---

## 6. Máy trạng thái

**Mũ, mỗi liên kết (`nav.c`):**
```
searching ─(thấy thiết bị phù hợp)─► connecting ─► securing ─┬─(xe)──────────────► up (đo)
                                                            └─(mũ, mình central)─► handshake (HIS, NAV_HELLO) ─► up (đo)
(mũ, mình peripheral) ứng viên ─(NAV_HELLO)─► up (phản xạ, nhận số)
up ─(mất liên kết / lỗi CS)─► searching
```
Bộ đo trên liên kết `up` mà mình là central: `CS caps → CS config → CS security → CS enable → measuring`.

**Xe (`nav_hub.c`):** tắt ↔ bật (tự tắt sau 10 phút). Khi bật: tạm dừng BIG; mọi kết nối đến được bật vai trò phản xạ; kết nối nào tạo cấu hình CS thì là mũ (đếm thủ tục, gỡ khỏi luồng điện thoại). Tắt thì ngắt các mũ và tạo lại BIG.

Luồng xử lý:
- Mũ: trạng thái chỉ đổi trong thread bộ điều khiển (`EVT_NAV`). Lệnh HCI của bộ đo, IFFT và gửi HIS chạy trong system workqueue.
- Xe: lệnh HCI chạy trong system workqueue.

---

## 7. Khoảng cách

| Bước | Chi tiết |
|---|---|
| Thu | Mỗi bước mode 2: PCT của bên đo theo kênh 2–76 (IPT: đã gồm pha hai chiều) |
| Ước lượng | `cs_de_ifft()`, NFFT 512 |
| Lọc | Trung vị 5 lượt gần nhất (2,5 s ở chu kỳ 500 ms) |
| Chất lượng | `q = 100 × (1 − MAD)`, MAD = độ lệch tuyệt đối trung vị (m). In trong `helmet nav` |
| Tuổi | Giữ số gần nhất tối đa 30 s; cũ hơn 2,5 s thì in kèm tuổi |

---

## 8. Tham số CS và radio

| Tham số | Giá trị | Lý do |
|---|---|---|
| ACL (do central chọn) | 33,75 ms | Không là bội của 10 ms, tránh khoá pha với BIG / CIS (bài học hub_link) |
| Chu kỳ thủ tục | Mũ–xe 500 ms (`helmet nav period <ms>`), mũ–mũ 1000 ms | Mỗi lần in (2 s) có 2–4 lượt đo |
| Subevent | Mũ–xe 1,25–**2,5** ms, mũ–mũ 1,25–3,75 ms | Lượt ngắn dễ chen vào khoảng trống trong lịch radio của xe |
| Kênh | Mũ–xe **36** (mỗi 2 kênh, `helmet nav ch <1\|2\|4>`), mũ–mũ 72 | Lượt đo ngắn bằng nửa. Dải tần vẫn trải ~72 MHz nên độ phân giải gần như không đổi; khoảng không nhập nhằng còn 75 m |
| Cấu hình | Mode 2, 3 bước mode 0, kênh #3b, CS sync 1M, thủ tục 2M, IPT | Như mẫu `ipt_initiator` |

Mũ có mã nhỏ hơn đo 2 liên kết (xe + mũ kia), mũ còn lại đo 1 liên kết (xe) và phản xạ 1 liên kết.

---

## 9. Cấu hình và bộ nhớ

**Mũ** (`apps/main/prj.conf`, `apps/main/Kconfig`, `src/features/nav/CMakeLists.txt`):

| Mục | Giá trị | Lý do |
|---|---|---|
| `BT_MAX_CONN` | 4 (`PERIPHERAL_COUNT` 2) | Tới 2 central (xe + mũ kia) cùng với điện thoại |
| `BT_CTLR_SDC_CS_COUNT` | 2 | Hai liên kết có CS cùng lúc |
| `BT_CHANNEL_SOUNDING_REASSEMBLY_BUFFER_CNT` / `SIZE` | 2 / 1000 | Mỗi thủ tục chạy song song cần một bộ đệm (mặc định 1) |
| `CONFIG_HELMET_CAPTURE` | tắt mặc định | Bộ đệm RTT 16 KB của `debug/capture.c` nhường RAM. Cần `make audio` thì build `EXTRA=-DCONFIG_HELMET_CAPTURE=y` |

**Xe** (`apps/bike/prj.conf`):

| Mục | Giá trị | Lý do |
|---|---|---|
| `BT_MAX_CONN`, `PERIPHERAL_COUNT` | 5, 5 | Điện thoại + 2 mũ ghép BHS + 2 mũ Định vị |
| `BT_EXT_ADV_MAX_ADV_SET` | 4 | Thêm quảng bá Định vị |
| `BT_CHANNEL_SOUNDING`, `BT_CTLR_SDC_CS_ROLE_REFLECTOR_ONLY` | y | Xe chỉ phản xạ |
| `BT_CTLR_SDC_CS_COUNT` | 2 | Hai mũ |
| `BT_CTLR_EXTENDED_FEAT_SET`, `BT_TRANSMIT_POWER_CONTROL` | y | IPT; CS và ACL khác PHY |
| `…REASSEMBLY_BUFFER_CNT / SIZE` | 2 / 1000 | Bên phản xạ cũng nhận kết quả subevent |

| Bản build | FLASH | RAM |
|---|---|---|
| Mũ, `feature/Broadcast-Sink` 81b9ecc (trước Định vị, capture bật) | 646 924 B (41,5 %) | 251 027 B (95,8 %) |
| **Mũ + Định vị, nhánh test (27/10/2026, capture tắt)** | **720 052 B (46,1 %)** | **259 427 B (99,0 %)** |
| Xe, `feature/Broadcast-Sink` 81b9ecc | 440 044 B (28,2 %) | 116 408 B (44,4 %) |
| **Xe + Định vị, nhánh test (27/10/2026)** | **493 756 B (31,6 %)** | **132 648 B (50,6 %)** |

RAM mũ tăng thêm khoảng 24 KB so với trước Định vị (bù lại 16 KB capture): bộ nhớ controller `sdc_mempool` +12 KB (2 ngữ cảnh CS, 4 kết nối), các bảng theo số kết nối của BAP (`uni_cli_insts`, `srv_insts`) +3,2 KB, bộ nhớ IFFT 4 KB, bộ đệm ráp kết quả CS 2 KB.

---

## 10. Shell và log

| Lệnh | Việc |
|---|---|
| `helmet nav` | Trạng thái từng liên kết, bên nào đo, bước CS, khoảng cách + chất lượng, số thủ tục ok / huỷ / bỏ / hỏng, dòng 3 khoảng cách |
| `helmet nav on` / `off` | Bật / tắt (như giữ Call 1 s) |
| `helmet nav ch <1\|2\|4>`, `period <ms>` | Liên kết tới xe: số kênh (mặc định mỗi 2 kênh) / chu kỳ (áp dụng từ lần nối xe sau) |
| `bike nav` | Bật / tắt, các mũ đang đo với xe (RUNNING, số thủ tục) |
| `bike nav on` / `off` | Như Call ngắn trên xe |
| `bike nav big pause` / `keep` | Tạm dừng (mặc định) / giữ Auracast trong lúc Định vị |

Log mũ có **mã nhỏ hơn** (đo cả hai liên kết):
```
Navigation mode: leaving Auracast broadcast (rejoins when navigation ends)
Navigation mode ON (10 min), tag 1a2b3c4d - looking for the bike and the other helmet
NAV bike: EF:7B:65:FD:43:AE (random) found (RSSI -40 dBm) - connecting
Connected: ... (navigation)
NAV bike: linked ... - this helmet measures (CS initiator)
NAV bike: peer CS reflector yes (IPT t_ip2 0x...., t_sw .. us)
NAV bike: CS config 0 (mode 2, T_IP2 .. us, ...)
NAV bike: ACL 33.75 ms -> 1 procedure every 15 events (~506 ms), 72 channels
NAV bike: MEASURING - subevent .. us x ../event, procedure every 15 conn events, TX .. dBm
NAV peer helmet: ... found (RSSI .. dBm, tag 5e6f7a8b > ours 1a2b3c4d) - connecting
NAV peer helmet: linked ... - this helmet measures (CS initiator)
NAV peer helmet: MEASURING ...
NAV: bike 4.21 m | helmet ... 2.10 m | that helmet -> bike 3.80 m      (mỗi 2 s)
```
Log mũ còn lại:
```
NAV: incoming connection ... (waiting for NAV_HELLO)
NAV peer helmet: linked ... - the other helmet measures (this one reflects)
NAV: bike 3.80 m | helmet ... 2.10 m | that helmet -> bike 4.21 m
```
Log xe:
```
Navigation mode ON - advertising; helmets measure their distance to the bike
Broadcast pausing for navigation mode
Broadcast paused (navigation mode): BIG stopped
NAV: helmet ... is measuring its distance to the bike (bike = CS reflector, mode 2)
NAV: helmet ... ranging RUNNING (procedure every 15 conn events)
heartbeat ... nav: helmet ... RUNNING, 230 procedures, 4 aborted
```

---

## 11. Thử với một J-Link

J-Link không cấp nguồn cho mạch, và `make rtt` chỉ gắn vào, không reset, nên chuyển J-Link giữa các mạch không làm chúng dừng.

1. Nạp:
   ```
   make APP=apps/bike pristine && make APP=apps/bike flash     # J-Link ở xe
   make APP=apps/main pristine && make APP=apps/main flash     # J-Link ở mũ A
   make APP=apps/main flash                                    # chuyển sang mũ B
   ```
2. Bật: xe **Call ngắn**, hai mũ **giữ Call 1 s**. Không có nút thì gắn J-Link lần lượt và gõ `bike nav on` / `helmet nav on`.
3. Gắn J-Link vào **mũ bất kỳ**: `make APP=apps/main rtt`. Log mỗi 2 s có đủ 3 khoảng cách. `helmet nav` cho chi tiết.
4. Đo ở vài khoảng cách đã biết (1 / 3 / 5 / 10 m), ghi trung vị, `q` và số `aborted`.
5. Muốn xem phía xe thì chuyển J-Link sang xe: `make APP=apps/bike rtt`, `bike nav`.

| Thử | Đạt khi |
|---|---|
| Một mũ + xe | `bike x.xx m` sau ≤ 5 s kể từ khi bật |
| Hai mũ | Mũ mã nhỏ: `helmet ... x.xx m`; mũ kia cũng thấy cùng số đó |
| Đủ 3 | Cả hai mũ in đủ 3 số; số chéo khớp nhau (A thấy "that helmet → bike" = B thấy "bike") |
| Mất liên kết | Đi xa rồi quay lại: tự nối lại, số có lại |
| Đang nghe nhạc qua xe rồi bật Định vị | Mũ rời broadcast, xe dừng BIG; tắt Định vị trên xe và mũ thì trong vài giây mũ tự nghe lại nhạc |
| Đang intercom rồi bật Định vị | Intercom kết thúc, Định vị chạy; giữ Vol± để chọn intercom trong lúc Định vị bị từ chối |

---

## 12. Giới hạn và hướng mở rộng

| # | Giới hạn | Ghi chú |
|---|---|---|
| 1 | Không chạy song song với intercom hay Auracast (chủ ý, mục 1.1) | Mỗi cặp thiết bị chỉ có một kết nối BLE; BIG của xe chặn phần lớn lượt đo mũ–xe |
| 2 | Không có cửa sổ ghép: thiết bị nào đang Định vị gần đó cũng tham gia (mỗi mũ lấy 1 xe, 1 mũ thấy trước) | Đủ cho thử. Sản phẩm nên lọc theo xe / mũ đã ghép |
| 3 | Độ chính xác chưa hiệu chuẩn (một ăng-ten, đầu người che sóng làm đo dài hơn) | Đo ở mục 11 bước 4, thêm bù lệch nếu ổn định |
| 4 | IPT chỉ dùng pha, không chống giả khoảng cách | Không dùng cho khoá / chống trộm |
| 5 | Không có hướng | Ba khoảng cách chỉ cho góc không dấu. Hướng theo mặt người đội cần IMU + từ kế trên mũ |
| 6 | RAM mũ 99 % (còn ~2,7 KB) | Chạy được (cấp phát tĩnh) nhưng hết chỗ cho tính năng mới. Lấy lại được: bỏ shell UART20 (~5,4 KB, còn shell RTT), gom 4 bộ giải mã 48 kHz của `audio_out` thành kho dùng chung (~15 KB) |
| 7 | `BT_CHANNEL_SOUNDING`, `BT_CS_DE` còn EXPERIMENTAL ở NCS v3.4.0 | |
| 8 | Trong lúc xe Định vị, Auracast tạm dừng: điện thoại phát nhạc qua xe thì các mũ không nghe được | Đúng thiết kế chức năng riêng. `bike nav big keep` chỉ để thử (mũ vẫn rời broadcast khi Định vị) |

---

## 13. Nhật ký thử

| Ngày | Hiện tượng | Nguyên nhân | Sửa |
|---|---|---|---|
| 22/10/2026 (bản 2, lần 1) | Đủ 3 khoảng cách, cạnh mũ–mũ có số gần như mỗi giây (0,52–0,79 m). Cạnh **mũ–xe** ở cả hai mũ chỉ có số khoảng 10–30 s một lần (1,05–1,22 m; mũ kia 0,29–0,70 m), mỗi lần chỉ hiện 3 s rồi về `(measuring)` | Liên kết mũ–mũ không có BIG nên đo đều. Xe phát **BIG 10 ms liên tục** (kể cả khung im lặng), nên các subevent 3,75 ms hiếm khi có chỗ, đa số lượt đo mũ–xe bị huỷ. Thêm vào đó, log cũ bỏ số sau 3 s | Xe tạm dừng BIG khi Định vị. Liên kết tới xe dùng 36 kênh, subevent ≤ 2,5 ms. Log 2 s, giữ số 30 s kèm tuổi, mũ kia gửi tuổi thật. Dòng thống kê `aborted` mỗi 10 s để kiểm chứng |
