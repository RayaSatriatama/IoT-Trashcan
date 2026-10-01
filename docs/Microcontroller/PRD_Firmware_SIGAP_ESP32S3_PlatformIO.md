# PRD Firmware SIGAP ESP32 S3 dengan PlatformIO

Versi 1.3  |  1 Oktober 2026  |  Kontrak implementasi firmware, configuration architecture, telemetry archive, dan integrasi backend

Satu ESP32-S3 DevKitC-1 N16R8 menangani tiga slot Organik, Anorganik, dan B3. Firmware membaca seluruh sensor, melakukan kalibrasi, filtering, normalisasi, evaluasi FIS, dan safety lokal, mencatat setiap sampel serta hasil per siklus, lalu mengirim ringkasan dan arsip telemetry ke backend SIGAP. Untuk sumber `HARDWARE_EDGE_V3`, hasil yang diproduksi MCU adalah source of truth: backend Go mengautentikasi, memvalidasi kontrak, menyimpan, memproyeksikan current state, dan mengekspor data tanpa menghitung ulang FIS/safety. Konfigurasi runtime perangkat tidak berupa kumpulan mutable global variable: firmware menggunakan snapshot `AppConfig` tervalidasi dan berversi, secret terpisah, calibration/model profile terpisah, serta runtime state yang tidak diperlakukan sebagai konfigurasi. Jalur simulator/legacy mempertahankan pipeline backend yang ada sampai dimigrasikan secara terpisah. Dokumen ini menetapkan perilaku produk, kontrak configuration/provisioning, telemetry, rencana PlatformIO, serta kriteria penerimaan untuk memulai implementasi.

Revisi 1.3 menyatukan PRD dengan TSD v1.2 dan keputusan dari skill embedded yang telah di-merge: source of truth keputusan hardware berada di edge; arsip setiap sampel menjadi kontrak P0; policy storage-full dikunci ke **no overwrite of pending**; LittleFS menjadi 8 MiB dengan quota pending awal 6 MiB; schema telemetry v3 dibekukan; dan configuration architecture dibagi menjadi compile-time board/build constants, versioned `AppConfig`, write-only `SecretStore`, versioned calibration/model profiles, serta ephemeral `RuntimeState`. Mutable non-const global configuration dilarang. Revisi 1.2 menjadi asal freeze telemetry/archive; revisi 1.1 menjadi asal mode kalibrasi wajib P0 dan microSD opsional. Wi-Fi tetap jalur pengiriman utama; BLE tetap P1 bersyarat.

**Authority artefak:** file Markdown ini adalah source normative revisi 1.3. `PRD_Firmware_SIGAP_ESP32S3_PlatformIO.docx` adalah derived artifact dan tidak boleh dianggap setara sampai diregenerasi dari revisi ini serta diverifikasi layout.

## 1 Tujuan dan pengguna

Operator membutuhkan kondisi tiga slot yang dapat dipercaya, termasuk ketika sensor gagal atau koneksi terputus. Peneliti membutuhkan semua kanal mentah beserta waktu, kualitas pembacaan, dan versi kalibrasi agar eksperimen dapat direproduksi. Teknisi membutuhkan konfigurasi pin dan proses kalibrasi yang terpisah dari kredensial.

## 2 Landasan repo

Baseline historis repo adalah RayaSatriatama/IoT-Trashcan, branch main, commit `6fed00f02b6ef7dabc159d924f798d344a6072ba`. Reconciliation terbaru untuk revisi 1.3 dilakukan terhadap `main` commit `7dd35f6d05cde09fe31a8a2f56933d04e4dc456a` pada 1 Oktober 2026.

### Reconciliation terhadap HEAD

| Commit | Klasifikasi | Dampak normatif |
| --- | --- | --- |
| `98e4439` | Runtime/schema | Memperbarui ingest, fuzzy, safety, history/read API, simulator, serta menambah `004_device_research_trace.sql`. Permukaan ini dipertahankan sebagai compatibility legacy. |
| `292aa47` | Dokumen firmware | Menambahkan PRD/TSD firmware awal. |
| `ad65d5f` | Tooling/skills | Menambahkan skill proyek tanpa perubahan runtime. |
| `a0a9dc9` | Reconciliation docs/test | Menyelaraskan baseline/HEAD dan test migration 004. |
| `7dd35f6` | Embedded skills | Mengimpor skill upstream `esp32-development`, `embedded-development`, `cpp-coding-standards`, `config-nvs`, dan `memory-audit`. Skill menjadi panduan engineering; PRD/TSD tetap authority proyek. |

`POST /api/ingest` tetap menjadi **summary-v2 compatibility route**. Arsip lengkap memakai route baru `POST /api/telemetry/batches`; route baru bersifat additive dan tidak mengubah request/response existing. Migration terakhir pada HEAD ini adalah `004_device_research_trace.sql`, sehingga nama yang **diharapkan saat ini** untuk arsip adalah `005_telemetry_archive.sql`; nomor wajib dicek ulang tepat sebelum implementation PR.

Untuk data `HARDWARE_EDGE_V3`, backend tidak memanggil `Normalize`, evaluator fuzzy, atau evaluator safety legacy. `trace_json` dan `safety_json` milik jalur legacy tetap dipertahankan dan tidak dipakai ulang sebagai storage arsip v3. History dashboard tetap menggunakan cursor `(recorded_at, reading_id)`; archive API memakai `(device_id, boot_id, sequence)` dan dapat menyimpan `recorded_at:null`.

| Area | Kondisi HEAD | Implikasi firmware |
| --- | --- | --- |
| Runtime | Go `net/http`, SQLite, dan Expo Web; belum ada proyek firmware PlatformIO. | Firmware ditambahkan sebagai boundary kompilasi baru tanpa mengganti backend/web existing. |
| Ingest | `POST /api/ingest`, Bearer token, tepat tiga stream. | Summary-v2 tetap kompatibel; archive-v3 memakai handler terpisah. |
| Replay | Timestamp sama hanya identik bila raw payload sama. | Retry summary menyimpan exact bytes; v3 memakai batch ID/body hash + record hash. |
| Research | `004_device_research_trace.sql` menambah `trace_json` dan `safety_json`. | Arsip v3 additive dan tidak mereinterpretasi evidence legacy. |
| Skills | Lima skill embedded/config/C++ upstream telah di-merge. | Hindari mutable global state; configuration/persistence harus bounded, explicit ownership, recoverable, dan resource-aware. |

PRD ini melengkapi SIGAP_PRD_v1.0.md, SIGAP_SDD_v1.0.md, dan konsep hardware v0.8. Sensor MQ-7 dan MQ-8 mengikuti keputusan perangkat terbaru; kandidat MQ-137/MQ-138 dalam dokumen lama tidak menjadi baseline firmware ini.

## 2A Keputusan normative revisi 1.3

Urutan authority untuk pekerjaan firmware adalah: **PRD v1.3 → TSD v1.2 → contract/schema fixture v3 → implementation/tests**. Skill repo adalah panduan engineering dan tidak dapat mengalahkan kontrak produk ini.

