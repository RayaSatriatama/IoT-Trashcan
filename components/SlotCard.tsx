import { Image as RNImage } from 'react-native';
import { Card as TamaguiCard, styled, Text, XStack, YStack } from 'tamagui';

import b3Artwork from '../assets/bin_b3_single_faithful.png';
import inorganicArtwork from '../assets/bin_inorganic_single_faithful.png';
import organicArtwork from '../assets/bin_organic_single_faithful.png';
import type { ResearchSlotDTO, SlotDTO } from '../lib/api/types';
import { textStyles } from '../lib/ui/typography';
import { buildDecisionInputRows, formatResearchNumber, formatSafetyState, formatShortTime, getRelatedSensorLabels, getSensorParameterLabel, getStreamTone, shouldShowFinalStatus } from '../lib/ui/slotPresentation';
import { useAnimatedNumber } from '../lib/ui/useAnimatedNumber';
import { Clock3, Scale, ShieldCheck, Wind } from '../lib/ui/icons';

import { getStreamLabel } from './DeviceCard';
import { MetricBar } from './MetricBar';
import { SlotFillGauge } from './SlotFillGauge';
import { SlotMetricCell } from './SlotMetricCell';
import { StatusBadge } from './StatusBadge';

const streamArtwork: Record<string, number> = { organic: organicArtwork, inorganic: inorganicArtwork, b3: b3Artwork };
const streamBorder: Record<string, string> = { green: '$primary', blue: '$blue', red: '$statusSafetyAlert', neutral: '$borderColor' };
const Card = styled(TamaguiCard, { height: '100%' });

export interface SlotCardProps { slot: SlotDTO; research?: ResearchSlotDTO | null }

function scoreLabel(research?: ResearchSlotDTO | null) {
  const score = research?.fis_score ?? research?.fis_trace?.score;
  return typeof score === 'number' && Number.isFinite(score) ? `${Math.round(score)} / 100` : '—';
}

function DecisionInputs({ slot, research }: { slot: SlotDTO; research?: ResearchSlotDTO | null }) {
  const items = buildDecisionInputRows(slot, research);
  return <YStack gap="$2"><YStack gap="$1"><Text {...textStyles.bodySecondary} color="$color" fontWeight="700">Dasar keputusan angkut</Text><Text {...textStyles.caption} color="$colorMuted">Empat kondisi pembentuk prioritas pengangkutan.</Text></YStack><XStack gap="$2" flexWrap="wrap">{items.map((item) => <YStack key={item.key} width="48%" minWidth={130} flexGrow={1} gap="$1" padding="$2" backgroundColor="$surfaceMuted" borderRadius="$2"><Text {...textStyles.caption} color="$colorMuted">{item.label}</Text><XStack alignItems="center" justifyContent="space-between" gap="$2"><Text {...textStyles.bodySecondary} color="$color" fontWeight="700" fontVariant={['tabular-nums']}>{item.value}</Text><Text {...textStyles.caption} color="$color" fontWeight="600">{item.category}</Text></XStack></YStack>)}</XStack></YStack>;
}

function SafetyPanel({ research }: { research?: ResearchSlotDTO | null }) {
  const safety = research?.safety;
  const temperature = research?.normalized.temperature ?? research?.raw.temperature ?? null;
  if (!safety && temperature === null) return null;
  const alert = safety?.state === 'Safety Alert';
  return <YStack gap="$2" padding="$3" borderRadius="$3" backgroundColor={alert ? '$statusSafetyAlertSoft' : '$statusNormalSoft'} borderWidth={1} borderColor={alert ? '$statusSafetyAlert' : '$statusNormalSoft'}><XStack alignItems="center" gap="$2"><ShieldCheck size={17} color={alert ? '#E4515D' : '#0A9B62'} aria-hidden /><Text {...textStyles.bodySecondary} color={alert ? '$statusSafetyAlert' : '$statusNormal'} fontWeight="700">Safety monitoring</Text><Text {...textStyles.bodySecondary} color="$color" marginLeft="auto">{formatSafetyState(safety?.state ?? research?.safety_state)}</Text></XStack><XStack gap="$4" flexWrap="wrap"><Text {...textStyles.caption} color="$colorMuted">Suhu {formatResearchNumber(temperature, ' °C')}</Text>{safety?.channels.filter((channel) => /mq[-_]?7|mq[-_]?8/i.test(channel.sensor_type)).map((channel) => <Text key={channel.sensor_id} {...textStyles.caption} color="$colorMuted">{getSensorParameterLabel(channel.parameter || channel.sensor_type)} {formatResearchNumber(channel.normalized, ' index')}</Text>)}</XStack></YStack>;
}

