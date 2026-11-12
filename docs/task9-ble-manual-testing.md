# Uji manual BLE tanpa passkey

## Ruang lingkup

Halaman lokal menguji interaksi operator dengan image diagnostik BLE-only. Server hanya menyajikan helper statis di localhost. Tidak ada Go API, token, scan otomatis saat load, koneksi otomatis, flashing, reset, atau akses sensor/aktuator. Untuk identitas/status/scan yang dimulai operator, helper Wi-Fi kandidat memakai timeout 30 detik per percobaan dan maksimal 3 percobaan baca; CONNECT/simpan tidak diulang.

Kode sumber memakai GATT biasa tanpa passkey, pairing wajib, MITM, atau autentikasi BLE pada karakteristik. `GET_DEVICE_ID` hanya memeriksa identitas/routing, bukan autentikasi. Image yang berjalan pada perangkat tidak diperiksa oleh halaman. Build atau kesiapan browser bukan bukti keberhasilan fisik. Task 9 tetap HOLD dan penerimaan keamanan BLE `DEFERRED_BY_OWNER`.

## Menjalankan helper

Dari root repository:

```bash
npm run ble:manual
```

Server mendengarkan di `127.0.0.1:8765`, lalu buka `http://localhost:8765` pada browser yang menyediakan Web Bluetooth melalui secure context. Helper tidak menghubungkan perangkat saat halaman dibuka. Jika port sudah dipakai, server gagal tanpa memilih port lain atau menghentikan proses lain.

Pada komputer Linux yang menjadi konteks historis dokumen ini, Chromium 153 tanpa flag tidak mengekspos `navigator.bluetooth`. Profil khusus berikut pernah digunakan untuk menguji ketersediaan API pada localhost:

```bash
chromium \
  --user-data-dir="$HOME/.cache/sigap-ble-manual-chromium" \
  --enable-experimental-web-platform-features \
  --no-first-run \
  http://localhost:8765
```

Profil tersebut terpisah dari profil browser utama. Ketersediaan API tidak membuktikan koneksi radio atau GATT fisik.

## Langkah operator

1. Pastikan halaman menyatakan Web Bluetooth tersedia pada secure context localhost.
2. Klik `1. Pilih dan hubungkan`, lalu pilih perangkat diagnostik yang sesuai. Nama BLE hanya petunjuk discovery.
3. Akses karakteristik seharusnya tidak meminta pairing atau autentikasi. Jika browser atau sistem meminta pairing, passkey, atau autentikasi, hentikan. Jangan masukkan kredensial, hapus bond, ubah NVS, reset, atau flash ulang.
4. Klik `2. Verifikasi identitas`. Target wire ID yang diuji adalah `000050c1f516a398`; mismatch menutup koneksi dan tidak mengizinkan status.

Untuk fixture board dengan hardware MAC `98:A3:16:F5:C1:50`, Arduino SDK merepresentasikan byte eFuse sebagai `uint64` little-endian pada target ini. Karena itu ID wire di atas berbeda dari teks MAC `000098a316f5c150`, dan firmware menamai diagnostik `SIGAP-A398` dari 16 bit terbawah nilai SDK. Ini turunan source dari fixture historis, bukan pembacaan fisik baru.
5. Klik `3. Baca status`. Helper hanya menerima `OK STATUS mode=BLE_DIAGNOSTIC wifi=DISABLED` setelah identitas cocok pada koneksi saat ini.
6. Klik `Putuskan`, lalu `Sambungkan ulang`. Verifikasi ulang identitas sebelum membaca status. Tidak ada pembacaan atau retry otomatis.
7. Jika koneksi putus saat service discovery, catat kode error tanpa secret dan hentikan. Jangan menghapus bond atau mengubah autentikasi untuk memaksa lanjut.

## Batas helper dan keselamatan