| Area | Keputusan dibekukan |
| --- | --- |
| Source of truth hardware | MCU menghitung calibration/filter/normalization/FIS/safety untuk `HARDWARE_EDGE_V3`; backend tidak menghitung ulang. |
| Legacy | Existing `POST /api/ingest` dan simulator/legacy tetap memakai perilaku sekarang agar backward compatible. |
| Archive | Semua sample attempt, result, event, metadata dicatat; invalid bukan alasan membuang raw. |
| Storage-full | Pending durable immutable sampai ACK; tidak ada overwrite/drop-oldest. Quota penuh → `LOGGING_BLOCKED` + gap accounting. |
| Flash | 16 MiB: app 4 MiB, LittleFS 8 MiB, quota pending awal 6 MiB; tanpa OTA pada MVP. |
| SD | Optional; pending quota awal 20 GiB pada SDHC 32 GB teruji; target 72 jam wajib dibuktikan. |
| Contract v3 | uint64 sebagai decimal string; per-record hash deterministic/canonical; batch hash = SHA-256 exact request bytes; schema/error/ACK fixture dibekukan sebelum coding. |
| Runtime configuration | Tidak ada mutable non-const global configuration. Runtime memakai immutable snapshot `AppConfig` yang di-load sekali saat boot dan diinjeksi ke modul. |
| Persistent config | `AppConfig` disimpan versioned di NVS dengan slot A/B, checksum/hash, readback verification, dan active pointer; commit baru aktif setelah reboot terkontrol. |
| Secrets | Wi-Fi credential dan device token berada di `SecretStore`/namespace NVS terpisah, write-only dari maintenance, dan tidak masuk config export/telemetry/log. |
| Scientific profiles | Calibration, FIS, safety, dan profile penelitian bukan `AppConfig`; masing-masing versioned, memiliki provenance/hash, dan pointer aktif atomik. |
| Runtime state | Koneksi, queue depth, health, counters, heater phase, dan state task bersifat ephemeral; tidak dipersist sebagai configuration. |
| Migration | Additive; expected next `005_telemetry_archive.sql`, recheck HEAD sebelum membuat file. |

## 3 Ruang lingkup dan konfigurasi perangkat

Konfigurasi dikunci untuk satu unit dengan tiga slot. Dua unit prototipe dapat memakai firmware yang sama dengan device code, token, dan kalibrasi masing-masing. Jumlah sensor berikut berlaku per unit.

| Komponen | Jumlah | Fungsi |
| --- | --- | --- |
| ESP32-S3 DevKitC-1 N16R8 | 1 | 16 MB flash, 8 MB PSRAM sesuai pilihan pengguna; profil awal mengasumsikan WROOM-1/1U dengan Quad flash dan Octal PSRAM. |
| Load cell 20 kg dan HX711 | 3 + 3 | Berat masing-masing slot; kapasitas operasional ditentukan mekanik dan berbeda dari rating load cell. |
| Sensor jarak TRIG ECHO | 3 | Baseline implementasi ultrasonic; model HC-SR04/JSN-SR04T belum dikunci. |
| MQ-4, MQ-135, MQ-7, MQ-8 | 1 tiap tipe | Empat kanal analog eksperimen; tidak menggunakan keluaran DO modul. |
| DS18B20 | 1 | Suhu B3 melalui 1-Wire dengan suplai eksternal 3,3 V. |
| SHT31 | 2 | Suhu dan RH; berbagi I2C dengan alamat 0x44 dan 0x45. |
| Rangkaian daya MQ-7 | 1 | Pemilih rail heater tinggi/rendah yang dikendalikan satu GPIO; tegangan mengikuti datasheet unit aktual. |
| Socket microSD dan kartu SDHC | 1 opsional | Queue offline lebih panjang; SPI berlogika 3,3 V, FAT32 dan kartu teruji. |

### Pembagian slot awal

| Slot | Sensor khusus | Makna data |
| --- | --- | --- |
| Organik | MQ-4, MQ-135, SHT31 alamat 0x44 | Kanal gas dan kondisi lingkungan untuk eksperimen; OGI tetap hasil riset backend. |
| Anorganik | Load cell dan sensor jarak | Fill dan berat sebagai baseline; gas_raw dan temperature boleh null. |
| B3 | MQ-7, MQ-8, DS18B20, SHT31 alamat 0x45 | CO/H2 sebagai kandidat eksperimen; DS18B20 menjadi sumber temperature operasional B3. |

Penempatan empat MQ adalah usulan untuk pengujian integrasi, belum keputusan ilmiah final. MQ-4 berkaitan dengan metana, MQ-7 dengan CO, dan MQ-8 dengan hidrogen. Respons kanal tersebut tidak cukup untuk menyimpulkan bahwa semua jenis B3 aman. Semua kanal dipertahankan terpisah dan tidak dirata-ratakan menjadi gas_raw.

### Batas MVP

P0 mencakup akuisisi semua sensor, quality flag, mode kalibrasi wajib, configuration/provisioning lokal melalui USB, versioned `AppConfig` di NVS, secret store terpisah, waktu NTP, authenticated HTTPS, antrean persisten, pemulihan koneksi, dukungan microSD opsional, dan integrasi health di backend. Portal konfigurasi publik dan remote-maintenance endpoint tidak termasuk P0. Perubahan config P0 memakai flow `DRAFT → VALIDATE → PREVIEW → COMMIT → REBOOT/ACTIVATE`; tidak ada hot reload scheduler/storage/network sampai dibuktikan aman. BLE gateway adalah opsi P1. OTA, actuator tutup, buzzer, kamera, AI klasifikasi, MQTT, GPS, RTC tambahan, serta finalisasi angka FIS dan safety berada di luar MVP ini. Sistem memakai suplai listrik langsung tanpa charging.

## 4 Mapping GPIO untuk N16R8

Mapping baseline tanpa microSD memakai 20 GPIO unik; varian microSD memakai 24 GPIO unik. Baseline terdiri dari 6 HX711, 6 ultrasonic, 4 ADC MQ, 2 I2C, 1 DS18B20, dan 1 kontrol heater MQ-7. Resistor divider dan driver daya bukan GPIO expander. HX711 tetap dibutuhkan sebagai front-end load cell; larangan extension pin tidak menghapus komponen ini.

| Slot | Sinyal | GPIO | Mode |
| --- | --- | --- | --- |
| Organik | MQ-4 AO | 4 | ADC1 CH3 melalui divider |
| Organik | MQ-135 AO | 5 | ADC1 CH4 melalui divider |
| Organik | HX711 DOUT | 10 | Input digital 3,3 V |
| Organik | HX711 SCK | 11 | Output digital |
| Organik | Ultrasonic TRIG | 16 | Output digital |
| Organik | Ultrasonic ECHO | 17 | Input melalui divider bila 5 V |
| Anorganik | HX711 DOUT | 12 | Input digital 3,3 V |
| Anorganik | HX711 SCK | 13 | Output digital |
| Anorganik | Ultrasonic TRIG | 18 | Output digital |
| Anorganik | Ultrasonic ECHO | 21 | Input melalui divider bila 5 V |
| B3 | MQ-7 AO | 6 | ADC1 CH5 melalui divider |
| B3 | MQ-8 AO | 7 | ADC1 CH6 melalui divider |
| B3 | HX711 DOUT | 14 | Input digital 3,3 V |
| B3 | HX711 SCK | 15 | Output digital |
| B3 | Ultrasonic TRIG | 47 | Output digital |
| B3 | Ultrasonic ECHO | 42 | Input; menggantikan GPIO48 |
| B3 | DS18B20 DATA | 2 | 1-Wire; pull-up 4,7 kOhm ke 3,3 V |
| B3 | MQ-7 HEATER SELECT | 1 | Output ke driver pemilih rail |
| Bersama | SHT31 SDA | 8 | I2C 0x44 dan 0x45 |
| Bersama | SHT31 SCL | 9 | I2C shared |

GPIO0/3/45/46 dihindari untuk strapping; GPIO26-37 dihindari untuk memori; GPIO19/20 tetap untuk USB; Pada varian microSD, GPIO43 menjadi CS dan UART0 TX tidak dipakai aplikasi; GPIO44 tidak dipakai sensor. Console maintenance menggunakan USB native pada semua varian. GPIO38 dan GPIO48 tidak dipakai sensor karena revisi board berbeda menempatkan RGB LED pada salah satunya. GPIO39-42 dipakai sensor dan SPI varian microSD sehingga JTAG eksternal tidak tersedia; debug melalui USB bawaan tetap menjadi pilihan. Mapping SPI tambahan ada di bagian 10A.

Tabel memakai nomor GPIO, bukan urutan pin header. Sebelum wiring, cocokkan label modul, revisi PCB, dan schematic board aktual. Profil ini tidak otomatis berlaku untuk setiap board berlabel S3 N16R8.

## 5 Daya dan kondisi pembacaan sensor

