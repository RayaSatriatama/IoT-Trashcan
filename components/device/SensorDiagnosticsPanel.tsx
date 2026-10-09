import { Card, Separator, Text, XStack, YStack } from 'tamagui';

import type { SensorDiagnostic } from '../../lib/api/calibration';
import { AppButton } from '../AppButton';

export type SensorDiagnosticsPanelProps = {
  diagnostic: SensorDiagnostic | null;
  loading?: boolean;
  error?: string | null;
  onRefresh?: () => void;
};

const unavailable = 'Belum tersedia';
function value(value: string | number | boolean | null | undefined): string {
  if (value === null || value === undefined || value === '') return unavailable;
  if (typeof value === 'boolean') return value ? 'Ya' : 'Tidak';
  return String(value);
}

export function SensorDiagnosticsPanel({ diagnostic, loading = false, error = null, onRefresh }: SensorDiagnosticsPanelProps) {
  return <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3" role="region" aria-label="Diagnostik sensor">
    <XStack justifyContent="space-between" alignItems="center" gap="$3"><YStack flex={1} gap="$1"><Text color="$color" fontSize="$5" fontWeight="800">Diagnostik sensor</Text><Text color="$colorMuted" fontSize="$2">Bukti fisik tetap dipisahkan dari nilai operasional.</Text></YStack>{onRefresh ? <AppButton variant="ghost" size="sm" onPress={onRefresh} disabled={loading} ariaLabel="Perbarui diagnostik sensor">Perbarui</AppButton> : null}</XStack>
    {loading ? <Text color="$colorMuted">Memuat diagnostik...</Text> : null}
    {error ? <Text role="alert" color="$statusSafetyAlert">{error}</Text> : null}
    {!loading && !error && !diagnostic ? <Text color="$colorMuted">Sensor belum terdeteksi.</Text> : null}
    {diagnostic ? <>
      <XStack gap="$2" flexWrap="wrap"><Text color="$color" fontWeight="700">{diagnostic.sensor_id}</Text><Text color="$colorMuted">Stream {diagnostic.stream}</Text><Text color="$colorMuted">Quality {diagnostic.quality}</Text><Text color="$colorMuted">Reason {diagnostic.reason}</Text></XStack>
      <Separator borderColor="$borderColor" />
      <XStack gap="$3" flexWrap="wrap"><YStack minWidth={130} flex={1}><Text color="$colorMuted" fontSize="$2">Bukti raw</Text><Text color="$color">{value(diagnostic.raw.present)}{diagnostic.raw.unit ? ` (${diagnostic.raw.unit})` : ''}</Text></YStack><YStack minWidth={130} flex={1}><Text color="$colorMuted" fontSize="$2">Nilai operasional</Text><Text color="$color">{value(diagnostic.converted_value)}{diagnostic.converted_unit ? ` ${diagnostic.converted_unit}` : ''}</Text></YStack><YStack minWidth={130} flex={1}><Text color="$colorMuted" fontSize="$2">Kesegaran</Text><Text color="$color">{diagnostic.fresh ? `Segar (${value(diagnostic.age_ms)} ms)` : 'Stale'}</Text></YStack></XStack>
      <XStack gap="$3" flexWrap="wrap"><YStack minWidth={130} flex={1}><Text color="$colorMuted" fontSize="$2">Identitas / CRC / selesai</Text><Text color="$color">{value(diagnostic.raw.identity_verified)} / {value(diagnostic.raw.crc_valid)} / {value(diagnostic.raw.conversion_complete)}</Text></YStack><YStack minWidth={130} flex={1}><Text color="$colorMuted" fontSize="$2">Profil</Text><Text color="$color">{diagnostic.profile_present ? `${value(diagnostic.profile_version)} (${value(diagnostic.profile_hash)})` : unavailable}</Text></YStack><YStack minWidth={130} flex={1}><Text color="$colorMuted" fontSize="$2">Kalibrasi</Text><Text color="$color">{diagnostic.calibration_state} · {diagnostic.accepted_samples} diterima / {diagnostic.rejected_samples} ditolak</Text></YStack></XStack>
    </> : null}
  </Card>;
}

