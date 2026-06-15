# WEB_VERSION 4.9 — LoRa-aware operation status

Thay đổi chính:
- Không xem sensor offline chỉ sau vài phút, vì LoRa Sensor Node gửi gói khoảng 10 phút/lần.
- Ngưỡng trạng thái:
  - <= 15 phút từ gói LoRa gần nhất: ổn định.
  - > 15 phút: theo dõi LoRa chậm.
  - > 20 phút: mất dữ liệu LoRa / cần kiểm tra.
- Gateway status không còn bung thành nhiều card kỹ thuật.
- Bỏ MQTT khỏi Dashboard vì firmware hiện không dùng MQTT thật.
- Status được gom thành panel cảnh báo vận hành: WiFi, LoRa, pin, heap, health_flag.
- OTA giữ trong Config modal.
- Khôi phục logo ảnh tại assets/logo_crop.png.

Cần copy cả thư mục assets/ cùng index.html, app.js, style.css, config.js, supabase.js.
Sau khi thay file nên Ctrl + F5 để xóa cache trình duyệt.