Rail MQ dan ultrasonic menggunakan suplai yang sesuai modul; AO MQ dan ECHO 5 V harus diturunkan sebelum GPIO ESP32-S3. Usulan divider MQ adalah R atas 15 kOhm dari AO ke ADC dan R bawah 10 kOhm dari ADC ke GND, sehingga 5 V menjadi 2 V. Validasi pengaruh beban divider terhadap output modul, toleransi rail, dan settling ADC pada rig. Simpan rasio divider dan hasil kalibrasi ADC dalam metadata.

Heater tidak ditenagai GPIO atau regulator 3,3 V board. Gunakan rail daya terpisah dengan GND bersama; ukur konsumsi puncak, penurunan tegangan saat Wi-Fi transmit, dan noise HX711. Pastikan DOUT HX711 berlevel 3,3 V; suplai eksitasi load cell tidak boleh membuat logika output menjadi 5 V. Pull-up I2C kedua SHT31 harus menuju 3,3 V, termasuk resistor bawaan breakout.

### Siklus heater MQ 7

Driver memisahkan suplai heater VH dari suplai rangkaian pengukuran VC. Firmware mengendalikan state PREHEAT, HIGH_CLEAN, LOW_DETECT, dan FAULT. MQ-7B Winsen memakai 5 V selama 60 detik lalu 1,5 V selama 90 detik; angka ini hanya contoh profile MQ-7B, bukan default yang dianggap benar untuk setiap MQ-7. Model, pabrikan, tegangan rendah, toleransi, dan titik sampling harus berasal dari datasheet sensor yang dipasang.

Satu GPIO memilih dua rail teratur melalui driver dengan pencegahan backfeed dan keadaan boot yang ditentukan hardware. GPIO tidak membawa arus heater. Modul empat pin dengan heater terikat permanen ke 5 V perlu modifikasi rangkaian atau modul yang mengekspos heater. Firmware tidak dapat memperbaiki kondisi itu hanya dengan analogRead. PWM mentah pada VCC seluruh modul tidak menjadi baseline.

Saat fase pembersihan dan stabilisasi, mq7 bernilai operasional null dengan flag HEATER_PHASE. Pembacaan diagnostik tetap boleh dicatat. Aging awal mengikuti manual pabrikan; waktu aging pertama tidak disamakan dengan settling setiap reboot. Setelah reset atau brownout, siklus dimulai ulang dan kualitas gas belum READY.

### Jadwal awal yang dapat dikonfigurasi

| Pekerjaan | Target awal | Batas perilaku |
| --- | --- | --- |
| HX711 | Output filter tiap 1 s | Baca hanya saat ready; tidak menunggu DOUT tanpa batas. |
| Ultrasonic | Tiap slot 1 s | Trigger bergantian; gap awal 60 ms; timeout awal 30 ms, disesuaikan model dan dimensi. |
| MQ ADC | 10 sampel per detik per kanal | Filter per kanal; validity MQ-7 mengikuti fase heater. |
| DS18B20 | 1 s | Request conversion lalu baca setelah selesai; 12 bit dapat perlu 750 ms. |
| SHT31 | 2 s per sensor | Alamat terpisah; CRC dan timeout wajib. |
| Snapshot telemetry | 15 s | Tidak diperlambat oleh kegagalan satu sensor; nilai dan quality membentuk snapshot konsisten. |
| Reconnect | 1 hingga 60 s | Exponential backoff dengan jitter dan timeout jaringan terukur. |

Nilai jadwal adalah target engineering awal, bukan threshold penelitian. Scheduler memakai waktu monotonic; hindari delay panjang dan pembacaan sensor yang memblokir siklus lain.

## 6 Kalibrasi dan kualitas data

### Fill dan berat

Kalibrasi D_empty dan D_full dilakukan per slot setelah pemasangan mekanik. Rumus fill adalah 100 × (D_empty − D_measured) / (D_empty − D_full). Konfigurasi dengan D_empty ≤ D_full ditolak. Median pendek mengurangi spike; timeout atau jarak di luar jangkauan valid menghasilkan null. Clamp 0-100 hanya untuk overshoot kecil dengan tolerance yang tercatat; pembacaan di luar tolerance diberi OUT_OF_RANGE dan tidak disamarkan menjadi penuh atau kosong.

Load cell menyimpan offset tare, faktor konversi, dan versi kalibrasi masing-masing. Uji beberapa beban referensi, pengulangan, hysteresis, serta corner loading. Tare dilakukan dalam mode kalibrasi lewat maintenance USB saat wadah benar-benar kosong; tidak otomatis setiap boot. Negatif kecil hanya dapat menjadi nol jika masuk deadband kalibrasi yang disetujui. Negatif lebih besar tetap masuk diagnostik dan weight operasional menjadi null.

Firmware mengirim weight dalam kg. Backend menghitung weight utilization dari operational_capacity pada slots. Rating load cell 20 kg tidak langsung menjadi pembagi tanpa validasi wadah, beban mati, dan margin mekanik.

### Gas dan temperatur

Setiap MQ menyimpan adc_raw, adc_mv, sample age, heater phase bila relevan, quality, serta calibration_version. Pembacaan mV berasal dari jalur ADC yang dikalibrasi, bukan asumsi linear 4095 = 3,3 V. Bila kalibrasi tidak tersedia, simpan adc_raw dan tandai status mV. Nilai Rs atau Rs/R0 hanya ditambahkan ketika VC, RL efektif, divider, dan R0 diketahui. OGI dan ppm tidak dihitung menggunakan konstanta generik internet.

DS18B20 memakai identitas ROM sensor yang disimpan, presence check, CRC, dan conversion-complete check. Nilai power-on 85 °C tidak dipakai sebelum conversion valid; hasil 85 °C yang benar setelah konversi tidak dibuang hanya karena nilainya sama. Error disconnect library tidak dikirim sebagai temperatur. SHT31 menggunakan quality dan age terpisah untuk kedua alamat; humidity tetap disimpan meski tidak menjadi input FIS universal.

### Aturan quality

| Flag | Arti | Perlakuan |
| --- | --- | --- |
| READY | Pembacaan baru dan valid | Boleh dipakai sebagai input operasional. |
| WARMING_UP / HEATER_PHASE | Gas belum dalam fase valid | Operasional null; kanal lain tetap berjalan. |
| UNCALIBRATED | Konversi belum dapat dipercaya | Raw dipertahankan; nilai terkonversi null. |
| CALIBRATING | Target dalam sesi kalibrasi | Nilai operasional null; raw trial dan target sesi dicatat. |
| TIMEOUT / DISCONNECTED / CRC_ERROR | Akuisisi gagal | Null; catat reason dan consecutive failures. |
| STALE / OUT_OF_RANGE | Terlalu lama atau tidak masuk envelope valid | Tidak menggunakan last good value sebagai nilai baru. |

Freshness awal 5 s untuk HX711, jarak, DS18B20, dan SHT31; MQ-4/135/8 10 s. MQ-7 mengikuti sample valid cycle dan maksimal satu cycle sesuai profile. Flag hardware ONLINE/DEGRADED/FAILED terpisah dari keputusan dashboard. Sensor wajib awal adalah fill dan weight tiap slot serta DS18B20 B3. Kegagalan kanal MQ opsional tidak membuat seluruh unit gagal; model FIS kelak boleh menetapkan input wajib tambahan secara eksplisit.

## 6A Mode kalibrasi wajib

FR CAL 01 Mode kalibrasi adalah fitur P0 yang selalu tersedia melalui maintenance USB native, termasuk saat Wi-Fi terputus dan microSD tidak terpasang. Kalibrasi memilih sensor dan slot secara eksplisit. Mode berlaku per target: sensor lainnya tetap dibaca, heater tetap mengikuti profile, heartbeat tiga slot tetap berjalan, dan target ditandai CALIBRATING. Tidak ada tare otomatis setiap boot.

### Alur teknisi

