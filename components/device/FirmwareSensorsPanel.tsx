import { useEffect, useRef, useState } from 'react';
import { Button, Card, Input, Text, XStack, YStack } from 'tamagui';
import { DescriptionHint } from '../SectionHeader';
import { CALIBRATION_SENSOR_IDS, createCalibrationApi, type SensorDiagnostic } from '../../lib/api/calibration';
import { textStyles } from '../../lib/ui/typography';

type Row = [string, string];
const fmt = (value: unknown, unit: string) => value === null || value === undefined ? '—' : `${String(value)} ${unit}`;
export function sensorValueRows(sensor: typeof CALIBRATION_SENSOR_IDS[number], converted: unknown, raw: unknown): Row[] {
  const c = converted && typeof converted === 'object' ? converted as Record<string, unknown> : {};
  const r = raw && typeof raw === 'object' ? raw as Record<string, unknown> : {};
  switch (sensor) {
    case 'hx711_organic': case 'hx711_inorganic': case 'hx711_b3': return [['Massa', fmt(c.kg, 'kg')], ['Hitungan HX711', fmt(r.counts, 'counts')]];
    case 'ultrasonic_organic': case 'ultrasonic_inorganic': case 'ultrasonic_b3': return [['Jarak', fmt(c.distance_mm, 'mm')], ['Echo', fmt(r.echo_us, 'µs')]];
    case 'mq4': case 'mq135': case 'mq7': case 'mq8': return [['ADC', fmt(c.adc_mv, 'mV')], ['Hitungan ADC', fmt(r.adc_counts, 'counts')]];
    case 'ds18b20_b3': return [['Suhu', fmt(c.temperature_c, '°C')], ['Konversi selesai', fmt(r.conversion_complete, '')], ['CRC scratchpad', fmt(r.crc_valid, '')], ['Identitas ROM', fmt(r.rom, '')]];
    case 'sht31_44': case 'sht31_45': return [['Suhu', fmt(c.temperature_c, '°C')], ['Kelembapan', fmt(c.humidity_percent, '%RH')], ['Tick mentah suhu', fmt(r.temperature_ticks, 'ticks')], ['Tick mentah RH', fmt(r.humidity_ticks, 'ticks')]];
  }
}
const labels: Record<typeof CALIBRATION_SENSOR_IDS[number], string> = {
  hx711_organic:'HX711 organik',hx711_inorganic:'HX711 anorganik',hx711_b3:'HX711 B3',ultrasonic_organic:'Ultrasonik organik',ultrasonic_inorganic:'Ultrasonik anorganik',ultrasonic_b3:'Ultrasonik B3',mq4:'MQ-4',mq135:'MQ-135',mq7:'MQ-7',mq8:'MQ-8',ds18b20_b3:'DS18B20 B3',sht31_44:'SHT31 0x44',sht31_45:'SHT31 0x45',
};
export { labels as firmwareSensorLabels };