export function SlotCard({ slot, research = null }: SlotCardProps) {
  const tone = getStreamTone(slot.stream);
  const streamLabel = getStreamLabel(slot.stream);
  const score = useAnimatedNumber(research?.fis_score ?? research?.fis_trace?.score ?? null, { precision: 0 });
  const metricIcons = [<Scale key="weight" size={18} color="#5D7397" />, <Wind key="ogi" size={18} color="#5D7397" />, <Clock3 key="time" size={18} color="#5D7397" />, <Clock3 key="update" size={18} color="#5D7397" />];
  const researchOgi = research?.derived_parameters.ogi ?? research?.normalized.ogi ?? slot.ogi;
  const researchTime = research?.derived_parameters.accumulation_hours ?? research?.normalized.accumulation_hours ?? slot.accumulation_hours;
  const researchRecordedAt = research?.recorded_at ?? slot.recorded_at;
  const metricItems = [['Berat', formatResearchNumber(slot.weight, ' kg')], ['Gas', formatResearchNumber(researchOgi)], ['Akumulasi', formatResearchNumber(researchTime, ' jam')], ['Update', formatShortTime(researchRecordedAt)]];
  const decisionStatus = research?.fis_status ?? research?.fis_trace?.status ?? null;
  const decisionStatusLabel = decisionStatus ?? 'Belum tersedia';
  const showFinalStatus = shouldShowFinalStatus(decisionStatus, slot.status);
  const related = getRelatedSensorLabels(slot.stream);
  return <Card borderWidth={1.5} borderColor={streamBorder[tone]} backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3" role="group" aria-label={`Slot ${streamLabel}`}><XStack justifyContent="space-between" alignItems="center" gap="$3"><XStack alignItems="center" gap="$3" flex={1} minWidth={0}>{streamArtwork[slot.stream] ? <RNImage source={streamArtwork[slot.stream]} accessibilityLabel={`Ilustrasi slot ${streamLabel}`} resizeMode="contain" style={{ width: 48, height: 52 }} /> : null}<YStack gap="$1" minWidth={0}><Text color="$color" {...textStyles.cardTitle} numberOfLines={1}>{streamLabel}</Text><Text color="$colorMuted" {...textStyles.caption}>Slot {streamLabel.toLowerCase()}</Text></YStack></XStack><StatusBadge status={slot.status} compact /></XStack><XStack alignItems="center" gap="$3" flexWrap="wrap"><SlotFillGauge value={slot.fill} tone={tone} /><YStack gap="$2" flex={1} minWidth={150} backgroundColor="$surfaceMuted" borderRadius="$3" padding="$3"><XStack justifyContent="space-between" gap="$2"><Text {...textStyles.metricLabel} color="$colorMuted">Keputusan angkut</Text><Text {...textStyles.bodySecondary} color="$color" fontWeight="700">{decisionStatusLabel}</Text></XStack><XStack justifyContent="space-between" gap="$2"><Text {...textStyles.metricLabel} color="$colorMuted">Prioritas angkut</Text><Text {...textStyles.bodySecondary} color="$color" fontWeight="700" fontVariant={['tabular-nums']}>{score === null ? scoreLabel(research) : `${Math.round(score)} / 100`}</Text></XStack>{showFinalStatus ? <XStack justifyContent="space-between" gap="$2"><Text {...textStyles.metricLabel} color="$colorMuted">Status akhir</Text><Text {...textStyles.bodySecondary} color="$color" fontWeight="700">{slot.status}</Text></XStack> : null}</YStack></XStack><MetricBar label="Utilisasi berat" value={slot.weight_utilization} tone={tone} compact unavailableLabel="—" /><XStack borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$3" gap="$1">{metricItems.map(([label, value], index) => <SlotMetricCell key={label} icon={metricIcons[index]} value={value} label={label} accessibilityLabel={`${label}: ${value}`} />)}</XStack><DecisionInputs slot={slot} research={research} />{slot.stream === 'b3' ? <SafetyPanel research={research} /> : null}{slot.dominant_reason && slot.status && slot.status !== 'Normal' ? <YStack gap="$1" borderLeftWidth={3} borderLeftColor="$statusWaspada" backgroundColor="$statusWaspadaSoft" paddingHorizontal="$3" paddingVertical="$2" borderRadius="$2"><Text {...textStyles.overline} color="$statusWaspada">ALASAN KEPUTUSAN</Text><Text {...textStyles.bodySecondary} color="$color" numberOfLines={2}>{slot.dominant_reason}</Text></YStack> : null}{related.length ? <Text {...textStyles.caption} color="$colorMuted" numberOfLines={2}>Sensor terkait: {related.join(', ')}</Text> : null}</Card>;
}

export const SlotVisualCard = SlotCard;