- Masuk MAINTENANCE melalui USB lokal, lalu calibration start <slot> <sensor>. Tampilkan calibration_version aktif dan persyaratan kosong atau beban referensi.
- CAPTURE mengambil beberapa pembacaan stabil dan menampilkan raw, variasi, unit, quality serta target yang sedang dikerjakan. Timeout, sensor tidak hadir, atau hasil tidak stabil membatalkan capture dengan alasan.
- VALIDATE memeriksa anchor dan faktor, lalu PREVIEW memperlihatkan hasil konversi draft. Teknisi menguji titik referensi lain sebelum memilih commit.
- COMMIT menyimpan profil baru dan calibration_version secara atomik di NVS, lalu memverifikasi pembacaan kembali. Draft tidak mengubah profil aktif sebelum commit berhasil.
- CANCEL, timeout sesi, kabel terputus, atau reboot membuang draft dan mempertahankan profil terakhir yang utuh. Profil aktif sebelumnya disimpan untuk rollback eksplisit.
- Keluar mode kembali ke RUN. Ekspor/impor profil tanpa kredensial melalui USB; impor memeriksa schema, checksum, identitas sensor dan slot sebelum diaktifkan.

| Sensor | Data kalibrasi | Gate commit |
| --- | --- | --- |
| HX711 tiap slot | Offset tare, beban referensi kg, counts per kg dan deadband | Wadah kosong stabil; referensi positif; faktor terhingga dan tidak nol; tanda mengikuti wiring. |
| Jarak tiap slot | D_empty, D_full, mm, tolerance dan mounting | Anchor stabil dan D_empty > D_full; berada dalam jangkauan model aktual. |
| Empat MQ | Raw baseline, ADC/divider, VC/RL dan R0 bila diketahui | Warm-up dan fase valid; tanpa referensi gas tidak mengklaim ppm atau kalibrasi ilmiah. |
| DS18B20 dan SHT31 | Identitas ROM/alamat; perbandingan referensi; offset opsional | CRC valid; offset hanya jika hasil uji mendukung, bukan koreksi otomatis. |

FR CAL 02 Profil menyimpan schema_version, calibration_version, target, sensor identity, koefisien beserta unit, waktu/quality waktu, catatan referensi dan checksum. Jika belum ada waktu UTC, kalibrasi lokal tetap bisa disimpan dengan boot_id dan waktu monotonic; jangan membuat waktu UTC palsu. Tidak adanya profil pada sensor yang membutuhkan konversi menghasilkan UNCALIBRATED dengan nilai terkonversi null.

FR CAL 03 health memuat device_mode RUN/MAINTENANCE/CALIBRATING, calibration_targets dan versi profil per target. Nilai operasional target menjadi null selama kalibrasi; raw trial tetap dicatat. Data trial diberi provenance kalibrasi dan tidak memicu FIS/safety sebagai reading normal. Saat draft selesai, snapshot berikutnya mencantumkan versi baru; paket lama dalam antrean tetap memakai byte dan versi asalnya.

## 7 Project PlatformIO, struktur kode, dan configuration architecture

Gunakan C++ dengan framework Arduino pada PlatformIO untuk MVP. Backend dan frontend tetap memakai toolchain existing. Platform Espressif32 dipin ke 7.1.3; implementasi tidak mengasumsikan API Arduino 3.x. Gunakan subset C++ yang didukung toolchain target dan native-test environment; jangan mengadopsi C++20/23 hanya karena skill C++ generik menyarankannya. Prinsip yang diambil adalah typed interfaces, RAII bila sesuai, `const`/`constexpr` default, explicit ownership, bounded memory, dan larangan mutable non-const global configuration.

### Rancangan platformio awal

```ini
[platformio]
default_envs = sigap_n16r8

[env]
framework = arduino
monitor_speed = 115200
test_framework = unity

[env:sigap_n16r8]
platform = espressif32@7.1.3
board = esp32-s3-devkitc-1
board_upload.flash_size = 16MB
board_build.flash_mode = qio
board_build.arduino.memory_type = qio_opi
board_build.psram_type = opi
board_build.partitions = partitions_n16r8.csv
board_build.filesystem = littlefs
build_flags =
  -DBOARD_HAS_PSRAM
  -DSIGAP_BOARD_N16R8
  -DARDUINO_USB_MODE=1
  -DARDUINO_USB_CDC_ON_BOOT=1

[env:sigap_n16r8_sd]
extends = env:sigap_n16r8
build_flags =
  ${env:sigap_n16r8.build_flags}
  -DSIGAP_USE_SD=1

[env:native]
platform = native
test_framework = unity
```

Board ID bawaan adalah profil N8 tanpa PSRAM; override N16R8 harus dibuktikan pada hardware. Dependency sensor dipin exact setelah spike build.

### Partition awal tanpa OTA

```csv
# Name, Type, SubType, Offset, Size
nvs, data, nvs, 0x9000, 0x5000
phy_init, data, phy, 0xe000, 0x1000
factory, app, factory, 0x10000, 0x400000
spiffs, data, spiffs, 0x410000, 0x800000
```

Label `spiffs` dipakai partition Arduino untuk LittleFS. App 4 MiB dan filesystem 8 MiB tidak overlap pada flash 16 MiB. Quota pending awal LittleFS adalah 6 MiB; sekitar 2 MiB dicadangkan untuk journal, model/profile, metadata gap, filesystem overhead, dan ruang kerja. NVS menyimpan config/secret/active-pointer kecil, bukan archive telemetry.

### Struktur file target

```text
firmware/
├── platformio.ini
├── partitions_n16r8.csv
├── include/
│   └── sigap/
│       ├── board/board_pins.h
│       ├── build/build_config.h
│       ├── build/firmware_version.h
│       └── config/config_defaults.h
├── src/
│   ├── main.cpp
│   └── app/
│       ├── sigap_app.h
│       └── sigap_app.cpp
├── lib/
│   ├── config/
│   │   ├── config_types.h
│   │   ├── config_manager.h
│   │   ├── config_manager.cpp
│   │   ├── config_validator.h
│   │   ├── config_validator.cpp
│   │   ├── config_codec.h
│   │   └── nvs_config_store.cpp
│   ├── provisioning/
│   │   ├── secret_store.h
│   │   ├── secret_store.cpp
│   │   └── usb_provisioning.cpp
│   ├── profiles/
│   ├── calibration/
│   ├── sensors/
│   ├── heater/
│   ├── processing/
│   ├── fuzzy/
│   ├── safety/
│   ├── storage/
│   ├── telemetry/
│   └── runtime/
├── data/
│   ├── models/
│   └── factory_defaults/
└── test/
    ├── native/
    │   ├── config/
    │   ├── processing/
    │   ├── fuzzy/
    │   └── telemetry/
    ├── contract/
    └── target/
```

`main.cpp` hanya entry point; composition root berada di `SigapApp`. Driver tidak membaca global config secara implicit. Modul menerima typed config yang dibutuhkan melalui constructor/`begin()` sebagai value atau `const&`, dan tidak menyimpan pointer ke draft config.

### 7A Klasifikasi variable yang dapat dikustomisasi

| Kategori | Contoh | Persistensi | Mutability P0 |
| --- | --- | --- | --- |
| `BuildConfig` | schema protocol, feature flag, hard buffer limit | firmware image | compile-time only |
| `BoardConfig` | GPIO, bus, flash/PSRAM/partition | firmware image | compile-time only |
| `AppConfig` | sampling period, summary period, retry/backoff, batch bounds, storage quota dalam batas fisik | NVS versioned snapshot | draft/validate/commit; aktif setelah reboot |
| `SecretStore` | Wi-Fi SSID/password, Bearer device token | namespace NVS terpisah | write/rotate via USB; secret tidak dapat di-echo |
| `CalibrationProfile` | HX711 tare/scale, D_empty/D_full, ADC/R0 evidence | versioned profile + active pointer | calibration workflow |
| `ModelProfile` | FIS membership/rules, safety threshold/hysteresis yang telah disetujui | versioned profile + active pointer | profile workflow, bukan config set |
| `RuntimeState` | Wi-Fi state, queue depth, heater phase, counters, task health | RAM; checkpoint diagnostic khusus bila perlu | bukan configuration |

GPIO, schema/hash semantics, record ID format, dan hard protocol contract **tidak boleh** menjadi runtime-configurable variable. Calibration coefficient dan model ilmiah juga tidak boleh disamarkan sebagai generic config.

### 7B AppConfig lifecycle dan atomic activation

