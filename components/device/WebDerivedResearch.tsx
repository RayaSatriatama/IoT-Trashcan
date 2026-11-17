import { Card, Text, XStack, YStack } from 'tamagui';
import type { FirmwareMonitoringSample, SensorHistoryPoint } from '../../lib/api/types';
import { buildTestOnlyDiagnostics, TEST_ONLY_PROFILE, type DerivedMetric, type DiagnosticReason } from '../../lib/ui/testOnlyDiagnostics';
import { formatFirmwareMetric } from '../../lib/ui/firmwarePresentation';
import { formatResearchNumber } from '../../lib/ui/slotPresentation';
import { textStyles } from '../../lib/ui/typography';

const reasons: Record<DiagnosticReason, string> = {
  READY: 'Input memenuhi batas profil web', INPUT_MISSING: 'Input atau histori terkini belum lengkap',
  INPUT_NOT_READY: 'Kualitas input sensor belum siap', INPUT_INVALID: 'Input tidak valid atau snapshot histori tidak cocok',
  INPUT_STALE: 'Input terlambat atau umur capture belum dapat dipercaya', INPUT_TIME_UNTRUSTED: 'Waktu input belum tepercaya',
  INPUT_ORIGIN_UNVERIFIED: 'Asal input belum terverifikasi', INPUT_OUT_OF_DOMAIN: 'Input di luar rentang model prototipe',
  NO_EMPTY_OBSERVATION: 'Awal akumulasi belum teramati', CLEAR_CYCLES_NOT_MET: 'Observasi suhu belum cukup',
  NO_ALERT_OBSERVED: 'Tidak ada alert teramati; bukan pernyataan aman', TEMPERATURE_ALERT: 'Suhu mencapai ambang perhatian prototipe',
  TEMPERATURE_HYSTERESIS_BAND: 'Suhu berada pada pita perhatian prototipe',
};
const inputLabels: Record<string, string> = { fill_pct: 'Keterisian', weight_utilization_pct: 'Utilisasi berat', ogi: 'OGI relatif', accumulation_hours: 'Akumulasi teramati' };
const displayMetric = (value: number | null | undefined, unit = '') =>
  typeof value === 'number' && Number.isFinite(value)
    ? formatResearchNumber(value, unit ? ` ${unit}` : '') : formatFirmwareMetric(value, unit);
