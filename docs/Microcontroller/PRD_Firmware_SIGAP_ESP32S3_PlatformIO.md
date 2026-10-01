# PRD Firmware SIGAP ESP32 S3 dengan PlatformIO

Versi 1.3  |  1 Oktober 2026  |  Kontrak implementasi firmware, configuration architecture, telemetry archive, dan integrasi backend

Satu ESP32-S3 DevKitC-1 N16R8 menangani tiga slot Organik, Anorganik, dan B3. Firmware membaca seluruh sensor, melakukan kalibrasi, filtering, normalisasi, evaluasi FIS, dan safety lokal, mencatat setiap sampel serta hasil per siklus, lalu mengirim ringkasan dan arsip telemetry ke backend SIGAP. Untuk sumber `HARDWARE_EDGE_V3`, hasil yang diproduksi MCU adalah source of truth: backend Go mengautentikasi, memvalidasi kontrak, menyimpan, memproyeksikan current state, dan mengekspor data tanpa menghitung ulang FIS/safety. Konfigurasi runtime perangkat tidak berupa kumpulan mutable global variable: firmware menggunakan snapshot `AppConfig` tervalidasi dan berversi, secret terpisah, calibration/model profile terpisah, serta runtime state yang tidak diperlakukan sebagai konfigurasi. Jalur simulator/legacy mempertahankan pipeline backend yang ada sampai dimigrasikan secara terpisah. Dokumen ini menetapkan perilaku produk, kontrak configuration/provisioning, telemetry, rencana PlatformIO, serta kriteria penerimaan untuk memulai implementasi.

Revisi 1.3 menyatukan PRD dengan TSD v1.2 dan keputusan dari skill embedded yang telah di-merge: source of truth keputusan hardware berada di edge; arsip setiap sampel menjadi kontrak P0; policy storage-full dikunci ke **no overwrite of pending**; LittleFS menjadi 8 MiB dengan quota pending awal 6 MiB; schema telemetry v3 dibekukan; dan configuration architecture dibagi menjadi compile-time board/build constants, versioned `AppConfig`, write-only `SecretStore`, versioned calibration/model profiles, serta ephemeral `RuntimeState`. Mutable non-const global configuration dilarang. Revisi 1.2 menjadi asal freeze telemetry/archive; revisi 1.1 menjadi asal mode kalibrasi wajib P0 dan microSD opsional. Wi-Fi tetap jalur pengiriman utama; BLE tetap P1 bersyarat.

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
    "boot_id":"boot-example","sequence":120,
    "wifi_rssi":-61,"uptime_s":1800,"time_quality":"SYNCED",
    "queue_depth":0,"dropped_snapshots":0,
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

## 10 Waktu jaringan dan antrean

### Waktu pengukuran

recorded_at memakai UTC RFC3339 dengan fraksi milidetik, ditetapkan saat snapshot dibentuk. Waktu NTP menghasilkan timebase yang ditambatkan pada monotonic clock. Saat NTP koreksi mundur, timestamp snapshot tetap unik dan meningkat dalam boot yang sama; catat koreksi dan uncertainty. boot_id dan sequence membantu audit, tetapi tidak menggantikan timestamp sebagai kunci replay backend existing.

Sesudah cold boot tanpa waktu terpercaya, akuisisi berjalan dengan timestamp monotonic dan time_quality UNSYNCED. Snapshot sementara disimpan sebagai untimed. NTP pertama pada boot yang sama dapat memberi anchor untuk merekonstruksi UTC dengan flag RECONSTRUCTED. Jika listrik mati sebelum anchor tersedia, data untimed disimpan untuk diagnostik dan tidak dikirim sebagai waktu yang dibuat-buat. RTC tidak termasuk hardware baseline. Setelah reboot, jangan menganggap UTC terakhir dalam NVS sebagai jam yang terus berjalan selama listrik mati.

### HTTP dan pengakuan server

