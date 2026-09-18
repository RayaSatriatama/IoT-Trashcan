import { useRef, useState } from 'react';
import { Card, Separator, Text, XStack, YStack } from 'tamagui';

import type { ResearchResponse, ResearchSlotDTO } from '../lib/api/types';
import { formatNullableMetric } from '../lib/format';
import { textStyles } from '../lib/ui/typography';
import { AppButton } from './AppButton';
import { EmptyState } from './EmptyState';
import { ErrorState } from './ErrorState';
import { LoadingState } from './LoadingState';

export const RESEARCH_PANEL_TITLE = 'Detail Riset';
export const RESEARCH_UNAVAILABLE_COPY = 'Belum tersedia';

export interface ResearchPanelProps {
  data?: ResearchResponse | null;
  loading?: boolean;
  error?: Error | null;
  onRetry?: () => void;
  onOpen?: () => void;
}

export type ResearchPanelState = 'loading' | 'error' | 'empty' | 'data';

export function getResearchPanelState({
  data,
  loading = false,
  error = null,
}: Pick<ResearchPanelProps, 'data' | 'loading' | 'error'>): ResearchPanelState {
  if (loading) return 'loading';
  if (error) return 'error';
  if (!data || data.slots.length === 0) return 'empty';
  return 'data';
}

export function formatResearchValue(
  value: number | string | null | undefined,
  unit?: string,
): string {
  if (value === null || value === undefined || value === '') return RESEARCH_UNAVAILABLE_COPY;
  if (typeof value === 'number' && !Number.isFinite(value)) return RESEARCH_UNAVAILABLE_COPY;
  if (typeof value === 'number') return formatNullableMetric(value, unit ? ` ${unit}` : '');
  return unit ? `${value} ${unit}` : String(value);
}

export function formatSensorHealth(value: unknown): string {
  if (value === null || value === undefined || value === '') return RESEARCH_UNAVAILABLE_COPY;
  if (typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean') {
    return String(value);
  }

  try {
    const serialized = JSON.stringify(value);
    return serialized && serialized !== '{}' ? serialized : RESEARCH_UNAVAILABLE_COPY;
  } catch {
    return RESEARCH_UNAVAILABLE_COPY;
  }
}

function MetricRow({
  label,
  value,
  unit,
}: {
  label: string;
  value: number | string | null | undefined;
  unit?: string;
}) {
  return (
    <XStack justifyContent="space-between" alignItems="flex-start" gap="$3" paddingVertical="$2">
      <Text color="$colorMuted" {...textStyles.bodySecondary} flex={1}>
        {label}
      </Text>
      <Text color="$color" {...textStyles.bodySecondary} fontWeight="600" textAlign="right" flex={1}>
        {formatResearchValue(value, unit)}
      </Text>
    </XStack>
  );
}

function Section({
  title,
  children,
}: {
  title: string;
  children: React.ReactNode;
}) {
  return (
    <YStack gap="$1">
      <Text color="$color" {...textStyles.metricValue}>
        {title}
      </Text>
      <Card
        borderWidth={1}
        borderColor="$borderColor"
        backgroundColor="$background"
        borderRadius="$2"
        paddingHorizontal="$3"
        paddingVertical="$1"
      >
        {children}
      </Card>
    </YStack>
  );
}

