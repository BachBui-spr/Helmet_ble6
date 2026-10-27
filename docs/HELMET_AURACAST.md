# Helmet: nghe chung nhạc từ một điện thoại qua Auracast

| | |
|---|---|
| Áp dụng cho | `apps/main` (firmware đầy đủ), board `me54bs62` (nRF54L15) |
| Trạng thái | **Đã hiện thực và nạp lên mạch SN 69750999 ngày 19/10/2026.** Đã thử cách tự tìm với Xiaomi Pad 7 Pro (broadcast mã hoá, 48 kHz, 2 BIS): nghe được. Bản sửa tiếng tạch và chờ khi nguồn tạm dừng đã nạp, **chưa thử lại**. Cách điện thoại chỉ đường chưa thử |
| Dựa trên | NCS v3.4.0 / Zephyr 4.4. Đã đối chiếu với sample `zephyr/samples/bluetooth/bap_broadcast_sink` và mã nguồn thư viện `bap_broadcast_sink.c`, `bap_scan_delegator.c` |
| Mã nguồn | `apps/main/src/features/broadcast/broadcast_sink.c` (mới). Sửa thêm lúc đó: `main.c`, `le_audio.c`, `intercom.c`, `buttons.c`, `prompts.c`, `helmet.h`, `prj.conf`, `CMakeLists.txt` (từ 27/10/2026 nằm ở `apps/main/src/app/`, `features/`, `lib/`, xem [README.md](../README.md) mục *Kiến trúc mã nguồn*) |
| Tài liệu liên quan | [HELMET_HUONG_DAN_SU_DUNG_VA_KIEM_TRA.md](HELMET_HUONG_DAN_SU_DUNG_VA_KIEM_TRA.md), [HELMET_THIET_KE_INTERCOM.md](HELMET_THIET_KE_INTERCOM.md). Mọi lỗi broadcast đã gặp và cách sửa: [BROADCAST_VAN_DE_VA_GIAI_PHAP.md](BROADCAST_VAN_DE_VA_GIAI_PHAP.md) |

---

## Mục lục

