# WEB_VERSION 4.6 — Bigger UI + Logo + OTA modal fix

Thay toàn bộ file trong web bằng thư mục này.

Thay đổi chính:
- Giữ OTA trong Config modal.
- Sửa nút OTA không phản hồi bằng cách bind event bằng JavaScript thay vì chỉ phụ thuộc inline onclick.
- Hỗ trợ fallback ID cũ: otaFileInput, otaStatusLog, firmwareFile.
- Nút OTA sẽ hiện trạng thái Đang xử lý khi bấm.
- Thêm logo ảnh trong assets/logo_crop.png.
- Tăng kích thước chữ/card/dashboard/modal.

Nếu bấm OTA vẫn không chạy: mở DevTools Console và xem lỗi Supabase Storage/RLS hoặc bucket.