`AppConfig` memiliki `schema_version`, monotonic `config_version`, dan `config_hash`. Factory defaults berasal dari `constexpr`/typed defaults dan dipakai hanya jika perangkat belum pernah dikonfigurasi; config corrupt pada perangkat yang sudah provisioned tidak boleh diam-diam kembali ke default produksi.

Persistence memakai dua slot NVS, misalnya `cfg_a` dan `cfg_b`. Commit menulis **seluruh serialized snapshot** ke slot inactive, read-back + decode + validate + checksum/hash, lalu menulis pointer `cfg_active`. Power loss sebelum pointer berpindah mempertahankan snapshot lama; setelah pointer berpindah firmware harus menemukan snapshot baru lengkap atau fallback previous-valid. Multi-key `Preferences::put*` tidak dianggap sebagai transaksi atomik lintas seluruh config.

```text
ACTIVE
  └─ config edit/start
       ↓
     DRAFT
       ↓ set ...
    VALIDATE ── invalid → tetap DRAFT + reason
       ↓
     PREVIEW
       ↓
     COMMIT → inactive slot → readback verify → switch active pointer
       ↓
  REBOOT_REQUIRED
       ↓ controlled reboot
     ACTIVE(new config_version)
```

P0 tidak melakukan hot reload sampling/network/storage. Parameter baru aktif pada boot berikutnya sehingga seluruh task melihat snapshot konsisten. P1 boleh menambah hot reload per-field hanya setelah ownership/synchronization dan recovery path diuji.

### 7C Validation dan provenance

Setiap field runtime config harus mempunyai type, unit, default, min/max atau enum, dan apply policy.

| Field | Default awal | Guard |
| --- | ---: | --- |
| `sampling.hx711_period_ms` | 100 ms | >0; capability driver/CPU/logger harus lolos |
| `sampling.mq_period_ms` | 100 ms | tidak membuat ADC/logger overload |
| `sampling.ultrasonic_period_ms` | 1000 ms | acoustic gap antar-slot tetap dijaga |
| `telemetry.summary_period_ms` | 15000 ms | 5000–300000 ms sebelum pilot |
| `telemetry.max_records_per_batch` | 128 | 1–128 dan body hard limit tetap berlaku |
| `network.retry_min_ms` | 1000 ms | >0 dan ≤ retry_max |
| `network.retry_max_ms` | 60000 ms | ≥ retry_min |
| `storage.flash_pending_quota_bytes` | 6 MiB | ≤ usable LittleFS setelah reserve |
| `storage.sd_pending_quota_bytes` | 20 GiB | ≤ capacity/retention budget kartu tervalidasi |

Angka di atas adalah engineering defaults/gates, bukan angka ilmiah sensor. Setiap sample/result/event mencantumkan `config_version` dan `config_hash` aktif sehingga perubahan sampling/retry/storage dapat diaudit bersama calibration/model refs.

### 7D SecretStore dan provisioning

Secret berada pada boundary terpisah dari `AppConfig`. API maintenance boleh `set/rotate`, tetapi `show/export` hanya menampilkan presence atau redacted metadata. Secret tidak masuk JSON config export, calibration/model profile, telemetry, health, crash log, atau debug dump.

USB maintenance MVP memakai physical-access trust boundary. Factory reset dibedakan: reset `AppConfig` tidak otomatis menghapus calibration/model; reset secret memerlukan command/operator intent terpisah; full factory erase adalah tindakan maintenance eksplisit dengan warning.

## 8 Modul dan alur runtime

| Path rencana | Tanggung jawab |
| --- | --- |
| `firmware/include/sigap/board/board_pins.h` | GPIO/bus compile-time dan duplicate/reserved pin checks. |
| `firmware/include/sigap/build/` | Build constants dan firmware version. |
| `firmware/include/sigap/config/config_defaults.h` | Typed factory/engineering defaults tanpa secret. |
| `firmware/lib/config/` | `AppConfig`, validator, codec, NVS A/B snapshot, migration schema config. |
| `firmware/lib/provisioning/` | `SecretStore`, redaction, USB provisioning dan token rotation flow. |
| `firmware/lib/profiles/` | Manifest/hash/pointer untuk calibration/FIS/safety profiles. |
| `firmware/src/app/SigapApp` | Composition root dan ownership lifecycle; tidak ada service-locator mutable global. |
| `firmware/lib/sensors/` | Driver sensor. |
| `firmware/lib/heater/` | State machine MQ-7. |
| `firmware/lib/processing/` | Filter, conversion, quality, result snapshot. |
| `firmware/lib/calibration/` | Draft calibration, validate, commit, rollback. |
| `firmware/lib/storage/` | Archive LittleFS/SD, record codec, journal ACK, recovery. |
| `firmware/lib/telemetry/` | Batch builder, exact serializer, HTTPS, ACK validation, retry policy. |
| `firmware/lib/runtime/` | Scheduler, timebase, health, watchdog, USB maintenance state. |

### Urutan startup

1. Capture reset reason, board/build identity, flash/PSRAM, dan `boot_id`.
2. Load active `AppConfig` slot dari NVS, decode/hash/validate. Active corrupt → previous-valid. Unit provisioned tanpa snapshot valid → `CONFIG_REQUIRED`, bukan silent factory fallback.
3. Load secret presence tanpa mencetak nilai; load pointer calibration/model dan validate profile hash.
4. Set GPIO safe, konstruksi/start modul dengan snapshot config yang sama. Tidak ada task memegang mutable draft config.
5. Mulai acquisition/logger sebelum Wi-Fi; UTC boleh null.
6. Mulai summary-v2/archive-v3 workers; ACK server memajukan journal.
7. Health menampilkan firmware/profile/config version/hash dan runtime counters, tetapi tidak secret.

Driver/resource shared memiliki owner dan synchronization boundary eksplisit. I2C/SPI/filesystem/NVS tidak diakses lintas task secara implicit. Jangan memakai global variable sebagai implicit synchronization. HTTP/TLS blocking tidak berada pada jalur timing acquisition. Draft config dan active config tidak berbagi object mutable.

Maintenance USB menyediakan diagnostics, provisioning, config edit/validate/preview/commit, calibration/profile workflow, dan controlled reboot.

## 9 Kontrak telemetry yang kompatibel

Pertahankan device_id, recorded_at, slots, dan health. Pada backend sekarang, device_id dicocokkan terhadap devices.device_code, bukan UUID devices.id. Contoh memakai SIGAP-01 hanya sebagai device code; perangkat serta tiga slot harus sudah diprovision di database dengan credential digest.

Scalar gas_raw awal dibiarkan null untuk menghindari pemilihan atau agregasi yang belum disepakati. Semua kanal gas, jarak, HX711, dan humidity dicatat terpisah dalam health.sensors. temperature Organik memakai SHT31; temperature B3 memakai DS18B20. health.sensors.sht31_b3 tetap memuat temperatur lingkungan dan RH sebagai kanal berbeda. Berikut contoh struktur paket; angka adalah fixture ilustrasi, bukan hasil eksperimen.

```json
{
  "device_id": "SIGAP-01",
  "recorded_at": "2026-09-30T06:30:00.000Z",
  "slots": [
    {"stream":"organic","fill":72,"weight":12.5,
     "gas_raw":null,"temperature":29.1},
    {"stream":"inorganic","fill":45,"weight":8.2,
     "gas_raw":null,"temperature":null},
    {"stream":"b3","fill":18,"weight":3.1,
     "gas_raw":null,"temperature":32.0}
  ],
  "health": {
    "telemetry_version":1,"firmware_version":"0.1.0",
    "device_mode":"RUN","calibration_targets":[],
    "board_profile":"n16r8","calibration_version":"cal-001",
    "config_version":"cfg-000004","config_hash":"sha256:<redacted-example>",
    "boot_id":"boot-example","sequence":"120",
    "wifi_rssi":-61,"uptime_s":1800,"time_quality":"SYNCED",
    "queue_depth":0,"unlogged_records":0,"gap_count":0,
    "sensors": {
      "mq4":{"stream":"organic","adc_raw":1234,
             "adc_mv":690,"quality":"READY","age_ms":90},
      "mq135":{"stream":"organic","adc_raw":1320,
               "adc_mv":735,"quality":"READY","age_ms":90},
      "mq7":{"stream":"b3","adc_raw":null,"adc_mv":null,
             "quality":"HEATER_PHASE","phase":"HIGH_CLEAN"},
      "mq8":{"stream":"b3","adc_raw":1100,"adc_mv":610,
             "quality":"READY","age_ms":90},
      "sht31_organic":{"address":"0x44","temperature_c":29.1,
                       "humidity_pct":68,"quality":"READY","age_ms":600},
      "sht31_b3":{"address":"0x45","temperature_c":30.2,
                  "humidity_pct":60,"quality":"READY","age_ms":600}
    }
  }
}
```

