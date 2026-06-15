# WEB_VERSION 4.8 — Gateway Alerts UI

Thay đổi chính:
- Gateway heartbeat không còn hiển thị thành 6 card dài.
- Bỏ MQTT khỏi dashboard vì hệ thống hiện không dùng MQTT.
- Gateway status được gom thành dạng cảnh báo: WiFi, LoRa, Heap, Firmware.
- Topbar dùng trạng thái gateway/sensor hợp lý hơn: LIVE, WARNING, SENSOR WARN, OFFLINE.
- Khôi phục logo ảnh trong sidebar bằng assets/logo_crop.png.
- OTA vẫn nằm trong Config modal.

Lưu ý:
- Khi copy thủ công, phải copy cả thư mục assets/ để logo hiện đúng.
- Sau khi thay file nên Ctrl+F5 để tránh cache app.js/style.css cũ.
