# apps/main: Helmet đầy đủ chức năng

Firmware hiện thực bản thiết kế [docs/HELMET_THIET_KE_CHUC_NANG_MOI.md](../../docs/HELMET_THIET_KE_CHUC_NANG_MOI.md), thêm intercom ([docs/HELMET_THIET_KE_INTERCOM.md](../../docs/HELMET_THIET_KE_INTERCOM.md)), Auracast / vào xe ([docs/HELMET_AURACAST.md](../../docs/HELMET_AURACAST.md)) và Định vị bằng Channel Sounding ([docs/HELMET_NAV_DINH_VI.md](../../docs/HELMET_NAV_DINH_VI.md)). Định vị là chức năng riêng: bật thì kết thúc intercom và tạm rời Auracast, tắt thì tự nghe lại nhạc của xe. Mã chia theo lớp: phần riêng của mũ nằm trong `src/`, phần dùng chung với xe nằm trong `lib/` ở gốc project (xem [README.md](../../README.md), mục *Kiến trúc mã nguồn*).

| Chức năng | File |
|---|---|
| Bộ điều khiển (máy trạng thái thiết bị, hàng sự kiện, heartbeat), quảng bá + kết nối, cử chỉ → hành động, shell, cài đặt, quy trình tắt máy | `src/app/`: `app_ctrl.c`, `app_bt.c`, `app_gesture.c`, `app_shell.c`, `app_config.c`, `power.c`; API cho lớp chức năng: `app.h` |
| Nghe nhạc + cuộc gọi với điện thoại: BAP Unicast Server, VCP + MCP, TBS client + MICP | `src/features/phone/`: `le_audio.c`, `media_ctrl.c`, `call_ctrl.c` |
| Intercom mũ–mũ | `src/features/intercom/`: `intercom.c`, `ic_service.c`, `ic_audio.c` |
| Nghe Auracast, vào xe bằng nút | `src/features/broadcast/`: `broadcast_sink.c`, `hub_link.c` |
| Nhiệt độ + Health Thermometer Service | `src/features/thermo/thermo.c` |
| Định vị: khoảng cách tới xe, tới mũ kia, mũ kia tới xe (Channel Sounding IPT, giữ Call 1 s) | `src/features/nav/nav.c` |
| Chẩn đoán: ghi SDU ra RTT (chỉ khi build `EXTRA=-DCONFIG_HELMET_CAPTURE=y`), log truy cập GATT | `src/debug/`: `capture.c`, `gatt_trace.c` |
| Đường ra loa: giải mã LC3, bù trôi clock từng mẫu, âm báo, lấy mẫu lại theo tần số I2S thật | `lib/services/audio_out.c` + `lib/core/pcm_ring.c`, `resampler.c`, `mixer.c`, `prompts.c` + `lib/hal/i2s_out.c` |
| Đàm thoại (chiều lên): micro PDM, khử vọng NLMS, NLP, giảm ồn, AGC, mã hóa LC3, gửi SDU | `lib/services/voice_tx.c` + `lib/core/voice_dsp.c` + `lib/hal/mic_in.c` |
| Cử chỉ nút: ngắn, đúp, giữ, giữ lâu, lặp, tổ hợp | `lib/services/buttons.c` + `lib/hal/keys.c` |
| Phần cứng nguồn (System OFF, giữ Power 2 s, đo VDD), LED, MLX90632 | `lib/hal/power_hw.c`, `status_led.c`, `mlx90632.c` + `lib/core/mlx90632_calc.c` |

## Build, nạp

```bash
make build        # apps/main là app mặc định; thư mục build: build/main
make flash
make rtt
```

Bản build đã kiểm tra (27/10/2026, sau khi tổ chức lại theo lớp): FLASH 41.45 % (646 924 B), RAM 95.76 % (251 027 B), không có cảnh báo từ mã của app.
Nhánh `test/Broadcast-Sounding` (thêm Định vị, capture tắt mặc định): FLASH 46.14 % (720 052 B), RAM 98.96 % (259 427 B).