Kirim HTTPS dengan validasi CA dan hostname ke endpoint yang dikonfigurasi. Sertifikat tidak boleh dilewati dengan setInsecure. HTTP lokal hanya untuk bench tanpa token production. Server Go saat ini melayani HTTP; deployment pilot membutuhkan terminasi TLS melalui proxy atau platform hosting dan filesystem SQLite yang persistent.

| Respons | Tindakan firmware |
| --- | --- |
| 200 dan ack valid | Hapus entry jika device_id dan waktu cocok serta readings_stored = 3; replay valid menerima deduplicated = true. |
| Timeout / 5xx / 429 | Simpan byte paket dan retry dengan backoff; hormati Retry-After bila tersedia. |
| 401 / 403 | Tandai AUTH_ERROR; hentikan retry cepat; operator memperbaiki credential lokal. |
| 400 / 404 / 409 / 415 | Pindahkan paket ke quarantine terbatas dengan reason; jangan mengganti timestamp untuk memaksa diterima. |
| 200 dengan ack rusak | Entry tetap ada; catat PROTOCOL_ERROR dan retry terbatas. |
| TLS error | Pertahankan queue; catat sebab; tidak downgrade ke insecure. |

### Persistensi dan pemulihan

Simpan byte UTF-8 JSON hasil satu kali serialisasi dengan panjang, record_id, dan checksum; storage manager memakai LittleFS staging serta microSD opsional sesuai bagian 10A. Retry memakai body identik, termasuk health dan queue_depth lama; jangan mengambil health baru ketika mengulang paket. Queue berbasis append/ring dan journal tahan power-loss, bukan file JSON besar yang ditulis ulang setiap snapshot. Recovery mengabaikan tail record rusak dengan counter yang tercatat. PSRAM boleh menjadi buffer kerja, tetapi tidak menggantikan persistensi.

Target fallback internal tanpa SD adalah 512 snapshot × maksimal 4096 byte, sekitar 2 MiB sebelum overhead, di filesystem 4 MiB. Interval 15 s memberi cakupan sekitar 2 jam 8 menit. Target penerimaan minimal putus Wi-Fi 1 jam tanpa kehilangan snapshot bertimestamp. Saat quota pending penuh, buang entry pending tertua dengan dropped_snapshots yang tercatat agar akuisisi terbaru tetap tersedia; quarantine memiliki quota terpisah. Uji penggunaan ruang dan wear flash sebelum menganggap kapasitas ini terjamin.

Setelah reconnect, kirim snapshot terbaru untuk memperbarui monitoring, lalu drain backlog bertahap. Backend harus membedakan last_seen pada waktu penerimaan dan recorded_at pengukuran, serta menahan keputusan operasional dari backlog yang terlalu lama. Exact replay sekarang tidak memperbarui heartbeat; heartbeat tetap lewat snapshot baru. OFFLINE_AFTER_SECONDS tetap konfigurasi deployment yang disepakati, bukan nilai penelitian otomatis.

## 10A Penyimpanan offline dengan microSD opsional

FR STORE 01 Firmware menyediakan backend penyimpanan LittleFS dan microSD melalui SPI. Kartu belum menjadi komponen yang dikonfirmasi pengguna; varian tanpa SD harus tetap bekerja. Wi-Fi hilang, sinyal lemah, server tidak dapat dicapai, atau gateway BLE tidak tersedia semuanya mempertahankan paket pending. Keberhasilan koneksi radio atau penyerahan ke HP tidak sama dengan diterima server.

| Sinyal microSD | GPIO usulan | Konsekuensi |
| --- | --- | --- |
| SCK | 39 | SPI khusus; tidak berbagi driver sensor. |
| MISO | 40 | Input 3,3 V. |
| MOSI | 41 | Output 3,3 V. |
| CS | 43 | Mengambil UART0 TX; pull-up CS ke 3,3 V; console lewat USB native. |

