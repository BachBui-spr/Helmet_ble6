# ME54BS62 trên Helmet board: sơ đồ chân và ràng buộc của nRF54L15

Nguồn tham khảo:
- [ME54BS62-nRF54L15_Datasheet_K_EN.pdf](ME54BS62-nRF54L15_Datasheet_K_EN.pdf) (Minew v1.0.0)
- [PCB helmet main schematic.pdf](PCB%20helmet%20main%20schematic.pdf) cùng các schematic button / micro / charger
- nRF54L15 Datasheet v1.0, mục 10.1 "Pin assignments" (bảng chân gói CSP47 CAAA)

## Bên trong module (không có pad ra ngoài)

| Thành phần | Nối vào | Ghi chú |
|---|---|---|
| Thạch anh 32 MHz (Y1) | XC1/XC2 | HFXO, dùng tụ tải nội (`&hfxo`) |
| Thạch anh 32.768 kHz (Y2) | P1.00 / P1.01 (XL1/XL2) | LFXO. **P1.00 và P1.01 không có trên pad** |
| Cuộn cảm DC/DC (ML1) | DCC ↔ DECD | Cho phép bật `NRF5X_REG_MODE_DCDC` |
| Anten PCB | ANT | Phía dưới vùng anten không được có đồng hoặc linh kiện (datasheet mục 7) |

## Bảng chân trên Helmet board

Clock pin là chân được phép mang tín hiệu clock của SPIM/TWIM/PDM/I2S.

| Pad | Chân | Clock pin | Helmet board | Trong board `me54bs62` |
|---|---|---|---|---|
| B3 | SWDIO | | Header SWD (R2 22R) | |
| B4 | SWCLK | | Header SWD (R8 22R) | |
| C2 | nRESET | | Header SWD (R1 22R) + nút SW1 + R20 10k + C12 0.1µF | |
| D7 | VCC | | 3.3 V (AP2112K) | |
| **G2** | **P2.07** | | **LED Bluetooth** (R16 → Q1 SS8050 → D3) | `led0` / `bt_led`, active high |
| **E1** | **P0.02** | | **Nút Vol Up** (pull-up 10k) | `sw0`, `INPUT_KEY_VOLUMEUP` |
| **D1** | **P0.01** | | **Nút Vol Down** | `sw1`, `INPUT_KEY_VOLUMEDOWN` |
| **C1** | **P0.00** | | **Nút Call** | `sw2`, `INPUT_KEY_PLAYPAUSE` |
| **F1** | **P0.03** | có | **Nút Power** | `sw3`, `INPUT_KEY_POWER` |
| **G5** | **P1.12** | có | **I2S BCLK**, nối 2 MAX98357A (R14 22R) | `i2s20` SCK_M |
| **G4** | **P1.10** | | **I2S LRCLK** (R15 22R) | `i2s20` LRCK_M |
| **G7** | **P1.15** | | **I2S DIN** cho ampli (R11 22R) | `i2s20` SDOUT |
| **F5** | **P1.11** | có | **PDM CLK**, nối 2 IM69D130 (R19 22R) | `pdm20` CLK |
| **F4** | **P1.09** | | **PDM DATA** (R6/R7 100R) | `pdm20` DIN |
| **G6** | **P1.13** | | **I2C SDA**, MLX90632 (R13 22R) | `i2c_sensor` (bit-bang) |
| **G1** | **P0.04** | có | **I2C SCL**, MLX90632 (R17 22R) | `i2c_sensor` (bit-bang) |
| E6 | P1.04 | có | không nối | UART20 TX (console) |
| F7 | P1.05 | | không nối | UART20 RX |
| A3–A7, B2, B5 | P2.04, P2.05, P2.03, P2.01, P2.00, P2.06, P2.02 | P2.01, P2.06 | không nối | |
| B6, B7, C6, C7, D6 | P1.07, P1.08, P1.03, P1.06, P1.02 | P1.08, P1.03 | không nối | P1.02/P1.03 là GPIO (`nfct-pins-as-gpios`) |
| E2, F2, F3, F6 | P2.10, P2.09, P2.08, P1.14 | | không nối | |
| A1 | RF / GND (*) | | | |
| A2, B1, D2, E7, G3 | GND | | | |