IntelliSense: `${workspaceFolder}/build/main/main/compile_commands.json` đã có trong danh sách `compileCommands` của `.vscode/c_cpp_properties.json` (đứng đầu danh sách, và danh sách dự phòng `includePath` cũng trỏ vào `apps/main/src` + `build/main`). Cần chạy `make APP=apps/main build` ít nhất một lần để file đó tồn tại.

## Thử khi chưa có phần cứng

| Lệnh | Tác dụng |
|---|---|
| `helmet btn <volup\|voldown\|call\|power> <short\|double\|long\|vlong\|repeat\|combo>` | Giả lập cử chỉ nút: thử toàn bộ bảng hành động mà không cần board nút |
| `helmet prompt <tên>` / `helmet prompt stop` | Phát thử âm báo (`power_on`, `ring`, `mic_muted`…) |
| `helmet call [accept\|reject\|end]` | Trạng thái và lệnh cuộc gọi |
| `helmet mic [mute\|unmute]` | Tắt/bật micro qua MICP |
| `helmet voice`, `helmet dsp [<aec\|ns\|agc> <on\|off>]` | Thống kê chiều lên; bật/tắt từng khâu DSP |
| `helmet power [off]`, `helmet autooff <phút>`, `helmet vdd` | Nguồn |
| `helmet temp [raw\|cal\|emis <‰>\|alarm <°C>]` | Nhiệt độ |
| `helmet nav [status\|on\|off\|ch <1\|2\|4>\|period <ms>]` | Định vị: bật / tắt, trạng thái từng liên kết và khoảng cách |
| `helmet perf` | Thời gian CPU của giải mã, khối ra I2S, DSP giọng nói, mã hóa |
| `helmet status`, `kernel stacks` | Tổng quan; mức dùng stack của từng thread |

## Đã kiểm tra

| Kiểm tra | Kết quả |
|---|---|
| Build NCS v3.4.0, board `me54bs62/nrf54l15/cpuapp` | ✅ 0 lỗi, 0 cảnh báo của app |
| `voice_dsp.c` chạy trên PC, tiếng vọng tổng hợp (trễ 40 mẫu + phản xạ) | ✅ AEC hội tụ; NLP một mình giảm 16.5 dB |
| `voice_dsp.c`, cả hai cùng nói | ✅ Giọng người đội mũ giữ nguyên (−17.0 dBFS, bằng khi nói một mình) |
| `voice_dsp.c`, giảm ồn trên tín hiệu ngắt quãng | ✅ Đoạn có tiếng −0.17 dB, ồn nền −9.5 dB |
| Công thức MLX90632 (nay là `lib/core/mlx90632_calc.c`) với 17 bộ số kiểm thử chính thức của Melexis (`test/TestDSP.c`) | ✅ Tất cả đạt (dung sai 0.01) |
| Chạy trên mạch, điện thoại thật | ⏳ Chưa: firmware chưa được nạp |

## Còn phải làm / cần đo trên mạch

- Mọi số liệu đánh dấu **[Cần đo]** trong tài liệu thiết kế: CPU khi gọi (`helmet perf`), stack (`kernel stacks`), dòng khi tắt, độ trễ.
- Tự tắt khi VDD < 3.0 V đang **tắt** (`LOWBATT_POWER_OFF 0` trong `src/app/power.c`). Chỉ bật sau khi `helmet vdd` đo đúng trên mạch, để tránh tắt máy nhầm.
- Ghi tín hiệu micro và tham chiếu qua RTT để chỉnh AEC (thiết kế mục 4.10) chưa làm: cần sửa `scripts/helmet_audio.py` (nằm ngoài `apps/main`).
- Chưa dùng CMSIS-DSP. `voice_dsp.c` tự viết NLMS, biquad và FFT 512 bằng C thuần, để chạy được trên PC và không phải cấu hình bảng FFT của CMSIS.
