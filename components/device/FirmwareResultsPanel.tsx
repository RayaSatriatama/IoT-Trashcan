import type { FirmwareResult, FirmwareResultTrace } from '../../lib/api/types';
import { Text, YStack } from 'tamagui';
import { textStyles } from '../../lib/ui/typography';

const metrics = [['fill_pct', 'Keterisian', '%'], ['weight_kg', 'Berat', ' kg'], ['weight_utilization_pct', 'Utilisasi berat', '%'], ['ogi', 'OGI', ''], ['accumulation_hours', 'Waktu akumulasi', ' jam'], ['temperature_c', 'Suhu', ' °C']] as const;
const modelLabel = (model: string) => model === 'TEST_ONLY' ? 'TEST_ONLY (bukan produksi)' : model;
function traceLines(title: string, trace: FirmwareResultTrace | null): string[] {
  if (!trace) return [`${title}: trace tidak tersedia`];
  const lines = [`${title}: model ${trace.model_ref.version} · ${trace.model_ref.hash}`];
  for (const membership of trace.memberships) lines.push(`Keanggotaan ${membership.input} · ${membership.set}: ${membership.value}`);
  for (const rule of trace.rules) lines.push(`Aturan ${rule.id}: kekuatan ${rule.strength} · ${rule.output_id}`);
  if (trace.aggregate === null) lines.push('Agregat: tidak tersedia');
  else lines.push(`Agregat ${trace.aggregate.output_id}: area ${trace.aggregate.area ?? 'tidak tersedia'} · centroid ${trace.aggregate.centroid ?? 'tidak tersedia'}`);
  return lines;
}
export function FirmwareResultsPanel({ results, compact = false }: { results?: readonly FirmwareResult[]; compact?: boolean }) {
  if (!results || results.length === 0) return <Text color="$colorMuted" {...textStyles.bodySecondary}>{results ? 'Belum ada hasil pemrosesan firmware.' : 'Hasil firmware belum tersedia pada API.'}</Text>;
  return <YStack role="region" aria-label="Hasil firmware MCU" gap="$2"><Text color="$color" {...textStyles.cardTitle}>Hasil firmware MCU</Text>
    {results.map(result => <YStack key={`${result.stream}:${result.boot_id}:${result.sequence}`} gap="$1" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$2" minWidth={0}>
      <Text color="$color" {...textStyles.bodySecondary} fontWeight="700">{result.stream} · boot {result.boot_id} · sequence {result.sequence}</Text>
      <Text color="$colorMuted" {...textStyles.metadata}>Asal hasil: {result.result_origin === 'PHYSICAL' ? 'Fisik (metadata firmware)' : result.result_origin === 'SIMULATED' ? 'Simulasi' : 'Tidak diketahui'}{result.origin_firmware_version ? ` · firmware ${result.origin_firmware_version}` : ''}</Text>
      {metrics.map(([key, label, unit]) => <Text key={key} color="$colorMuted" {...textStyles.bodySecondary}>{label}: {result.processed[key] === null ? 'Tidak tersedia' : `${result.processed[key]}${unit}`}</Text>)}
      <Text color="$color" {...textStyles.bodySecondary}>FIS: {result.fuzzy.state}{result.fuzzy.score === null ? '' : ` · skor ${result.fuzzy.score}`}{result.fuzzy.label ? ` · ${result.fuzzy.label}` : ''}</Text>
      <Text color="$colorMuted" {...textStyles.metadata}>{result.fuzzy.reason} · model {modelLabel(result.fuzzy.model_state)}</Text>
      <Text color="$color" {...textStyles.bodySecondary}>Safety: {result.safety.state} · {result.safety.reason}</Text>
      <Text color="$colorMuted" {...textStyles.metadata}>Model safety: {modelLabel(result.safety.model_state)}</Text>
      {!compact ? <><Text color="$colorMuted" {...textStyles.metadata}>Sumber: {result.source_sample_ids.join(', ') || 'tidak tersedia'}</Text>
        <Text color="$colorMuted" {...textStyles.metadata}>Waktu: {result.recorded_at ?? 'tidak diketahui'} · {result.time_quality}{result.time_quality === 'UNSYNCED' ? ' · waktu tidak tersinkron' : ''}{result.age_ms === null ? ' · umur tidak diketahui' : ` · umur ${result.age_ms} ms`}{result.fresh === null ? ' · kesegaran tidak diketahui' : result.fresh ? ' · segar' : ' · tidak segar'}{result.future_clock_skew ? ' · jam firmware di masa depan' : ''}{result.ordering_unknown ? ' · urutan/cross-boot tidak diketahui' : ''}</Text>
        {[...traceLines('Trace FIS', result.fuzzy.trace), ...traceLines('Trace safety', result.safety.trace)].map((line, index) => <Text key={index} color="$colorMuted" {...textStyles.metadata}>{line}</Text>)}</> : <Text color="$colorMuted" {...textStyles.metadata}>Waktu {result.time_quality === 'UNSYNCED' ? 'tidak tersinkron' : result.age_ms === null ? 'tidak diketahui' : `${result.age_ms} ms`}{result.future_clock_skew ? ' · jam di masa depan' : ''}{result.ordering_unknown ? ' · urutan/cross-boot tidak diketahui' : ''}</Text>}
    </YStack>)}
  </YStack>;
}
