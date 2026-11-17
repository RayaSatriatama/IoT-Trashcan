# Antrean aktif firmware-v3 tanpa menghapus arsip lama

## Tujuan dan batas

`SIGAP_LIVE_ARCHIVE_SPLIT=1` memilih generasi pengiriman baru. Default tetap `0`, sehingga build lama tidak beralih namespace tanpa keputusan eksplisit.

Mode ini memisahkan akuisisi baru dari arsip historis yang hampir memenuhi kuota dan tidak memiliki workspace untuk whole-copy reclaim. Ini **bukan migrasi, ACK, penghapusan, atau bukti pengiriman backlog lama**. Data historis tetap `UNDELIVERED`; status penyimpanan tetap degraded selama file lama tidak kosong.

Build empat sensor memakai `SIGAP_SENSOR_SIMULATION=0`, `SIGAP_PHYSICAL_REFERENCE_ONLY=1`, dan identitas `sigap-task6-physical4-v2`. Referensi fisik tetap HX711 organik GPIO10/11, HC-SR04 organik GPIO16/17, MQ135 ADC5, SHT31 alamat0x44 pada I2C8/9. Constructor dan scheduler kanal lain tetap nonaktif; heater MQ-7 tidak diaktifkan. Nilai raw tidak menunggu kalibrasi. Counts dan ADC bukan kg atau ppm.

## Namespace dan admission

File lama berikut tidak menjadi antrean aktif dan tidak diubah oleh mode baru:

- `/sigap/archive.bin`, `/sigap/archive.ckp`, `/sigap/gaps.bin` dan sidecar recovery-nya.
- `/sigap/batches.bin`, `/sigap/batches.ckp`.
- `/sigap/acks.bin`, `/sigap/acks.ckp`.

File aktif baru:

- `/sigap/live-v3.bin`, `/sigap/live-v3.ckp`, `/sigap/live-v3.gaps`.
- `/sigap/live-v3.batches`, `/sigap/live-v3.batches.ckp`.
- `/sigap/live-v3.acks`, `/sigap/live-v3.acks.ckp`.

Append baru hanya diizinkan bila kapasitas aktual diketahui dan:

```
free_bytes >= active_pending_bytes + 2 * incoming_record_bytes + 65536
```

Perhitungan overflow-safe. Kuota aktif lama tetap berlaku dan guard reclaim dasar 4096 byte tetap dipertahankan. Arsip historis tetap dibebankan pada pemakaian filesystem aktual. Reserve 64 KiB merupakan anggaran konservatif, bukan reservasi atomik atau jaminan universal LittleFS. Semua I/O tetap mengikuti exclusion yang sudah ada. Bila ruang atau status kapasitas tidak cukup, append ditolak tanpa menimpa pending records. Retry identik yang sudah durable tidak membutuhkan append baru.

## Siklus pengiriman

Format arsip, canonical telemetry-v3, hash, identitas, dan validasi ACK tidak diganti. Runtime baru membungkus runtime batch yang sudah ada dengan satu immutable batch aktif.

Urutan normal:

1. Body immutable dan pending manifest tersimpan.
2. ACK server divalidasi terhadap owner, batch, hash, count, dan persisted source binding.
3. Journal ACK di-checkpoint.
4. Hanya record sumber yang tepat direclaim dari antrean aktif.
5. Final reclaim journal tersimpan.
6. Hanya body batch selesai yang tepat dikosongkan, lalu journal yang sudah tidak mengandung manifest dikompakkan.

Body dan journal tidak terus menumpuk setelah ACK. File aktif kosong dipertahankan agar forensic tail yang lama tidak dianggap sebagai antrean baru. Sebelum retry setelah partial write, index batch/journal dipulihkan dengan mekanisme quarantine yang sudah ada. Byte asli yang terpotong tetap menjadi forensic tail, bukan dihapus.

Saat boot, bukti ACK/final reclaim yang tepat dapat diputar ulang dari journal. Tidak adanya ACK tidak pernah dipromosikan menjadi sukses. Bukti final reclaim yang bertentangan dengan record sumber yang masih ada ditolak. Manifest yang tidak cocok, lebih dari satu batch tersimpan saat cleanup, atau recovery gagal tetap fail-closed.

## Monitoring mentah pada web

Respons GET perangkat, daftar perangkat, dan detail lokasi memuat `firmware_monitoring` dari arsip telemetry-v3 yang sudah diterima. Dashboard memakai DTO daftar/detail lokasi melalui kartu perangkat yang sama; detail perangkat memakai DTO perangkat. Array kosong berarti belum ada sampel diterima. API lama tanpa properti tersebut ditandai belum mendukung monitoring, bukan dianggap mempunyai pembacaan nol.

Monitoring tidak memerlukan token operator sementara. Ini tidak membuka profil kalibrasi atau melemahkan otorisasi device-control: proyeksi hanya membawa identitas sensor/stream/boot/sequence, kualitas/alasan, asal versi firmware, angka raw/konversi yang diizinkan, dan ketidakpastian waktu. ROM, profil, kredensial, payload bebas, dan hasil perintah tidak diproyeksikan. Nilai nol yang hadir tetap nol; konversi engineering hanya ditampilkan untuk kualitas `READY`. Tidak ada turunan ppm, berat, keputusan FIS, atau metrik slot palsu untuk data belum terkalibrasi.