export function FirmwareSensorsPanel({ deviceCode }: { deviceCode: string }) {
  const [token, setToken] = useState('');
  const [items, setItems] = useState<Partial<Record<typeof CALIBRATION_SENSOR_IDS[number], SensorDiagnostic | null>>>({});
  const [busy, setBusy] = useState(false); const [error, setError] = useState('');
  const active = useRef<AbortController | null>(null); const generation = useRef(0);
  const context = useRef({ deviceCode, generation: 0 });
  // Retire synchronously during render: old callbacks cannot borrow a new generation.
  if (context.current.deviceCode !== deviceCode) {
    context.current = { deviceCode, generation: context.current.generation + 1 };
    active.current?.abort(); active.current = null;
    setItems({}); setBusy(false); setError(''); setToken('');
  }
  const renderContext = context.current;
  useEffect(() => () => {
    // A previous render's cleanup must not retire the replacement render's context.
    if (context.current === renderContext) {
      context.current = { deviceCode: '', generation: context.current.generation + 1 };
      active.current?.abort(); active.current = null;
    }
  }, [deviceCode]);
  async function readOnce() {
    const admitted = context.current;
    if (admitted !== renderContext || admitted.deviceCode !== deviceCode) return;
    const bearerToken = token.trim(); setToken(''); setError(''); setItems({});
    if (!bearerToken) { setError('Masukkan otorisasi operator sementara untuk membaca.'); return; }
    active.current?.abort(); const controller = new AbortController(); active.current = controller; const owner = ++generation.current; setBusy(true);
    const isOwner = () => context.current === admitted && generation.current === owner && active.current === controller;
    try {
      const api = createCalibrationApi({ bearerToken });
      const results = await Promise.all(CALIBRATION_SENSOR_IDS.map(async sensor => {
        try {
          const result = await api.getDiagnostics(admitted.deviceCode, sensor, controller.signal);
          if (result.diagnostic && result.diagnostic.sensor_id !== sensor) throw new Error('Sensor identity mismatch');
          return [sensor, result.diagnostic ?? null] as const;
        } catch (e) {
          if (controller.signal.aborted) throw e;
          if (e && typeof e === 'object' && 'status' in e && e.status === 404) return [sensor, null] as const;
          throw e;
        }
      }));
      if (isOwner()) setItems(Object.fromEntries(results));
    } catch (e) {
      if (isOwner()) {
        controller.abort(); setItems({});
        setError(e && typeof e === 'object' && 'status' in e && e.status === 401 ? 'Otorisasi operator tidak berlaku. Masukkan sesi baru.' : 'Pembacaan gagal. Masukkan otorisasi baru dan coba lagi.');
      }
    } finally { if (isOwner()) { active.current = null; setBusy(false); } }
  }
  return <Card role="region" aria-label="Telemetri sensor firmware MCU" borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius={15} padding={14} gap={9}>
    <XStack alignItems="center" gap={4} flexWrap="wrap"><Text color="$color" {...textStyles.cardTitle}>Telemetri sensor MCU</Text><DescriptionHint ariaLabel="Informasi asal telemetri MCU" description="Pembacaan arsip firmware-v3. Umur dihitung dari waktu UTC yang tersimpan, dibulatkan ke bawah dalam milidetik; tidak ada batas umur segar yang ditetapkan. Nilai 0 ms berarti kurang dari 1 ms. Urutan lintas boot hanya diketahui bila arsip punya jangkar UTC yang dapat dibandingkan. Asal mengikuti metadata BOOT yang dinyatakan firmware, bukan verifikasi perangkat keras; nilai mentah dan konversi tetap terpisah." /></XStack>
    <XStack gap={8} alignItems="center" flexWrap="wrap"><Input aria-label="Otorisasi operator sementara" secureTextEntry value={token} onChangeText={setToken} placeholder="Otorisasi operator" autoComplete="off" flex={1} minWidth={180} /><Button type="button" disabled={busy} onPress={readOnce}>{busy ? 'Membaca…' : 'Baca 13 sensor'}</Button></XStack>
    {error ? <Text role="alert" color="$statusBahaya">{error}</Text> : null}
    <YStack gap={4}>{CALIBRATION_SENSOR_IDS.map(sensor => { const d=items[sensor]; const origin=d?.sample_origin === 'SIMULATED' ? 'Simulasi sensor firmware' : d?.sample_origin === 'PHYSICAL' ? 'Fisik (metadata firmware)' : 'Asal tidak diketahui'; return <XStack key={sensor} justifyContent="space-between" alignItems="flex-start" gap={8} borderBottomWidth={1} borderColor="$borderColor" paddingVertical={6} flexWrap="wrap"><YStack minWidth={140} flex={1}><Text color="$color" fontWeight="700">{labels[sensor]}</Text><Text color="$colorMuted" fontSize={11}>{d ? `${d.quality} · ${origin}${d.origin_firmware_version ? ` · ${d.origin_firmware_version}` : ''}` : items[sensor] === null ? 'Tidak tersedia' : 'Belum dibaca'}</Text>{d ? <>{d.ordering_unknown ? <Text color="$statusBahaya" fontSize={11}>Urutan lintas boot tidak diketahui</Text> : null}<Text color={d.future_clock_skew || d.fresh === false ? '$statusBahaya' : '$colorMuted'} fontSize={11}>{d.future_clock_skew ? 'Waktu arsip di masa depan; umur tidak diketahui' : d.age_ms === null ? 'Umur tidak diketahui (waktu MCU tidak tersinkron)' : `${d.fresh === false ? 'Kualitas tidak segar' : 'Kesegaran belum ditetapkan'} · ${d.age_ms} ms`}</Text></> : null}</YStack><YStack alignItems="flex-end">{d ? sensorValueRows(sensor,d.converted_value,d.raw).map(([name,value])=><Text key={name} color="$colorMuted" fontSize={11}>{name}: {value}</Text>) : <Text color="$colorMuted" fontSize={11}>—</Text>}</YStack></XStack>; })}</YStack>
  </Card>;
}
