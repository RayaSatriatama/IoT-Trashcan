# Analisis web dan sensor live

## Batas sistem

### PPM MQ135 belum terkalibrasi

Kartu live dan grafik MQ135 menampilkan `converted_value.adc_mv` dari firmware
dengan satuan mV. Nilai ini bukan konsentrasi gas. Status gas pada kartu adalah
**Belum terkalibrasi**, bukan validitas ppm. Sampel ADC yang READY tetap tidak
membuktikan konsentrasi NH3, CO2, atau gas lain.

Estimasi CO2 ekuivalen dari konstanta pustaka tidak dipakai lagi pada monitoring,
histori, atau klasifikasi. Helper `estimateMq135Ppm` hanya dipertahankan sebagai
rumus riset historis yang deprecated, bukan jalur tampilan aktif. Angka tidak
dipotong atau dipaksa masuk ambang klasifikasi.

Skema bukti kalibrasi lama `MQG1` mencatat Rs dan Rs/R0 yang dihitung perangkat, serta selalu mengembalikan `nh3_ppm: null`. Skema normalisasi `MQG2` berbeda: perangkat mengirim q, q0, dan rasio q/q0 dengan bukti ADC, rangkaian, profil, referensi, serta kualitas sumber tersarang. UI mempertahankan MQG1 sebagai tampilan legacy yang terpisah; decoder tidak mengubah bentuk satu skema menjadi skema lain. Browser tidak menghitung Rs, q, q0, rasio, atau konsentrasi gas.

Pada MQG2, nilai NH3 hanya dapat berasal dari field firmware dan hanya ditampilkan jika status perangkat `IN_RANGE`, respons berkualitas `READY`, bukti sumber masih segar, dan umur sumber ditambah waktu sejak dispatch permintaan baru masih di bawah batas. Hasil tetap berlabel `TEST_ONLY`; tidak ada formula JavaScript, clamp, fallback generik CO2, atau inferensi ppm dari telemetry.

Form referensi mengirim kontrak `response_mode=normalized` dengan kondisi referensi operasional atau udara bersih terkonfirmasi, atestasi eksplisit, kriteria sampel, rangkaian/divider/topologi, serta pemanasan aktual. Operational hanya membuat referensi rasio; jalur estimasi NH3 memerlukan deklarasi operator untuk udara bersih dan pemanasan aktual lebih dari 48 jam. Suhu dan RH opsional serta dicatat independen. Form tidak mengisi nilai RL, q0, beban modul, atau nilai rangkaian bawaan.