- Helper memakai codec dan framing F1/R1 dari `lib/ble/protocol.ts`; hanya `GET_DEVICE_ID` dan `GET_STATUS` yang dikirim.
- Identitas dan status yang tersimpan di UI dibersihkan saat disconnect. Balasan dari generasi koneksi lama dibuang.
- Halaman tidak menerima token, Wi-Fi credential, atau input kalibrasi. Tidak ada localStorage, IndexedDB, endpoint upload, atau penyimpanan log persisten.
- Image diagnostik memakai sesi read-only. Perintah Wi-Fi dan kalibrasi ditolak. Tidak ada perintah sensor, aktuator, SD, profil, flash, atau reset.
- Tidak ada pembacaan, penghapusan, migrasi, rotasi, atau perubahan record NVS/bond yang dilakukan oleh prosedur helper ini. SDK masih dapat melakukan inisialisasi NVS pada saat startup firmware; uji host tidak membuktikan kondisi fisik partisi.
- Perubahan perangkat lunak ini tidak menonaktifkan AppConfig/profile persistence yang sah, application SecretStore, otorisasi HTTP/API, atau validasi sertifikat HTTPS.

Hasil pengujian host terbaru, hash sumber/build, dan batas verifikasi tersedia pada [laporan verifikasi BLE tanpa passkey](task9-ble-no-passkey-verification.md). Tidak ada klaim PASS untuk pairing, koneksi, discovery, reconnect, atau penerimaan fisik.

## Kandidat terpisah BLE + Wi-Fi

`wifi.html` adalah halaman lokal kandidat untuk image `sigap_ble_wifi_test`, bukan bagian dari helper BLE-only di atas. Bundle kandidat harus diletakkan di direktori artifact baru, tidak di cache helper yang sedang berjalan. Halaman memakai `BleProvisioningClient` dan protokol F1/R1 produksi: operator memilih koneksi, memverifikasi ID, meminta status, memulai scan, memilih SSID, lalu memasukkan password lokal. Password tidak dicatat; field dikosongkan langsung saat permintaan dikirim dan tidak ada penyimpanan password ke URL atau storage browser. Tidak ada aksi koneksi/scan saat load.

Panel `Jejak batas diagnostik klien` pada `wifi.html` hanya menampilkan maksimal 60 metadata aman di memori tab. `scan_pending` lalu `request_timeout` menunjukkan scan belum menyelesaikan respons; `response_fragment` dengan `receivedCount` lebih kecil dari `expectedCount` menunjukkan fragmen respons belum lengkap; `decoder_rejected` dengan kategori allowlist menunjukkan respons lengkap ditolak decoder klien. Jejak juga mencatat request/write completion dan disconnect, bukan log internal firmware/radio. Untuk identitas/status/scan, metadata `attempt`/`maxAttempts` menunjukkan batas tiga percobaan; timeout hanya diulang setelah seluruh write selesai, tidak untuk penolakan eksplisit, penolakan decoder, error write, atau disconnect. Perintah CONNECT/simpan dan kalibrasi tidak diulang. Event observer opsional tidak mengubah framing, filter ACK, penerimaan, atau deadline per percobaan. Tidak ada frame mentah, nilai payload, SSID, password, atau pesan exception bebas yang dicatat; hasil host sintetis tidak membuktikan penyebab scan fisik.

Parser scan memvalidasi jumlah dan struktur semua entri mentah, lalu menghilangkan entri valid dengan SSID kosong dari daftar yang dapat dipilih (hidden network). Pemindaian yang hanya berisi entri hidden tetap berhasil dengan daftar selectable kosong; baris rusak tetap ditolak. Ini kompatibilitas parser klien, bukan bukti isi payload dari suatu scan fisik.

Image kandidat memanggil `NvsSecretStore`, `WifiConnectionManager`, `ArduinoWifiStation`, `WifiProvisioningBackend`, dan sesi/service BLE produksi. Ia bukan image diagnostik `wifi=DISABLED`. Bila record jaringan lama tersedia, `WifiConnectionManager::begin()` memuat hanya record `sigap_secret/wifi` melalui `load_wifi_for_network()` lalu langsung memulai koneksi jaringan tersimpan. Jalur Wi-Fi tersebut tidak membaca atau menulis record `sigap_secret/token`; pemuatan gabungan Wi-Fi/token yang ketat tetap digunakan oleh jalur jaringan/API terautentikasi. `WiFi.persistent(false)` mencegah persistensi konfigurasi station otomatis Arduino, tetapi tidak membuktikan SDK tidak mengakses NVS untuk PHY/driver. Candidate guard menolak operasi erase partisi/NVS; startup berhenti jika terdeteksi.

