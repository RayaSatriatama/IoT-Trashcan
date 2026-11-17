import { Card, Separator, Text, XStack, YStack } from 'tamagui';

import type { SensorDiagnostic } from '../../lib/api/calibration';
import { AppButton } from '../AppButton';
import { DescriptionHint } from '../SectionHeader';
import { textStyles } from '../../lib/ui/typography';
import { Activity, RefreshCw } from '../../lib/ui/icons';

export type SensorDiagnosticsPanelProps = {
  diagnostic: SensorDiagnostic | null;
  source?: 'authenticated' | 'legacy' | 'ble';
  loading?: boolean;
  error?: string | null;
  onRefresh?: () => void;
};

const unavailable = 'Belum tersedia';
function value(value: unknown): string {
  if (value === null || value === undefined || value === '') return unavailable;
  if (typeof value === 'boolean') return value ? 'Ya' : 'Tidak';
  if (typeof value === 'object') return JSON.stringify(value);
  return String(value);
}

export function SensorDiagnosticsPanel({ diagnostic, source = 'authenticated', loading = false, error = null, onRefresh }: SensorDiagnosticsPanelProps) {
  return <Card flex={1} minWidth={0} borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius={15} padding={19} gap={15} $deviceToolsMobile={{ flexGrow: 0, flexShrink: 0, flexBasis: 'auto' }} $devicePhone={{ padding: 14 }} role="region" aria-label="Diagnostik sensor">
    <XStack justifyContent="space-between" alignItems="flex-start" gap={12}><Text color="$color" {...textStyles.cardTitle} fontSize={16} lineHeight={22.4}>Diagnostik</Text>{onRefresh ? <AppButton variant="secondary" size="sm" onPress={onRefresh} disabled={loading} ariaLabel="Perbarui diagnostik sensor"><RefreshCw size={16} color="#10275B" aria-hidden /> Perbarui</AppButton> : null}</XStack>
    {loading ? <Text color="$colorMuted">Memuat diagnostik...</Text> : null}
    {error ? <Text role="alert" color="$statusSafetyAlert">{error}</Text> : null}
    {!loading && !error && !diagnostic ? <XStack role="status" paddingVertical={13} paddingHorizontal={14} borderWidth={1} borderColor="$deviceStatusBorder" backgroundColor="$deviceStatusSurface" borderRadius={11} gap={11} alignItems="flex-start"><XStack width={38} height={38} flexShrink={0} borderRadius={12} backgroundColor="$deviceToolGreen" alignItems="center" justifyContent="center"><Activity size={19} color="#087B50" aria-hidden /></XStack><Text color="$color" {...textStyles.cardTitle} fontSize={13} lineHeight={18} flex={1}>{source === 'legacy' ? 'Diagnostik kalibrasi belum tersedia' : 'Sensor belum terdeteksi'}</Text></XStack> : null}
    {diagnostic ? <>
      {source !== 'authenticated' ? <Text color="$colorMuted" {...textStyles.caption}>{source === 'legacy' ? 'Monitoring' : 'BLE'} · informatif</Text> : null}
      <XStack gap="$2" flexWrap="wrap"><Text color="$color" fontWeight="700">{diagnostic.sensor_id}</Text><Text color="$colorMuted">Stream {diagnostic.stream}</Text><Text color="$colorMuted">Quality {diagnostic.quality}</Text><Text color="$colorMuted">Reason {diagnostic.reason}</Text></XStack>
      <Separator borderColor="$borderColor" />
      <XStack gap="$3" flexWrap="wrap"><YStack minWidth={130} flex={1}><Text color="$colorMuted" fontSize="$2">Bukti raw</Text><Text color="$color">{value(diagnostic.raw?.present)}{diagnostic.raw?.unit ? ` (${diagnostic.raw.unit})` : ''}</Text></YStack><YStack minWidth={130} flex={1}><Text color="$colorMuted" fontSize="$2">Nilai operasional</Text><Text color="$color">{value(diagnostic.converted_value)}{diagnostic.converted_unit ? ` ${diagnostic.converted_unit}` : ''}</Text></YStack><YStack minWidth={130} flex={1}><Text color="$colorMuted" fontSize="$2">Kesegaran</Text><Text color="$color">{diagnostic.fresh ? `Segar (${value(diagnostic.age_ms)} ms)` : 'Stale'}</Text></YStack></XStack>
      <XStack gap="$3" flexWrap="wrap"><YStack minWidth={130} flex={1}><Text color="$colorMuted" fontSize="$2">Identitas / CRC / selesai</Text><Text color="$color">{value(diagnostic.raw?.identity_verified)} / {value(diagnostic.raw?.crc_valid)} / {value(diagnostic.raw?.conversion_complete)}</Text></YStack><YStack minWidth={130} flex={1}><Text color="$colorMuted" fontSize="$2">Profil</Text><Text color="$color">{diagnostic.profile_present ? `${value(diagnostic.profile_version)} (${value(diagnostic.profile_hash)})` : unavailable}</Text></YStack><YStack minWidth={130} flex={1}><Text color="$colorMuted" fontSize="$2">Kalibrasi</Text><Text color="$color">{diagnostic.calibration_state} · {diagnostic.accepted_samples} diterima / {diagnostic.rejected_samples} ditolak</Text></YStack></XStack>
    </> : null}
  </Card>;
}
