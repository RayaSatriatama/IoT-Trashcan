import { Text, XStack, YStack } from 'tamagui';
import type { FirmwareResult } from '../../lib/api/types';
import { firmwareStreamLabel, formatFirmwareMetric } from '../../lib/ui/firmwarePresentation';
import { formatTimestamp } from '../../lib/format';
import { textStyles } from '../../lib/ui/typography';

const fuzzyLabel = (state: FirmwareResult['fuzzy']['state']) => ({ READY: 'Siap', UNCONFIGURED: 'Belum dikonfigurasi', UNAVAILABLE: 'Belum tersedia' })[state];
const fuzzyInputs: Record<string, string> = { fill_pct: 'Keterisian', weight_utilization_pct: 'Utilisasi berat', ogi: 'OGI', accumulation_hours: 'Akumulasi' };
function FuzzyEvidence({ result }: { result: FirmwareResult }) {
  const trace = result.fuzzy.trace;
  if (!trace) return null;
  const memberships = trace.memberships.filter(item => fuzzyInputs[item.input]);
  const activeRules = trace.rules.filter(rule => Number.isFinite(rule.strength) && rule.strength > 0).length;
  return <YStack gap="$1"><Text {...textStyles.caption} color="$colorMuted">Aturan aktif: {activeRules}</Text>
    {memberships.map((item, index) => <Text key={`${fuzzyInputs[item.input]}:${index}`} {...textStyles.caption} color="$colorMuted">Keanggotaan {fuzzyInputs[item.input]}: {formatFirmwareMetric(item.value)}</Text>)}
  </YStack>;
}
const reasonLabel = (reason: string) => ({ MODEL_NOT_CONFIGURED: 'Model fuzzy belum dikonfigurasi', SAFETY_NOT_CONFIGURED: 'Model keselamatan belum dikonfigurasi', INVALID_PROFILE: 'Profil kalibrasi belum valid', INPUT_MISSING: 'Input sensor belum lengkap', INPUT_UNCALIBRATED: 'Input sensor belum dikalibrasi' } as Record<string, string>)[reason] ?? 'Keterangan belum tersedia';
const safetyLabel = (result: FirmwareResult) => {
  if (result.safety.state === 'CLEAR' && result.safety.model_state !== 'PRODUCTION') return 'Belum dinilai';
  return ({ CLEAR: 'Aman', ALERT: 'Peringatan keselamatan', FAULT: 'Gangguan', UNCONFIGURED: 'Belum dikonfigurasi', UNAVAILABLE: 'Belum tersedia' } as Record<string, string>)[result.safety.state] ?? 'Belum tersedia';
};

export function FirmwareResearch({ results }: { results: readonly FirmwareResult[] }) {
  return <YStack gap="$3" aria-label="Hasil MCU">
    {results.map(result => <YStack key={result.stream} gap="$2" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$3" minWidth={0}>
      <XStack alignItems="center" justifyContent="space-between" gap="$2" flexWrap="wrap">
        <Text {...textStyles.cardTitle} color="$color">{firmwareStreamLabel(result.stream)}</Text>
        <Text {...textStyles.caption} color="$colorMuted">{formatTimestamp(result.recorded_at)}</Text>
      </XStack>
      <Text {...textStyles.bodySecondary} color="$color">Status fuzzy: {fuzzyLabel(result.fuzzy.state)}{result.fuzzy.state === 'READY' && result.fuzzy.model_state === 'PRODUCTION' && result.fuzzy.score !== null ? ` · skor ${result.fuzzy.score}` : ''}</Text>
      <Text {...textStyles.caption} color="$colorMuted">{reasonLabel(result.fuzzy.reason)}</Text>
      <FuzzyEvidence result={result} />
      <XStack gap="$3" flexWrap="wrap">
        {([['fill_pct', 'Keterisian', '%'], ['weight_utilization_pct', 'Utilisasi berat', '%'], ['ogi', 'OGI', ''], ['accumulation_hours', 'Akumulasi', 'jam']] as const).map(([key, label, unit]) => <YStack key={key} flex={1} minWidth={125} gap="$1">
          <Text {...textStyles.caption} color="$colorMuted">{label}</Text>
          <Text {...textStyles.bodySecondary} color="$color">{formatFirmwareMetric(result.processed[key], unit)}</Text>
        </YStack>)}
      </XStack>
      <Text {...textStyles.bodySecondary} color="$color">Keselamatan: {safetyLabel(result)}</Text>
      <Text {...textStyles.caption} color="$colorMuted">{reasonLabel(result.safety.reason)}</Text>
    </YStack>)}
  </YStack>;
}