Paket lengkap juga memuat hx711_organic/inorganic/b3 dengan counts dan age_ms; distance_organic/inorganic/b3 dengan distance_mm dan quality; ds18b20_b3 dengan ROM, temperature_c dan quality. Sebagian objek ini disingkat pada contoh agar mudah dibaca. Limit serializer awal 4096 byte termasuk seluruh health; melebihi limit menghasilkan error eksplisit, tidak truncation. Satu field null selalu berarti tidak tersedia, tidak sama dengan hasil nol.

Schema health v1 perlu didokumentasikan dan dites karena backend sekarang menyimpannya tanpa validasi semantik. Perubahan format berikutnya menaikkan telemetry_version; payload lama tetap dapat dibaca dengan aturan legacy yang eksplisit.

Archive-v3 adalah kontrak additive yang terpisah dari contoh summary-v2 di atas. Setiap record archive-v3 mempunyai `record_id`, `record_hash`, decimal-string uint64, `config_ref`, dan `profile_refs`; `recorded_at:null` tetap sah ketika UTC belum tersedia. `manifest_ids` tidak menjadi field wire archive-v3 karena server memperoleh record ID dari array dan provenance profile dibawa oleh `profile_refs`.

Freeze archive-v3 juga mengikat `sensor_id` ke stream (`sht31_44` ke organic dan `sht31_45` ke B3), serta memakai payload typed untuk setiap keluarga sensor, result, event, dan metadata. Payload arbitrary, ppm/R0/OGI gas yang belum memiliki profile ilmiah, dan trace tanpa schema ditolak oleh validator serta fixture corpus pada `contracts/telemetry/v3`.

## 10 Waktu jaringan dan antrean

### Waktu pengukuran

Archive record selalu mempunyai `boot_id`, uint64-string `sequence`, dan `monotonic_us`. `recorded_at` adalah UTC RFC3339 atau null. NTP menambahkan clock anchor append-only; koreksi jam berikutnya tidak menulis ulang record lama. Cold boot tanpa waktu tetap mengakuisisi, menyimpan, dan akhirnya mengunggah archive-v3 dengan UTC-null bila tidak pernah memperoleh anchor. Summary-v2 hanya dibentuk saat UTC yang memenuhi quality policy tersedia.

### HTTP dan pengakuan server

Wi-Fi/HTTPS adalah transport P0. Summary-v2 mempertahankan exact replay semantics existing. Archive-v3 memakai batch immutable dengan body hash exact-wire dan ACK durable server.

| Respons archive-v3 | Tindakan firmware |
| --- | --- |
| `200` + ACK valid | Tandai record batch `server_acked`; reclaim hanya setelah journal/checkpoint. |
| Timeout / `429` / `5xx` | Pertahankan byte batch identik; retry dengan backoff + jitter; hormati `Retry-After`. |
| `401` / `403` | `AUTH_ERROR`; pending dipertahankan; operator merotasi token via USB. |
| `400` / `404` / `409` / `415` | Quarantine/fault manifest tanpa menghapus record. |
| ACK malformed/hash/count mismatch | `PROTOCOL_ERROR`; journal lokal tidak maju. |

Network worker tidak memblokir acquisition/logger. Token berada pada Authorization header dan tidak disimpan di batch sehingga token rotation tidak mengubah immutable pending body.

### Persistensi dan pemulihan

Pisahkan counter `acquired`, `durable`, dan `server_acked`. Record baru dianggap durable hanya setelah framing/checksum dan flush/checkpoint storage berhasil. Server ACK hanya mengubah delivery journal; tidak mengubah raw record. Power loss/reboot melakukan recovery tail dan ACK journal sebelum upload dilanjutkan. Pending yang pernah dikirim tidak boleh diserialisasi ulang menjadi body berbeda.

## 10A Penyimpanan offline dengan microSD opsional

microSD menjadi archive backend utama bila sehat; LittleFS 8 MiB menjadi fallback. Hardware SD tetap opsional, tetapi firmware storage abstraction wajib mendukung kedua backend. Archive menyimpan **semua sample attempt, result, event, dan metadata**, bukan hanya snapshot 15 s.

FR STORE 01 Setiap record memiliki frame versioned, length bound, record ID/kind, checksum, payload, dan commit marker/checkpoint. Tail parsial dikarantina saat recovery; index dapat dibangun ulang dari record utuh.

FR STORE 02 Pending immutable sampai ACK server. Batch boleh dibentuk ulang hanya sebelum percobaan pengiriman pertama. Setelah `batch_id/body_hash` ditetapkan, retry memakai exact bytes yang sama.

FR STORE 03 LittleFS pending quota awal 6 MiB dengan reserve terpisah untuk journal/gap/profile/filesystem overhead. Gate tanpa SD adalah **≥60 detik seluruh archive record** pada sampling profile yang disetujui; kapasitas nyata diukur, bukan dihitung dari snapshot 15 s.

FR STORE 04 SDHC 32 GB opsional memakai pending quota awal 20 GiB. Gate varian SD adalah **≥72 jam seluruh archive record** pada workload worst-case yang disetujui. FAT32/card model harus diuji pada hardware.

FR STORE 05 SD absent/mount fault/I/O/full → `STORAGE_DEGRADED` dan fallback flash bila tersedia. Pending pada SD tidak dihapus. Memasukkan kartu tidak memformat otomatis. Migrasi flash→SD memakai copy → verify → checkpoint → reclaim.

FR STORE 06 Jika seluruh quota tidak dapat menerima record baru, **jangan overwrite/drop pending tertua**. Runtime masuk `LOGGING_BLOCKED`; health mencatat `unlogged_records`, gap ranges, media/cause, pending age/bytes, dan counter durability. Produk tidak boleh mengklaim data tersebut tersimpan.

FR STORE 07 Config/secret/calibration/model active-pointer berada pada NVS/profile store terpisah dari archive. Config commit memakai A/B snapshot; archive failure tidak boleh mengubah config aktif dan config reset tidak boleh menghapus backlog.

Uji append/checkpoint/ACK/power-cut/hot-remove/quota-full serta p99 write latency, wear flash, high-water queue, tail loss, dan final ID/hash reconciliation.

## 10B Jalur WiFi dan opsi Bluetooth

FR NET 01 Wi-Fi memakai HTTPS ke API existing dan menjadi transport P0. Kualitas RSSI hanya diagnostik; status delivered ditentukan ack server. Firmware terus mengakuisisi dan menyimpan saat Wi-Fi tidak memenuhi jangkauan. Reconnect memakai backoff dan tidak memaksa reset hanya karena internet terputus.

FR NET 02 ESP32-S3 menyediakan Bluetooth Low Energy, tanpa Bluetooth Classic. BLE dapat membawa telemetry melalui layanan GATT custom, tetapi perlu aplikasi HP atau gateway komputer yang meneruskan byte paket ke HTTPS backend. BLE tidak diperlakukan sebagai jaringan IP pengganti Wi-Fi secara otomatis. Opsi ini mengikuti permintaan pengguna bila layak, bukan kewajiban MVP.

### Rancangan gateway bila spike lolos