function ResearchSlot({ slot }: { slot: ResearchSlotDTO }) {
  return (
    <Card
      borderWidth={1}
      borderColor="$borderColor"
      backgroundColor="$surface"
      borderRadius="$3"
      padding="$4"
      gap="$3"
    >
      <XStack justifyContent="space-between" alignItems="flex-start" gap="$3" flexWrap="wrap">
        <YStack gap="$1" flex={1} minWidth={160}>
          <Text color="$color" {...textStyles.cardTitle} fontSize={18}>
            {slot.stream}
          </Text>
          <Text color="$colorMuted" {...textStyles.caption}>
            Terekam: {formatResearchValue(slot.recorded_at)}
          </Text>
        </YStack>
          <Text color="$colorMuted" {...textStyles.bodySecondary} accessibilityLabel={`Stream ${slot.stream}`}>
          Audit sensor
        </Text>
      </XStack>

      <Section title="Raw">
        <MetricRow label="fill" value={slot.raw.fill} unit="%" />
        <Separator borderColor="$borderColor" />
        <MetricRow label="weight" value={slot.raw.weight} unit="kg" />
        <Separator borderColor="$borderColor" />
        <MetricRow label="gas_raw" value={slot.raw.gas_raw} />
        <Separator borderColor="$borderColor" />
        <MetricRow label="temperature" value={slot.raw.temperature} unit="°C" />
      </Section>

      <Section title="Normalized">
        <MetricRow label="fill_pct" value={slot.normalized.fill_pct} unit="%" />
        <Separator borderColor="$borderColor" />
        <MetricRow label="weight_utilization" value={slot.normalized.weight_utilization} unit="%" />
        <Separator borderColor="$borderColor" />
        <MetricRow label="ogi" value={slot.normalized.ogi} />
        <Separator borderColor="$borderColor" />
        <MetricRow label="accumulation_hours" value={slot.normalized.accumulation_hours} unit="jam" />
        <Separator borderColor="$borderColor" />
        <MetricRow label="temperature" value={slot.normalized.temperature} unit="°C" />
      </Section>

      <Section title="Decision">
        <MetricRow label="fis_score" value={slot.fis_score} />
        <Separator borderColor="$borderColor" />
        <MetricRow label="fis_status" value={slot.fis_status} />
        <Separator borderColor="$borderColor" />
        <MetricRow label="fis_version" value={slot.fis_version} />
        <Separator borderColor="$borderColor" />
        <MetricRow label="dominant_reason" value={slot.dominant_reason} />
        <Separator borderColor="$borderColor" />
        <MetricRow label="safety_state" value={slot.safety_state} />
      </Section>

      <Section title="Sensor health">
        <XStack justifyContent="space-between" alignItems="flex-start" gap="$3" paddingVertical="$2">
          <Text color="$colorMuted" {...textStyles.bodySecondary} flex={1}>
            sensor_health
          </Text>
          <Text color="$color" {...textStyles.bodySecondary} fontWeight="600" textAlign="right" flex={1}>
            {formatSensorHealth(slot.sensor_health)}
          </Text>
        </XStack>
      </Section>
    </Card>
  );
}

export function ResearchPanel({
  data = null,
  loading = false,
  error = null,
  onRetry,
  onOpen,
}: ResearchPanelProps) {
  const [expanded, setExpanded] = useState(false);
  const hasOpened = useRef(false);

  const toggle = () => {
    const nextExpanded = !expanded;
    setExpanded(nextExpanded);
    if (nextExpanded && !hasOpened.current) {
      hasOpened.current = true;
      onOpen?.();
    }
  };

  return (
    <Card
      borderWidth={1}
      borderColor="$borderColor"
      backgroundColor="$surface"
      borderRadius="$3"
      padding="$4"
      gap="$4"
    >
      <XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap">
        <YStack gap="$1" flex={1} minWidth={180}>
          <Text color="$color" {...textStyles.cardTitle} fontSize={22}>
            {RESEARCH_PANEL_TITLE}
          </Text>
          <Text color="$colorMuted" {...textStyles.bodySecondary}>
            Raw, normalized, and decision fields per slot.
          </Text>
        </YStack>
        <AppButton
          variant="secondary"
          size="md"
          onPress={toggle}
          ariaLabel={expanded ? 'Tutup detail riset' : 'Buka detail riset'}
          ariaExpanded={expanded}
        >
          {expanded ? 'Tutup' : 'Buka'}
        </AppButton>
      </XStack>

      {expanded ? (
        <YStack gap="$3">
          {getResearchPanelState({ data, loading, error }) === 'loading' ? (
            <LoadingState label="Memuat detail riset" rows={2} />
          ) : null}
          {getResearchPanelState({ data, loading, error }) === 'error' ? (
            <ErrorState
              title="Detail riset tidak dapat dimuat"
              message="Periksa koneksi API lalu coba lagi."
              onRetry={onRetry}
            />
          ) : null}
          {getResearchPanelState({ data, loading, error }) === 'empty' ? (
            <EmptyState
              title="Data riset belum tersedia"
              description="Pembacaan audit per slot akan muncul setelah tersedia dari API."
            />
          ) : null}
          {data?.slots.map((slot) => <ResearchSlot key={slot.stream} slot={slot} />)}
        </YStack>
      ) : null}
    </Card>
  );
}
