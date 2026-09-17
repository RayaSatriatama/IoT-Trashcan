# IoT-Trashcan

IoT-Trashcan memuat dokumentasi dan aset SIGAP Smart Bin, sistem pengelolaan sampah cerdas berbasis IoT dan fuzzy logic.

## Dokumen utama

- [Product Requirements Document](docs/SIGAP_PRD_v1.0.md)
- [Software Design Document](docs/SIGAP_SDD_v1.0.md)
- [Konsep sistem dan RAB](docs/SIGAP_Smart_Bin_Konsep_dan_RAB_v0.8.md)
- [Indeks dokumentasi](docs/README.md)
- [Paket aset visual](docs/SIGAP_Asset_Pack_Complete_v0.6/README.md)

## Status project

SIGAP MVP memiliki satu runtime Go dengan SQLite embedded, authenticated ingest, monitoring read APIs, history, CSV export, dan frontend Expo Web yang di-embed saat release build. Nilai research final untuk FIS, safety, OGI, offline duration, verified-empty, dan event transition tetap deferred.

## Menjalankan development

Frontend:

```bash
npm ci
npm test -- --run
npm run typecheck
npm run web
```

Go API:

```bash
SIGAP_DB_PATH=./sigap.db SIGAP_HTTP_ADDR=:8080 go run ./cmd/sigap
```

Monitoring frontend menggunakan relative same-origin `/api/...` pada web. `EXPO_PUBLIC_API_BASE_URL` hanya digunakan untuk development native bila diperlukan.

## Release binary

Gunakan script deterministic berikut. Script membangun Expo SPA, men-stage bundle sementara ke Go `go:embed`, menjalankan seluruh test dan vet/race check, lalu menghasilkan satu binary di `/tmp/sigap-release` tanpa menyimpan bundle generated ke Git.

```bash
SIGAP_RELEASE_OUTPUT=/tmp/sigap-release ./scripts/build-release.sh
SIGAP_DB_PATH=./sigap.db SIGAP_HTTP_ADDR=:8080 /tmp/sigap-release
```

Runtime production menggunakan `SIGAP_DB_PATH`, `SIGAP_HTTP_ADDR`, dan optional `OFFLINE_AFTER_SECONDS`. Database SQLite tetap file external dan persistent, bukan bagian dari binary.

Production FIS dan safety numeric configuration tetap research-blocked. Release runtime tidak menggunakan test fixture thresholds dan tidak memalsukan keputusan valid ketika engine production belum dikonfigurasi.

## Simulation mode

Simulation mode bersifat opt-in untuk demo tanpa hardware. Buat environment lokal lalu jalankan runtime Go yang sama dengan pipeline production:

```bash
cp .env.simulation.example .env.simulation
./scripts/run-simulation.sh
```

Runner simulasi terlebih dahulu menjalankan release build agar Expo SPA benar-benar masuk ke binary Go, lalu menjalankan binary tersebut dengan environment simulasi. Ini diperlukan karena clean checkout hanya menyimpan placeholder untuk `go:embed`, bukan bundle frontend generated.

Buka `http://localhost:8080`. Simulator membuat 3 lokasi, 4 device, dan 12 slot. ID database simulator memakai prefix `sim-`, sedangkan device code yang terlihat di UI memakai prefix `SIM-`. Simulator lalu mengirim telemetry melalui authenticated ingest. Siklus demo mencakup Normal, Waspada, Angkat, Safety Alert, Data Invalid, Offline, dan Recovery. Data sintetis, FIS, dan safety threshold pada mode ini hanya fixture demo, bukan kalibrasi production atau research.

`SIGAP_SIMULATION_INTERVAL_SECONDS` harus bilangan bulat positif. `SIGAP_SIMULATION_SEED` membuat variasi kecil dapat direproduksi dan merotasi credential simulator lokal saat database yang sama digunakan kembali. Untuk memulai ulang data demo, hapus database secara manual:

```bash
rm sigap-simulation.db
```

Saat aktif, UI menampilkan banner `MODE SIMULASI`. Saat `SIGAP_SIMULATION_ENABLED` tidak ada atau `false`, seeding, simulator, dan demo decision configuration tidak dijalankan.

## Kontribusi

Workflow GitHub project tersedia di [skill project](.agents/skills/github-project-practices/SKILL.md). Ringkasannya:

1. Buat branch singkat dari `main`.
2. Kerjakan satu perubahan terfokus.
3. Jalankan pemeriksaan yang relevan dan periksa diff.
4. Buka pull request kecil menggunakan template project.
5. Gunakan squash merge lalu hapus branch.

Jangan commit credential, file `.env`, virtual environment, dependency terunduh, atau artefak build.