Rujukan produsen: [Winsen MQ135 product manual v1.6, valid from 2021-07-01](https://www.winsen-sensor.com/manual/mq135.html?lang=en) dan [PDF v1.6](https://www.winsen-sensor.com/d/files/mq135-%28ver1_6%29---manual.pdf). Figure 3 memplot Rs/R0 terhadap konsentrasi untuk H2, Air, NH3, dan Toluene. Manual juga mencatat respons MQ-135 terhadap sulfida dan uap seri benzena; ini mendasari peringatan sensitivitas lintas-gas, bukan klaim selektivitas atau validasi akurasi NH3. Manual menyebut preheat time over 48 hours sebagai kondisi standar. Kurva NH3 yang dipaketkan firmware diberi label riset `TEST_ONLY`, bukan kalibrasi produksi.

Mode firmware latest-only tidak menyimpan backlog sensor untuk dikirim ulang.
Riwayat UI adalah sampel yang diterima server, bukan semua percobaan capture.
Saat offline atau request gagal, pembacaan dapat hilang dan tidak direkonstruksi.
Satu payload aktif tetap immutable selama HTTPS berlangsung; ini bukan antrean histori.
Sensor terbaru dan hasil web tetap ditampilkan ketika hasil MCU belum tersedia,
tanpa menyatakan seluruh data riset hilang. Angka analisis ditampilkan maksimal
dua desimal, sementara nilai internal/perhitungan tidak dibulatkan ulang.

Panel sensor membaca telemetry yang diterima API. Polling otomatis bukan bukti capture baru. `READY` adalah kualitas pembacaan, bukan freshness. Label terbaru memakai batas UI 30 detik; nilai `fresh` dari server, termasuk `null`, tidak diubah. Umur presentasi ditambah memakai jam monotonik browser dan tidak menjadi muda lagi hanya karena respons cache yang sama diterima ulang. Error API ditampilkan, bukan disembunyikan sebagai aliran live.

Animasi berupa transisi warna/border 400 ms dengan highlight maksimum 1200 ms. Pemicu adalah capture baru yang recent dan READY, bukan setiap poll, tick umur, mount awal, atau sequence yang mundur pada boot yang sama. Reduced motion meniadakan gerak. Tidak ada nilai acak, count-up numerik, interpolasi titik, atau POST ke perangkat dari fitur ini.

Histori dipakai bersama oleh chart dan model. SensorHistoryCharts hanya mengambil head ulang ketika capture identity berubah, atau saat refresh eksplisit; model tidak membuka poller sendiri. Callback membawa deviceId dan histori per sensor. Cleanup, abort, cursor, boot guard dan batas 1000 titik dipertahankan. Hasil MCU historis ditampilkan terpisah.

## Profil model web v2

Seluruh keluaran berprovenance `WEB_DERIVED_TEST_ONLY`, bukan keluaran MCU, kalibrasi produksi, keputusan safety tervalidasi atau kendali aktuator. Asal `UNKNOWN` tidak dinaikkan menjadi `PHYSICAL`.

| Parameter | Nilai/asumsi | Sumber |
| --- | --- | --- |
| Kapasitas | 10 kg, hanya asumsi web | docs/prototype-operation.md, PrototypeConfig |
| Jarak kosong/penuh | 230.153 mm / 20 mm, hanya jika jarak tersedia | firmware/lib/acquisition/src/acquisition/default_profiles.h |
| Referensi ADC | 3300 mV | default_profiles.h dan processing_engine.h |
| Ambang kosong berat | 0.05 kg | processing_engine.h |
| Fuzzy | 4 input, 16 aturan, min/max, centroid 101 titik | firmware/lib/models/src/models/prototype_defaults.cpp |
| Diagnostik suhu | perhatian >=60 C, pita 55 sampai60 C, 2 observasi clear | prototype_defaults.cpp |

Parameter tersebut bukan pembacaan profil NVS atau hasil kalibrasi fisik perangkat. Panel memperlihatkan asumsi tersebut; perubahan web tidak menulis parameter ke MCU.

### Keterisian dan utilisasi

Jika firmware menerbitkan `converted_value.fill_percent`, nilai tersebut dipakai apa adanya setelah validasi 0..100. Nol dipertahankan. Jarak tidak direkonstruksi dari persentase. Nilai persentase yang hadir tetapi invalid tidak ditutupi fallback.

Jika persentase tidak diterbitkan namun jarak engineering tersedia, perhitungan prototipe adalah `clamp(100*(230.153-distance_mm)/(230.153-20),0,100)`. Utilisasi memakai `100*kg/10`. Berat negatif invalid; utilisasi di luar domain fuzzy tidak dipaksa menjadi hasil normal.

### OGI

OGI relatif adalah `clamp(100*adc_mv/3300,0,100)` untuk MQ-135. Ini ekstensi web yang dinyatakan secara eksplisit dari asumsi indeks tegangan prototipe. Implementasi MCU saat ini membaca kanal MQ7/MQ8 meskipun komentarnya menyebut MQ135; implementasi web tidak diklaim identik byte-per-byte dengan firmware. OGI bukan ppm, R0, konsentrasi kimia, probabilitas atau akurasi.

### Akumulasi teramati

Durasi dihitung hanya antara sampel berat kosong terakhir yang benar-benar teramati dan endpoint dalam boot yang sama, memakai monotonic_us. Tanpa observasi kosong, hasil tidak tersedia. Berat negatif tidak dianggap kosong. Tidak ada klaim bahwa seluruh kejadian pengosongan di sela sampel telah tertangkap, bahwa interval tanpa data bersifat kontinu, atau bahwa awal jendela adalah awal seluruh riwayat perangkat. MCU prototipe saat ini hanya menghasilkan nol saat kosong, tanpa timer persistent; perhitungan histori web adalah fitur terpisah.

### Fuzzy dan diagnostik

Fuzzy memakai keterisian, utilisasi berat, OGI dan akumulasi yang masing-masing memenuhi syarat. Keanggotaan adalah derajat 0..1, bukan probabilitas. Trace input/keanggotaan/aturan ditampilkan saat hasil tersedia. Tidak memakai hasil MCU lama sebagai fallback tersembunyi.

Safety hanya dapat menampilkan perlu perhatian atau belum cukup data. Tidak ada status aman produksi dan tidak ada kontrol heater/motor/relay. Kanal B3/anorganik yang tidak tersedia tetap insufficient data.

## Validitas dan bukti

Per metrik wajib ada snapshot sensor yang dimiliki device aktif: READY, umur finite 0..30000 ms, tidak fresh=false dan tidak future_clock_skew. Histori harus sesuai boot/origin dan endpoint sequence snapshot, tidak melampauinya atau berisi duplicate identity. Histori beda boot tidak digabung. Kegagalan menghasilkan nilai null dengan alasan, bukan nol palsu.

Unit fixtures dan browser fixture tests hanya membuktikan perilaku kode/UI. Bukti browser dengan API nyata dipisahkan. Jika firmware berhenti mengirim, panel menampilkan data terlambat dan model unavailable; UI tidak menyimulasikan capture agar tampak live.
