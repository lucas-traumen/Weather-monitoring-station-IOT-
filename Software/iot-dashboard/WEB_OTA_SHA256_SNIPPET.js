/* ═══════════════════════════════════════════════════════════════════
 * 4. OTA PIPELINE + SHA256
 * ═══════════════════════════════════════════════════════════════════ */

async function sha256Hex(file) {
  const buf = await file.arrayBuffer();
  const digest = await crypto.subtle.digest("SHA-256", buf);
  return [...new Uint8Array(digest)]
    .map(b => b.toString(16).padStart(2, "0"))
    .join("");
}

async function uploadAndDeployFirmware() {
  const fileInput = document.getElementById('otaFileInput');
  const logEl     = document.getElementById('otaStatusLog');
  if (!fileInput || !logEl) return;

  const file = fileInput.files[0];
  if (!file) {
    logEl.textContent = "⚠ Vui lòng chọn file firmware dạng .bin trước!";
    logEl.style.color = "var(--orange)";
    return;
  }

  if (!file.name.toLowerCase().endsWith(".bin")) {
    logEl.textContent = "⚠ File OTA phải là firmware .bin!";
    logEl.style.color = "var(--orange)";
    return;
  }

  try {
    logEl.innerHTML = "⏳ Đang tính SHA256 firmware...";
    logEl.style.color = "var(--text-2)";

    const ota_sha256 = await sha256Hex(file);
    const ota_seq = Date.now();

    console.log("[OTA] file =", file.name);
    console.log("[OTA] ota_seq =", ota_seq);
    console.log("[OTA] ota_sha256 =", ota_sha256);

    logEl.innerHTML = "⏳ Đang tải firmware lên Supabase Storage...";

    /*
     * Đổi OTA_BUCKET cho đúng bucket đang dùng trên web của bạn:
     * - Nếu screenshot đang hiện Bucket: firmware => dùng "firmware"
     * - Nếu project cũ dùng ota-binaries => đổi lại "ota-binaries"
     */
    const OTA_BUCKET = "firmware";
    const fileName = `firmware_${ota_seq}_esp32_gateway.bin`;

    const { data: uploadData, error: uploadError } = await supabaseClient
      .storage.from(OTA_BUCKET)
      .upload(fileName, file, {
        cacheControl: "0",
        upsert: false,
        contentType: "application/octet-stream"
      });

    if (uploadError) throw uploadError;

    const { data: { publicUrl } } = supabaseClient
      .storage.from(OTA_BUCKET)
      .getPublicUrl(fileName);

    logEl.innerHTML = "⏳ Ghi lệnh OTA vào device_configs kèm SHA256...";

    const { error: dbError } = await supabaseClient
      .from('device_configs')
      .upsert({
        device_id: "ESP32_LORA_GW",
        ota_url: publicUrl,
        ota_seq,
        ota_sha256,
        updated_at: new Date().toISOString()
      }, { onConflict: "device_id" });

    if (dbError) throw dbError;

    logEl.innerHTML = `✅ <b>PHÁT LỆNH OTA THÀNH CÔNG!</b><br>
      • Bucket: ${OTA_BUCKET}<br>
      • Device: ESP32_LORA_GW<br>
      • ota_seq: ${ota_seq}<br>
      • SHA256: <span style="font-size:0.62rem;color:var(--text-2);">${ota_sha256}</span><br>
      • URL: <span style="font-size:0.62rem;color:var(--text-2);">${publicUrl}</span>`;
    logEl.style.color = "var(--green)";
    fileInput.value = "";

  } catch (err) {
    console.error('[OTA Pipeline Error]:', err);
    logEl.textContent = `❌ LỖI OTA PIPELINE: ${err.message}`;
    logEl.style.color = "var(--red)";
  }
}