1. [Bài toán và cách giải](#1-bài-toán-và-cách-giải)
2. [Kiến thức nền](#2-kiến-thức-nền)
3. [Luồng hoạt động](#3-luồng-hoạt-động)
4. [Kiến trúc phần mềm](#4-kiến-trúc-phần-mềm)
5. [Chi tiết hiện thực từng bước](#5-chi-tiết-hiện-thực-từng-bước)
6. [Ưu tiên và loại trừ với các chức năng khác](#6-ưu-tiên-và-loại-trừ-với-các-chức-năng-khác)
7. [Thay đổi cấu hình, GATT, quảng bá, bộ nhớ](#7-thay-đổi-cấu-hình-gatt-quảng-bá-bộ-nhớ)
8. [Nút, âm báo, lệnh shell, log](#8-nút-âm-báo-lệnh-shell-log)
9. [Kiểm thử](#9-kiểm-thử)
10. [Giới hạn và rủi ro](#10-giới-hạn-và-rủi-ro)
11. [Phụ lục: vị trí trong SDK](#11-phụ-lục-vị-trí-trong-sdk)

---

## 1. Bài toán và cách giải

**Mục tiêu:** hai mũ cùng nghe một bài nhạc phát từ **một** điện thoại.

**Vì sao trước đây không làm được:** nhạc đi qua LE Audio *unicast*. Đó là các luồng CIS riêng giữa điện thoại và **một** thiết bị đang hoạt động. Android chỉ gửi nhạc tới một thiết bị LE Audio tại một thời điểm. Ngoại lệ là một bộ tai nghe được khai báo là "một cặp" bằng CSIP, nhưng hai mũ của hai người không phải một cặp.

**Cách giải (phương án A):** dùng **Auracast**, tức LE Audio *broadcast*.
- Điện thoại **quảng bá** nhạc một lần lên không trung (BIG gồm các BIS).
- Mỗi mũ tự **thu** luồng đó. Số mũ nghe cùng lúc không giới hạn.
- Điện thoại không gửi riêng cho từng mũ, nên hai mũ không tranh nhau.

Để làm được, mỗi mũ có thêm hai vai trò:

| Vai trò | Việc làm |
|---|---|
| **BAP Broadcast Sink** | Đồng bộ periodic advertising (PA), đọc BASE, đồng bộ BIG, nhận BIS, giải mã LC3, phát ra loa |
| **BAP Scan Delegator** (dịch vụ GATT **BASS**) | Cho điện thoại (*Broadcast Assistant*) "chỉ đường": luồng nào cần nghe, mã giải mã là gì. Mũ báo ngược trạng thái qua *Receive State* |

Phần giải mã LC3 → bù trôi clock → I2S → loa **dùng lại nguyên** đường nghe nhạc hiện có (`audio_out.c`).

---

## 2. Kiến thức nền

| Thuật ngữ | Ý nghĩa |
|---|---|
| **Broadcast Source** | Bên phát (điện thoại). Có một bộ quảng bá mở rộng (extended advertising), nhận diện bằng địa chỉ + **SID** |
| **Broadcast Audio Announcement** | Service Data UUID `0x1852` trong quảng bá mở rộng, mang **Broadcast ID** 24 bit |
| **Broadcast Name** | AD type `0x30`, tên hiển thị của broadcast (vd "Pixel của Bách") |
| **PA** (Periodic Advertising) | Chuỗi gói quảng bá định kỳ đi kèm bộ quảng bá. Mang **BASE** và **BIGInfo** |
| **BASE** | Mô tả nội dung: các *subgroup* (codec, metadata), mỗi subgroup gồm các **BIS** (chỉ số 1..31, kèm Audio Location: trái/phải/mono) |
| **BIGInfo** | Thông số vô tuyến của BIG (khoảng ISO, số BIS, PHY, SDU, **có mã hoá không**). Nằm trong ACAD của gói PA |
| **BIG / BIS** | Broadcast Isochronous Group / Stream: luồng âm thanh thật. Ví dụ 1 BIG = BIS1 (trái) + BIS2 (phải) |
| **Broadcast Code** | Khoá 16 byte khi broadcast được mã hoá. "Chia sẻ âm thanh" của Android mặc định mã hoá |
| **Broadcast Assistant** | Thiết bị điều khiển (điện thoại). Ghi lệnh vào BASS của mũ |
| **Receive State** | Đặc tính BASS. Mũ báo trạng thái PA (chưa / đang xin SyncInfo / đã đồng bộ / lỗi), mã hoá, BIS đã đồng bộ |
| **PAST** | Periodic Advertising Sync Transfer: điện thoại gửi thông tin đồng bộ PA qua kết nối ACL, mũ **không phải tự quét** |

---

## 3. Luồng hoạt động

### 3.1 Cách 1: điện thoại chỉ đường (Android "Chia sẻ âm thanh")

Đây là cách chính: cả hai mũ đều đã ghép đôi và kết nối LE Audio với điện thoại.

```
Điện thoại (Assistant + Source)                  Mũ (Scan Delegator + Broadcast Sink)
--------------------------------                  ------------------------------------
Bắt đầu broadcast, dừng nhạc unicast
BASS Add Source (địa chỉ, SID, Broadcast ID,
  PA sync = "có PAST", BIS sync = 0x3) ───────►  sd_pa_sync_req()  ──EVT_BC──► on_pa_req()
                                                  sd_bis_sync_req()           past_start():
                                                                              subscribe PAST, PA state = SyncInfo Request
        ◄─────────── Receive State (notify: SyncInfo Request)
PAST (LL_PERIODIC_SYNC_IND) ─────────────────►  pa_synced_cb() ──► on_pa_synced():
                                                  bt_bap_broadcast_sink_create()  (PA state = Synced)
                                                PA: BASE   ──► bs_base_recv(): phân tích subgroup/BIS
                                                PA: BIGInfo──► bs_syncable(): mã hoá?
        ◄─────────── Receive State (Big_Encryption = Broadcast_Code required)
BASS Set Broadcast Code ─────────────────────►  sd_broadcast_code()
                                                reconcile(): select_bis() → bt_bap_broadcast_sink_sync()
                                                BIG Sync Established ──► bs_started() → PROMPT_BC_JOINED
                                                bc_stream_started() → audio_out_stream_start() → loa
        ◄─────────── Receive State (BIS_Sync = 0x3)
```

Kết thúc: điện thoại gửi Modify Source (PA = không, BIS = 0) rồi Remove Source.
- Mũ dừng BIG, xoá PA và sink.
- Mũ phát âm `bc_left`.

### 3.2 Cách 2: mũ tự tìm (không cần điện thoại chỉ đường)

Dùng khi điện thoại phát Auracast công khai nhưng không làm Assistant với mũ. Cũng dùng để thử với một broadcast bất kỳ.

1. **Power ngắn → giữ Call 2 s** (hoặc `helmet bc scan [tên]`).
2. Mũ quét thụ động (cửa sổ 30 ms / chu kỳ 60 ms) tìm các bộ quảng bá có PA và Service Data `0x1852`.
3. Thấy broadcast đầu tiên thì nghe thêm 2 s và chọn cái có RSSI mạnh nhất. Nếu có lọc tên thì chỉ chọn tên chứa chuỗi đó, không phân biệt hoa thường.
4. Mũ tự đồng bộ PA bằng `bt_le_per_adv_sync_create()`, rồi đi tiếp như mục 3.1.
5. Thư viện tự thêm một *Receive State* cục bộ. Điện thoại nối với mũ cũng thấy mũ đang nghe broadcast nào.
6. Broadcast mã hoá: mã nhập bằng `helmet bc code <mật khẩu>`.
   - Có thể nhập trước, hoặc trong lúc mũ đang chờ: mã được dùng ngay.
   - Mã **được lưu** (settings `bcast/code`), khởi động lại không phải nhập lại.
   - Đã có PA và BASE thì mũ **chờ không giới hạn**: chờ nguồn phát nhạc (BIGInfo) hoặc chờ mã. Rời bằng nút hoặc `helmet bc stop`.

---

## 4. Kiến trúc phần mềm

### 4.1 Luồng thực thi

| Ngữ cảnh | Việc làm trong `broadcast_sink.c` |
|---|---|
| **bt_rx** (callback Bluetooth: BASS, PA, Broadcast Sink, quét) | Chỉ chép dữ liệu vào biến chung (giữ `k_spinlock lock`), bật một bit trong `ev_bits`, gọi `app_post(EVT_BC)` |
| **Thread bộ điều khiển** (`main.c`, cùng thread với intercom) | `bcast_handle()` đọc và xoá các bit, ra mọi quyết định có lệnh HCI (tạo PA, tạo sink, đồng bộ BIG, dừng, xoá) |
| **bt_rx** (luồng BIS) | `bc_stream_started/stopped/recv`: đường âm thanh chạy thẳng tới `audio_out` như `le_audio.c` |
| **ISR** (k_timer) | Hẹn giờ chỉ bật bit sự kiện |

Lý do chọn mô hình này:
- Mọi quyết định chạy trên **một** thread nên không cần khoá giữa intercom và Auracast.
- Callback bt_rx không chặn lâu.
- Một hàm `reconcile()` chạy cuối mỗi lần xử lý, so trạng thái hiện tại với điều kiện để đồng bộ hoặc dừng BIG. Nhờ vậy thứ tự đến của BASE, BIGInfo, mã và yêu cầu BIS không quan trọng.

### 4.2 Máy trạng thái (`enum bc_mode`)

| Trạng thái | Ý nghĩa | Thoát khi |
|---|---|---|
| `BC_OFF` | Không nghe broadcast | Điện thoại Add Source / nút / `bc scan` |
| `BC_SCAN` | Tự quét (tối đa 20 s) | Chọn được broadcast → `PA_WAIT`; hết giờ → `OFF` + `bc_fail` |
| `BC_PA_WAIT` | Chờ PAST hoặc chờ tự đồng bộ PA (10 s) | Đồng bộ → `PA_SYNCED`. Không có PAST → tự quét. Lỗi → thử lại |
| `BC_PA_SYNCED` | Có PA. Chờ BASE, BIGInfo, mã (nếu mã hoá) và yêu cầu BIS | Đủ điều kiện → `BIG_SYNCING`. Tự tìm: 15 s chưa có BASE → `bc_fail`. Có BASE rồi thì chờ không giới hạn (log nhắc lý do sau 15 s) |
| `BC_BIG_SYNCING` | Đã gửi lệnh BIG Create Sync | BIG lên → `STREAMING`. Lỗi → về `PA_SYNCED`, thử lại |
| `BC_STREAMING` | Đang nhận BIS và phát loa | Nguồn tạm dừng / mất BIG (PA còn) → về `PA_SYNCED` chờ BIGInfo mới. Điện thoại dừng, nút, intercom, tắt máy → `OFF` |

Thử lại: tối đa **3 lần** cho mỗi phiên (`BC_MAX_RETRY`). Lần thử lại có giãn cách (BIG: 300/600/900 ms; PA: tạo lại ngay). Quá 3 lần thì báo `Failed` trong Receive State (nếu do điện thoại yêu cầu) và phát `bc_fail`.

### 4.3 Sự kiện (`ev_bits`)

| Bit | Nguồn |
|---|---|
| `EV_PA_REQ`, `EV_PA_TERM`, `EV_SRC_REMOVED`, `EV_CODE`, `EV_BIS_REQ` | Lệnh BASS từ điện thoại (`sd_*`) |
| `EV_PA_SYNCED`, `EV_PA_LOST` | `pa_synced_cb`, `pa_term_cb` |
| `EV_BASE`, `EV_SYNCABLE`, `EV_BIG_STARTED`, `EV_BIG_STOPPED` | Callback Broadcast Sink (`bs_*`) |
| `EV_T_PHASE`, `EV_T_CHOOSE`, `EV_T_RETRY` | Hẹn giờ |
| `EV_CMD_SCAN`, `EV_CMD_STOP` | Lệnh shell (shell chạy thread khác nên cũng chỉ bật bit) |

`bcast_handle()` xử lý theo thứ tự cố định. Thứ tự này loại bỏ sự kiện "đến trễ" của phiên cũ:
1. Kết thúc phiên (lệnh stop, `PA_TERM`, `SRC_REMOVED`).
2. Nhận BASE / BIGInfo, **chỉ khi sink còn tồn tại**.
3. Kết quả từ controller (`PA_LOST`, `BIG_STOPPED`, `BIG_STARTED`).
4. Yêu cầu mới của điện thoại (`PA_REQ`), rồi mới tới mã và yêu cầu BIS đi kèm. Phiên mới không xoá mất mã vừa nhận.
5. Tự tìm, PA đồng bộ, hẹn giờ.
6. `reconcile()`.

---

## 5. Chi tiết hiện thực từng bước

### 5.1 Đăng ký BASS trước `bt_enable()`

`bcast_register()` gọi `bt_bap_scan_delegator_register()`. Hàm này **đăng ký dịch vụ GATT động** BASS (`bass_register()` → `bt_gatt_service_register()`).

Theo quy tắc đã sửa lỗi "điện thoại kết nối/ngắt liên tục" (xem chú thích trong `main()`), mọi dịch vụ động phải đăng ký **trước** `bt_enable()`. Nếu không, Database Hash sẽ lệch giữa các lần khởi động.

BASS được đăng ký **sau cùng** (sau ASCS), nên handle của các dịch vụ cũ giữ nguyên:

```
bt_tmap_register → media_ctrl_register (VCS) → call_ctrl_register (MICS)
  → le_audio_register (PACS, ASCS) → bcast_register (BASS) → bt_enable → settings_load
  → ... → le_audio_init → bcast_init
```

`bcast_init()` (sau `settings_load`) làm các việc sau:
- Đăng ký callback cho 2 luồng BIS.
- Đăng ký `bt_bap_broadcast_sink_register_cb`.
- Đăng ký `bt_le_per_adv_sync_cb_register` (PA synced / terminated).
- Đăng ký `bt_le_scan_cb_register` (tự tìm). Callback quét chỉ làm việc khi ở `BC_SCAN`, nên không đụng tới việc quét của intercom master.

> **Lưu ý một lần:** DB GATT có thêm BASS nên hash đổi **một lần**. Zephyr gửi Service Changed cho điện thoại đã ghép. Nếu điện thoại không hiện tuỳ chọn chia sẻ âm thanh cho mũ, hãy **quên thiết bị và ghép lại một lần**. Android có thể giữ danh sách profile cũ.

### 5.2 Nhận yêu cầu từ điện thoại (BASS)

`sd_pa_sync_req()` chạy trong bt_rx, khi điện thoại ghi Add Source hoặc Modify Source có xin đồng bộ PA.
- Nếu đang intercom hoặc đang tắt máy, hàm trả `-EBUSY`. Scan Delegator trả lỗi ghi cho điện thoại.
- Ngược lại, hàm chép địa chỉ, SID, Broadcast ID, PA interval, Source ID và cờ "có PAST". Hàm giữ 1 ref `bt_conn` rồi trả 0.

`on_pa_req()` (thread bộ điều khiển):
- Nếu đang nghe nguồn khác hoặc đang tự tìm: `teardown()` rồi chuyển sang nguồn điện thoại yêu cầu. Điện thoại được ưu tiên.
- **Có PAST** → `past_start()`:
  - `bt_le_per_adv_sync_transfer_subscribe(conn, skip 2, timeout)`;
  - `bt_bap_scan_delegator_set_pa_state(src, SyncInfo Request)`. Điện thoại thấy trạng thái này qua notify và gửi PAST.
- **Không có PAST**, hoặc subscribe lỗi → `pa_create()`.
- Không nhận được PAST sau 10 s:
  - huỷ subscribe;
  - đặt PA state `No PAST`;
  - tự đồng bộ bằng `pa_create()`.

`pa_create()`:
- Gọi `bt_le_per_adv_sync_create(addr, SID, skip = 2, timeout)`. Có lọc bản trùng (ADI) nếu controller hỗ trợ. **[SDK]** Host tự bật quét khi có đồng bộ PA đang chờ (`scan.c`, `BT_LE_SCAN_USER_PER_SYNC`), nên không cần tự `bt_le_scan_start`.
- Timeout giám sát PA = 4 × interval × (skip + 1), tối thiểu 2 s. Nếu không biết interval thì dùng 10 s.

Nhận ra "PA của mình" trong `pa_synced_cb()`:
- So SID.
- So kiểu nguồn: qua PAST thì `info->conn != NULL`; tự tạo thì `info->conn == NULL`.

Không so nguyên địa chỉ, vì địa chỉ có thể là RPA đã được controller phân giải. Thư viện cũng làm như vậy theo BAP 6.5.4: kiểu địa chỉ + SID + Broadcast ID.

### 5.3 Tạo Broadcast Sink và phân tích BASE

`on_pa_synced()` gọi `bt_bap_broadcast_sink_create(pa, broadcast_id, &sink)`.
- **[SDK]** Thư viện tìm Receive State khớp (kiểu địa chỉ, SID, Broadcast ID) và đặt PA state = Synced.
- Nếu không có Receive State khớp (cách tự tìm), thư viện tự thêm một Receive State cục bộ.

`bs_base_recv()` được gọi **mỗi sự kiện PA có BASE** (vài chục lần mỗi giây).
- Mũ giữ bản sao BASE gần nhất (`base_raw`) và chỉ phân tích lại khi BASE đổi. Việc này đỡ tốn CPU.
- Phân tích:
  - `bt_bap_base_foreach_subgroup` → `bt_bap_base_subgroup_codec_to_codec_cfg`: codec của subgroup (LC3?).
  - `bt_bap_base_subgroup_foreach_bis` → với từng BIS: chép codec của subgroup, rồi **ghi đè** các giá trị mức BIS (thường là Audio Location) bằng `bt_audio_codec_cfg_set_val`. Theo BAP, giá trị mức BIS thắng mức subgroup. Thư viện cũng gộp đúng như vậy khi đồng bộ.
  - `le_audio_parse_sink_cfg()` (hàm mới, dùng chung kiểm tra của unicast) đổi cấu hình sang `helmet_stream_cfg` và loại cấu hình `audio_out` không phát được.
- Biến tạm của phần phân tích để **static** (chỉ bt_rx dùng) cho đỡ tốn stack của bt_rx.

`bs_syncable()` (mỗi BIGInfo cho tới khi đồng bộ BIG) chỉ báo khi trạng thái mã hoá đổi.

### 5.4 Chọn BIS (`select_bis()`)

Chỉ xét các BIS điện thoại yêu cầu trong từng subgroup. Giá trị `0xFFFFFFFF` nghĩa là "không ưu tiên". Ở cách tự tìm, mọi BIS đều được xét. Trong một subgroup, thứ tự ưu tiên là:

1. **Một BIS stereo** (2 kênh, FL+FR).
2. **Cặp BIS FL + BIS FR**, dùng 2 luồng (`CONFIG_BT_BAP_BROADCAST_SNK_STREAM_COUNT=2`).
3. **Một BIS mono**: `audio_out` phát ra cả hai loa.
4. Chỉ có một bên (FL hoặc FR): `audio_out` tự chép sang loa còn lại.

Nếu có nhiều subgroup (vd chất lượng cao 48 kHz và chuẩn 24 kHz), mũ chọn subgroup có tần số cao nhất.

### 5.5 Đồng bộ BIG (`reconcile()`)

Điều kiện gửi `bt_bap_broadcast_sink_sync(sink, bits, streams, code)` (tất cả phải đúng):
- ở `PA_SYNCED`, PA còn sống, không đang chờ thử lại;
- có BASE và BIGInfo;
- điện thoại có yêu cầu ít nhất một BIS (cách tự tìm: luôn có);
- broadcast không mã hoá, hoặc đã có mã. Mã đến từ điện thoại (`sd_broadcast_code`) hoặc từ `helmet bc code`;
- `select_bis()` khác 0.

Khi điện thoại đổi yêu cầu BIS lúc đang nghe, có hai trường hợp:
- Yêu cầu về 0 → `stop_big()`.
- Yêu cầu khác tập BIS đang nghe → dừng rồi đồng bộ lại.

Khi BIG dừng không do mũ:
- **Sai mã** (lý do `0x3D` MIC failure) → bỏ mã cũ, **ở lại** chờ mã mới.
  - **[SDK]** Thư viện đặt "Bad Code" trong Receive State, điện thoại gửi mã mới.
  - Cách tự tìm thì phát `bc_fail` và chờ `helmet bc code`.
- **Nguồn kết thúc BIG** (lý do `0x13`, thường là tablet dừng nhạc) mà PA còn → về `PA_SYNCED`, chờ **BIGInfo mới**, không tính là lỗi. Nguồn phát lại thì tự đồng bộ lại.
- **Mất BIG / không đồng bộ được** (lý do khác) mà PA còn → cũng chờ BIGInfo mới. Thử lại sau 300·n ms, tối đa 3 lần liên tiếp.
- **PA cũng đã mất** → làm lại từ đầu (tạo lại PA).

Thông số BIG được ghi một lần mỗi phiên khi nhận BIGInfo: `BIGInfo: ... ISO interval, NSE, BN, IRC, PTO ...`. BN > 1 nghĩa là nhiều SDU đến cùng lúc.

Khi **mất PA lúc đang phát**, mũ **giữ BIG**. BIG không cần PA để chạy tiếp. Mũ chỉ làm lại khi BIG cũng mất. Cách này tránh ngắt tiếng chỉ vì sóng PA chập chờn trong lúc radio bận CIS với điện thoại.

### 5.6 Từ BIS ra loa

| Callback (bt_rx) | Việc làm |
|---|---|
| `bc_stream_started` | Đọc `stream->codec_cfg` (thư viện đã gộp subgroup + BIS) → `helmet_stream_cfg`. Bật `started`. `out_update()` → `audio_out_stream_start(khe 0/1)` |
| `bc_stream_recv` | Đếm `rx` / `bad`. Nếu `out` thì `audio_out_stream_data(khe, valid, ts, data)` |
| `bc_stream_stopped` | Tắt `started` → `audio_out_stream_stop` |

- Hai BIS trong một BIG có **cùng mốc thời gian SDU** (BIG reference point). `audio_out` ghép trái/phải theo `ts` giống hai ASE unicast.
- `audio_out_stream_data` chỉ được gọi từ bt_rx (biến tĩnh bên trong), như mọi luồng khác.

### 5.7 Dọn dẹp (`teardown()`)

Thứ tự bắt buộc:
1. Dừng hẹn giờ, dừng quét (nếu đang tự tìm), huỷ subscribe PAST.
2. Đặt `cur_pa = NULL` trước. Lần kết thúc PA bên dưới là do mũ tự làm, nên `pa_term_cb` bỏ qua.
3. `bt_bap_broadcast_sink_stop()`. **[SDK]** Callback stopped chạy ngay trong lời gọi này (`bt_iso_big_terminate` đồng bộ).
4. `bt_le_per_adv_sync_delete(pa)`. Sink vẫn còn nên thư viện kịp đặt PA state = Not synced.
5. `bt_bap_broadcast_sink_delete(sink)`.
6. Cách tự tìm: `bt_bap_scan_delegator_rem_src()` để giải phóng ô Receive State cục bộ.

`end_session()` = `teardown()` + về `BC_OFF` + âm báo + `power_activity()` (hẹn giờ tự tắt chạy lại nếu đang IDLE).

---

## 6. Ưu tiên và loại trừ với các chức năng khác

| Tình huống | Hành vi |
|---|---|
| **Có luồng unicast sink** (cuộc gọi, chuông qua LE Audio, nhạc unicast, giọng mũ master khi là slave intercom) | `le_audio.c` gọi `bcast_audio_hold(true)` **trước** khi mở khe giải mã. BIS vẫn đồng bộ nhưng thôi đưa ra loa. Hết unicast thì `bcast_audio_hold(false)` và BIS phát lại. Unicast luôn **ưu tiên** |
| **Chọn vai trò intercom** (Power → giữ Vol±) | `intercom_select()` gọi `bcast_leave()` trước: Auracast và intercom **loại trừ nhau**. Lý do: CPU (cuộc gọi đã ~98 % khi 32 kHz), thời gian radio, chung khe giải mã 0 |
| **Điện thoại xin nghe broadcast khi đang intercom** | Từ chối (`-EBUSY`), điện thoại nhận lỗi ghi BASS |
| **Tự tìm khi đang gọi / đổ chuông / intercom** | Không cho, ghi log. Khi intercom thì phát `bc_fail` |
| **Tắt máy** | `app_bt_shutdown()` gọi `bcast_leave()` trước intercom |
| **Tự tắt khi IDLE** | Không tự tắt khi **đang phát** Auracast (`bcast_mode() == BC_STREAMING`). Nghe bằng cách tự tìm không cần điện thoại nên có thể đang IDLE. Đang chờ (nguồn tạm dừng, chưa có mã) thì vẫn tự tắt như IDLE; hẹn giờ tính lại từ lúc hết tiếng |
| **Âm lượng** | Vol± và VCP từ điện thoại vẫn chỉnh `audio_out`, nên áp dụng cho cả broadcast |

---

## 7. Thay đổi cấu hình, GATT, quảng bá, bộ nhớ

### 7.1 `prj.conf`

| Kconfig | Giá trị | Lý do |
|---|---|---|
| `BT_PER_ADV_SYNC` | y | Đồng bộ PA |
| `BT_PER_ADV_SYNC_TRANSFER_RECEIVER` | y | Nhận PAST từ điện thoại. Controller SDC hỗ trợ |
| `BT_ISO_SYNC_RECEIVER` | y | Nhận BIS |
| `BT_BAP_SCAN_DELEGATOR` | y | BASS |
| `BT_BAP_SCAN_DELEGATOR_RECV_STATE_COUNT` | 2 | Nguồn cũ chưa bị xoá vẫn còn chỗ cho nguồn mới |
| `BT_BAP_BROADCAST_SINK` | y | |
| `BT_BAP_BROADCAST_SNK_STREAM_COUNT` | 2 | Trái + phải |
| `BT_BAP_BASS_MAX_SUBGROUPS` | 2 | BASE có thể có 2 subgroup |
| `BT_CTLR_SYNC_ISO_STREAM_MAX/COUNT` | 2 | Mặc định bằng `BT_ISO_MAX_CHAN` = 4, phí RAM controller |
| `BT_ISO_RX_BUF_COUNT` | 4 (từ 1) | 2 BIS + CIS cùng lúc |
| `BT_AUDIO_CODEC_CFG_MAX_METADATA_SIZE` | 64 (từ 32) | Metadata subgroup trong Add Source / BASE (Program Info, ngôn ngữ...) |
| `BT_AUDIO_CODEC_CFG_MAX_DATA_SIZE` | 32 (từ 19) | Codec BIS sau khi gộp subgroup + BIS vượt 19 B |
| `BT_ATT_PREPARE_COUNT` | 4 (từ 1) | Do metadata 64 B, ASCS bắt buộc đủ chỗ ghi dài (build assert). Lệnh BASS Add Source dài hơn MTU cũng cần |
| `BT_RX_STACK_SIZE` | 2560 (từ 2200) | bt_rx xử lý thêm BASS + BASE |

### 7.2 GATT, quảng bá, TMAP

- **BASS** được thêm vào cuối DB GATT (mục 5.1).
- **Quảng bá**: thêm `0x184F` (BASS) vào danh sách UUID16, và Service Data BASS rỗng như sample Zephyr. Áp dụng cho cả gói thường và gói intercom slave.
- **TMAP**: thêm vai trò **BMR** (Broadcast Media Receiver). **[SDK]** `BT_TMAP_BMR_SUPPORTED` tự bật khi có CAP Acceptor + Broadcast Sink + VCP Renderer.
- Gói quảng bá vẫn ≈ 60 B, trong giới hạn `BT_CTLR_ADV_DATA_LEN_MAX=191`.

### 7.3 Bộ nhớ (build ngày 19/10/2026)

| | Trước | Sau |
|---|---|---|
| FLASH | 584 636 B (37,46 %) | 631 824 B (40,49 %) |
| RAM | 236 752 B (90,31 %) | 250 683 B (**95,63 %**) |

Phần RAM tăng chủ yếu gồm:
- vùng nhớ controller SDC: thêm PA sync, BIS sink, PAST;
- cấu trúc codec lớn hơn (metadata 64 B) trong mọi ASE / luồng;
- `scan_delegator`, `broadcast_sinks`, bộ đệm ISO RX.

Mọi thứ đều cấp phát tĩnh (không dùng heap), nên 95 % vẫn chạy được. Nếu cần chỗ cho tính năng sau, có thể tắt `capture.c` để giải phóng 16 KB (`rtt_buf`).

---

## 8. Nút, âm báo, lệnh shell, log

### 8.1 Nút

| Cử chỉ | Hành động |
|---|---|
| **Power ngắn → trong 2 s nhấn và giữ Call 2 s** | Đang không nghe: **tự tìm** broadcast mạnh nhất. Đang nghe: **rời** broadcast |
| Power ngắn → nhả Call trước 2 s | Như một lần nhấn Call ngắn (play/pause...). Giữ quá 600 ms rồi nhả: bỏ qua |
| (giữ nguyên) Power ngắn → giữ Vol+ / Vol− 2 s | Intercom master / slave (rời Auracast trước) |

Khi điện thoại chỉ đường thì không cần nhấn nút: mọi thứ do điện thoại điều khiển.

### 8.2 Âm báo mới

| Tên (`helmet prompt <tên>`) | Âm | Khi nào |
|---|---|---|
| `bc_search` | 3 tiếng ngắn đi lên, lặp mỗi ~1,4 s | Đang tự tìm (tới khi vào được hoặc lỗi) |
| `bc_joined` | 2 tiếng đi lên (1319 → 1760 Hz) | Đã nhận được BIS (cả hai cách) |
| `bc_left` | 2 tiếng đi xuống | Rời broadcast (nút, điện thoại dừng chia sẻ) |
| `bc_fail` | 3 tiếng trầm 494 Hz | Không tìm thấy, sai mã, thử lại quá 3 lần |

### 8.3 Lệnh shell

| Lệnh | Việc làm |
|---|---|
| `helmet bc` hoặc `helmet bc status` | Trạng thái, nguồn, BASE/BIGInfo/mã, BIS yêu cầu/đã đồng bộ, số SDU từng luồng |
| `helmet bc scan [tên]` | Tự tìm, có thể lọc theo một phần tên broadcast |
| `helmet bc code <mật khẩu>` / `helmet bc code clear` | Mã broadcast (≤ 16 ký tự) cho cách tự tìm. Được lưu vào flash; nhập lúc đang chờ thì dùng ngay / xoá |
| `helmet bc stop` | Rời broadcast |
| `helmet status` | Có thêm dòng `Auracast  : ...` |

### 8.4 Log cần nhìn

Khởi động (bắt buộc phải có):
```
<inf> bcast: Auracast sink: BASS (2 receive states), up to 2 BIS, PAST yes
```
Điện thoại chia sẻ âm thanh (thứ tự mong đợi):
```
<inf> bcast: Phone (assistant): join broadcast 0x......, SID x, src 1, PAST yes
<inf> bcast: Phone (assistant): BIS sync request src 1: 0xffffffff 0x00000000   (hoặc 0x00000003)
<inf> bcast: Waiting for PAST from phone (SID x)
<inf> bcast: Auracast OFF -> PA_WAIT
<inf> bcast: PA synced: interval ... ms, PHY 2
<inf> bcast: Auracast PA_WAIT -> PA_SYNCED
<inf> bcast: BASE: 1 subgroup(s), presentation delay 40000 us
<inf> bcast:   subgroup 0 BIS 1: LC3 48000 Hz, 10000 us, 1 ch (loc 0x1), 100 B
<inf> bcast:   subgroup 0 BIS 2: LC3 48000 Hz, 10000 us, 1 ch (loc 0x2), 100 B
<inf> bcast: BIGInfo: encrypted, 2 BIS, ISO interval ... us, SDU interval 10000 us, NSE ., BN ., IRC ., PTO ., ...
<inf> bcast: Phone (assistant): broadcast code received (src 1)
<inf> bcast: Syncing to BIG: BIS 0x00000003 (encrypted)
<inf> bcast: BIS 1 -> stream 0: LC3 48000 Hz ...
<inf> audio_out: Stream 0 decoding: 48000 Hz, 10000 us, 1 ch, 100 B/frame, alloc 0x1 -> L
<inf> bcast: Receiving broadcast "" 0x......: BIS 0x00000003
```
Heartbeat (mỗi 10 s) khi đang Auracast:
```
<inf> bcast: auracast: STREAMING 0x...... "...", BIS 0x00000003, rx 1000/1000, bad 0/0
```

---

## 9. Kiểm thử

### 9.0 Chuẩn bị

1. Nạp `apps/main` cho **cả hai mũ**. Mũ thứ hai nên đặt tên khác để dễ phân biệt trên điện thoại, và dùng thư mục build riêng:
   ```
   make APP=apps/main BUILD_DIR=build/main2 EXTRA='-DCONFIG_BT_DEVICE_NAME=\"Helmet_2\"' build
   make APP=apps/main BUILD_DIR=build/main2 JLINK_SN=<sn mũ 2> flash
   ```
2. Trên điện thoại: **quên** hai mũ rồi ghép lại (DB GATT đã đổi, mục 5.1). Mỗi mũ ghép như bình thường.
3. Điện thoại cần hỗ trợ **LE Audio broadcast + Broadcast Assistant**:
   - Trên Android, mục này thường tên là "Chia sẻ âm thanh" / "Audio sharing" / "Auracast" trong cài đặt Bluetooth của thiết bị LE Audio.
   - Mình **chưa kiểm chứng** được máy nào hỗ trợ thiết bị bên thứ ba như mũ. Cần bạn thử trên máy của mình. Hãy cho mình biết **hãng / đời máy / phiên bản Android** để mình đối chiếu.
4. Mở `make rtt` cho mũ đang thử. Không mở hai trình đọc RTT trên cùng một mạch.

### 9.1 Kiểm tra tĩnh (một mũ, không điện thoại)

| Bước | Kỳ vọng |
|---|---|
| Khởi động | Có dòng `Auracast sink: BASS (2 receive states), up to 2 BIS, PAST yes`. Không có lỗi `Scan delegator (BASS) register failed` |
| `helmet bc` | `Auracast  : off` |
| `helmet prompt bc_joined` / `bc_left` / `bc_fail` / `bc_search` rồi `helmet prompt stop` | Nghe đúng âm |
| Power ngắn → giữ Call 2 s | Log `Auracast: searching for broadcasts (20 s)`, âm `bc_search`. Không có broadcast thì sau 20 s có `No Auracast broadcast found`, âm `bc_fail` |
| Power ngắn → giữ Call 2 s, rồi làm lại lần nữa trong lúc đang tìm | Lần hai: `leaving broadcast (button)`, âm `bc_left` |
| Điện thoại kết nối lại sau khi nạp | Kết nối ổn định, nhạc/gọi như trước. Đây là kiểm tra hồi quy: hash GATT chỉ đổi một lần |

### 9.2 Kiểm thử chính: chia sẻ âm thanh từ điện thoại

1. Kết nối cả hai mũ với điện thoại. Phát nhạc: một mũ nghe được như cũ (unicast).
2. Bật "Chia sẻ âm thanh" trên điện thoại, chọn cả hai mũ.
3. Kỳ vọng ở **mỗi** mũ: chuỗi log mục 8.4, âm `bc_joined`, cả hai mũ cùng phát nhạc.
4. `helmet bc`: `STREAMING, phone-assisted`, `rx` tăng ~100/s mỗi luồng (khung 10 ms), `bad` thấp.
5. `helmet status`: phần I2S / SDU / PLC như khi nghe unicast.
6. Tắt chia sẻ trên điện thoại: `Phone ended broadcast ...`, âm `bc_left`, mũ quay lại nghe unicast nếu điện thoại tiếp tục phát.

Nếu không chạy, ghi lại log từ dòng `Phone (assistant)` đầu tiên và đối chiếu bảng sau:

| Triệu chứng trong log | Ý nghĩa / việc làm |
|---|---|
| Không có dòng `Phone (assistant)` nào | Điện thoại không coi mũ là Scan Delegator. Hãy quên thiết bị, ghép lại, kiểm tra menu chia sẻ âm thanh |
| `No PAST from phone - syncing by scanning`, rồi `PA synced` | Điện thoại không gửi PAST. Mũ tự quét được: vẫn ổn |
| `PA sync failed` lặp lại, `Giving up` | Mũ không thấy bộ quảng bá của điện thoại (địa chỉ đổi? quá xa?). Gửi log cho mình |
| `Broadcast is encrypted - waiting for the code from the phone` mãi | Điện thoại chưa gửi mã |
| `Wrong broadcast code (BIG MIC failure)` | Thứ tự byte của mã khác giả định (mục 10). Báo mình |
| `No playable BIS among those requested` | Cấu hình LC3 lạ (xem dòng `subgroup x BIS y: not playable`) |
| `BIG sync failed (reason 0x3e ...)` lặp | Radio quá bận (CIS + BIS + PA). Thử dừng nhạc unicast, đứng gần hơn |

### 9.3 Kiểm thử tự tìm

1. Cho điện thoại hoặc một thiết bị khác phát Auracast. Nếu có mật khẩu, nhập trước: `helmet bc code <mật khẩu>`.
2. Power ngắn → giữ Call 2 s, hoặc `helmet bc scan <một phần tên>`.
3. Kỳ vọng: `Found broadcast "..."`, sau 2 s `Joining broadcast ...`, rồi chuỗi PA → BASE → BIG như 9.2, âm `bc_joined`.
4. Power ngắn → giữ Call 2 s lần nữa: rời, âm `bc_left`.

### 9.4 Ưu tiên / loại trừ

| Thử | Kỳ vọng |
|---|---|
| Đang Auracast, có cuộc gọi đến (nếu điện thoại vẫn giữ kết nối) | `Auracast audio paused (unicast stream started)`: nghe chuông/cuộc gọi. Gác máy: `Auracast audio resumed` |
| Đang Auracast, Power ngắn → giữ Vol+ 2 s | `Intercom: leaving Auracast broadcast first`, rồi intercom master tìm như cũ |
| Đang intercom, điện thoại bật chia sẻ | `rejected (intercom active)` |
| Đang Auracast (tự tìm, không điện thoại), để quá thời gian tự tắt | Không tự tắt |
| Tắt máy khi đang Auracast | `Auracast: leaving broadcast`, tắt bình thường |

### 9.5 Đo thêm (khi đã chạy)

- `helmet cpu` khi đang Auracast 48 kHz stereo. Kỳ vọng tương đương nghe nhạc unicast.
- `kernel thread stacks`: phần còn trống của `BT RX` (`rx_thread`) sau khi vào/ra broadcast vài lần.
- Lệch tiếng giữa hai mũ. Đặt hai mũ cạnh nhau: mỗi mũ tự giải mã, lệch vài ms là bình thường (mục 10).

---

### 9.6 Kết quả thử ngày 19/10/2026 (Xiaomi Pad 7 Pro, cách tự tìm)

| Lần | Hiện tượng | Nguyên nhân | Xử lý |
|---|---|---|---|
| 1 | PA + BASE có, `no BIGInfo` sau 15 s | Tablet chưa phát nhạc: chưa có BIG | Phát nhạc trước. Bản mới chờ không giới hạn |
| 2 | `encrypted - set the code` | Broadcast có mật khẩu | `helmet bc code <mật khẩu>` (bản mới: lưu, dùng ngay) |
| 3 | Nghe được, nhưng `Next buffers not supplied on time` / `I2S write failed` 1–3 lần/s, tiếng tạch. `rx` đủ, `bad` 4. CPU idle 53 %. Decode trung bình 1,5 ms, lớn nhất 4,2 ms / SDU | `audio_out` giải mã hết hàng đợi rồi mới nạp I2S, mà I2S chỉ cho ~10 ms. SDU broadcast đến dồn cục | Nạp I2S sau mỗi SDU + giữ 4 khối (chịu ~20 ms). Nạp bản sửa ngày 19/10/2026, chờ kiểm tra `I2S err` không tăng |

## 10. Giới hạn và rủi ro

| # | Nội dung |
|---|---|
| 1 | **Chưa thử với điện thoại thật.** Code bám theo sample Zephyr và mã thư viện, nhưng mỗi hãng điện thoại làm Assistant hơi khác (thứ tự lệnh, PAST, mã) |
| 2 | **Hỗ trợ của điện thoại**: không phải Android nào cũng cho chia sẻ âm thanh tới thiết bị LE Audio bên thứ ba |
| 3 | **Thứ tự byte của Broadcast Code** khi nhập bằng `helmet bc code`: mũ chép chuỗi nguyên văn (byte 0 = ký tự đầu), như sample Zephyr. Mã điện thoại gửi qua BASS thì đã đúng định dạng |
| 3b | **Độ trễ**: `audio_out` giữ 4 khối I2S (trước là 3): trễ trong mũ tăng ~10 ms cho mọi luồng (nhạc, cuộc gọi, broadcast), đổi lại hết tiếng tạch khi SDU đến dồn cục |
| 4 | **Đồng bộ tiếng giữa hai mũ**: mũ chưa áp dụng *presentation delay* của BASE. Mỗi mũ phát ngay sau bộ đệm của `audio_out`. Hai mũ cùng firmware nên độ trễ gần nhau, nhưng không khớp mẫu. Có thể bổ sung sau nếu nghe thấy vọng |
| 5 | **Radio**: CIS (điện thoại) + PA + BIG cùng lúc. Nordic có thử 1 CIS + 1 BIS. Nhiều hơn chưa có số liệu. Khi chia sẻ, điện thoại thường dừng CIS nhạc nên tải giảm |
| 6 | **RAM 95,6 %**. Còn ~11 KB. Dự phòng: tắt `capture.c` (16 KB) |
| 7 | **BASE bị phân mảnh** (> ~240 B, nhiều subgroup có metadata dài) không được ráp lại (`BT_PER_ADV_SYNC_BUF_SIZE=0`). Broadcast thường gặp có BASE ~50–100 B |
| 8 | Auracast và intercom **loại trừ nhau** (giai đoạn này). Nghe broadcast trong lúc đàm thoại mũ–mũ là việc của giai đoạn sau |
| 9 | Cùng lúc chỉ nghe **một** broadcast (`BT_BAP_BROADCAST_SNK_COUNT=1`, `BT_PER_ADV_SYNC_MAX=1`) |

---

## 11. Phụ lục: vị trí trong SDK

| Nội dung | File (NCS v3.4.0 / zephyr) |
|---|---|
| BASS đăng ký dịch vụ GATT động | `subsys/bluetooth/audio/bap_scan_delegator.c`: `bass_register()`, `bt_bap_scan_delegator_register()` |
| Add Source → `pa_sync_req`, rồi `bis_sync_req` | `bap_scan_delegator.c`: `scan_delegator_add_src()` |
| PAST chỉ dùng khi hai bên hỗ trợ | `bap_scan_delegator.c`: `supports_past()` |
| Remove Source bị từ chối khi PA/BIS còn đồng bộ | `bap_scan_delegator.c`: `scan_delegator_rem_src()` |
| Sink create tìm Receive State theo kiểu địa chỉ + SID + Broadcast ID | `subsys/bluetooth/audio/bap_broadcast_sink.c`: `find_recv_state_by_sink_fields_cb()`, `bt_bap_broadcast_sink_create()` |
| Sink tự thêm Receive State khi không có assistant | `bap_broadcast_sink.c`: `broadcast_sink_add_src()` |
| BASE gọi lại mỗi lần nhận; chỉ phân tích khi đổi | `bap_broadcast_sink.c`: `pa_decode_base()` |
| Gộp codec subgroup + BIS khi đồng bộ | `bap_broadcast_sink.c`: `sync_base_subgroup_bis_index_cb()` |
| Cần BIGInfo trước; cần mã nếu mã hoá | `bap_broadcast_sink.c`: `bt_bap_broadcast_sink_sync()` |
| Host tự quét khi có đồng bộ PA đang chờ | `subsys/bluetooth/host/scan.c`: `BT_LE_SCAN_USER_PER_SYNC` |
| Kích thước bộ nhớ SDC cho BIS sink / PA sync / PAST | `nrf/subsys/bluetooth/controller/bt_ctlr_used_resources.h` |
| TMAP BMR tự bật | `subsys/bluetooth/audio/Kconfig.tmap`: `BT_TMAP_BMR_SUPPORTED` |
| ASCS cần `BT_ATT_PREPARE_COUNT` đủ lớn | `subsys/bluetooth/audio/ascs.c`: `BUILD_ASSERT` sau `ASE_BUF_SIZE` |
