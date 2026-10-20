# Helmet – đo khoảng cách mũ-mũ bằng Channel Sounding (Bluetooth 6.0)

Nhánh `feature/Sounding-Channel` (tách từ `main`, có intercom, chưa có Auracast). Code: `apps/main/src/ranging.c`.

## 1. Mục tiêu giai đoạn 1

- Khi hai mũ **đang gọi intercom**, đo khoảng cách giữa hai mũ bằng Channel Sounding (CS) và **in ra log trên cả hai mũ**, khoảng 1 lần mỗi giây.
- Chưa làm: âm báo hoặc đèn theo khoảng cách, đo khi không gọi.

## 2. Cách đo: CS Inline PCT Transfer (IPT)

- **Mode 2 (PBR, phase-based ranging):** hai bên lần lượt phát tone trên 72 kênh cách nhau 1 MHz, dùng 1 đường ăng-ten.
- **IPT ("CS enhancements"):** bên phản xạ chỉnh pha tone gửi lại theo pha tone vừa nhận. Nhờ vậy bên khởi tạo đo được tổng pha của cả hai chiều và **tự tính khoảng cách** bằng IFFT (`cs_de_ifft`). Bên khởi tạo không cần nhận số đo của bên kia qua GATT (Ranging Service).

| | IPT (đang dùng) | Ranging Service (RAS) |
|---|---|---|
| Truyền số đo của bên phản xạ | Không cần | Qua GATT, MTU ≥ 247, bộ đệm ACL 502 B |
| RAM | Ít (khoảng 12,5 KB tổng cộng) | Nhiều hơn đáng kể, không vừa với mũ (RAM 90 %) |
| Thiết lập | Không phải dò GATT | Dò dịch vụ, đăng ký thông báo |
| Bảo mật chống giả khoảng cách | Thấp hơn (chỉ PBR, không có RTT) | Cao hơn (kết hợp mode 1 RTT) |

Với intercom giữa hai mũ, mức bảo mật của IPT là đủ.

## 3. Vai trò và luồng

| | Master intercom (central) | Slave intercom (peripheral) |
|---|---|---|
| Vai trò CS | **Khởi tạo** (đo và tính) | **Phản xạ** |
| Bắt đầu khi | Âm thanh intercom đã chạy **và** ACL đã đổi xong sang 60 ms | LINKED |
| Log khoảng cách | Tự tính | Do master gửi sang (HIS) |

```
Slave  (LINKED)      : bt_le_cs_set_default_settings(reflector)
Master (đang phát)   : bt_conn_le_param_update(60 ms) -> chờ le_param_updated (tối đa 3 s)
                     -> default settings (initiator) -> đọc khả năng CS bên kia (phải có IPT)
                     -> tạo cấu hình 0 (mode 2, IPT) cho cả hai bên -> bật bảo mật CS
                     -> tham số thủ tục -> bật thủ tục (lặp tới khi ngắt)
Mỗi thủ tục xong     : PCT từng kênh -> cs_de_ifft() -> trung vị 9 mẫu -> log
                     -> ghi HIS Control Point: IC_OP_RANGE [0x30][0][cm thấp][cm cao]
Slave nhận           : log "Distance to peer helmet: x.xx m (measured by master)"
Ngắt liên kết        : CS tự dừng
```

Master chỉ bắt đầu CS sau khi đã đổi chu kỳ ACL, để không chạy thủ tục LL của CS song song với thủ tục đổi tham số kết nối. Gói `IC_OP_RANGE` chỉ dùng để hiển thị: `ic_service.c` chuyển thẳng cho `ranging.c`, không đi qua hàng đợi intercom. Mũ chạy firmware cũ nhận op này thì bỏ qua.

## 4. Tham số

| Tham số | Giá trị | Lý do |
|---|---|---|
| Chu kỳ thủ tục | 16 sự kiện ACL × 60 ms ≈ **1 s** (`helmet cs period <ms>`) | Đủ để hiển thị, ít chiếm radio |
| Độ dài tối đa một thủ tục | 6 chu kỳ ACL | Cho controller chia thủ tục ra nhiều sự kiện CS |
| Subevent | **2,5–5 ms** | Ngắn hơn khoảng trống giữa hai sự kiện CIS (10 ms, CIS chiếm khoảng 2 ms) |
| Kênh | Tất cả kênh hợp lệ (72) (`helmet cs ch 2` / `4` để bớt kênh) | Cách 1 MHz thì dải không nhầm lẫn là 150 m; cách 2 MHz là 75 m |
| Cấu hình | Mode 2, 3 bước mode 0, chọn kênh #3b, CS sync 1M PHY, thủ tục 2M PHY | Như mẫu `ipt_initiator` của Nordic |
| Lọc | Trung vị 9 mẫu gần nhất | Bỏ bớt mẫu sai do đa đường |

## 5. Kconfig và bộ nhớ (`apps/main/prj.conf`)

