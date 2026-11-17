import type { CalibrationSensorId } from '../api/calibration';

const sensorLabels: Record<CalibrationSensorId, string> = {
  hx711_organic: 'Berat · Organik (HX711)',
  hx711_inorganic: 'Berat · Anorganik (HX711)',
  hx711_b3: 'Berat · B3 (HX711)',
  ultrasonic_organic: 'Ketinggian · Organik',
  ultrasonic_inorganic: 'Ketinggian · Anorganik',
  ultrasonic_b3: 'Ketinggian · B3',
  mq135_gas: 'Respons Gas Relatif · MQ-135',
  mq4: 'Gas · MQ-4', mq135: 'Gas · MQ-135', mq7: 'Gas · MQ-7', mq8: 'Gas · MQ-8',
  ds18b20_b3: 'Suhu · B3 (DS18B20)',
  sht31_44: 'Suhu & kelembapan · SHT31 0x44',
  sht31_45: 'Suhu & kelembapan · SHT31 0x45',
};
export const calibrationSensorLabel = (id: CalibrationSensorId): string => sensorLabels[id];
export type WorkflowStep = { id: string; title: string; description: string };
export const CALIBRATION_STEPS: readonly WorkflowStep[] = [
  { id: 'diagnostics', title: 'Diagnostik', description: 'Periksa bukti aktual dan kesiapan sensor sebelum memulai.' },
  { id: 'criteria', title: 'Kriteria', description: 'Masukkan referensi fisik dan kriteria yang diminta perangkat.' },
  { id: 'capture', title: 'Capture', description: 'Ambil sampel pada anchor yang sesuai dengan referensi fisik.' },
  { id: 'validate', title: 'Validasi', description: 'Periksa konsistensi bukti sebelum membuat proposal.' },
  { id: 'preview', title: 'Preview', description: 'Tinjau proposal dan identitas bukti sebelum menyimpan.' },
  { id: 'commit', title: 'Commit', description: 'Simpan hanya proposal tervalidasi yang masih berlaku.' },
  { id: 'done', title: 'Selesai', description: 'Profil aktif harus dikonfirmasi oleh perangkat.' },
];
export const WIFI_STEPS: readonly WorkflowStep[] = [
  { id: 'connect', title: 'Hubungkan BLE', description: 'Buka koneksi lokal ke perangkat yang dipilih.' },
  { id: 'identity', title: 'Cocokkan identitas', description: 'Cocokkan GET_DEVICE_ID dengan identitas pada halaman ini. Ini bukan autentikasi BLE.' },
  { id: 'network', title: 'Pilih jaringan', description: 'Pindai jaringan, isi password, lalu tunggu konfirmasi koneksi perangkat.' },
];