FR STORE 02 Gunakan socket/modul microSD dengan logika 3,3 V, suplai stabil dan decoupling. Empat pin tambahan memberi total 24 GPIO tanpa expander; card-detect tidak diwajibkan. GPIO43 juga terhubung jalur TX UART0 pada DevKitC. Aplikasi tidak mengaktifkan UART0 TX; ukur CS saat reset dan flashing. Jangan clock SPI sebelum CS HIGH dan SD siap; review schematic board aktual sebelum pin di-freeze.

### Format queue dan kegagalan storage

FR STORE 03 LittleFS menjadi staging persisten setiap snapshot, termasuk online. Storage manager memindahkan record ke segmen append SD dengan record_id (boot_id + sequence), panjang, checksum, JSON immutable, serta journal ack. Copy staging baru boleh dihapus setelah write/flush SD dan checkpoint terverifikasi. Saat reboot, gabungkan record flash dan SD berdasarkan record_id; jangan membuat timestamp atau JSON baru. Ack valid server menandai delivered secara durable sebelum reclaim segmen. Quarantine dan diagnostik mempunyai quota terpisah.

FR STORE 04 Kartu hilang, gagal mount, I/O error, atau penuh menghasilkan STORAGE_DEGRADED dan beralih ke flash fallback yang terbatas tanpa menghentikan sensor. Mount tidak memformat kartu otomatis. Lepas kartu melalui storage eject setelah write/flush selesai; hot removal diperlakukan sebagai fault. Backlog pada kartu yang dilepas ditandai unavailable dan dipulihkan saat kartu yang sama dipasang kembali. Jangan menganggap backlog tersebut telah dikirim.

FR STORE 05 Quota pending SD awal 128 MiB dengan ruang kerja di luar quota, target minimal 72 jam pada interval 15 s dan body ≤4096 byte. 72 jam berisi 17280 snapshot, maksimal 67,5 MiB JSON sebelum overhead. Format kartu baseline FAT32 pada kartu SDHC yang dipilih dan diuji. Kapasitas keseluruhan kartu tidak dianggap kapasitas antrean tanpa batas. Quota flash tetap 512 snapshot; saat media aktif penuh, buang pending tertua sesuai policy dan laporkan dropped_snapshots serta rentang sequence yang hilang.

FR STORE 06 Recovery memverifikasi checksum dan memulihkan record utuh; tail tidak lengkap dikarantina. Journal dan flush mengurangi risiko, tetapi FAT/SD konsumen tidak menjamin seluruh kartu selamat dari kehilangan daya arbitrer. Uji fault pada append, migration dan ack dengan kartu aktual; catat kehilangan maksimum yang terukur. Konfigurasi dan kalibrasi tetap di NVS, bukan hanya SD. health mencatat storage_backend, sd_state, pending_count, oldest_pending_age_s, used_bytes dan dropped_snapshots tanpa secret.

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

Menerima paket saja belum memenuhi produk hardware. Perubahan berikut harus masuk urutan implementasi agar sensor gagal, waktu replay, dan data penelitian ditangani dengan benar. Semua tetap berada dalam Go monolith existing.

### Validasi dan keputusan

internal/ingest/model.go menambahkan DTO health v1 yang tervalidasi sambil mempertahankan envelope dan data mentah. internal/ingest/validation.go memeriksa required measurements, quality, freshness, konfigurasi kalibrasi, serta device_mode dan calibration_targets. Sensor dalam kalibrasi tidak dipakai untuk keputusan operasional; reason CALIBRATING ditampilkan pada slot terkait dan raw tetap tersedia untuk riset. Fill atau weight null pada slot, atau DS18B20 B3 yang gagal, menghasilkan Data Invalid dengan reason. Firmware masih mengirim ketiga slot agar sensor rusak tidak menghilangkan heartbeat.

