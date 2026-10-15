# apps/main: Helmet đầy đủ chức năng

Firmware hiện thực bản thiết kế [docs/HELMET_THIET_KE_CHUC_NANG_MOI.md](../../docs/HELMET_THIET_KE_CHUC_NANG_MOI.md). Phần nghe nhạc kế thừa từ `apps/Helmet`; `apps/Helmet` giữ nguyên, không bị sửa.

| Chức năng | File |
|---|---|
| Nghe nhạc LE Audio; `audio_out` thế hệ 2: lấy mẫu lại theo tần số I2S thật, bù trôi clock từng mẫu, âm báo | `le_audio.c`, `audio_out.c`, `prompts.c` |
| Đàm thoại: ASE source, micro PDM, khử vọng NLMS, NLP, giảm ồn, AGC, mã hóa LC3, gửi SDU | `mic_in.c`, `voice_dsp.c`, `voice_tx.c` |
| Cuộc gọi: TBS client (nghe / từ chối / kết thúc), MICP (tắt micro) | `call_ctrl.c` |
| Cử chỉ nút: ngắn, đúp, giữ, giữ lâu, lặp, tổ hợp | `buttons.c` |
| Nguồn: System OFF, bật bằng giữ Power 2 s, tự tắt, quảng bá chậm, cảnh báo VDD | `power.c` |
| Nhiệt độ MLX90632 + Health Thermometer Service | `mlx90632.c`, `thermo.c` |
| Bộ điều khiển ứng dụng (máy trạng thái thiết bị), quảng bá, shell | `main.c` |

## Build, nạp

```bash
make APP=apps/main build        # thư mục build: build/main
make APP=apps/main flash
make APP=apps/main rtt
```

Bản build đã kiểm tra: RRAM 31.4 % (490 KB), RAM 80.8 % (212 KB), không có cảnh báo từ mã của app.

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
| `helmet perf` | Thời gian CPU của giải mã, khối ra I2S, DSP giọng nói, mã hóa |
| `helmet status`, `kernel stacks` | Tổng quan; mức dùng stack của từng thread |

## Đã kiểm tra

| Kiểm tra | Kết quả |
|---|---|
| Build NCS v3.4.0, board `me54bs62/nrf54l15/cpuapp` | ✅ 0 lỗi, 0 cảnh báo của app |
| `voice_dsp.c` chạy trên PC, tiếng vọng tổng hợp (trễ 40 mẫu + phản xạ) | ✅ AEC hội tụ; NLP một mình giảm 16.5 dB |
| `voice_dsp.c`, cả hai cùng nói | ✅ Giọng người đội mũ giữ nguyên (−17.0 dBFS, bằng khi nói một mình) |
| `voice_dsp.c`, giảm ồn trên tín hiệu ngắt quãng | ✅ Đoạn có tiếng −0.17 dB, ồn nền −9.5 dB |
| `mlx90632.c` với 17 bộ số kiểm thử chính thức của Melexis (`test/TestDSP.c`) | ✅ Tất cả đạt (dung sai 0.01) |
| Chạy trên mạch, điện thoại thật | ⏳ Chưa: firmware chưa được nạp |

## Còn phải làm / cần đo trên mạch

- Mọi số liệu đánh dấu **[Cần đo]** trong tài liệu thiết kế: CPU khi gọi (`helmet perf`), stack (`kernel stacks`), dòng khi tắt, độ trễ.
- Tự tắt khi VDD < 3.0 V đang **tắt** (`LOWBATT_POWER_OFF 0` trong `power.c`). Chỉ bật sau khi `helmet vdd` đo đúng trên mạch, để tránh tắt máy nhầm.
- Ghi tín hiệu micro và tham chiếu qua RTT để chỉnh AEC (thiết kế mục 4.10) chưa làm: cần sửa `scripts/helmet_audio.py` (nằm ngoài `apps/main`).
- Chưa dùng CMSIS-DSP. `voice_dsp.c` tự viết NLMS, biquad và FFT 512 bằng C thuần, để chạy được trên PC và không phải cấu hình bảng FFT của CMSIS.
