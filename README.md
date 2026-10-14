# SICS Gantry Watering V2.0 - Dashboard Kontrol Irigasi Presisi

**Proyek Tugas Akhir / Penelitian:**  
*Rancang Bangun Sistem Irigasi Otomatis Multi-Tanaman Menggunakan Mekanisme Linear Motion Berbasis ESP32*  
- **Nama Peneliti / Mahasiswa:** Azkia Azraei Shahrefi  
- **NIM:** 2305041030  
- **Kelas:** EK-6B  
- **Status Kode Firmware:** Percobaan Ke-12 (Tahap Final) • 07 Juli 2026  

---

## 🌟 Desain & Pembaruan Tampilan (Clean, Minimalist & Profesional)

Dashboard telah diperbarui dengan pendekatan **desain bersih, standar, dan tanpa elemen visual berlebihan (tanpa nuansa neon)**:

1. **Tipografi Standar & Sangat Terbaca:**
   - Menggunakan font sistem modern **Inter / Segoe UI / Roboto** dengan hierarki ukuran yang rapi dan nyaman dibaca.
   - Menghilangkan font pixel/game (seperti VT323) agar tampilan tampak formal untuk sidang akademik dan standar industri.

2. **Skema Warna Natural & Minimalis:**
   - Latar belakang abu-abu terang bersih (`#f8fafc`) dengan kartu putih berbatas halus (`#e2e8f0`).
   - Warna aksen alami: Biru standar (`#2563eb`) untuk kontrol utama, Hijau daun (`#16a34a`) untuk status normal/irigasi, Amber (`#d97706`) untuk peringatan tanah kering, dan Merah (`#dc2626`) untuk tombol darurat E-STOP.
   - Seluruh efek *glow neon*, bayangan berlebih, dan animasi mencolok telah dihilangkan sepenuhnya.

3. **Kesesuaian Presisi dengan Kode ESP32:**
   - **Matriks Grid 8 Baris &times; 6 Kolom (48 Sel Tanaman):**
     - **Zona A:** Baris H, G, F, E (24 titik tanaman bagian atas).
     - **Zona B:** Baris D, C, B, A (24 titik tanaman bagian bawah).
   - **Kalkulasi Langkah Stepper (Steps):**
     - Sumbu X: 0 s/d 767.33 steps (Spasi: 153.47 steps/kolom).
     - Sumbu Y: 0 s/d 2118.33 steps (Baris H = 0 steps, Baris A = 2118 steps).
   - **Perintah REST API ESP32 yang Akurat:**
     - `SCAN` : Full Scan (H1 &rarr; A1 serpentine).
     - `ZONA A` : Scan Zona A (Baris H, G, F, E).
     - `ZONA B` : Scan Zona B (Baris D, C, B, A).
     - `ROW_A` s/d `ROW_H` : Scan per baris spesifik.
     - `COL_1` s/d `COL_6` : Scan per kolom spesifik.
     - `GOTO&cell=...` : Menuju sel tertentu (cth: `C3`).
     - `HOME`, `ES`, `RESET`, `STATUS`.
   - **Tampilan Virtual LCD 16x2:**
     - Tampilan matte dot-matrix hijau lembut yang berotasi setiap 5 detik (Mode 1: Waktu & Tanggal, Mode 2: Suhu & RH, Mode 3: Zona Tanah) persis seperti fungsi `updateLCDMode()` pada firmware.
   - **Manajemen Jadwal EEPROM/Preferences:**
     - Mendukung penambahan hingga 16 jadwal irigasi otomatis dengan waktu (HH:MM) dan target.

---

## 📂 Struktur File

```
d:\dashboard\
├── index.html          # Struktur HTML bersih dan terorganisir
├── style.css           # Styling CSS minimalis, rapi, dan profesional (tanpa neon)
├── app.js              # Logika kontrol, telemetri, dan komunikasi ESP32
├── standalone.html     # Versi portabel mandiri (HTML+CSS+JS dalam 1 berkas)
└── README.md           # Dokumentasi teknis
```

---

## 🚀 Cara Menjalankan

### Cara 1: Buka Berkas Langsung (Paling Cepat)
Klik ganda berkas [**`standalone.html`**](file:///d:/dashboard/standalone.html) atau [**`index.html`**](file:///d:/dashboard/index.html) menggunakan browser apa saja (Google Chrome, Microsoft Edge, Firefox).

### Cara 2: Melalui Server Lokal
Server lokal telah aktif di port `8080`:
```text
http://localhost:8080/index.html
```

---

## 🔌 Cara Menghubungkan ke ESP32 Fisik

1. Sambungkan ESP32 ke jaringan WiFi lokal.
2. Buka dashboard web, lalu klik tombol **Simulasi** pada bilah navigasi kanan atas.
3. Pilih opsi **Mode Hardware ESP32**.
4. Masukkan IP Address ESP32 (contoh: `192.168.1.50` atau biarkan kosong jika dashboard ini di-host langsung oleh ESP32), lalu klik **Simpan Pengaturan**.