internal/ingest/service.go memetakan health per slot dan menyimpan keputusan invalid walaupun FIS production belum tersedia. Bila FIS belum dikonfigurasi, reading valid tetap tersimpan tanpa hasil Normal buatan. Optional gas warming tidak menghapus data fill/berat. B3 safety hanya mengevaluasi input valid dengan konfigurasi penelitian yang disetujui dan berlaku pada stream B3. Kondisi invalid tidak boleh membatalkan alert yang sudah didukung bukti valid.

Tambahkan policy penerimaan umur timestamp dan clock skew sebagai konfigurasi teknis. Backlog tetap tersimpan sebagai research reading; efek terhadap current status, safety dan event harus berdasarkan waktu pengukuran serta freshness, sehingga paket lama yang baru datang tidak memicu tindakan seolah baru. Uji seluruh policy ini dengan engine fixture tanpa memakai threshold fixture sebagai production default.

### Research dan ekspor

Backend existing menyimpan raw_payload penuh pada setiap reading dan health pada sensor_health. Ini cukup untuk landing data multikanal tanpa migrasi schema pertama. Research API mengembalikan sensor_health, tetapi scalar raw gas hanya mengambil gas_raw. Tambahkan projection typed per sensor ke research DTO dan panel riset agar empat MQ, dua humidity, serta raw distance/HX711 dapat dibaca langsung.

Perluas internal/history dan /api/export.csv dengan kolom sensor berunit, quality, age, firmware_version, calibration_version, dan time_quality. Setelah format ekspor disepakati, sediakan mode kompatibel atau export schema version agar konsumen lama tidak berubah diam-diam. Jangan mengubah route yang digunakan dashboard. Data health dan raw yang diekspos harus memakai allowlist; jangan menyalin config atau secret.

Dengan satu paket tiap 15 s terdapat 5760 paket dan 17280 row reading per device per hari. Raw envelope dan health direplikasi ke tiga row oleh kode saat ini. Sebagai ilustrasi, envelope 4 KiB saja dapat menambah sekitar 67,5 MiB per hari per device sebelum health tambahan, index dan overhead SQLite. Ukur payload nyata dan rencanakan retention atau tabel envelope bersama sebelum pilot panjang; tidak mengklaim penyimpanan tanpa batas.

### Provisioning dan versioning

Buat prosedur atau CLI lokal untuk mendaftarkan location, device_code, credential digest, tiga slots, serta kapasitas operasional. Tooling provisioning hardware belum terlihat pada entry point runtime; simulator tidak boleh dijadikan sumber credential production. Digest mengikuti internal/ingest/auth.go. Pembaruan firmware_version di devices perlu dipetakan dari health yang tervalidasi karena field database tersebut tidak diperbarui oleh ingest saat ini.

Migrations baru hanya diperlukan bila hasil pengukuran kapasitas atau kebutuhan query menuntut schema kanal yang terpisah. Pada HEAD reconciliation, migration berikutnya diperkirakan `005_telemetry_archive.sql`; cek ulang urutan file sebelum membuatnya. Simulator tetap opt-in dan mempertahankan penandaan sintetis. Test integrasi harus mencakup payload legacy dan payload hardware v1.

## 12 Tahapan implementasi

Kerjakan sebagai perubahan kecil dengan branch terfokus dan PR sesuai workflow repo. Estimasi berikut adalah hari kerja engineering, tidak termasuk pengadaan, aging sensor, atau kalibrasi ilmiah FIS. Rencana baseline 8-12 hari kerja bertambah sekitar 2-4 hari untuk mode kalibrasi terarah dan varian microSD, menjadi estimasi 10-16 hari setelah rig tersedia. BLE gateway memerlukan spike terpisah sekitar 1-2 hari; estimasi aplikasi gateway baru ditetapkan setelah spike.

