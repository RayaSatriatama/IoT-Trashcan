import { describe, expect, it } from 'vitest';
import { CALIBRATION_SENSOR_IDS } from '../../lib/api/calibration';
import { sensorValueRows } from '../../lib/ui/firmwarePresentation';

// Formatting fixtures only; these are not physical-device evidence.
describe('firmware sensor value presentation', () => {
  it('renders schema-shaped converted values with actual units and keeps zero', () => {
    expect(sensorValueRows('sht31_44', { temperature_c: 0, humidity_percent: 0 }, { temperature_ticks: 0, humidity_ticks: 0, temperature_crc_valid: true, humidity_crc_valid: true })).toEqual([
      ['Suhu', '0 °C'], ['Kelembapan', '0 %RH'], ['Tick mentah suhu', '0 ticks'], ['Tick mentah RH', '0 ticks'],
    ]);
  });
  it('covers every canonical MCU sensor identity without converting gas counts to ppm', () => {
    expect(CALIBRATION_SENSOR_IDS).toHaveLength(13);
    expect(sensorValueRows('mq7', { adc_mv: 0 }, { adc_counts: 0 })).toEqual([['ADC', '0 mV'], ['Hitungan ADC', '0 counts']]);
  });
  it('renders all 13 IDs using production telemetry-v3 fixture raw/converted shapes', () => {
    const cases = [
      ['hx711_organic', { kg: 1 }, { counts: 1500 }, [['Massa', '1 kg'], ['Hitungan HX711', '1500 counts']]],
      ['hx711_inorganic', { kg: 0 }, { counts: 0 }, [['Massa', '0 kg'], ['Hitungan HX711', '0 counts']]],
      ['hx711_b3', { kg: 2 }, { counts: -2 }, [['Massa', '2 kg'], ['Hitungan HX711', '-2 counts']]],
      ['ultrasonic_organic', { distance_mm: 1700, fill_percent: 50 }, { echo_us: 1000 }, [['Jarak', '1700 mm'], ['Echo', '1000 µs']]],
      ['ultrasonic_inorganic', { distance_mm: 0 }, { echo_us: 0 }, [['Jarak', '0 mm'], ['Echo', '0 µs']]],
      ['ultrasonic_b3', null, null, [['Jarak', '—'], ['Echo', '—']]],
      ['mq4', null, { adc_counts: 111 }, [['ADC', '—'], ['Hitungan ADC', '111 counts']]],
      ['mq135', { adc_mv: 0 }, { adc_counts: 0 }, [['ADC', '0 mV'], ['Hitungan ADC', '0 counts']]],
      ['mq7', null, { adc_counts: 900, heater_phase: 'PREHEAT' }, [['ADC', '—'], ['Hitungan ADC', '900 counts']]],
      ['mq8', { adc_mv: 1234 }, { adc_counts: 1234 }, [['ADC', '1234 mV'], ['Hitungan ADC', '1234 counts']]],
      ['ds18b20_b3', { temperature_c: 85 }, { conversion_complete: true, crc_valid: true, rom: '280102030405069e' }, [['Suhu', '85 °C'], ['Konversi selesai', 'true '], ['CRC scratchpad', 'true '], ['Identitas ROM', '280102030405069e ']]],
      ['sht31_44', { temperature_c: 26, humidity_percent: 61 }, { temperature_ticks: 30000, humidity_ticks: 40000, temperature_crc_valid: true, humidity_crc_valid: true }, [['Suhu', '26 °C'], ['Kelembapan', '61 %RH'], ['Tick mentah suhu', '30000 ticks'], ['Tick mentah RH', '40000 ticks']]],
      ['sht31_45', { temperature_c: 0, humidity_percent: 0 }, { temperature_ticks: 0, humidity_ticks: 0, temperature_crc_valid: false, humidity_crc_valid: true }, [['Suhu', '0 °C'], ['Kelembapan', '0 %RH'], ['Tick mentah suhu', '0 ticks'], ['Tick mentah RH', '0 ticks']]],
    ] as const;
    expect(cases.map(([sensor, converted, raw]) => [sensor, sensorValueRows(sensor, converted, raw)])).toEqual(
      cases.map(([sensor, , , expected]) => [sensor, expected]),
    );
    expect(cases.map(([sensor]) => sensor)).toEqual(CALIBRATION_SENSOR_IDS);
  });
  it('does not render mismatched value keys as a measurement', () => {
    expect(sensorValueRows('mq7', { temperature_c: 25 }, { counts: 12 })).toEqual([['ADC', '—'], ['Hitungan ADC', '—']]);
  });
});