Asal `PHYSICAL` hanya klasifikasi metadata BOOT yang dinyatakan firmware, bukan autentikasi perangkat keras. Tag simulasi dan asal tidak diketahui tetap berbeda. Umur yang tidak diketahui, jam masa depan, kualitas stale, dan urutan lintas boot yang tidak pasti tetap terlihat. Sequence uint64 dipertahankan sebagai string. Rincian boot/sequence/versi pada detail perangkat dibuka secara eksplisit; pratinjau di kartu dashboard tidak menambahkan kontrol interaktif di dalam kartu yang dapat ditekan.

Metrik slot dan status konektivitas lama tidak diisi ulang dari raw counts. Kosongnya `readings` legacy tidak lagi menyembunyikan sampel firmware yang sudah diterima. Mengubah proyeksi web tidak membuat firmware yang belum mengirim HTTPS menjadi sudah terkirim.

Uji tambahan meliputi transisi ACK dengan frame ber-CRC valid (orphan, urutan salah, duplikat, konflik hash/count/owner), preservasi body yang belum terbukti selesai, prefix selesai yang sah, serta route produksi dengan SQLite read-only dan present-zero. Harness C++/backend yang sudah ada tetap menggunakan SDK palsu dan database tes sekali pakai; pemeriksaan terhadap arsip tersebut bukan bukti fisik.

## Debug HTTPS

Konfigurasi optional yang sudah ada tetap digunakan:

- `SIGAP_DEBUG_DEVICE_CODE`: owner registry yang tepat, bukan tebakan dari nama BLE.
- `SIGAP_DEBUG_ENDPOINT`: HTTPS lengkap `/api/device-debug/device/<owner>/events`.
- CA dan token berasal dari jalur trust/SecretStore yang sudah ada. Tidak ada token baru atau rotasi otomatis.

Mengganti pilihan pembacaan debug pada web tidak mengaktifkan uploader firmware. Debug HTTPS dan telemetry archive HTTPS merupakan jalur berbeda.

## Verifikasi dan pemasangan

Uji host relevan:

```bash
python3 -m unittest tests.test_live_archive tests.test_live_delivery tests.test_live_app -v
```

Uji meliputi kapasitas tidak diketahui/kurang/batas tepat, retry identik, ACK salah, 320 siklus ACK, tujuh batas restart, corrupt tail, partial batch/journal write, dan actual-app wiring dengan SDK palsu. Actual-app test menginjeksi sampel untuk memeriksa `UNCALIBRATED` dan present-zero; ini bukan pembacaan sensor fisik.

Gunakan hanya konfigurasi kandidat yang masih menautkan perbaikan allocator minimal yang telah ditinjau. Target generik yang memakai SDK lama tanpa perbaikan tersebut bukan pengganti kandidat yang aman hanya karena compile berhasil. Konfigurasi endpoint/debug harus diperiksa pada build efektif dan linked artifact, bukan sekadar header yang tidak dipakai.

Build sukses bukan izin flash. Pemasangan aplikasi, readback, boot, TLS tervalidasi, authenticated ingest dari boot baru, ACK/checkpoint/reclaim fisik, dan render dashboard adalah gerbang terpisah. Tidak ada erase/format, perubahan partisi/bootloader, token, atau calibration COMMIT yang diizinkan oleh flag ini.

Marker startup `HISTORICAL_ARCHIVE_PRESERVED` hanya melaporkan jalur recovery berhasil dan ukuran file yang dipertahankan, bukan hash atau bukti fisik byte-identik. Bukti byte preservation host dan readback fisik tidak boleh dicampur.

Pada mode prioritas web, ketujuh file aktif memakai namespace `web-b<32-hex-boot-id>` dan tetap terpisah dari `/sigap/web-sync-v3.*` generasi sebelumnya. File lama, termasuk semua sidecar dan suffix turunannya, tetap `UNDELIVERED`: hanya dihitung sebagai retained bytes, tidak dibuka untuk penulisan, dipulihkan, diganti nama, di-ACK, atau dihapus. Timestamp lama tidak ditulis ulang. Readback fisik dan gerbang pengiriman/ACK tetap perlu diverifikasi saat pemasangan; uji host bukan bukti fisik.


LittleFS boot-scoped active filenames use the full 128-bit boot identity encoded as unpadded Base64URL; this is a lossless 22-character token, not a hash or truncation. The persisted filesystem superblock has `name_max=32`; a larger SDK setting does not change that on-disk limit. The seven one-letter active extensions and recovery suffixes remain within 32-character basename limit. Older hex-named boots and fixed web-sync files remain retained and counted as `UNDELIVERED`; no format, erase, or historical-file mutation is performed.

Normal ACK reclaim still uses its original transaction and recovery sequence. Only new compact boot-archive paths map the overlong `.migrate.bak` sidecar to `.mb`; legacy paths retain `.migrate.bak`, and existing backups or older recovery algorithms are not renamed or rewritten.