| Tahap | Output yang dapat direview | Gate selesai |
| --- | --- | --- |
| T0<br>0,5-1 hari | Cocokkan modul N16R8, ultrasonic, MQ-7 dan driver heater; freeze pin dan schema health. Tentukan varian SD dan console USB native. | Schematic cocok; tanpa konflik GPIO; tegangan GPIO terukur. |
| T1<br>1 hari | Project PlatformIO, profile N16R8, partition, native tests, boot diagnostics. | Build MCU dan native pass; flash dan PSRAM terbaca pada target. |
| T2<br>2-3 hari | Driver sensor, heater state machine, filter, quality, mode kalibrasi wajib dengan draft/commit/rollback. | Seluruh sensor terbaca bersamaan; failure isolation terbukti. |
| T3<br>1-2 hari | Serializer v1, HTTPS, device provisioning, NTP dan replay exact. | Paket real tersimpan tiga row; invalid token ditolak; duplicate tidak menambah row. |
| T4<br>2-4 hari | Flash staging, SD opsional, quota, journal ack, power-loss recovery dan failover storage. | Offline 1 jam tanpa SD; 72 jam pada varian SD teruji; replay tidak menggandakan row. |
| T5<br>1-2 hari | Health validation, research projection, CSV multikanal, firmware version. | Sensor wajib gagal tampil Data Invalid; semua kanal dapat diekspor. |
| T6<br>1-2 hari | End-to-end, soak test, wiring dan SOP kalibrasi final. | Acceptance utama terpenuhi; hasil hardware dan risiko tersisa tercatat. |

T3 memakai kontrak T0, T4 memakai serializer T3, dan T5 memakai definisi quality T2. Akuisisi firmware dan validasi backend dapat dikerjakan terpisah setelah kontrak v1 disepakati. Freeze payload example sebagai contract fixture, bukan membuat frontend menebak format sensor.

### Strategi verifikasi

Native tests mencakup formula fill dengan anchor berbeda per slot, kalibrasi/tare, null dan freshness, state heater, retry ack, draft kalibrasi, commit/rollback, checksum queue, failover flash/SD, quota, koreksi jam mundur, serta rollover waktu bila memakai counter 32 bit. Gunakan input dan hasil yang diturunkan dari persyaratan, bukan salinan implementasi.

Tes Go mencakup tiga stream, token mismatch, exact replay, konflik timestamp, null sensor wajib, optional gas warming, stale backlog, dan projection/export v1. Jalankan focused ingest/read/history tests sebelum go test ./...; vet/race mengikuti pemeriksaan existing. npm test dan typecheck diperlukan ketika research UI diubah. Build PlatformIO dan tes native menjadi CI firmware; kalibrasi dan elektrik tetap membutuhkan hardware.

Simpan laporan bench dengan identitas board, firmware commit, konfigurasi heater, calibration_version, instrumen referensi, kondisi uji, serta hasil. Compile pass tidak membuktikan akurasi sensor atau wiring. Soak test 24 jam dapat berjalan sambil dokumentasi diselesaikan, tetapi rilis pilot menunggu hasilnya.

## 13 Kriteria penerimaan

