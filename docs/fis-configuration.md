# Konfigurasi FIS transport (lima input)

Status: **rancangan web TEST_ONLY.** Bukan model produksi, bukan keluaran MCU,
bukan kalibrasi gas, dan bukan klaim akurasi fisik. Skor tidak boleh dipakai
sebagai dasar operasional sebelum model dan sensor divalidasi.

Implementasi: `lib/ui/transportFuzzy.ts`, `lib/ui/slotDecision.ts`,
`lib/ui/useSlotDecision.ts`, `lib/ui/useOrganicCapacity.ts`. Panel rumus:
`components/FisBreakdown.tsx`. Uji: `tests/device/transport-fuzzy.test.ts`.

## Struktur

| Item | Nilai |
| --- | --- |
| Mesin | Mamdani |
| Jumlah input | 5 |
| Himpunan per input | 3 |
| Rule dievaluasi | 3⁵ = 243 kombinasi |
| Operator AND | `min` |
| Agregasi | `max` |
| Defuzzifikasi | centroid, integrasi trapesium |
| Grid | 101 titik pada domain 0–100 |
| Output | skor 0–100 dan satu dari empat label |
| Input hilang | skor `null`, status UNAVAILABLE — tidak diisi nol |

## Variabel input

| # | Input | ID | Satuan | Domain | Sumber |
| --- | --- | --- | --- | --- | --- |
| 1 | Keterisian | `fill` | % | 0–100 | `ultrasonic_organic.fill_percent` |
| 2 | Utilisasi berat | `weight_utilization` | % | 0–100 | `hx711_organic.kg` ÷ kapasitas |
| 3 | Amonia | `ammonia` | ppm | 0–300 | estimasi NH3 dari `mq135.adc_pin_mv` |
| 4 | Waktu akumulasi | `accumulation` | jam | 0–100 | timer sejak pembacaan pertama |
| 5 | Suhu | `temperature` | °C | 0–60 | `sht31_44.temperature_c` |

## Fungsi keanggotaan

| Input | Himpunan | Bentuk | Titik |
| --- | --- | --- | --- |
| Keterisian | Rendah | trapesium | 0, 0, 25, 45 |
| Keterisian | Sedang | segitiga | 30, 55, 80 |
| Keterisian | Tinggi | trapesium | 65, 85, 100, 100 |
| Utilisasi berat | Ringan | trapesium | 0, 0, 20, 40 |
| Utilisasi berat | Sedang | segitiga | 25, 50, 75 |
| Utilisasi berat | Berat | trapesium | 60, 80, 100, 100 |
| Amonia | Rendah | trapesium | 0, 0, 10, 20 |
| Amonia | Sedang | segitiga | 10, 25, 40 |
| Amonia | Tinggi | trapesium | 30, 50, 300, 300 |
| Waktu akumulasi | Singkat | trapesium | 0, 0, 20, 40 |
| Waktu akumulasi | Sedang | segitiga | 25, 50, 75 |
| Waktu akumulasi | Lama | trapesium | 60, 80, 100, 100 |
| Suhu | Dingin | trapesium | 0, 0, 15, 25 |
| Suhu | Sedang | segitiga | 20, 30, 40 |
| Suhu | Panas | trapesium | 35, 45, 60, 60 |

Empat input pertama berasal dari tabel pemilik. **Himpunan suhu adalah asumsi
desain** karena tabel tersebut tidak memuat suhu; himpunan tengahnya dinamai
`Sedang`, bukan `Normal`, agar tidak terbaca sebagai kategori keputusan.

## Himpunan output dan pemetaan kategori

| Himpunan | Bentuk | Titik | Centroid | Ambang | Kategori UI |
| --- | --- | --- | --- | --- | --- |
| Rendah | trapesium | 0, 0, 30, 40 | 17,62 | z* ≥ 0 | Normal |
| Sedang | segitiga | 25, 45, 65 | 45,00 | z* ≥ 25 | Waspada |
| Tinggi | segitiga | 50, 70, 90 | 70,00 | z* ≥ 50 | Angkat |
| Kritis | trapesium | 75, 90, 100, 100 | 87,50 | z* ≥ 75 | Angkat |

Keputusan tekstual: Rendah "Belum perlu diangkut", Sedang "Perlu dipantau",
Tinggi "Direkomendasikan diangkut", Kritis "Penanganan segera".

## Rule dan severity

Consequent tiap rule ditentukan dari jumlah indeks himpunan pemenang, diskalakan
ke tabel empat input yang dipublikasikan pemilik:

```
severity = ceil((Σindeks × 4 / jumlah_input − 1) / 2), dijepit ke 0..3
```

| Σ indeks (4 input) | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Severity | 0 | 0 | 1 | 1 | 2 | 2 | 3 | 3 | 3 |

| Σ indeks (5 input) | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Severity | 0 | 0 | 1 | 1 | 2 | 2 | 2 | 3 | 3 | 3 | 3 |

Kekuatan rule = `min` derajat kelima antecedent. Level tiap himpunan output =
`max` kekuatan rule yang menuju ke sana.

**Rule base ini bukan dari pemilik.** Tabel yang diberikan hanya memuat parameter
himpunan dan rentang output, sehingga aturan severity di atas adalah konstruksi
deterministik dari implementasi.

## Sifat skor yang perlu diketahui

Karena tiap input umumnya hanya punya satu himpunan berderajat > 0 pada nilai
crisp, biasanya hanya **satu rule** yang menyala. Akibatnya skor jatuh ke salah
satu dari empat centroid: 17,62 / 45,00 / 70,00 / 87,50. Nilai antara hanya
muncul saat input berada di pita tumpang-tindih antarhimpunan, atau saat suhu
menyeberangi batas severity.

Sapuan suhu dengan input lain tetap (fill 60, weight 40, ammonia 20, akumulasi 30):

| Suhu (°C) | 10–15 | 20 | 25 | 30 | 35 | 40 | 45–55 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Skor | 52,63 | 52,63 | 70,00 | 70,00 | 70,00 | 70,00 | 70,00 |

Perubahan skor terjadi saat severity rule berubah, bukan gradasi halus. Jika skor
yang mulus dibutuhkan, output perlu diubah menjadi himpunan singleton atau rule
base ditulis eksplisit.

## Kapasitas berat

`ORGANIC_CAPACITY_KG` default **2 kg**. Nilai dapat diubah operator lewat tombol
`Ubah` di kartu, dibatasi 0,1–100 kg, disimpan per perangkat di `localStorage`
(`sigap.organic.capacity.<deviceId>`). Nilai lama 5 kg (default sebelumnya)
dimigrasi ke 2 kg; nilai lain dipertahankan. Perubahan hanya memengaruhi
tampilan, utilisasi, dan input fuzzy berat — tidak menulis NVS atau firmware.

## Batas bukti

- Suhu masuk FIS, tetapi pengaruhnya kasar (lihat tabel sapuan di atas).
- Himpunan suhu adalah asumsi desain, bukan tabel pemilik.
- Skor tidak menjadi keputusan produksi; ia berlabel TEST_ONLY di panel.
- Estimasi NH3 memakai ekstrapolasi di luar domain 10–1000 ppm; lihat
  [kurva NH3](mq135-winsen-nh3-curve.md).
