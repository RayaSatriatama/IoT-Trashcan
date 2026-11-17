import { useState } from 'react';
import { Card, Separator, Text, XStack, YStack, useMedia } from 'tamagui';

import type { FirmwareMonitoringSample, FirmwareResult, ResearchResponse, ResearchSlotDTO } from '../lib/api/types';
import { FirmwareResearch } from './device/FirmwareResearch';
import { SensorHistoryCharts, type SensorHistorySnapshot } from './device/SensorHistoryCharts';
import { useLiveMonitoring } from '../lib/ui/useLiveMonitoring';
import { formatNullableMetric } from '../lib/format';
import { buildDerivedResearchRows, buildResearchSensorRows, formatResearchNumber, formatResearchTimestamp, getSensorParameterLabel, getStreamLabel } from '../lib/ui/slotPresentation';
import { textStyles } from '../lib/ui/typography';
import { useReducedMotion } from '../lib/ui/motionContext';
import { AnimatePresence } from 'tamagui';
import { AppButton } from './AppButton';
import { EmptyState } from './EmptyState';
import { ErrorState } from './ErrorState';
import { FuzzyTrace } from './FuzzyTrace';
import { LoadingState } from './LoadingState';
import { SensorStateBadge } from './SensorStateBadge';

export const RESEARCH_PANEL_TITLE = 'Detail Riset';
export const RESEARCH_UNAVAILABLE_COPY = 'Belum tersedia';

export interface ResearchPanelProps {
  firmwareResults?: readonly FirmwareResult[];
  deviceId?: string;
  monitoring?: FirmwareMonitoringSample[];
  monitoringError?: boolean;
  data?: ResearchResponse | null;
  loading?: boolean;
  error?: Error | null;
  onRetry?: () => void;
  onOpen?: () => void;
}

export type ResearchPanelState = 'loading' | 'error' | 'empty' | 'data';

export function getResearchPanelState({ data, loading = false, error = null }: Pick<ResearchPanelProps, 'data' | 'loading' | 'error'>): ResearchPanelState {
  if (loading) return 'loading';
  if (error) return 'error';
  if (!data || data.slots.length === 0) return 'empty';
  return 'data';
}

export function formatResearchValue(value: number | string | null | undefined, unit?: string): string {
  if (value === null || value === undefined || value === '') return RESEARCH_UNAVAILABLE_COPY;
  if (typeof value === 'number' && !Number.isFinite(value)) return RESEARCH_UNAVAILABLE_COPY;
  if (typeof value === 'number') return formatNullableMetric(value, unit ? ` ${unit}` : '');
  return unit ? `${value} ${unit}` : String(value);
}