| ID | Kriteria | Bukti yang diperlukan |
| --- | --- | --- |
| AC01 | Build profile N16R8 dan native test berhasil dari checkout bersih. | Log pio run dan pio test dengan dependency exact. |
| AC02 | 20 GPIO baseline atau 24 dengan SD, tanpa expander; pin memori dan USB terjaga. | Pin manifest; native USB teruji; CS43 tidak konflik dengan UART0. |
| AC03 | 3 HX711, 3 jarak, 4 MQ, DS18B20 dan 2 SHT31 terakuisisi bersama. | Capture diagnostik dengan quality tiap sensor. |
| AC04 | Fill memakai calibration tiap slot; target awal MAE ≤5 percentage points. | Uji beberapa level termasuk 70-85 persen; raw dan error disimpan. |
| AC05 | Berat repeatable dan hysteresis/corner loading terukur. | Uji beban referensi; toleransi final ditetapkan dari rig dan dicatat. |
| AC06 | MQ-7 mengikuti profile datasheet yang benar; output di fase invalid tidak dipakai. | Pengukuran rail dan timing heater, log state dan sampling. |
| AC07 | Kegagalan sensor wajib memberi null dan Data Invalid dengan sebab. | Cabut atau fault injection tiap kelas sensor tanpa menghentikan tiga-slot heartbeat. |
| AC08 | Setiap snapshot valid diterima dengan tiga row dan timestamp terpercaya. | Query SQLite serta dashboard/research API dari hardware. |
| AC09 | Ack hilang dan retry tidak menambah row; payload berubah di waktu sama dikarantina. | Replay identik → deduplicated; konflik → 409. |
| AC10 | Putus Wi-Fi 1 jam tanpa kehilangan snapshot bertimestamp dalam kapasitas queue. | Bandingkan sequence, raw packets dan row tersimpan setelah drain. |
| AC11 | Power-loss tidak merusak seluruh queue atau config. | Matikan daya saat append dan ack; restart memulihkan record utuh. |
| AC12 | NTP gagal dan koreksi mundur tidak membuat waktu palsu atau timestamp duplikat. | Test cold boot, same-boot reconstruction, dan clock adjustment. |
| AC13 | Semua kanal MQ dan SHT31 tersedia di research/export dengan unit dan quality. | CSV dan research API cocok dengan capture fixture/hardware. |
| AC14 | Tidak ada token/password pada Git, health, API research, CSV atau log biasa. | Review secret paths dan response allowlist; token salah dan CA salah ditolak. |
| AC15 | Soak 24 jam tidak mengalami reset tak terencana atau pertumbuhan resource tak terbatas. | Reset reason, heap trend, queue usage dan dropout logs. |
| AC16 | Nilai FIS/OGI/safety yang belum dikalibrasi tetap unavailable. | Uji mode production tanpa fixture thresholds; tidak membuat label Normal dari data hilang. |

AC04 adalah target engineering dari konsep hardware v0.8. Target kuantitatif berat, gas repeatability, thermal response, dan safety perlu ditetapkan bersama eksperimen; nilai yang belum disepakati tidak diberi angka seolah telah tervalidasi.

MVP selesai ketika AC01-AC18 dan AC21 memiliki bukti uji terbaru. AC19-AC20 wajib untuk varian SD; threshold ilmiah tetap ditunda sampai eksperimen selesai. Klaim keamanan B3 atau deteksi gas dalam ppm tidak menjadi acceptance firmware MVP. Pilot hanya menampilkan keputusan dengan konfigurasi dan kualitas input yang dapat diaudit.

## 13A Penerimaan revisi kalibrasi dan storage

| ID | Kriteria | Bukti yang diperlukan |
| --- | --- | --- |
| AC17 | Mode kalibrasi bisa dipakai offline per slot; draft tidak mengubah profil aktif. | USB session: start, capture, preview, commit, cancel; anchor salah/unstable ditolak. |
| AC18 | Kalibrasi tersimpan setelah reboot; interrupted commit memakai profil utuh; rollback bisa dilakukan. | Fault injection saat commit NVS; version/checksum; paket lama tetap memakai profil asal. |
| AC19 | Varian SD menyimpan minimal 72 jam pada interval 15 s dan limit body; semua pending dapat dikirim tanpa row ganda. | Tes endurance nyata dan recovery, serta accelerated test kapasitas; hitung sequence dan ack server. |
| AC20 | SD hilang/rusak/penuh beralih ke flash; pemasangan ulang tidak menggandakan queue. | Lepas kartu, mount fault, quota penuh, reboot saat migration/ack; ukur tail loss dan STORAGE_DEGRADED. |
| AC21 | Data CALIBRATING tidak memicu keputusan operasional normal; status dan raw tetap tersedia. | Contract test mode/targets/versi; heartbeat tiga slot; backend reason dan dashboard. |