| Kconfig | Giá trị | Ghi chú |
|---|---|---|
| `BT_CHANNEL_SOUNDING` | y | Host CS (EXPERIMENTAL) |
| `BT_CTLR_SDC_CS_ROLE_BOTH` | y | Hai mũ cùng firmware: master khởi tạo, slave phản xạ |
| `BT_CTLR_SDC_CS_MAX_ANTENNA_PATHS`, `NUM_ANTENNAS` | 1 | Module có 1 ăng-ten |
| `BT_CTLR_SDC_CS_STEP_MODE3`, `CHANNEL_SOUNDING_TEST` | n | Bớt RAM / FLASH |
| `BT_CTLR_EXTENDED_FEAT_SET` | y | Bắt buộc cho IPT |
| `BT_TRANSMIT_POWER_CONTROL` | y | Cho phép CS và ACL dùng PHY khác nhau |
| `BT_CHANNEL_SOUNDING_REASSEMBLY_BUFFER_SIZE` | 2000 | Mỗi bước mode 2 khoảng 12 B. Mặc định 5600 |
| `BT_CS_DE`, `BT_CS_DE_512_NFFT` | y | IFFT 512 điểm, bộ nhớ tạm 4 KB (1024 điểm cần 8 KB) |
| `NRF_RRAM_WRITE_BUFFER_SIZE` | 1 | Mỗi lần ghi RRAM chỉ cần 0,6 ms radio trống thay vì 8,1 ms. Khi CIS hoặc CS chạy theo chu kỳ 10 ms thì không bao giờ có khoảng trống 8,1 ms (lỗi đã gặp trên hub) |

Build ngày 20/10/2026: **FLASH 651 692 B (41,8 %), RAM 249 276 B (95,1 %)**. Trước khi thêm CS: 37,5 % / 90,3 %.

Phần RAM tăng thêm: `iq` 4096 B, bộ đệm ráp 2000 B, phần CS trong `sdc_mempool`.

## 6. Log và shell

Master:
```
Channel Sounding: initiator - starting after the call link settles
Channel Sounding: peer reflector yes, IPT yes (t_ip2 0x...., t_sw .. us)
Channel Sounding: config 0 created (initiator, mode 2, T_IP2 .. us, T_FCS .. us, T_PM .. us)
Channel Sounding: ACL 60.00 ms -> 1 procedure every 16 events (~960 ms), subevent 2500-5000 us, 72 channels
Channel Sounding: RUNNING as initiator - subevent .. us x ../event, ...
Distance to peer helmet: 1.20 m (now 1.25 m, median of 9, +960 ms)
```
Slave:
```
Channel Sounding: reflector ready - the master helmet measures
Channel Sounding: RUNNING as reflector - ...
Distance to peer helmet: 1.20 m (measured by master)
```
Heartbeat 10 s (khi đang gọi): `ranging: RUNNING, distance 1.20 m, procedures 120 ok / 3 aborted / 0 dropped, 1 invalid`.

Shell:
- `helmet cs`: trạng thái, khoảng cách, số thủ tục thành công / bị huỷ (kèm lý do), lịch do controller chọn.
- `helmet cs ch <1|2|4>`, `helmet cs period <ms>`: áp dụng **từ cuộc gọi sau**.

## 7. Kiểm thử

1. Nạp `make APP=apps/main flash` cho **cả hai mũ** (một J-Link nên nạp lần lượt).
2. Mũ A: Power ngắn → giữ Vol+ 2 s (MASTER). Mũ B: Power ngắn → giữ Vol− 2 s (SLAVE).
3. Chờ `Intercom LINKED` và âm thanh chạy. Sau khoảng 1–2 s phải có `RUNNING` rồi `Distance ...` mỗi giây trên cả hai mũ.
4. Đổi khoảng cách 0,5 m → 1 m → 3 m → 5 m. Ghi lại giá trị trung vị, kèm số mẫu `aborted` và `invalid`.
5. **Nghe intercom trong lúc đo**: có tiếng tách khoảng mỗi giây không? So `bad` / PLC phía âm thanh khi CS chạy và khi không.

## 8. Rủi ro và giới hạn

- **CS và CIS cùng lúc chưa đo trên mạch này.** Controller phải xếp lịch cả hai trên cùng liên kết:
  - Nếu `aborted` tăng nhanh, nghĩa là CS nhường cho CIS: thử `helmet cs ch 2` hoặc `ch 4` (thủ tục ngắn lại một nửa hoặc còn một phần tư).
  - Nếu tiếng bị tách mỗi giây, nghĩa là CIS nhường cho CS: cũng bớt kênh, hoặc tăng `period`.
- **Độ chính xác chưa hiệu chuẩn.** Đo một ăng-ten bằng IFFT thường lệch vài chục cm tới khoảng 1 m trong nhà vì đa đường. Đầu và thân người che sóng cũng làm sai số tăng. Có thể thêm bù lệch cố định sau khi đo thực tế.
- `BT_CHANNEL_SOUNDING` và `BT_CS_DE` đang là EXPERIMENTAL trong NCS v3.4.0.