- ESP32-S3 bertindak sebagai BLE peripheral; aplikasi gateway bertindak sebagai central. Gateway membuat koneksi authenticated dan dapat mengunggah saat internet gateway tersedia.
- Fragment paket berdasarkan MTU yang dinegosiasikan; setiap pesan memakai record_id, indeks/total fragmen, panjang dan checksum. Rekonstruksi menghasilkan body JSON identik dengan Wi-Fi.
- Status RECEIVED_BY_GATEWAY belum menghapus antrean device. Hanya ack backend yang cocok dengan device_id, recorded_at dan hash body diteruskan sebagai SERVER_ACK; timeout dan gateway restart menyebabkan replay aman.
- Gunakan satu arbiter pengiriman: Wi-Fi didahulukan, BLE hanya ketika gateway tersedia dan policy mengizinkan. Ack yang terlambat dari salah satu transport tetap idempotent; retry menggunakan snapshot asal.
- Pairing saja tidak dianggap cukup untuk otorisasi gateway. Spike menentukan provisioning kunci gateway dan autentikasi ack; device bearer token tidak disiarkan melalui characteristic terbuka atau log.
- BLE terputus atau aplikasi HP ditutup tidak menghentikan penyimpanan. Jika Wi-Fi dan gateway sama-sama tidak tersedia, data tetap pending pada SD/flash.

FR NET 03 Jalankan spike end-to-end sebelum memasukkan BLE ke scope implementasi: ukur throughput body 4 KiB tiap 15 s dan drain backlog, coexistence Wi-Fi/BLE, konsumsi RAM, fragment retry, koneksi kembali, serta perilaku HP ketika layar mati dan aplikasi di background. Repo frontend Expo Web belum merupakan gateway BLE native. Dukungan Android/iOS dan library native perlu ditentukan terpisah, bukan diasumsikan dari dashboard web.

Gate P1: BLE gateway diterapkan hanya jika hasil spike memenuhi throughput akuisisi, tidak mengganggu timing sensor, dan alur lapangan dapat dipakai. Bila tidak, mode kalibrasi lokal, Wi-Fi, serta microSD tetap memenuhi baseline. Alternatif operasional adalah ekspor log dari SD saat maintenance; ekspor tidak mengubah flag delivered sebelum backend mengakui paket.

## 11 Perubahan integrasi backend yang diperlukan

Semua perubahan tetap berada dalam Go monolith existing dan bersifat additive.

### Validasi dan keputusan

`internal/ingest/model.go`/`service.go` tetap summary-v2/legacy. Untuk `HARDWARE_EDGE_V3`, buat DTO/validator/service terpisah (misalnya `internal/telemetry`) yang memeriksa schema, auth device binding, record ID, uint64-string sequence, time quality, `config_version/config_hash`, profile refs, source IDs, quality/reason, hash, body/record limits, dan enum.

Jalur hardware-v3 **tidak boleh memanggil** `Normalize`, `persistDecision`, fuzzy engine, atau safety evaluator backend. Hasil MCU disimpan persis bersama provenance. Simulator/legacy mempertahankan inference backend sampai migrasi tersendiri.

Backlog/current policy menggunakan waktu ukur/time quality sehingga data lama yang baru tiba tidak memundurkan current projection dan tidak memicu event seolah baru.

### Research dan ekspor

Archive/read/export menyediakan raw, converted, quality/reason, time provenance, firmware version, `config_version/config_hash`, calibration/model refs, dan source IDs. SQLite `INTEGER` bertanda tidak dapat mewakili seluruh uint64; archive menyimpan `sequence_decimal` asli dan `sequence_sort` 20-digit zero-padded untuk cursor/order numerik yang stabil. Existing history cursor/CSV tidak berubah diam-diam; archive endpoint/export version baru memakai cursor `(device_id,boot_id,sequence)`. Secret/config payload penuh tidak diekspos; hanya allowlisted non-secret manifest/provenance.

### Provisioning dan versioning

Backend provisioning mendaftarkan location, `device_code`, credential digest, tiga slots, dan operational capacity. Hardware provisioning melalui USB menyimpan runtime setting pada versioned `AppConfig` dan credential pada `SecretStore`.

Token rotation: provision token baru via USB, aktifkan digest baru server-side, uji batch, baru revoke token lama. Pending body tidak mengandung token.

Expected migration pada HEAD ini adalah `005_telemetry_archive.sql`; implementation PR wajib recheck `migrations/`. Integration test mencakup legacy summary, hardware-v3 archive, config provenance, token binding/rotation, dan redaction.

## 12 Tahapan implementasi

Kerjakan sebagai perubahan kecil yang dapat diverifikasi; estimasi kalender tidak dibekukan sebelum hardware/profile capacity terukur.

| Tahap | Output yang dapat direview | Gate selesai |
| --- | --- | --- |
| T0 | Freeze hardware identity/wiring, schema telemetry v3, config schema v1, secret boundary, profile identity. | Tidak ada konflik authority/GPIO; field config punya type/unit/range/apply policy. |
| T1 | PlatformIO, partition, `SigapApp`, `AppConfig` types/defaults/validator, NVS A/B store, SecretStore skeleton, native tests. | Build MCU/native pass; config power-cut/rollback tests pass. |
| T2 | Driver sensor/heater, scheduler, record envelope, calibration/profile manager, config provenance. | Seluruh sensor terakuisisi; failure isolation; no mutable global config. |
| T3 | Processing/FIS/safety edge + maintenance USB config/calibration/provisioning. | DRAFT/VALIDATE/PREVIEW/COMMIT/reboot dan profile rollback terbukti. |
| T4 | Logger LittleFS/SD, journal/gap accounting, exact immutable batch, summary-v2/archive-v3. | ≥60 s flash; SD gate bila dipilih; retry/ACK idempotent. |
| T5 | Backend v3 migration/service/read/export/current projection. | Legacy compatible; hardware-v3 tidak menjalankan inference backend. |
| T6 | Fault injection, config corruption/power cut, network/storage failure, throughput/endurance, 24 h soak, SOP. | Acceptance relevan mempunyai evidence terbaru. |

### Strategi verifikasi

Native tests mencakup config defaults, bounds/cross-field constraints, deterministic encoding/hash, A/B slot selection, corrupt-active fallback, interrupted commit, config schema migration/rejection, secret redaction, serta memastikan calibration/model tidak tercampur ke generic config. Tambahkan static/code review untuk mutable non-const globals dan heap allocation pada hot path.

Target MCU menguji boot valid/corrupt/missing config, controlled reboot activation, NVS persistence, secret rotation, acquisition deadlines, stack high-water, heap/PSRAM trend, flash/SD throughput, watchdog dan power-cut. Go tests menguji provenance config, archive idempotency, auth binding, UTC-null, cursor/current projection dan no-secret output.

## 13 Kriteria penerimaan