AC19 dan AC20 menjadi gate wajib bila hardware microSD dipilih. Baseline tanpa SD memakai AC10 dan AC11. Menambah kartu tidak menghapus batas quota, aturan timestamp, quality, serta kebutuhan ack server.

### Penerimaan bersyarat untuk BLE P1

Jika BLE gateway dipilih setelah spike, uji radio putus pada setiap tahap fragment, restart gateway, receipt lokal tanpa ack server, serta dua transport yang mencoba record yang sama. Queue device tidak boleh terhapus sebelum SERVER_ACK tervalidasi. Beri evidence throughput/heap, target OS, perilaku background dan kehilangan data; kegagalan gate ini menunda BLE, bukan mode kalibrasi wajib atau logging offline.

## 14 Keputusan yang perlu dikunci saat implementasi

| Keputusan | Status | Cara menyelesaikan |
| --- | --- | --- |
| Controller dan MQ | Dikonfirmasi | N16R8; MQ-4/135/7/8; satu MCU per tiga slot. |
| Modul dan revisi PCB | Perlu inspeksi | Baca label WROOM dan schematic; verifikasi topology flash/PSRAM serta RGB. |
| Model ultrasonic | Asumsi TRIG ECHO | Kunci model, blind zone, mounting, timeout dan divider. VL53L1X memerlukan profil driver/address yang berbeda. |
| MQ-7 pabrikan dan driver | Perlu datasheet | Kunci VH rendah/tinggi, timing, akses heater, rail selection dan safe boot state. |
| Penempatan sensor gas | Usulan eksperimen | Organik MQ-4/135; B3 MQ-7/8; pindah melalui profil tanpa mengubah channel identity. |
| SHT31 kedua | Usulan B3 | Sesuai definisi parameter v0.8; tabel RAB lama menyebut Anorganik. Kunci mounting sebelum kalibrasi. |
| Endpoint dan TLS | Perlu deployment | Host backend dengan TLS dan SQLite persistent; provision credential hardware. |
| Waktu dan queue | Target awal | 15 s; flash fallback 512 snapshot; SD pending quota awal 128 MiB dan target 72 jam; ukur kapasitas, time policy dan wear. |
| microSD | Opsional hardware | Dukungan firmware wajib tersedia; pilih modul logika 3,3 V dan kartu sebelum freeze wiring. |
| Bluetooth sebagai transport | P1 bersyarat | Gateway HP/komputer + BLE GATT; lanjutkan hanya jika prototype dan UX lapangan layak. |
| FIS dan safety final | Deferred research | Membership, OGI, verified-empty, thresholds, dan offline duration mengikuti eksperimen/config yang memiliki sumber. |

## 15 Referensi untuk developer

Repo dan kode historis dibaca pada commit 6fed00f02b6ef7dabc159d924f798d344a6072ba; reconciliation dilakukan terhadap main commit ad65d5fe73c30b16c61da917eb517cdf2a783ba6 pada 30 September 2026. Dokumentasi vendor diperiksa pada tanggal yang sama. Nomor bagian berikut dapat digunakan untuk menelusuri keputusan; semua URL adalah sumber, bukan hasil uji hardware.

- Repo: https://github.com/RayaSatriatama/IoT-Trashcan/tree/6fed00f02b6ef7dabc159d924f798d344a6072ba — README; docs/SIGAP_PRD_v1.0.md; docs/SIGAP_SDD_v1.0.md; docs/SIGAP_Smart_Bin_Konsep_dan_RAB_v0.8.md.
- Kontrak historis: internal/http/ingest_handler.go; internal/ingest/model.go; service.go; validation.go; normalize.go; internal/http/read_api.go; history_api.go; internal/device/offline.go; cmd/sigap/main.go; migrations/001_initial.sql.
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