function Metric({ label, metric }: { label: string; metric: DerivedMetric }) {
  return <YStack flex={1} minWidth={180} gap="$1" padding="$3" borderWidth={1} borderColor="$borderColor" borderRadius="$3">
    <Text {...textStyles.caption} color="$colorMuted">{label}</Text>
    <Text {...textStyles.cardTitle} color="$color">{displayMetric(metric.state === 'READY' ? metric.value : null, metric.unit)}</Text>
    <Text {...textStyles.caption} color="$colorMuted">{reasons[metric.reason]}</Text>
    {metric.notes.map(note => <Text key={note} {...textStyles.caption} color="$colorMuted">{note}</Text>)}
  </YStack>;
}
export function WebDerivedResearch({ deviceId, monitoring, history }: {
  deviceId: string; monitoring: readonly FirmwareMonitoringSample[];
  history: Readonly<Record<string, readonly SensorHistoryPoint[]>>;
}) {
  if (!deviceId) return null;
  const result = buildTestOnlyDiagnostics({ monitoring, history });
  const { fuzzy, safety } = result;
  const originUnknown = monitoring.some(sample => sample.sample_origin !== 'PHYSICAL');
  return <Card role="region" aria-label="Analisis web TEST_ONLY" testID="web-derived-research" borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$3" padding="$3" gap="$3" minWidth={0}>
    <XStack justifyContent="space-between" alignItems="center" flexWrap="wrap" gap="$2">
      <Text {...textStyles.cardTitle} color="$color">Analisis web</Text>
      <Text {...textStyles.caption} color="$statusWaspada">TEST_ONLY · Prototipe organik v{result.profile.version}</Text>
    </XStack>
    <Text {...textStyles.bodySecondary} color="$colorMuted">Perhitungan web, bukan hasil MCU, keputusan safety produksi, atau perintah aktuator.</Text>
    <Text {...textStyles.caption} color="$colorMuted">Asumsi web: kapasitas {TEST_ONLY_PROFILE.bin.capacityKg} kg; jarak kosong {TEST_ONLY_PROFILE.bin.emptyDistanceMm} mm; penuh {TEST_ONLY_PROFILE.bin.fullDistanceMm} mm. Bukan profil NVS perangkat atau hasil kalibrasi fisik terverifikasi.</Text>
    <Text {...textStyles.caption} color="$colorMuted">Input terkini harus berumur paling lama 30 detik. Histori harus sesuai boot dan capture snapshot; data terlambat tidak menjadi hasil baru.</Text>
    {originUnknown ? <Text {...textStyles.caption} color="$statusWaspada">Asal sebagian input belum terverifikasi. Provenance tetap tidak diketahui; tidak dinaikkan menjadi bukti fisik.</Text> : null}
    <XStack gap="$2" flexWrap="wrap">
      <Metric label="Keterisian web" metric={result.fillPercent} />
      <Metric label="Utilisasi berat web" metric={result.weightUtilization} />
      <Metric label="OGI relatif" metric={result.ogi} />
      <Metric label="Akumulasi teramati" metric={result.accumulationHours} />
    </XStack>
    <Text {...textStyles.caption} color="$colorMuted">OGI memakai rasio tegangan MQ-135 terhadap referensi 3300 mV, bukan ppm atau konsentrasi kimia. Akumulasi hanya mengukur jendela sejak sampel kosong teramati dalam boot yang sama; kontinuitas dan pengosongan di sela sampel tidak dapat dipastikan.</Text>
    <YStack gap="$1" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$3">
      <Text {...textStyles.bodySecondary} color="$color" fontWeight="700">Fuzzy web: {fuzzy.state === 'READY' ? `skor relatif ${displayMetric(fuzzy.score)}` : 'Belum tersedia'}</Text>
      <Text {...textStyles.caption} color="$colorMuted">{reasons[fuzzy.reason]}</Text>
      {fuzzy.missingInputs.length ? <Text {...textStyles.caption} color="$colorMuted">Input belum memenuhi syarat: {fuzzy.missingInputs.map(id => inputLabels[id] ?? 'Input model').join(', ')}</Text> : null}
      {fuzzy.state === 'READY' ? <>
        <Text {...textStyles.caption} color="$colorMuted">Mamdani min/max, centroid 101 titik. Aturan aktif: {fuzzy.rules.length}. Derajat keanggotaan bukan probabilitas atau akurasi.</Text>
        {fuzzy.memberships.map(item => <Text key={`${item.input}:${item.set}`} {...textStyles.caption} color="$colorMuted">{inputLabels[item.input]} · {item.set.includes('HIGH') ? 'Tinggi' : 'Rendah'}: {displayMetric(item.degree)}</Text>)}
        {fuzzy.rules.map(rule => <Text key={rule.id} {...textStyles.caption} color="$colorMuted">Aturan {rule.id}: kekuatan {displayMetric(rule.strength)}</Text>)}
      </> : null}
    </YStack>
    <YStack gap="$1" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$3">
      <Text {...textStyles.bodySecondary} color={safety.state === 'NEEDS_ATTENTION' ? '$statusWaspada' : '$color'} fontWeight="700">Diagnostik safety: {safety.state === 'NEEDS_ATTENTION' ? 'Perlu perhatian (TEST_ONLY)' : 'Belum cukup data'}</Text>
      <Text {...textStyles.caption} color="$colorMuted">{reasons[safety.reason]}. Ambang prototipe: perhatian ≥60 °C; pita histeresis 55 sampai 60 °C.</Text>
      <Text {...textStyles.caption} color="$colorMuted">Kanal B3 dan anorganik belum cukup. Tidak ada status aman produksi atau kendali perangkat dari panel ini.</Text>
    </YStack>
  </Card>;
}