Sebelum uji perangkat, minta otorisasi eksplisit untuk flash application-only ke image kandidat serta restart akibat flash, inisialisasi NVS oleh SDK, pembacaan internal credential Wi-Fi `sigap_secret/wifi` tanpa menampilkan nilainya, kemungkinan read/write SDK Wi-Fi PHY/driver, koneksi otomatis ke kredensial lama, scan Wi-Fi, dan satu credential uji/operator-approved yang ditulis ke `sigap_secret/wifi` hanya sesudah association dan IP berhasil. Penulisan credential baru mengganti record Wi-Fi tersimpan; tidak ada backup atau ekspor password. Jalur Wi-Fi kandidat tidak memerlukan akses ke `sigap_secret/token`; otorisasi pembacaan atau penulisan token tidak diminta. Tolak erase/re-init recovery, clear/rotate token, perubahan AppConfig/profil, penghapusan bond, sensor/aktuator, serta pengiriman aplikasi/API. Stop bila startup meminta erase atau perluasan mutasi muncul. Pengujian persistence lintas reboot membutuhkan persetujuan reset tersendiri.

Audit source/binary SDK yang dipin mengidentifikasi namespace PHY `phy` dengan record `cal_version`, `cal_mac`, dan `cal_data`, yang dapat dibaca dan ditulis saat inisialisasi. `WiFi.persistent(false)` tidak menghilangkan akses NVS tersebut; perilaku internal driver Wi-Fi belum seluruhnya dipastikan. Scope SDK ini harus disetujui secara eksplisit sebelum flash. Semua erase tetap ditolak dan tidak ada klaim preservasi byte NVS. Kandidat ESP32-S3 memakai header aplikasi 16 MB, 80 MHz, DIO; build requests `qio`/`qio_opi`. Label N8 generik dan ukuran slot aplikasi 4 MiB bukan bukti mismatch N16R8.

Runbook fisik yang berlaku setelah review dan izin memakai `/home/raya/.hermes/artifacts/sigap-ble-wifi-correction-cycle2-20261019/`, bukan artifact preparation awal atau correction cycle 1. BIN yang diizinkan harus memiliki SHA-256 `0a80ca9adcf291afeee539794b9440328cb30c6b4c353f8ffcf3019abdc2f772`.

1. Operator klik `Putuskan` pada helper lama sebelum flashing; server `8765` dan cache diagnostik tidak diganti. Tidak ada chooser, reconnect, atau scan BLE otomatis.
2. Periksa bahwa port `8766` bebas, lalu sajikan hanya bundle kandidat dengan `python -m http.server 8766 --bind 127.0.0.1 --directory /home/raya/.hermes/artifacts/sigap-ble-wifi-correction-cycle2-20261019/candidate-bundle`. Jangan hentikan proses lain bila port terpakai.
3. Setelah izin, cocokkan board/image/partition, flash hanya aplikasi pada `0x10000`, verifikasi readback dan boot. Jangan tulis bootloader, partition table, atau NVS melalui flasher. Hentikan bila guard melaporkan erase atau boot gagal.
4. Buka `http://localhost:8766/wifi.html`, hubungkan perangkat secara manual, verifikasi ID, lalu baca status dan pindai jaringan lewat tombol halaman.
5. Untuk penulisan yang disetujui, pilih SSID uji dan masukkan password hanya pada field lokal. Kirim sekali dan tunggu `CONNECTED`; marker serial `BLE_WIFI_TEST_IP a.b.c.d` hanya dicetak setelah association dan IP. Jangan kirim password melalui chat atau menyimpan screenshot saat field terisi.
6. Catat hasil tanpa credential dan hentikan sebelum uji reboot/reconnect persistence. Keberhasilan IP bukan keberhasilan Internet/API. Langkah fisik belum dijalankan oleh persiapan host-only ini.

