# IoT-Trashcan

IoT-Trashcan memuat dokumentasi dan aset SIGAP Smart Bin, sistem pengelolaan sampah cerdas berbasis IoT dan fuzzy logic.

## Dokumen utama

- [Product Requirements Document](docs/SIGAP_PRD_v1.0.md)
- [Software Design Document](docs/SIGAP_SDD_v1.0.md)
- [Konsep sistem dan RAB](docs/SIGAP_Smart_Bin_Konsep_dan_RAB_v0.8.md)
- [Indeks dokumentasi](docs/README.md)
- [Paket aset visual](docs/SIGAP_Asset_Pack_Complete_v0.6/README.md)

## Model dan kalibrasi

- [Konfigurasi FIS](docs/fis-configuration.md): lima input, 243 rule, centroid, kapasitas 2 kg, dan batas buktinya.
- [Kurva NH3 MQ-135](docs/mq135-winsen-nh3-curve.md): digitasi Winsen v1.6 Figure 3 beserta domain dan disclaimer.
- [Kalibrasi default](docs/calibration-defaults.md): profil pabrik dan presedensinya.
- [Analisis web dan sensor live](docs/web-derived-research.md): provenance `WEB_DERIVED_TEST_ONLY` dan validitas nilai.

## Status project

Status integrasi sensor terbaru, urutan prioritas, dan batas penerimaan fisik: [current sensor integration checkpoint](docs/task9-current-status.md). Publikasi source bukan bukti bahwa seluruh sensor sudah streaming ke web atau menghasilkan FIS produksi.

SIGAP MVP memiliki satu runtime Go dengan SQLite embedded, authenticated ingest, monitoring read APIs, history, CSV export, dan frontend Expo Web yang di-embed saat release build. Kontrak ingest mendukung bukti sensor fisik opsional, nilai turunan, trace keputusan FIS, dan kanal safety B3. Nilai dan threshold production final untuk FIS, safety, OGI, offline duration, verified-empty, dan event transition tetap deferred.

Mode prototype latest-only memakai HTTPS dengan target snapshot bersama 1 detik untuk empat sensor aktif, tanpa backlog/replay sensor di RAM dan tanpa backup SD. Akuisisi tetap berjalan saat request aktif; riwayat hanya berisi sampel yang diterima server, bukan seluruh pembacaan. Lihat [kebijakan prototype](docs/prototype-operation.md) dan [analisis web TEST_ONLY](docs/web-derived-research.md). Build bukan bukti pemasangan atau penerimaan fisik.

## Prasyarat

| Alat | Versi | Catatan |
| --- | --- | --- |
| Node.js | 24 atau lebih baru | dipakai `package-lock.json`; tidak ada `engines` yang dipatok |
| Go | 1.27.1 | sesuai `go.mod`; `go` harus ada di `PATH` |
| PlatformIO | terbaru | hanya untuk firmware, dijalankan lewat `uv tool run --from platformio` |
| Python 3 + `uv` | terbaru | untuk helper BLE manual dan PlatformIO |