(*) Bảng "Pin Definition" của Minew ghi A1 là RF, nhưng sơ đồ nguyên lý của ME54BS62 lại vẽ A1 là GND.

## Ràng buộc miền nguồn của nRF54L15 (datasheet v1.0 mục 10.1)

> *"As a general rule, peripherals must use GPIO pins in their own domain for all peripheral functions."*

| Cổng | Miền | Peripheral dùng được | Ngắt GPIO |
|---|---|---|---|
| P0 | Low-power | UARTE30, SPIM30, TWIM30 | Có (GPIOTE30) |
| P1 | Peripheral | UARTE/SPIM/TWIM 20–22, PWM20–22, I2S20, PDM20/21, SAADC AIN0–7 | Có (GPIOTE20) |
| P2 | MCU | SPIM00 / UARTE00 (tốc độ cao), FLPR | **Không có** |

- Ngoại lệ cross-domain **chỉ dành cho SPIM/SPIS/UARTE 20/21**, dùng một số chân P2 khi bật Constant Latency. **TWIM, I2S, PDM và PWM không có ngoại lệ này.**
- Tín hiệu **TWIM SCL, PDM CLK, I2S SCK/MCK, SPIM SCK** bắt buộc nằm trên clock pin. Clock pin của gói CAAA: P0.03, P0.04, P1.03, P1.04, P1.08, P1.11, P1.12, P2.01, P2.06.

## ⚠ Các điểm cần xem lại trên phần cứng Helmet board

0. **⚠ LED Bluetooth: R16 không nối tới module (đo ngày 13/10/2026)**. Trên mạch hiện tại mới chỉ có các linh kiện của mạch chính và nguồn. Board nút, micro và loa chưa nối, MLX90632 chưa gắn.
   - Firmware có điều khiển P2.07 (đọc thanh ghi thấy OUT đảo 1 Hz, CTRLSEL = GPIO), nhưng LED không sáng.
   - Chạm dây thả nổi vào R16 (đầu nào cũng được) thì LED sáng. Vậy LED, R12 và Q1 đều tốt; Q1 không có điện trở kéo xuống ở chân B nên rất nhạy nhiễu. Việc đầu R16 phía module cũng "bắt nhiễu" được chứng tỏ đầu này đang thả nổi.
   - Đã quét **cả 30 chân GPIO** bằng pull-up/pull-down nội qua J-Link: **không chân nào** thấy tải của R16 → chân B của Q1. Riêng P2.07 (pad G2) phản ứng giống hệt một pad không nối gì.

   Kết luận: đường mạch "Bluetooth LED" giữa pad G2 của module và R16 bị hở. Thứ tự kiểm tra đề xuất:
   1. Trong file PCB Altium, chạy DRC và tìm *Un-Routed Net* cho net "Bluetooth LED". Kiểm tra pad G2 của footprint ME54BS62 có thật sự đi dây tới R16 không.
   2. Ngắt nguồn, đo thông mạch từ đầu R16 phía module tới via hoặc đường mạch gần pad G2.
   3. Nếu layout đúng thì hàn lại module: pad G2 nằm dưới bụng module LGA. Có thể khò lại với flux.
   4. Hàn lại xong, chạy `hw blink 500` và đo đầu R16 phía module: phải thấy xung 0 ↔ 3.3 V với chu kỳ 1 s.

   Kết quả phụ của lần quét: pad C1 (P0.00, nút Call) thấy một điện trở kéo lên bên ngoài, còn D1/E1/F1 không thấy. Nếu R23/R24/R26 chưa được hàn thì đây là bình thường. Board definition đã bật pull-up nội cho 4 nút, để chân không thả nổi khi board nút chưa cắm.

