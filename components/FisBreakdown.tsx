import { useState } from 'react';
import { Button, Text, XStack, YStack } from 'tamagui';

import type { TransportPriority, TransportRuleTrace } from '../lib/ui/transportFuzzy';
import { TRANSPORT_INPUTS, TRANSPORT_OUTPUT_SETS } from '../lib/ui/transportFuzzy';
import { textStyles } from '../lib/ui/typography';

/**
 * Read-only breakdown of one fuzzy evaluation.
 *
 * Shows the numbers the engine actually used: the crisp input, every membership
 * degree of that input, the fired rules with their AND strength, the aggregated
 * level per output set, and the centroid formula that produced the score.
 * Nothing is recomputed here; the trace comes from the evaluator.
 */

const number = (value: number, digits = 3): string => {
  if (!Number.isFinite(value)) return '—';
  const text = value.toFixed(digits).replace(/0+$/, '').replace(/\.$/, '');
  return text === '' || text === '-' ? '0' : text.replace('.', ',');
};

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <YStack gap="$1">
      <Text {...textStyles.caption} color="$color" fontWeight="700">{title}</Text>
      {children}
    </YStack>
  );
}

function Rule({ rule, index }: { rule: TransportRuleTrace; index: number }) {
  const antecedent = rule.antecedents.map(item => `μ${item.set}(${item.label}) = ${number(item.degree)}`).join(', ');
  return (
    <Text {...textStyles.caption} color="$colorMuted" fontVariant={['tabular-nums']}>
      R{index + 1}: min({antecedent}) = {number(rule.strength)} → {rule.output}
    </Text>
  );
}

export function FisBreakdown({ trace, defaultOpen = false }: { trace: TransportPriority; defaultOpen?: boolean }) {
  const [open, setOpen] = useState(defaultOpen);
  const aggregate = trace.aggregate;
  const activeRules = trace.rules.filter(rule => rule.strength > 0);
  return (
    <YStack gap="$2" paddingTop="$2" borderTopWidth={1} borderTopColor="$borderColor">
      <XStack alignItems="center" justifyContent="space-between" gap="$2" flexWrap="wrap">
        <YStack gap="$1" flex={1} minWidth={0}>
          <Text {...textStyles.bodySecondary} color="$color" fontWeight="700">Rumus perhitungan FIS</Text>
          <Text {...textStyles.caption} color="$colorMuted">
            Mamdani: min untuk AND, max untuk agregasi, centroid 101 titik.
          </Text>
        </YStack>
        <Button size="$2" chromeless height={22} paddingHorizontal="$2" onPress={() => setOpen(value => !value)}
          aria-label={open ? 'Sembunyikan rumus FIS' : 'Tampilkan rumus FIS'}>
          <Text {...textStyles.caption} color="$actionPrimary" fontWeight="700">{open ? 'Sembunyikan' : 'Tampilkan'}</Text>
        </Button>
      </XStack>
      {open ? <YStack gap="$3">
        <Section title="1. Fuzzifikasi — derajat keanggotaan tiap input">
          {trace.memberships.map(item => {
            const spec = TRANSPORT_INPUTS.find(entry => entry.id === item.input);
            const crisp = item.set === null ? 'tidak tersedia' : `μ maksimum ${number(item.degree)} pada set ${item.set}`;
            const degrees = item.memberships.map(entry => `μ${entry.set} = ${number(entry.degree)}`).join(' · ');
            return (
              <YStack key={item.input} gap="$1">
                <Text {...textStyles.caption} color="$color" fontVariant={['tabular-nums']}>
                  {item.label} ({spec?.unit ?? ''}): {crisp}
                </Text>
                <Text {...textStyles.caption} color="$colorMuted" fontVariant={['tabular-nums']}>
                  {degrees || '—'}
                </Text>
              </YStack>
            );
          })}
        </Section>

        <Section title="2. Inferensi — rule aktif dan kekuatannya">
          {activeRules.length ? activeRules.slice(0, 8).map((rule, index) => <Rule key={`${rule.output}-${index}`} rule={rule} index={index} />) : <Text {...textStyles.caption} color="$colorMuted">Tidak ada rule aktif.</Text>}
          {activeRules.length > 8 ? <Text {...textStyles.caption} color="$colorMuted">… dan {activeRules.length - 8} rule lain bernilai positif.</Text> : null}
          <Text {...textStyles.caption} color="$colorMuted">
            Severity = ceil((Σ indeks himpunan × 4 / jumlah input − 1) / 2), dipetakan ke {TRANSPORT_OUTPUT_SETS.map(set => set.id).join(' / ')}.
          </Text>
        </Section>

        <Section title="3. Agregasi Max — level tiap himpunan output">
          {aggregate ? <Text {...textStyles.caption} color="$color" fontVariant={['tabular-nums']}>
            {aggregate.levels.map(item => `${item.set} = ${number(item.level)}`).join(' · ')}
          </Text> : <Text {...textStyles.caption} color="$colorMuted">Tidak ada agregasi karena input belum lengkap.</Text>}
        </Section>

        <Section title="4. Defuzzifikasi — centroid">
          {aggregate ? <YStack gap="$1">
            <Text {...textStyles.caption} color="$color" fontVariant={['tabular-nums']}>
              Luas A = {number(aggregate.area, 2)} · momen M = {number(aggregate.moment, 2)}
            </Text>
            <Text {...textStyles.caption} color="$color" fontVariant={['tabular-nums']}>
              z* = M / A = {number(aggregate.moment, 2)} / {number(aggregate.area, 2)} = {number(aggregate.centroid, 2)}
            </Text>
            <Text {...textStyles.caption} color="$colorMuted">
              Integrasi trapesium {aggregate.grid} titik pada domain 0–100.
            </Text>
          </YStack> : null}
        </Section>

        <Section title="5. Hasil">
          <Text {...textStyles.bodySecondary} color="$color" fontWeight="700" fontVariant={['tabular-nums']}>
            {trace.state === 'READY' && trace.score !== null
              ? `z* = ${number(trace.score, 2)} / 100 → ${trace.label} · ${trace.decision}`
              : `Tidak dapat dihitung: input ${trace.missingInputs.join(', ') || '—'} belum tersedia.`}
          </Text>
          {trace.state !== 'READY' ? <Text {...textStyles.caption} color="$colorMuted">
            Input yang hilang tidak diganti nol; skor dibiarkan kosong.
          </Text> : null}
        </Section>
      </YStack> : null}
    </YStack>
  );
}
