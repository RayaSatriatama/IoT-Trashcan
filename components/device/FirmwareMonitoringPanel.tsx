import { useState } from 'react';
import { Text, XStack, YStack } from 'tamagui';
import { AppButton } from '../AppButton';
import { CALIBRATION_SENSOR_IDS } from '../../lib/api/calibration';
import type { FirmwareMonitoringSample } from '../../lib/api/types';
import { textStyles } from '../../lib/ui/typography';
import { firmwareSensorLabels, sensorValueRows } from './FirmwareSensorsPanel';

export function FirmwareMonitoringPanel({ samples, compact = false }: {
  samples: readonly FirmwareMonitoringSample[] | undefined;
  compact?: boolean;
}) {
  const [details, setDetails] = useState(false);
  const physical4Channels = ['hx711_organic', 'ultrasonic_organic', 'sht31_44', 'mq135'] as const;
  const physical4Metadata = !!samples?.length && samples.every(sample =>
    sample.sample_origin === 'PHYSICAL' &&
    sample.origin_firmware_version === samples[0].origin_firmware_version &&
    (sample.origin_firmware_version === 'sigap-task6-physical4-v1' || sample.origin_firmware_version === 'sigap-task6-physical4-v2') &&
    physical4Channels.includes(sample.sensor_id as typeof physical4Channels[number])
  );
  const missingPhysical4Channels = physical4Metadata
    ? physical4Channels.filter(sensorId => !samples.some(sample => sample.sensor_id === sensorId))
    : [];
  if (!samples || !samples.length) return <Text color="$colorMuted" {...textStyles.bodySecondary}>
    {samples ? 'Belum ada sampel firmware yang diterima.' : 'Monitoring firmware belum tersedia pada API.'}
  </Text>;
  return <YStack role="region" aria-label="Monitoring sensor firmware" gap="$3" minWidth={0}>
    <Text color="$color" {...textStyles.cardTitle}>Sensor firmware</Text>
    {samples.map(sample => {
      if (!CALIBRATION_SENSOR_IDS.includes(sample.sensor_id)) return <Text key={sample.sensor_id} role="alert" color="$statusBahaya">Identitas sensor tidak dikenali.</Text>;
      const origin = sample.sample_origin === 'PHYSICAL' ? 'Fisik (metadata firmware)' : sample.sample_origin === 'SIMULATED' ? 'Simulasi sensor firmware' : 'Asal tidak diketahui';
      const values = sensorValueRows(sample.sensor_id, sample.quality === 'READY' ? sample.converted_value : null, sample.raw)
        .filter(([name, value]) => value !== '—' && name !== 'Identitas ROM');
      return <YStack key={`${sample.sensor_id}:${sample.boot_id}:${sample.last_sequence}`} gap="$1" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$2" minWidth={0}>
        <XStack justifyContent="space-between" gap="$2" flexWrap="wrap">
          <Text color="$color" {...textStyles.bodySecondary} fontWeight="700">{firmwareSensorLabels[sample.sensor_id]}</Text>
          <Text color="$colorMuted" {...textStyles.metadata}>{sample.quality} · {sample.reason}</Text>
        </XStack>
        {values.length ? values.map(([name, value]) => <Text key={name} color="$color" {...textStyles.bodySecondary}>{name}: {value}</Text>) : <Text color="$colorMuted" {...textStyles.bodySecondary}>Nilai mentah belum tersedia.</Text>}
        <Text color="$colorMuted" {...textStyles.metadata}>{origin}</Text>
        <Text color={sample.future_clock_skew || sample.fresh === false ? '$statusBahaya' : '$colorMuted'} {...textStyles.metadata}>
          {sample.future_clock_skew ? 'Waktu arsip di masa depan; umur tidak diketahui' : sample.age_ms === null ? 'Umur tidak diketahui (waktu MCU tidak tersinkron)' : `${sample.fresh === false ? 'Kualitas tidak segar' : 'Kesegaran belum ditetapkan'} · ${sample.age_ms} ms`}
        </Text>
        {sample.ordering_unknown ? <Text color="$statusBahaya" {...textStyles.metadata}>Urutan lintas boot tidak diketahui</Text> : null}
        {!compact && details ? <YStack gap="$1">
          <Text color="$colorMuted" {...textStyles.metadata}>Boot: {sample.boot_id}</Text>
          <Text color="$colorMuted" {...textStyles.metadata}>Sequence: {sample.last_sequence}</Text>
          <Text color="$colorMuted" {...textStyles.metadata}>Firmware: {sample.origin_firmware_version ?? 'Tidak diketahui'}</Text>
        </YStack> : null}
      </YStack>;
    })}
    {missingPhysical4Channels.map(sensorId => <YStack key={sensorId} gap="$1" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$2" minWidth={0}>
      <Text color="$color" {...textStyles.bodySecondary} fontWeight="700">{firmwareSensorLabels[sensorId]}</Text>
      <Text color="$colorMuted" {...textStyles.bodySecondary}>Belum ada sampel untuk kanal pada profil firmware ini.</Text>
      <Text color="$colorMuted" {...textStyles.metadata}>Ekspektasi berdasarkan metadata profil firmware, bukan konfirmasi koneksi fisik.</Text>
    </YStack>)}
    {!compact ? <AppButton variant="ghost" size="sm" ariaExpanded={details} onPress={() => setDetails(value => !value)}>{details ? 'Tutup rincian sampel' : 'Rincian sampel'}</AppButton> : null}
  </YStack>;
}