| ID | Kriteria | Bukti yang diperlukan |
| --- | --- | --- |
| AC01 | Build N16R8/native dari checkout bersih. | `pio run` dan `pio test`. |
| AC02 | Board/pin profile aman untuk N16R8/USB/memory. | Hardware/pin evidence. |
| AC03 | Semua sensor terakuisisi bersama. | Diagnostic capture + quality. |
| AC04 | Fill calibration memenuhi target engineering awal. | Multi-level reference test. |
| AC05 | Weight repeatability/hysteresis terukur. | Reference load test. |
| AC06 | MQ-7 mengikuti datasheet/profile aktual. | Rail/timing/state evidence. |
| AC07 | Sensor wajib fault → null/invalid + reason; kanal lain lanjut. | Fault injection. |
| AC08 | Summary-v2 existing tetap kompatibel. | SQLite/API integration. |
| AC09 | Summary exact replay dedupe dan conflicting replay ditolak. | Retry/409 tests. |
| AC10 | LittleFS ≥60 s full archive tanpa overwrite pending. | ID/hash reconciliation. |
| AC11 | Power-loss tidak merusak seluruh queue/config. | Cut-power append/ACK/config tests. |
| AC12 | UTC/time anchor tidak memalsukan/menulis ulang historical time. | Cold boot/NTP/backward clock tests. |
| AC13 | Research/export menyediakan semua kanal + unit/quality/provenance. | Export/API fixture. |
| AC14 | Tidak ada secret pada Git/config export/health/API/CSV/log. | Secret scan/redaction tests. |
| AC15 | 24 h soak tanpa reset/resource growth tak terbatas. | Heap/stack/queue/reset evidence. |
| AC16 | Model ilmiah belum valid tetap unavailable. | Negative model tests. |
| AC17 | Calibration offline draft/preview/commit/cancel. | USB workflow. |
| AC18 | Calibration/profile interrupted commit mempertahankan old/new profile utuh. | Power-cut/hash tests. |
| AC19 | Varian SD ≥72 h full archive. | Endurance/capacity + reconciliation. |
| AC20 | SD fault/removal/full fallback tanpa menghapus pending atau duplikasi recovery. | Storage fault tests. |
| AC21 | CALIBRATING tidak menjadi keputusan normal. | Contract/e2e. |
| AC22 | Hardware-v3 tidak menjalankan backend legacy inference. | Service/package test. |
| AC23 | Batch v3 atomik/idempotent; conflict → 409 tanpa partial commit. | Transaction fault tests. |
| AC24 | Result menyimpan source IDs + model/safety provenance lengkap. | Cross-language fixture. |
| AC25 | Completeness dibuktikan stable record ID + hash. | Generator→export reconciliation. |
| AC26 | Quota full → `LOGGING_BLOCKED`, no overwrite pending, gap accounting. | Quota/reboot tests. |
| AC27 | UTC-null diterima end-to-end; derived time additive. | Clock tests. |
| AC28 | Archive cursor stable dan backlog tidak memundurkan current. | Paging/backlog tests. |
| AC29 | Backlog drain sambil sampling tetap; durability counters reconcile. | 24 h + throughput. |
| AC30 | Tidak ada mutable non-const global configuration/implicit sync. | Static search + architecture review. |
| AC31 | `AppConfig` A/B snapshot survive reboot/power-cut tanpa partial active state. | Native/target NVS tests. |
| AC32 | Invalid/corrupt config tidak dapat aktif; previous-valid/maintenance path jelas. | Range/schema/corruption tests. |
| AC33 | Config P0 aktif setelah controlled reboot; satu boot memakai satu `config_version/config_hash`. | Boot/telemetry assertions. |
| AC34 | Credential/token terpisah dari AppConfig dan write-only/redacted. | Rotation/redaction tests. |
| AC35 | Calibration/FIS/safety profile dan RuntimeState bukan generic AppConfig; refs/version/hash benar. | Schema/profile/reboot tests. |

MVP wajib membuktikan **AC01–AC18 dan AC21–AC35**. AC19–AC20 menjadi gate tambahan bila varian microSD dipilih. Klaim keamanan B3/ppm tetap membutuhkan validasi ilmiah terpisah.

### Penerimaan bersyarat untuk BLE P1

Jika BLE gateway dipilih setelah spike, uji radio putus pada setiap tahap fragment, restart gateway, receipt lokal tanpa ack server, serta dua transport yang mencoba record yang sama. Queue device tidak boleh terhapus sebelum SERVER_ACK tervalidasi. Beri evidence throughput/heap, target OS, perilaku background dan kehilangan data; kegagalan gate ini menunda BLE, bukan mode kalibrasi wajib atau logging offline.

## 14 Keputusan yang perlu dikunci saat implementasi

| Keputusan | Status | Cara menyelesaikan |
| --- | --- | --- |
| Controller/MQ | Dikonfirmasi | N16R8; MQ-4/135/7/8; satu MCU untuk tiga slot. |
| Modul/revisi PCB | Perlu inspeksi | Verifikasi WROOM topology, flash/PSRAM, RGB, schematic. |
| Ultrasonic | Perlu freeze model | Model/blind-zone/timeout/divider/mounting. |
| MQ-7 | Perlu datasheet aktual | Heater rail/timing/safe boot. |
| SHT31 placement | Perlu freeze mounting | Lokasi/alamat sebelum calibration. |
| Edge FIS/safety | Dikonfirmasi | Hardware-v3 di MCU; legacy tetap backend. |
| Storage | Dikonfirmasi awal | LittleFS 8 MiB/pending 6 MiB; SD optional 20 GiB; no-overwrite; capacity diuji. |
| Configuration ownership | Dikonfirmasi | Immutable per-boot `AppConfig`; no mutable global config. |
| Config activation | Dikonfirmasi P0 | Draft/validate/commit A/B NVS + controlled reboot; no hot reload P0. |
| Secret boundary | Dikonfirmasi | Store/namespace terpisah, write-only/redacted, token rotation. |
| Config schema evolution | Perlu implementasi | Explicit migrate/reject policy; no silent reinterpretation. |
| Factory reset | Perlu SOP | Bedakan AppConfig reset, secret reset, profile reset, full erase. |
| Endpoint/TLS | Perlu deployment | TLS + persistent SQLite + credential provisioning. |
| Scientific model final | Deferred | Versioned profile berdasarkan evidence. |

## 15 Referensi untuk developer

Repo historis dibaca pada `6fed00f02b6ef7dabc159d924f798d344a6072ba`; reconciliation revisi 1.3 dilakukan terhadap `main` `7dd35f6d05cde09fe31a8a2f56933d04e4dc456a` pada 1 Oktober 2026. Referensi vendor dari revisi sebelumnya tetap menjadi sumber hardware; keputusan configuration architecture tambahan berasal dari skill repo yang tercantum di bawah dan diverifikasi terhadap constraint SIGAP.

- Repo: https://github.com/RayaSatriatama/IoT-Trashcan/tree/6fed00f02b6ef7dabc159d924f798d344a6072ba — README; docs/SIGAP_PRD_v1.0.md; docs/SIGAP_SDD_v1.0.md; docs/SIGAP_Smart_Bin_Konsep_dan_RAB_v0.8.md.
- Kontrak historis: internal/http/ingest_handler.go; internal/ingest/model.go; service.go; validation.go; normalize.go; internal/http/read_api.go; history_api.go; internal/device/offline.go; cmd/sigap/main.go; migrations/001_initial.sql.
- Skill engineering revisi 1.3: `.agents/skills/esp32-development`, `.agents/skills/embedded-development`, `.agents/skills/cpp-coding-standards`, `.agents/skills/config-nvs`, dan `.agents/skills/memory-audit`. Guidance project-specific upstream tidak otomatis menjadi kontrak SIGAP.
- Kontrak HEAD yang wajib direconcile: internal/ingest, internal/fuzzy, internal/safety, internal/history, internal/http/read_api.go, internal/simulator, dan migrations/004_device_research_trace.sql.
- PlatformIO board: https://docs.platformio.org/en/latest/boards/espressif32/esp32-s3-devkitc-1.html — default N8, override board options.
- Platform manifest: https://github.com/platformio/platform-espressif32/blob/v7.1.3/platform.json — platform pin dan package framework. Board manifest: boards/esp32-s3-devkitc-1.json pada tag yang sama.
- Espressif DevKitC-1: https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html — GPIO, USB, memori dan perubahan pin RGB.
- SHT3x: https://sensirion.com/media/documents/213E6A3B/63A5A569/Datasheet_SHT3x_DIS.pdf — dua alamat I2C dan CRC.
- DS18B20: https://www.analog.com/media/en/technical-documentation/data-sheets/DS18B20.pdf — 1-Wire, konversi, identitas ROM dan CRC.
- MQ-7B contoh profile: https://www.winsen-sensor.com/d/files/manual/mq-7b.pdf — pemisahan VH/VC dan siklus 60/90 s. Datasheet MQ-7 aktual tetap wajib sebelum freeze driver.
- MQ lain: https://www.winsen-sensor.com/product/mq-4.html ; https://www.winsen-sensor.com/manual/mq135.html ; https://www.winsen-sensor.com/product/mq-8.html — fungsi, suplai dan aging sensor.
- SD SPI Espressif: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/sdspi_host.html — SPI dan CS melalui GPIO yang dipilih; API implementasi mengikuti Arduino core pinned, bukan disalin dari IDF latest.
- BLE Espressif: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/ble/overview.html — S3 mendukung BLE tanpa Classic; gateway dan protokol relay adalah rancangan produk dokumen ini.