1. **I2C của MLX90632 nằm ở 2 miền khác nhau** (SDA = P1.13 thuộc P1, SCL = P0.04 thuộc P0), nên không TWIM nào dùng được. Firmware đang chạy I2C **bit-bang bằng GPIO** (`gpio-i2c`, 100 kHz). Ở bản PCB sau nên đặt cả hai chân trên P1, với SCL ở clock pin P1.03/P1.04/P1.08 (P1.11/P1.12 đã dùng cho PDM/I2S). Như vậy sẽ dùng được TWIM20/21/22 phần cứng.
2. **Không có điện trở pull-up cho SDA/SCL.** Hiện đang dùng pull-up nội khoảng 13 kΩ. Nên thêm 4.7 kΩ lên 3.3 V.
3. **MLX90632**: trên mạch hiện tại **chưa gắn** cảm biến (chỉ có trong schematic), nên `hw mlx` báo `NO RESPONSE` và `i2c scan` không thấy thiết bị là kết quả đúng. Bus vẫn ở trạng thái tốt (SDA = SCL = 1). Khi gắn cảm biến, chạy lại `hw mlx`, kết quả mong đợi là `OK EE_VERSION=0x..05`.
4. **Micro PDM**: board micro **chưa được nối**, nên `hw mic` cho L và R giống hệt nhau (`L != R in 0/16000`). Đây là dấu hiệu chân DATA (P1.09) không có micro điều khiển, đúng với thực tế. Khi cắm hai micro (SELECT một cái nối GND, một cái nối VCC), số mẫu L≠R phải gần 100%, AC khoảng -60..-75 dBFS khi phòng yên tĩnh và tăng khi nói hoặc vỗ tay.
5. **Dây RESET: đã sửa xong (13/10/2026)**. Trước đó dây RESET bị cắm nhầm chân trên đầu cắm J-Link. Hậu quả là chip bị giữ ở trạng thái reset: firmware không chạy, không quảng bá BLE, và cổng SWD không phản hồi (`SWDReadDP 0 = ERROR`). Sau khi cắm lại đúng chân 15, các phép thử đều đạt:
   - Reset bằng chân: `r0`/`r1` của J-Link và `nrfutil device reset --reset-kind RESET_PIN` đều làm chip khởi động lại (biến đếm `toggle_count` trong RAM quay về 0).
   - Nạp kèm `reset=RESET_PIN`: CPU chạy ngay sau khi nạp.
   - Board hiện vẫn dùng `--softreset` (reset qua SWD), chạy được cả khi dây RESET lỏng.
6. **Thứ tự connector nút**: ở mạch chính, B1..B4 của connector là E1, D1, C1, F1; còn ở board nút, B1..B4 là C1, D1, E1, F1. Hãy nhấn thử từng nút và đối chiếu tên nút in ra trong log `hw_test`.

## Nối J-Link (SWD)

nRF54L15 **chỉ hỗ trợ SWD**, không có JTAG 4 dây.

Đầu cắm 20 chân của J-Link. Chân 1 được đánh dấu bằng tam giác hoặc mũi tên trên đầu cáp; hàng lẻ là hàng chứa chân 1:

```
 hàng lẻ   1    3    5    7    9   11   13   15   17   19
         VTref nTRST TDI SWDIO SWCLK RTCK SWO RESET nc  5V*
 hàng chẵn  2    4    6    8   10   12   14   16   18   20
          nc   GND  GND  GND  GND  GND  GND  GND  GND  GND
```
(*) Chân 19 cấp 5 V (mặc định tắt). **Không cắm dây RESET vào chân này.**

**Mọi chân hàng chẵn từ 4 tới 20 đều là GND**, kể cả chân 14 và 16 nằm sát chân 15. Nếu cắm nhầm dây RESET vào một chân GND, nRESET bị kéo xuống 0 V liên tục và chip bị giữ ở trạng thái reset mãi. Đó chính là hiện tượng đã gặp: mất SWD, firmware không chạy.

| J-Link | Header SW debugger trên Helmet board |
|---|---|
| 1 VTref | VTREF (3.3 V) |
| 7 SWDIO | SWDIO |
| 9 SWCLK | SWCLK |
| **15 RESET** | RESET |
| 4 (hoặc 6/8/…) GND | GND |

J-Link không cấp nguồn cho mạch. Mạch phải được cấp nguồn từ pin hoặc USB-C.