Toolchain firmware menargetkan ESP32-S3 (profil N16R8/OPI). Build host tidak
memberi izin flash; lihat [batas operasi](#batas-operasi).

## Menjalankan web dan API

Empat cara, pilih sesuai kebutuhan.

### 1. Frontend saja (paling cepat, tanpa backend)

```bash
npm ci
npm run typecheck
npm test
npm run web
```

Expo Dev Server mencetak URL-nya saat start (default web `http://localhost:8081`).
Tanpa `EXPO_PUBLIC_API_BASE_URL`, frontend memanggil `/api` pada origin yang sama,
jadi tanpa backend halaman hanya menampilkan keadaan kosong/galat. Berguna untuk
memeriksa tampilan.

### 2. API Go saja

```bash
SIGAP_DB_PATH=./sigap.db SIGAP_HTTP_ADDR=:8080 go run ./cmd/sigap
```

Default bila variabel tidak diisi: `SIGAP_DB_PATH=sigap.db`,
`SIGAP_HTTP_ADDR=:8080`. Endpoint pemeriksaan: `GET /api/runtime`.

### 3. Frontend + API (disarankan)

Jalankan API pada `:8080`, lalu arahkan frontend ke sana:

```bash
# terminal 1
SIGAP_DB_PATH=./sigap.db SIGAP_HTTP_ADDR=:8080 go run ./cmd/sigap

# terminal 2
EXPO_PUBLIC_API_BASE_URL=http://localhost:8080 npm run web
```

`ALLOWED_ORIGIN` pada API harus memuat origin frontend. Default-nya
`http://localhost:8081`; kalau port Expo berbeda, set `ALLOWED_ORIGIN` agar cocok
(origin dicocokkan persis, termasuk scheme dan port).

### 4. Satu binary dengan frontend ter-embed

```bash
./scripts/build-release.sh          # npm ci, test, typecheck, export web, lalu build Go
SIGAP_DB_PATH=./sigap.db SIGAP_HTTP_ADDR=:8080 /tmp/sigap-release
```

Skrip ini menyalin hasil `expo export` ke `internal/webapp/dist` sebelum
kompilasi Go, sehingga SPA ikut ter-embed. Set `SIGAP_RELEASE_OUTPUT` untuk
mengubah lokasi biner keluaran.

## Konfigurasi environment

| Variabel | Default | Fungsi |
| --- | --- | --- |
| `SIGAP_DB_PATH` | `sigap.db` | lokasi SQLite operasional |
| `SIGAP_HTTP_ADDR` | `:8080` | alamat bind API |
| `ALLOWED_ORIGIN` | `http://localhost:8081` | origin CORS, dipisah koma, dicocokkan persis |
| `SIGAP_TLS_REQUIRED` | mati | wajibkan TLS |
| `SIGAP_TLS_CERT_FILE` | — | path sertifikat TLS |
| `SIGAP_TLS_KEY_FILE` | — | path kunci TLS (privat, jangan pernah di-commit) |
| `SIGAP_SIMULATION_ENABLED` | mati | aktifkan mode simulasi |
| `SIGAP_SIMULATION_INDICATOR_ENABLED` | — | tampilkan penanda mode simulasi |
| `OFFLINE_AFTER_SECONDS` | tidak diset | turunkan perangkat ke Offline setelah jeda ini |
| `SIGAP_OPERATOR_ORIGIN` | — | origin operator untuk kontrol perangkat |
| `EXPO_PUBLIC_API_BASE_URL` | kosong | base URL API untuk frontend |

`ALLOWED_ORIGIN` menerima origin HTTP/HTTPS eksplisit yang dipisahkan koma dan
dicocokkan persis, termasuk scheme serta port. Nilai `*` hanya membuka CORS
publik tanpa `Access-Control-Allow-Credentials`; origin mirip, malformed, atau
header Origin ganda tidak mendapat grant.

API kontrol/debug memakai limiter default 60 request per key per menit dengan
state maksimal 4096 key per proses. Perilaku saat kapasitas penuh dijelaskan pada
[kontrol akses operator](docs/task9-web-calibration.md#operator-security).

## Pemeriksaan sebelum PR

```bash
npm run typecheck
npm test
go build ./...
go test ./...
```

E2E Playwright memerlukan konfigurasi eksplisit (`SIGAP_E2E_ORIGIN`,
`SIGAP_E2E_OUTPUT`, `SIGAP_E2E_BROWSER`) dan menolak menulis ke dalam checkout;
lihat [Playwright E2E](docs/task9-playwright.md).

## Firmware

Semua perintah dijalankan dari direktori `firmware/`.

```bash
cd firmware
uv tool run --from platformio pio test -e native          # unit test host
uv tool run --from platformio pio run -e sigap_n16r8      # build target
```

| Environment | Kegunaan |
| --- | --- |
| `native` | unit test host, tanpa perangkat |
| `sigap_n16r8` | build target utama |
| `sigap_n16r8_sensor_sim` | target dengan simulasi sensor |
| `sigap_n16r8_sd` | varian dengan SD card |
| `sigap_n16r8_debug` | build dengan debug |
| `sigap_ble_diag` | image diagnostik BLE-only |
| `sigap_ble_wifi_test` | kandidat BLE + Wi-Fi terisolasi |
| `sigap_bench` | profil bench |

Daftar env lengkap ada di `firmware/platformio.ini`. Build, termasuk build yang
lulus, **bukan** izin untuk upload, flash, atau pengujian fisik.

Image diagnostik BLE-only dibangun dengan `sigap_ble_diag` setelah meninjau
[laporan verifikasi BLE tanpa passkey](docs/task9-ble-no-passkey-verification.md).
Build ini menggunakan NimBLE-Arduino 2.5.1 dengan patch lokal yang memeriksa
hash sumber. Hasil build bukan izin untuk upload atau pengujian fisik; Task 9
tetap HOLD sampai gerbang NVS dan persetujuan perangkat terpenuhi.

Uji manual untuk image BLE-only yang sudah ditinjau tersedia melalui
`npm run ble:manual`, lalu buka `http://localhost:8765` pada profil Chromium
khusus pengujian. Helper memakai GATT biasa tanpa passkey atau autentikasi BLE,
dan hanya menyediakan verifikasi identitas serta status diagnostik setelah aksi
operator. Langkah browser tanpa passkey, reconnect, dan batas fisik tersedia di
[panduan uji manual BLE](docs/task9-ble-manual-testing.md). Server ini tidak
memerlukan Go API atau token; kesiapan environment bukan PASS HIL.

Kandidat BLE + Wi-Fi terisolasi dibangun dengan `pio run -e sigap_ble_wifi_test` dari `firmware/`. Target itu memakai provisioning BLE/Wi-Fi produksi dan guard erase terpisah; tidak menjalankan SigapApp/sensor/aktuator. Build host tidak memberi izin flash. Label profil PlatformIO generik `ESP32-S3-DevKitC-1-N8` bukan bukti ukuran flash aktual. Retained `esptool image_info --version 2` untuk image kandidat menunjukkan flash 16 MB dan header aplikasi DIO; build mewarisi permintaan `qio`/`qio_opi`. Jangan mengubah flag board/flash berdasarkan label N8 atau byte header mentah. Halaman kandidat dan izin NVS yang dibutuhkan dijelaskan pada panduan manual BLE.

The first firmware slice targets the ESP32-S3 DevKitC-1 PlatformIO profile,
typed AppConfig persistence, deterministic codec/hash, and native-testable NVS
A/B behavior. Sensor, calibration, FIS, telemetry transport, and provisioning
implementations remain deferred to later slices. The board profile retains the
N16R8/OPI assumptions from the firmware specification; physical hardware
validation is still required. Target startup logs detected flash and PSRAM
identity and reports `BOARD_MEMORY_MISMATCH` when the physical board does not
match the N16R8 expectation.

Monitoring frontend menggunakan relative same-origin `/api/...` pada web. `EXPO_PUBLIC_API_BASE_URL` hanya digunakan untuk development native bila diperlukan.

History API mengembalikan satu halaman cursor-bounded pada `items`, ringkasan lengkap untuk seluruh filter aktif pada `summary`, dan trend terbatas pada `trend`. Trend memakai kalender Asia/Jakarta, memilih bucket harian untuk rentang sampai 31 hari, mingguan sampai 180 hari, dan bulanan untuk rentang lebih panjang. Tanpa periode, trend memakai jendela 30 hari terbaru dan menandainya dengan `default_window`; `truncated` menandai saat bucket lama dipangkas agar tetap bounded. Ekspor CSV tetap memakai seluruh hasil yang cocok dengan filter aktif dan tidak dipengaruhi oleh `limit` atau cursor halaman.

## Release binary

Gunakan script deterministic berikut. Script membangun Expo SPA, men-stage bundle sementara ke Go `go:embed`, menjalankan seluruh test dan vet/race check, lalu menghasilkan satu binary di `/tmp/sigap-release` tanpa menyimpan bundle generated ke Git.

```bash
SIGAP_RELEASE_OUTPUT=/tmp/sigap-release ./scripts/build-release.sh
SIGAP_DB_PATH=./sigap.db SIGAP_HTTP_ADDR=:8080 /tmp/sigap-release
```

Runtime production menggunakan `SIGAP_DB_PATH`, `SIGAP_HTTP_ADDR`, dan optional `OFFLINE_AFTER_SECONDS`. Database SQLite tetap file external dan persistent, bukan bagian dari binary.

Production FIS dan safety numeric configuration tetap research-blocked. Release runtime tidak menggunakan test fixture thresholds dan tidak memalsukan keputusan valid ketika engine production belum dikonfigurasi.

## Firmware sensor simulation

ESP32 sensor-reader simulation is a separate, default-OFF build option. From
`firmware/`, `pio run -e sigap_n16r8_sensor_sim` selects simulated readers;
`pio run -e sigap_n16r8` selects the physical readers. Changing mode requires
building and installing the corresponding image, not a runtime web toggle.
Build success is not permission to flash or evidence of HIL acceptance.

The [sensor configuration and simulation guide](docs/firmware-sensor-simulation.md)
lists all 13 sensor identities, pins, sampling defaults, units, safety limits,
and current archive/API integration blockers. Full firmware-to-web acceptance
is not complete: real ingestion currently rejects the MQ-7 batch because its
required heater phase is missing. Do not substitute the Go demo below for
firmware-origin acceptance.

## Firmware HTTPS dengan arsip historis penuh

Mode opt-in antrean aktif terpisah, batas ruang reclaim, debug HTTPS, dan batas
pemasangan dijelaskan dalam [panduan live archive](docs/live-archive-delivery.md).
Arsip historis dipertahankan `UNDELIVERED`, bukan dianggap sudah terkirim.

## Simulation mode

Simulation mode bersifat opt-in untuk demo tanpa hardware. Buat environment lokal lalu jalankan runtime Go yang sama dengan pipeline production:

```bash
cp .env.simulation.example .env.simulation
./scripts/run-simulation.sh
```

Runner simulasi terlebih dahulu menjalankan release build agar Expo SPA benar-benar masuk ke binary Go, lalu menjalankan binary tersebut dengan environment simulasi. Ini diperlukan karena clean checkout hanya menyimpan placeholder untuk `go:embed`, bukan bundle frontend generated.

Buka `http://localhost:8080`. Simulator membuat 8 lokasi, 8 device, dan 24 slot. ID database simulator memakai prefix `sim-`, sedangkan device code yang terlihat di UI memakai prefix `SIGAP-`. Simulator lalu mengirim telemetry melalui authenticated ingest. Siklus demo mencakup Normal, Waspada, Angkat, Safety Alert, Data Invalid, Offline, dan Recovery. Simulation payload menyediakan Fill Sensor, HX711, MQ-135, MQ-4, MQ-7, MQ-8, dan DS18B20 sesuai stream. Dekoder collection FIS web memakai lima input: keterisian, utilisasi berat, NH3 (ppm), waktu akumulasi, dan suhu. MQ ditampilkan sebagai raw atau normalized index, bukan ppm, karena simulator tidak menyediakan calibration model. Data sintetis, profile FIS, dan safety threshold pada mode ini hanya fixture demo, bukan kalibrasi production atau research.

`SIGAP_SIMULATION_INTERVAL_SECONDS` harus bilangan bulat positif. `SIGAP_SIMULATION_SEED` membuat variasi kecil dapat direproduksi dan merotasi credential simulator lokal saat database yang sama digunakan kembali. Untuk memulai ulang data demo, hapus database secara manual:

```bash
rm sigap-simulation.db
```

Saat aktif, UI menampilkan indikator `SIMULASI`. Atur `SIGAP_SIMULATION_INDICATOR_ENABLED=false` untuk menyembunyikan indikator tanpa menonaktifkan simulator. Nilai default-nya adalah `true`. Saat `SIGAP_SIMULATION_ENABLED` tidak ada atau `false`, seeding, simulator, dan demo decision configuration tidak dijalankan.

## Vercel simulation snapshot

Vercel menjalankan frontend Expo Web dari `dist` dan Go Function pada `/api`. Masukkan nilai dari `.env.simulation` ke Production Environment Variables Vercel. `SIGAP_DB_PATH` tetap dipertahankan pada environment lokal, tetapi Function selalu memakai SQLite kecil di `/tmp` karena filesystem Vercel tidak persisten.

Saat Function dimulai dengan `SIGAP_SIMULATION_ENABLED=true`, SIGAP membuat 8 lokasi, 8 perangkat, 24 slot, lalu menjalankan satu tick telemetry. Tidak ada loop simulator dan `POST /api/ingest` ditolak, sehingga data ini adalah snapshot demo read-only yang dapat berubah saat cold start berikutnya. Database lokal `sigap-simulation.db` tidak diunggah ke Vercel.

Deployment produksi dilakukan setelah login Vercel dan pengisian environment variables:

```bash
npx vercel login
npx vercel --prod
```

## Kontribusi

Workflow GitHub project tersedia di [skill project](.agents/skills/github-project-practices/SKILL.md). Ringkasannya:

1. Buat branch singkat dari `main`.
2. Kerjakan satu perubahan terfokus.
3. Jalankan pemeriksaan yang relevan dan periksa diff.
4. Buka pull request kecil menggunakan template project.
5. Gunakan squash merge lalu hapus branch.

`main` dilindungi: merge commit ditolak dan semua perubahan harus lewat pull
request, jadi `git push origin main` akan gagal. Buka PR lalu squash merge.

Jangan commit credential, file `.env`, virtual environment, dependency
terunduh, database operasional, atau artefak build — `.gitignore` sudah mencakup
polanya.

## Batas operasi

Bagian ini menjelaskan apa yang **belum** terbukti, agar hasil build atau
tampilan tidak disalahartikan sebagai penerimaan.

| Hal | Status |
| --- | --- |
| Kalibrasi gas MQ-135 | belum tervalidasi; `q0` belum tersimpan di NVS |
| Baseline `q0` web (2700 mV) | asumsi operator, bukan pengukuran udara bersih |
| Pembagi tegangan fisik 10 kΩ / 14,7 kΩ | deklarasi; firmware masih 1:1 |
| Estimasi NH3 di luar 10–1000 ppm | ekstrapolasi power-law, berlabel `EXTRAPOLATED` |
| Himpunan suhu pada FIS | asumsi desain, tabel pemilik hanya empat input |
| Rule base FIS | diturunkan implementasi, bukan dari pemilik |
| Akurasi NH3 terhadap referensi | belum diuji |
| Streaming fisik empat sensor ke web | belum didemonstrasikan penuh |
| Uji stimulus terkendali dan durabilitas | belum dilakukan |
| Flash / upload firmware | di luar cakupan README ini; butuh izin terpisah |

Semua keluaran model web berprovenance `WEB_DERIVED_TEST_ONLY`, bukan keluaran
MCU dan bukan keputusan operasional. Fixture bertanda TEST_ONLY tidak membuktikan
akurasi fisik. Rincian per topik ada di [konfigurasi FIS](docs/fis-configuration.md),
[kurva NH3](docs/mq135-winsen-nh3-curve.md), dan
[checkpoint status](docs/task9-current-status.md).
