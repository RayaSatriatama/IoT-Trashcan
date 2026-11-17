import { useEffect, useRef, useState } from 'react';
import { Card, Text, XStack, YStack } from 'tamagui';
import type { FirmwareMonitoringSample } from '../../lib/api/types';
import { firmwareSensorLabels } from '../../lib/ui/firmwarePresentation';
import { arrivedSensorIds, describeLiveSample, RECENT_SAMPLE_MS } from '../../lib/ui/liveSensorState';
import { MQ135_BASELINE_PIN_MV, estimateNh3FromPinMv, formatNh3Estimate } from '../../lib/ui/mq135Nh3Estimate';
import { useReducedMotion } from '../../lib/ui/motionContext';
import { textStyles } from '../../lib/ui/typography';
import { SensorStateBadge } from '../SensorStateBadge';
import { DescriptionHint } from '../SectionHeader';

/** Receives UI-aged copies from the shared local clock, not invented captures. */
export function LiveSensorDashboard({ monitoring, connectionError = false }: {
  monitoring?: readonly FirmwareMonitoringSample[]; connectionError?: boolean;
}) {
  const reduceMotion = useReducedMotion();
  const previousRef = useRef<ReadonlyMap<string, FirmwareMonitoringSample>>(new Map());
  const [arrived, setArrived] = useState<readonly string[]>([]);
  const arrivalSignature = JSON.stringify((monitoring ?? []).map(sample => [sample.sensor_id, sample.boot_id, sample.last_sequence, sample.quality, sample.fresh, sample.future_clock_skew, typeof sample.age_ms === 'number' && Number.isFinite(sample.age_ms) && sample.age_ms >= 0 && sample.age_ms <= RECENT_SAMPLE_MS]));
  useEffect(() => {
    const next = connectionError ? [] : arrivedSensorIds(previousRef.current, monitoring ?? []);
    previousRef.current = new Map((monitoring ?? []).map(sample => [sample.sensor_id, sample]));
    setArrived(current => current.length === next.length && current.every((id, index) => id === next[index]) ? current : next);
    if (!next.length) return;
    const timer = setTimeout(() => setArrived([]), 1200);
    return () => clearTimeout(timer);
  }, [arrivalSignature, connectionError]);
  if (!monitoring?.length) return <Text color="$colorMuted" role="status" {...textStyles.bodySecondary}>Monitoring sensor live belum tersedia pada API.</Text>;
  return <YStack role="region" aria-label="Dashboard sensor live" gap="$3" minWidth={0}>
    <YStack gap="$1">
      <Text {...textStyles.cardTitle} color="$color">Sensor live</Text>
      <Text {...textStyles.caption} color="$colorMuted">Polling otomatis. Batas terbaru UI: 30 detik; bukan kebijakan freshness server.</Text>
      <Text {...textStyles.caption} color="$colorMuted">Target snapshot bersama: 1 detik. Penerimaan bergantung pada koneksi dan respons HTTPS. Umur tetap bertambah saat terputus; nilai lama tidak dianggap capture baru.</Text>
      {connectionError ? <Text role="alert" color="$statusWaspada">Koneksi API bermasalah. Menampilkan pembacaan terakhir, bukan capture baru.</Text> : null}
    </YStack>
    <XStack gap="$3" flexWrap="wrap">
      {monitoring.map(sample => {
        const view = describeLiveSample(sample);
        const isArrival = arrived.includes(sample.sensor_id);
        const moving = isArrival && !reduceMotion;
        const label = firmwareSensorLabels[sample.sensor_id] ?? 'Sensor';
        const state = view.state === 'VALID' && (view.recencyLabel === 'Tertunda (>30 detik)' || sample.fresh === false) ? 'STALE' : view.state;
        const nh3 = sample.sensor_id === 'mq135' ? estimateNh3FromPinMv(sample.converted_value?.adc_pin_mv) : null;
        return <Card key={sample.sensor_id} testID={`live-sensor-${sample.sensor_id}`}
          data-arrival={moving ? 'true' : 'false'} data-sample-age-ms={sample.age_ms ?? 'unknown'} flex={1} minWidth={220}
          borderWidth={1} borderColor={moving ? '$actionPrimary' : '$borderColor'}
          backgroundColor={moving ? '$primarySoft' : '$surface'} borderRadius="$3" padding="$3" gap="$2"
          transition={reduceMotion ? undefined : 'data'} role="group" aria-label={`Sensor ${label}`}>
          <XStack alignItems="center" justifyContent="space-between" gap="$2" flexWrap="wrap">
            <Text {...textStyles.bodySecondary} color="$color" fontWeight="700">{label}</Text>
            {sample.sensor_id === 'mq135' && state === 'VALID'
              ? <XStack backgroundColor="$statusWaspadaSoft" paddingHorizontal="$2" paddingVertical="$1" borderRadius="$6" role="status"><Text {...textStyles.caption} color="$statusWaspada" fontWeight="700">Belum terkalibrasi</Text></XStack>
              : <SensorStateBadge state={state} />}
          </XStack>
          {isArrival ? <Text {...textStyles.caption} color="$colorMuted" aria-live="polite">Sampel baru diterima</Text> : null}
          <YStack gap="$1">{view.values.map(value => <XStack key={value.label} justifyContent="space-between" gap="$2">
            <Text {...textStyles.caption} color="$colorMuted">{value.label}</Text>
            <Text {...textStyles.bodySecondary} fontWeight="700" color="$color">{value.text}</Text>
          </XStack>)}{nh3 ? <XStack justifyContent="space-between" gap="$2" role="status" aria-label={`Estimasi NH3: ${formatNh3Estimate(nh3)}`}>
            <Text {...textStyles.caption} color="$colorMuted">NH3 (estimasi)</Text>
            <Text {...textStyles.bodySecondary} fontWeight="700" color="$color">{formatNh3Estimate(nh3)}</Text>
          </XStack> : null}</YStack>
          {sample.sensor_id === 'mq135' ? <XStack alignItems="center" justifyContent="space-between" gap="$1">
            <Text {...textStyles.caption} color="$colorMuted" flex={1}>NH3 dihitung di web dari tegangan pin terkalibrasi (pembagi 1:1 sesuai profil ADC) memakai kurva Winsen dan baseline {MQ135_BASELINE_PIN_MV} mV (asumsi operator). Di luar rentang 10–1000 ppm nilainya ekstrapolasi, bukan kalibrasi gas.</Text>
            <DescriptionHint description="Kualitas sampel ADC tidak membuktikan konsentrasi gas. PPM membutuhkan model untuk gas tertentu, profil listrik dan referensi yang tervalidasi. Rs/R0 tersedia pada panel kalibrasi ketika bukti perangkat valid; tidak dihitung dari konstanta asumsi di web." ariaLabel="Info kalibrasi gas MQ-135" />
          </XStack> : null}
          <YStack gap="$1">
            <Text {...textStyles.caption} color="$colorMuted">{view.freshnessLabel}</Text>
            <Text {...textStyles.caption} color="$colorMuted">{view.ageLabel}</Text>
            {view.recencyLabel ? <Text {...textStyles.caption} color={state === 'STALE' ? '$statusWaspada' : '$colorMuted'}>{view.recencyLabel}</Text> : null}
            <Text {...textStyles.caption} color="$colorMuted">{view.originLabel}</Text>
            {view.orderingNote ? <Text {...textStyles.caption} color="$colorMuted">{view.orderingNote}</Text> : null}
            {view.reasonLabel ? <Text {...textStyles.caption} color="$colorMuted">{view.reasonLabel}</Text> : null}
          </YStack>
        </Card>;
      })}
    </XStack>
  </YStack>;
}
