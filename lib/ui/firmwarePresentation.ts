import type { FirmwareMonitoringSample, FirmwareResult } from '../api/types';

type SensorId = FirmwareMonitoringSample['sensor_id'];
export const firmwareSensorLabels: Record<SensorId, string> = {
  hx711_organic: 'HX711 organik', hx711_inorganic: 'HX711 anorganik', hx711_b3: 'HX711 B3',
  ultrasonic_organic: 'Ultrasonik organik', ultrasonic_inorganic: 'Ultrasonik anorganik', ultrasonic_b3: 'Ultrasonik B3',
  mq4: 'MQ-4', mq135: 'MQ-135', mq7: 'MQ-7', mq8: 'MQ-8',
  ds18b20_b3: 'DS18B20 B3', sht31_44: 'SHT31 0x44', sht31_45: 'SHT31 0x45',
};
const fmt = (value: unknown, unit: string) => value === null || value === undefined ? '—' : `${String(unit === 'kg' && typeof value === 'number' ? Math.abs(value) : value)} ${unit}`;
export function sensorValueRows(sensor: SensorId, converted: unknown, raw: unknown): [string, string][] {
  const c = converted && typeof converted === 'object' ? converted as Record<string, unknown> : {};
  const r = raw && typeof raw === 'object' ? raw as Record<string, unknown> : {};
  switch (sensor) {
    case 'hx711_organic': case 'hx711_inorganic': case 'hx711_b3':
      return [['Massa', fmt(c.kg, 'kg')], ['Hitungan HX711', fmt(r.counts, 'counts')]];
    case 'ultrasonic_organic': case 'ultrasonic_inorganic': case 'ultrasonic_b3':
      return [['Jarak', fmt(c.distance_mm, 'mm')], ['Echo', fmt(r.echo_us, 'µs')]];
    case 'mq4': case 'mq135': case 'mq7': case 'mq8':
      return [['ADC', fmt(c.adc_mv, 'mV')], ['Hitungan ADC', fmt(r.adc_counts, 'counts')]];
    case 'ds18b20_b3':
      return [['Suhu', fmt(c.temperature_c, '°C')], ['Konversi selesai', fmt(r.conversion_complete, '')], ['CRC scratchpad', fmt(r.crc_valid, '')], ['Identitas ROM', fmt(r.rom, '')]];
    case 'sht31_44': case 'sht31_45':
      return [['Suhu', fmt(c.temperature_c, '°C')], ['Kelembapan', fmt(c.humidity_percent, '%RH')], ['Tick mentah suhu', fmt(r.temperature_ticks, 'ticks')], ['Tick mentah RH', fmt(r.humidity_ticks, 'ticks')]];
  }
}
export const firmwareMetricRows = [
  ['fill_pct', 'Keterisian', '%'], ['weight_kg', 'Berat', 'kg'],
  ['weight_utilization_pct', 'Utilisasi berat', '%'], ['ogi', 'OGI', ''],
  ['accumulation_hours', 'Waktu akumulasi', 'jam'], ['temperature_c', 'Suhu', '°C'],
] as const;
export function formatFirmwareMetric(value: number | null | undefined, unit = ''): string {
  if (typeof value !== 'number' || !Number.isFinite(value)) return unit === 'ppm' ? 'Belum tersedia ppm' : 'Belum tersedia';
  // Voltages are integer millivolts on the wire; a reconstructed divider value
  // must not leak twelve decimals into the tile.
  const shown = unit === 'kg' ? Math.abs(value) : unit === 'ppm' || unit === 'mV' ? Math.round(value) : value;
  return `${shown}${unit ? ` ${unit}` : ''}`;
}
export function firmwareStreamLabel(stream: string): string {
  return ({ organic: 'Organik', inorganic: 'Anorganik', b3: 'B3' } as Record<string, string>)[stream] ?? stream;
}
export function firmwareReason(reason: string): string {
  return ({ MODEL_NOT_CONFIGURED: 'Model FIS belum dikonfigurasi', SAFETY_NOT_CONFIGURED: 'Model safety belum dikonfigurasi', INVALID_PROFILE: 'Profil kalibrasi belum valid', INPUT_MISSING: 'Input sensor belum lengkap', INPUT_UNCALIBRATED: 'Input sensor belum terkalibrasi' } as Record<string, string>)[reason] ?? 'Alasan belum tersedia';
}

export function bestFirmwareDecision(results: readonly FirmwareResult[]): FirmwareResult | undefined {
  const alert = results.find(result => result.safety.state === 'ALERT' && result.safety.model_state === 'PRODUCTION');
  if (alert) return alert;
  const ready = results.filter(result => result.fuzzy.state === 'READY' && (result.fuzzy.model_state === 'PRODUCTION' || result.fuzzy.model_state === 'TEST_ONLY') && result.fuzzy.score !== null && Number.isFinite(result.fuzzy.score));
  return ready.reduce<FirmwareResult | undefined>((best, item) => !best || item.fuzzy.score! > best.fuzzy.score! ? item : best, undefined) ?? results[0];
}
export function latestFirmwareTime(results: readonly FirmwareResult[] = []): string | null {
  return results.reduce<string | null>((latest, result) => {
    if (result.time_quality === 'UNSYNCED' || !result.recorded_at || !Number.isFinite(Date.parse(result.recorded_at))) return latest;
    return latest === null || Date.parse(result.recorded_at) > Date.parse(latest) ? result.recorded_at : latest;
  }, null);
}