export function formatSensorHealth(value: unknown): string {
  if (value === null || value === undefined || value === '') return RESEARCH_UNAVAILABLE_COPY;
  if (typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean') return String(value);
  try { const serialized = JSON.stringify(value); return serialized && serialized !== '{}' ? serialized : RESEARCH_UNAVAILABLE_COPY; } catch { return RESEARCH_UNAVAILABLE_COPY; }
}

function DataCell({ children, width, muted = false }: { children: React.ReactNode; width?: string; muted?: boolean }) {
  return <Text {...textStyles.caption} color={muted ? '$colorMuted' : '$color'} width={width} flex={width ? undefined : 1} minWidth={0}>{children}</Text>;
}

function SensorRow({ row, mobile }: { row: ReturnType<typeof buildResearchSensorRows>[number]; mobile: boolean }) {
  if (mobile) return <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$3" padding="$3" gap="$2"><XStack justifyContent="space-between" alignItems="flex-start" gap="$2"><YStack flex={1} minWidth={0}><Text {...textStyles.bodySecondary} color="$color" fontWeight="700" numberOfLines={1}>{row.label}</Text><Text {...textStyles.caption} color="$colorMuted" numberOfLines={1}>{row.parameter}</Text></YStack><SensorStateBadge state={row.state} /></XStack><XStack gap="$2" flexWrap="wrap"><DataCell width="50%" muted>Aktual {row.actual}</DataCell><DataCell width="50%" muted>{row.normalizedLabel} {row.normalized}</DataCell><DataCell width="100%" muted>Fuzzy {row.membership} · {row.role}</DataCell></XStack></Card>;
  return <YStack gap="$1" paddingVertical="$2" borderBottomWidth={1} borderBottomColor="$borderColor"><XStack alignItems="center" gap="$2"><YStack flex={1} minWidth={0}><Text {...textStyles.bodySecondary} color="$color" fontWeight="700" numberOfLines={1}>{row.label}</Text><Text {...textStyles.caption} color="$colorMuted" numberOfLines={1}>{row.parameter}</Text></YStack><SensorStateBadge state={row.state} /></XStack><XStack gap="$2" flexWrap="wrap"><DataCell width="48%" muted>Aktual {row.actual}</DataCell><DataCell width="48%" muted>{row.normalizedLabel} {row.normalized}</DataCell><DataCell width="100%" muted>Fuzzy {row.membership} · {row.role}</DataCell></XStack></YStack>;
}

function SafetyEvidence({ slot }: { slot: ResearchSlotDTO }) {
  if (slot.stream !== 'b3' || !slot.safety) return null;
  const alert = slot.safety.state === 'Safety Alert';
  return <YStack gap="$2" padding="$3" borderRadius="$3" backgroundColor={alert ? '$statusSafetyAlertSoft' : '$statusNormalSoft'} borderWidth={1} borderColor={alert ? '$statusSafetyAlert' : '$statusNormalSoft'}><Text {...textStyles.bodySecondary} color={alert ? '$statusSafetyAlert' : '$statusNormal'} fontWeight="700">Safety monitoring: {slot.safety.state === 'Clear' ? 'Aman' : slot.safety.state || 'Belum tersedia'}</Text>{slot.safety.reason ? <Text {...textStyles.caption} color="$colorMuted">{slot.safety.reason}</Text> : null}<XStack gap="$3" flexWrap="wrap">{slot.safety.channels.map((channel) => { const hasEngineeringValue = channel.value !== null && channel.value !== undefined; return <YStack key={channel.sensor_id} minWidth={100} gap="$1"><Text {...textStyles.caption} color="$colorMuted">{getSensorParameterLabel(channel.parameter || channel.sensor_type)}</Text><Text {...textStyles.bodySecondary} color="$color" fontWeight="700">{hasEngineeringValue ? formatResearchNumber(channel.value, channel.unit ? ` ${channel.unit}` : '') : RESEARCH_UNAVAILABLE_COPY}</Text><SensorStateBadge state={channel.sensor_state} /></YStack>; })}</XStack></YStack>;
}

function ResearchSlot({ slot }: { slot: ResearchSlotDTO }) {
  const media = useMedia();
  const mobile = !media.tablet;
  const sensorRows = buildResearchSensorRows(slot);
  const derivedRows = buildDerivedResearchRows(slot);
  return <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$3" padding="$3" gap="$3" flex={1} minWidth={mobile ? '100%' : 300}><XStack alignItems="center" justifyContent="space-between" gap="$2"><Text {...textStyles.cardTitle} color="$color">{getStreamLabel(slot.stream)}</Text><Text {...textStyles.caption} color="$colorMuted">{formatResearchTimestamp(slot.recorded_at)}</Text></XStack><YStack gap="$2"><Text {...textStyles.caption} color="$colorMuted">Sensor fisik</Text><YStack gap="$1">{sensorRows.map((row) => <SensorRow key={row.id} row={row} mobile={mobile} />)}</YStack></YStack><Separator borderColor="$borderColor" /><YStack gap="$2"><Text {...textStyles.caption} color="$colorMuted">Derived parameter</Text>{derivedRows.map((row) => <XStack key={row.id} alignItems="center" gap="$2" flexWrap="wrap"><DataCell width="48%">{row.label}</DataCell><DataCell width="48%" muted>{row.actual}</DataCell><DataCell width="48%" muted>{row.normalizedLabel} {row.normalized}</DataCell><DataCell width="48%" muted>{row.membership}</DataCell></XStack>)}</YStack><SafetyEvidence slot={slot} /><FuzzyTrace trace={slot.fis_trace} /></Card>;
}

export function ResearchPanel({ data = null, firmwareResults, deviceId, monitoring = [], monitoringError = false, loading = false, error = null, onRetry, onOpen }: ResearchPanelProps) {
  const reduceMotion = useReducedMotion();
  const agedMonitoring = useLiveMonitoring(deviceId ?? '', monitoring);
  const [history, setHistory] = useState<SensorHistorySnapshot>({ deviceId: '', bySensor: {} });
  const state = getResearchPanelState({ data, loading, error });
  const hasFirmware = Boolean(firmwareResults?.length);
  return <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$3" padding="$4" gap="$3">
    <XStack alignItems="center" justifyContent="space-between" gap="$3"><YStack gap="$1" flex={1} minWidth={0}>
      <Text color="$color" {...textStyles.sectionTitle}>Detail Riset</Text>
    </YStack>{onOpen ? <AppButton variant="ghost" size="sm" onPress={onOpen} ariaLabel="Muat detail riset">Perbarui</AppButton> : null}</XStack>
    {hasFirmware ? <YStack gap="$2"><Text {...textStyles.bodySecondary} color="$color" fontWeight="700">Hasil MCU terakhir</Text><Text {...textStyles.caption} color="$colorMuted">Snapshot MCU ini terpisah dari pembacaan sensor live dan analisis web. Timestamp lama bukan capture baru.</Text><FirmwareResearch results={firmwareResults!} /></YStack> : <AnimatePresence>
      {state === 'loading' ? <LoadingState label="Memuat detail riset" rows={2} /> : null}
      {state === 'error' ? <ErrorState title="Detail riset tidak dapat dimuat" message="Periksa koneksi API lalu coba lagi." onRetry={onRetry} /> : null}
      {state === 'empty' ? monitoring.length ? null : <EmptyState title="Data riset belum tersedia" description="Pembacaan audit per slot akan muncul setelah tersedia dari API." /> : null}
      {state === 'data' && data ? <XStack gap="$3" flexWrap="wrap" transition={reduceMotion ? undefined : 'standard'} enterStyle={reduceMotion ? undefined : { opacity: 0, y: -4 }}><>{data.slots.map((slot) => <ResearchSlot key={slot.stream} slot={slot} />)}</></XStack> : null}
    </AnimatePresence>}
    {deviceId ? <SensorHistoryCharts deviceId={deviceId} monitoring={agedMonitoring} onHistoryChange={setHistory} /> : null}
  </Card>;
}
