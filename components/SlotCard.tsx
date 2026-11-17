import { useState } from 'react';
import { Image as RNImage } from 'react-native';
import { Button, Card as TamaguiCard, Input, styled, Text, XStack, YStack } from 'tamagui';

import b3Artwork from '../assets/bin_b3_single_faithful.png';
import inorganicArtwork from '../assets/bin_inorganic_single_faithful.png';
import organicArtwork from '../assets/bin_organic_single_faithful.png';
import type { FirmwareMonitoringSample, FirmwareResult, ResearchSlotDTO, SlotDTO } from '../lib/api/types';
import { textStyles } from '../lib/ui/typography';
import { buildDecisionInputRows, formatResearchNumber, formatSafetyState, formatShortDateTime, getRelatedSensorLabels, getSensorParameterLabel, getStreamTone, shouldShowFinalStatus } from '../lib/ui/slotPresentation';
import { RECENT_SAMPLE_MS } from '../lib/ui/liveSensorState';
import { estimateNh3FromPinMv, formatNh3Estimate } from '../lib/ui/mq135Nh3Estimate';
import { formatElapsed, useAccumulationTimer } from '../lib/ui/useAccumulationTimer';
import { ORGANIC_CAPACITY_KG, weightUtilizationPercent } from '../lib/ui/transportFuzzy';
import { computeSlotDecision, type SlotDecision } from '../lib/ui/slotDecision';
import { FisBreakdown } from './FisBreakdown';
import { formatCapacity, MAX_CAPACITY_KG, MIN_CAPACITY_KG, useOrganicCapacity } from '../lib/ui/useOrganicCapacity';
import { useLiveMonitoring } from '../lib/ui/useLiveMonitoring';
import { useAnimatedNumber } from '../lib/ui/useAnimatedNumber';
import { EmptyState } from './EmptyState';
import { Clock3, Scale, ShieldCheck, Thermometer, Wind } from '../lib/ui/icons';

import { getStreamLabel } from './DeviceCard';
import { MetricBar } from './MetricBar';
import { SlotFillGauge } from './SlotFillGauge';
import { SlotMetricCell } from './SlotMetricCell';
import { StatusBadge } from './StatusBadge';

const streamArtwork: Record<string, number> = { organic: organicArtwork, inorganic: inorganicArtwork, b3: b3Artwork };
const streamBorder: Record<string, string> = { green: '$primary', blue: '$blue', red: '$statusSafetyAlert', neutral: '$borderColor' };
const Card = styled(TamaguiCard, { height: '100%', minHeight: 670 });

export interface SlotCardProps {
  slot: SlotDTO; research?: ResearchSlotDTO | null;
  /** Web NH3 estimate for the organic slot, already formatted. */
  gasEstimate?: string | null;
  /** Same estimate as a number, for the decision factor. */
  gasPpm?: number | null;
  /** Elapsed hours since the first reading of this device, web-side. */
  accumulationHours?: number | null;
  /** Human label for the same elapsed window, for example "2 jam 14 mnt". */
  accumulationLabel?: string | null;
  onResetAccumulation?: () => void;
  /** Web-computed collection decision when the MCU publishes none. */
  decisionStatus?: string | null;
  decisionScore?: number | null;
  /** Editable organic capacity in kg; defaults to the declared 2 kg. */
  capacityKg?: number;
  onCapacityChange?: (value: number) => void;
  /** Full fuzzy trace when the web already evaluated the five inputs. */
  decision?: SlotDecision | null;
}

function scoreLabel(research?: ResearchSlotDTO | null) {
  const score = research?.fis_score ?? research?.fis_trace?.score;
  return typeof score === 'number' && Number.isFinite(score) ? `${Math.round(score)} / 100` : '—';
}

function DecisionInputs({ slot, research, gasPpm = null, accumulationHours = null, accumulationLabel = null, onResetAccumulation }: {
  slot: SlotDTO; research?: ResearchSlotDTO | null; gasPpm?: number | null;
  accumulationHours?: number | null; accumulationLabel?: string | null; onResetAccumulation?: () => void;
}) {
  const items = buildDecisionInputRows(slot, research, gasPpm, accumulationHours, accumulationLabel, slot.temperature);
  return <YStack gap="$2"><YStack gap="$1"><Text {...textStyles.bodySecondary} color="$color" fontWeight="700">Dasar keputusan angkut</Text><Text {...textStyles.caption} color="$colorMuted">Lima kondisi pembentuk prioritas pengangkutan.</Text></YStack><XStack gap="$2" flexWrap="wrap">{items.map((item) => <YStack key={item.key} width="48%" minWidth={130} flexGrow={1} gap="$1" padding="$2" backgroundColor="$surfaceMuted" borderRadius="$2"><Text {...textStyles.caption} color="$colorMuted">{item.label}</Text><XStack alignItems="center" justifyContent="space-between" gap="$2"><Text {...textStyles.bodySecondary} color="$color" fontWeight="700" fontVariant={['tabular-nums']}>{item.value}</Text><XStack alignItems="center" gap="$1">
                <Text {...textStyles.caption} color="$color" fontWeight="600">{item.category}</Text>
                {item.key === 'accumulation_hours' && onResetAccumulation
                  ? <Button size="$2" chromeless paddingHorizontal="$1" height={20} onPress={onResetAccumulation} aria-label="Reset waktu akumulasi"><Text {...textStyles.caption} color="$actionPrimary" fontWeight="700">Reset</Text></Button>
                  : null}
              </XStack></XStack></YStack>)}</XStack></YStack>;
}

function SafetyPanel({ research }: { research?: ResearchSlotDTO | null }) {
  const safety = research?.safety;
  const temperature = research?.normalized.temperature ?? research?.raw.temperature ?? null;
  if (!safety && temperature === null) return null;
  const alert = safety?.state === 'Safety Alert';
  return <YStack gap="$2" padding="$3" borderRadius="$3" backgroundColor={alert ? '$statusSafetyAlertSoft' : '$statusNormalSoft'} borderWidth={1} borderColor={alert ? '$statusSafetyAlert' : '$statusNormalSoft'}><XStack alignItems="center" gap="$2"><ShieldCheck size={17} color={alert ? '#E4515D' : '#0A9B62'} aria-hidden /><Text {...textStyles.bodySecondary} color={alert ? '$statusSafetyAlert' : '$statusNormal'} fontWeight="700">Safety monitoring</Text><Text {...textStyles.bodySecondary} color="$color" marginLeft="auto">{formatSafetyState(safety?.state ?? research?.safety_state)}</Text></XStack><XStack gap="$4" flexWrap="wrap"><Text {...textStyles.caption} color="$colorMuted">Suhu {formatResearchNumber(temperature, ' °C')}</Text>{safety?.channels.filter((channel) => /mq[-_]?7|mq[-_]?8/i.test(channel.sensor_type)).map((channel) => <Text key={channel.sensor_id} {...textStyles.caption} color="$colorMuted">{getSensorParameterLabel(channel.parameter || channel.sensor_type)} {formatResearchNumber(channel.normalized, ' index')}</Text>)}</XStack></YStack>;
}

export function SlotCard({ slot, research = null, gasEstimate = null, gasPpm = null, accumulationHours = null, accumulationLabel = null, onResetAccumulation, decisionStatus: webDecisionStatus = null, decisionScore: webDecisionScore = null, capacityKg = ORGANIC_CAPACITY_KG, onCapacityChange, decision = null }: SlotCardProps) {
  const [editingCapacity, setEditingCapacity] = useState(false);
  const [capacityDraft, setCapacityDraft] = useState(String(capacityKg));
  const isOrganic = slot.stream === 'organic';
  const commitCapacity = () => {
    const parsed = Number.parseFloat(capacityDraft.replace(',', '.'));
    if (Number.isFinite(parsed) && parsed >= MIN_CAPACITY_KG && parsed <= MAX_CAPACITY_KG) onCapacityChange?.(parsed);
    setEditingCapacity(false);
  };
  const tone = getStreamTone(slot.stream);
  const streamLabel = getStreamLabel(slot.stream);
  const displayWeight = slot.weight === null ? null : Math.abs(slot.weight);
  const score = useAnimatedNumber(research?.fis_score ?? research?.fis_trace?.score ?? webDecisionScore, { precision: 0 });
  const metricIcons = [<Scale key="weight" size={18} color="#5D7397" />, <Wind key="ogi" size={18} color="#5D7397" />, <Thermometer key="temperature" size={18} color="#5D7397" />, <Clock3 key="update" size={18} color="#5D7397" />];
  const researchOgi = research?.derived_parameters.ogi ?? research?.normalized.ogi ?? slot.ogi;
  const researchTemperature = research?.normalized.temperature ?? research?.raw.temperature ?? slot.temperature;
  const researchRecordedAt = research?.recorded_at ?? slot.recorded_at;
  const metricItems = [['Berat', formatResearchNumber(displayWeight, ' kg')], ['Gas', gasEstimate ?? formatResearchNumber(researchOgi)], ['Suhu', formatResearchNumber(researchTemperature, ' °C')], ['Update', formatShortDateTime(researchRecordedAt)]];
  const decisionStatus = research?.fis_status ?? research?.fis_trace?.status ?? webDecisionStatus;
  const decisionStatusLabel = decisionStatus ?? 'Belum tersedia';
  const showFinalStatus = (decisionStatus !== null || slot.status === 'Safety Alert') && shouldShowFinalStatus(decisionStatus, slot.status);
  const related = getRelatedSensorLabels(slot.stream);
  return <Card borderWidth={1.5} borderColor={streamBorder[tone]} backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3" role="group" aria-label={`Slot ${streamLabel}`}><XStack justifyContent="space-between" alignItems="center" gap="$3"><XStack alignItems="center" gap="$3" flex={1} minWidth={0}>{streamArtwork[slot.stream] ? <RNImage source={streamArtwork[slot.stream]} accessibilityLabel={`Ilustrasi slot ${streamLabel}`} resizeMode="contain" style={{ width: 48, height: 52 }} /> : null}<YStack gap="$1" minWidth={0}><Text color="$color" {...textStyles.cardTitle} numberOfLines={1}>{streamLabel}</Text><Text color="$colorMuted" {...textStyles.caption}>Slot {streamLabel.toLowerCase()}</Text></YStack></XStack><StatusBadge status={slot.status} compact /></XStack><XStack alignItems="center" gap="$3" flexWrap="wrap"><SlotFillGauge value={slot.fill} tone={tone} /><YStack gap="$2" flex={1} minWidth={150} backgroundColor="$surfaceMuted" borderRadius="$3" padding="$3"><XStack justifyContent="space-between" gap="$2"><Text {...textStyles.metricLabel} color="$colorMuted">Keputusan angkut</Text><Text {...textStyles.bodySecondary} color="$color" fontWeight="700">{decisionStatusLabel}</Text></XStack><XStack justifyContent="space-between" gap="$2"><Text {...textStyles.metricLabel} color="$colorMuted">Prioritas angkut</Text><Text {...textStyles.bodySecondary} color="$color" fontWeight="700" fontVariant={['tabular-nums']}>{score === null ? scoreLabel(research) : `${Math.round(score)} / 100`}</Text></XStack>{showFinalStatus ? <XStack justifyContent="space-between" gap="$2"><Text {...textStyles.metricLabel} color="$colorMuted">Status akhir</Text><Text {...textStyles.bodySecondary} color="$color" fontWeight="700">{slot.status}</Text></XStack> : null}</YStack></XStack><MetricBar label="Utilisasi berat" value={isOrganic ? (displayWeight === null ? null : Math.round(displayWeight * 100) / 100) : slot.weight_utilization} max={isOrganic ? capacityKg : 100} unit={isOrganic ? ` kg / ${formatCapacity(capacityKg)} kg` : '%'} tone={tone} compact unavailableLabel="—"
            labelAction={isOrganic && onCapacityChange ? <Button size="$1" chromeless height={20} paddingHorizontal="$1" onPress={() => { setCapacityDraft(String(capacityKg)); setEditingCapacity(value => !value); }} aria-label="Ubah maksimum berat">
              <Text {...textStyles.caption} color="$actionPrimary" fontWeight="700">Ubah</Text>
            </Button> : null} />
          {isOrganic && editingCapacity ? <XStack alignItems="center" gap="$2" flexWrap="wrap">
            <Text {...textStyles.caption} color="$colorMuted">Maks (kg)</Text>
            <Input flex={1} minWidth={80} height={36} fontSize={14} value={capacityDraft} onChangeText={setCapacityDraft} keyboardType="decimal-pad" aria-label="Maksimum berat kilogram" />
            <Button size="$2" onPress={commitCapacity} aria-label="Simpan maksimum berat"><Text {...textStyles.caption} color="$color" fontWeight="700">Simpan</Text></Button>
            <Button size="$2" chromeless onPress={() => setEditingCapacity(false)} aria-label="Batal ubah maksimum berat"><Text {...textStyles.caption} color="$colorMuted" fontWeight="700">Batal</Text></Button>
          </XStack> : null}<XStack borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$3" gap="$1">{metricItems.map(([label, value], index) => <SlotMetricCell key={label} icon={metricIcons[index]} value={value} label={label} accessibilityLabel={`${label}: ${value}`} />)}</XStack><DecisionInputs slot={slot} research={research} gasPpm={gasPpm} accumulationHours={accumulationHours} accumulationLabel={accumulationLabel} onResetAccumulation={onResetAccumulation} />{isOrganic && decision ? <FisBreakdown trace={decision.priority} /> : null}{slot.stream === 'b3' ? <SafetyPanel research={research} /> : null}{related.length ? <Text {...textStyles.caption} color="$colorMuted" numberOfLines={2}>Sensor terkait: {related.join(', ')}</Text> : null}</Card>;
}

export const SlotVisualCard = SlotCard;

function isRecentSample(sample: { quality?: string; fresh: boolean | null; age_ms: number | null; future_clock_skew: boolean }): boolean {
  return (sample.quality === undefined || sample.quality === 'READY') && !sample.future_clock_skew && sample.fresh !== false &&
    typeof sample.age_ms === 'number' && Number.isFinite(sample.age_ms) && sample.age_ms >= 0 && sample.age_ms <= RECENT_SAMPLE_MS;
}

export function FirmwareSlotCard({ result }: { result: FirmwareResult }) {
  const production = result.fuzzy.model_state === 'PRODUCTION' && result.fuzzy.state === 'READY';
  const modelStatus = production && ['Normal', 'Waspada', 'Angkat', 'Safety Alert'].includes(result.fuzzy.label ?? '')
    ? result.fuzzy.label as SlotDTO['status'] : null;
  const fuzzyStatus = modelStatus === 'Normal' && !isRecentSample(result) ? null : modelStatus;
  const safetyState = result.safety.model_state === 'PRODUCTION'
    ? ({ CLEAR: 'Clear', ALERT: 'Safety Alert', FAULT: 'Gangguan' } as Record<string, string>)[result.safety.state] ?? 'Unavailable'
    : 'Unavailable';
  const status = safetyState === 'Safety Alert' ? 'Safety Alert' : fuzzyStatus;
  const weight = result.processed.weight_kg;
  const slot: SlotDTO = {
    stream: result.stream, fill: result.processed.fill_pct,
    weight: weight === null ? null : Math.abs(weight),
    weight_utilization: result.processed.weight_utilization_pct,
    ogi: result.processed.ogi, accumulation_hours: result.processed.accumulation_hours,
    temperature: result.processed.temperature_c, status,
    dominant_reason: null, safety_state: safetyState,
    recorded_at: result.time_quality === 'UNSYNCED' ? null : result.recorded_at,
  };
  const research: ResearchSlotDTO = {
    stream: result.stream, recorded_at: slot.recorded_at,
    raw: { fill: slot.fill, weight: slot.weight, gas_raw: null, temperature: slot.temperature },
    normalized: { fill_pct: slot.fill, weight_utilization: slot.weight_utilization,
      ogi: slot.ogi, accumulation_hours: slot.accumulation_hours, temperature: slot.temperature },
    derived_parameters: { ogi: slot.ogi, accumulation_hours: slot.accumulation_hours },
    fis_score: production && typeof result.fuzzy.score === 'number' && Number.isFinite(result.fuzzy.score) ? result.fuzzy.score : null,
    fis_status: fuzzyStatus, fis_version: null, dominant_reason: null,
    sensor_health: null, safety_state: safetyState, sensors: null, fis_trace: null,
    safety: { state: safetyState, channels: [] },
  };
  return <SlotCard slot={slot} research={research} />;
}

function readyValue(sample: FirmwareMonitoringSample | undefined, key: string): number | null {
  const value = sample?.quality === 'READY' ? sample.converted_value?.[key] : null;
  return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

const organicLiveInputs = [
  ['hx711_organic', 'kg'],
  ['ultrasonic_organic', 'fill_percent'],
  ['mq135', 'adc_mv'],
  ['sht31_44', 'temperature_c'],
] as const;

function hasRecentOrganicInputs(samples: readonly FirmwareMonitoringSample[]): boolean {
  return organicLiveInputs.every(([sensorId, key]) => {
    const sample = samples.find(item => item.sensor_id === sensorId);
    const value = sample?.converted_value?.[key];
    return Boolean(sample && sample.stream === 'organic' && isRecentSample(sample) && typeof value === 'number' && Number.isFinite(value));
  });
}

function MonitoringSlotCard({ stream, samples, deviceId }: { stream: 'organic' | 'inorganic' | 'b3'; samples: readonly FirmwareMonitoringSample[]; deviceId: string }) {
  const sample = (sensorId: string) => samples.find(item => item.sensor_id === sensorId);
  const nh3 = stream === 'organic' ? estimateNh3FromPinMv(sample('mq135')?.converted_value?.adc_pin_mv) : null;
  const gasEstimate = nh3 ? formatNh3Estimate(nh3) : null;
  const hasReading = samples.some(item => item.quality === 'READY');
  const accumulation = useAccumulationTimer(deviceId, hasReading);
  const weight = readyValue(sample(`hx711_${stream}`), 'kg');
  // Monitoring samples carry an age, not a timestamp; the newest age becomes the
  // Update instant so the tile shows the date of the freshest data.
  const freshestAge = samples.reduce<number | null>((best, item) => typeof item.age_ms === 'number' && Number.isFinite(item.age_ms) && (best === null || item.age_ms < best) ? item.age_ms : best, null);
  const newestAt = freshestAge === null ? null : new Date(Date.now() - freshestAge).toISOString();
  const capacity = useOrganicCapacity(deviceId);
  const webDecision = computeSlotDecision({
    fill: readyValue(sample('ultrasonic_organic'), 'fill_percent'),
    weightUtilization: weightUtilizationPercent(weight === null ? null : Math.abs(weight), capacity.capacityKg),
    ammoniaPpm: nh3?.ppm ?? null,
    accumulationHours: accumulation.elapsedMs === null ? null : accumulation.elapsedMs / 3600000,
    temperatureC: readyValue(sample(stream === 'organic' ? 'sht31_44' : 'ds18b20_b3'), 'temperature_c'),
  });
  const temperatureSensor = stream === 'organic' ? 'sht31_44' : stream === 'b3' ? 'ds18b20_b3' : null;
  const slot: SlotDTO = {
    stream, fill: readyValue(sample(`ultrasonic_${stream}`), 'fill_percent'),
    weight: weight === null ? null : Math.abs(weight),
    weight_utilization: stream === 'organic' && weight !== null ? Math.abs(weight) / capacity.capacityKg * 100 : null,
    status: stream === 'organic' && hasRecentOrganicInputs(samples) ? 'Normal' : null,
    dominant_reason: null, safety_state: null, recorded_at: newestAt, ogi: null,
    accumulation_hours: null, temperature: temperatureSensor ? readyValue(sample(temperatureSensor), 'temperature_c') : null,
  };
  const accumulationHours = accumulation.elapsedMs === null ? null : accumulation.elapsedMs / 3600000;
  return <SlotCard slot={slot} gasEstimate={gasEstimate === '—' ? null : gasEstimate}
    gasPpm={nh3?.ppm ?? null}
    accumulationHours={accumulationHours}
    accumulationLabel={accumulation.elapsedMs === null ? null : formatElapsed(accumulation.elapsedMs)}
    onResetAccumulation={accumulation.reset}
    decisionStatus={webDecision.status}
    decisionScore={webDecision.score}
    decision={webDecision}
    capacityKg={capacity.capacityKg}
    onCapacityChange={capacity.setCapacityKg} />;
}

function MonitoringSlotCards({ samples, deviceId, cardWidth, streams }: {
  samples: readonly FirmwareMonitoringSample[];
  deviceId: string;
  cardWidth: string;
  streams: readonly ('organic' | 'inorganic' | 'b3')[];
}) {
  const agedMonitoring = useLiveMonitoring(deviceId, samples);
  return <XStack gap="$3" flexWrap="wrap">{streams.map(stream => <YStack key={stream} width={cardWidth}><MonitoringSlotCard stream={stream} deviceId={deviceId} samples={agedMonitoring.filter(item => item.stream === stream)} /></YStack>)}</XStack>;
}

export function DeviceSlotCards({ firmwareResults, slots, researchSlots = [], monitoring, deviceId, cardWidth }: {
  firmwareResults: readonly FirmwareResult[];
  slots: readonly SlotDTO[];
  researchSlots?: readonly ResearchSlotDTO[];
  monitoring: readonly FirmwareMonitoringSample[];
  deviceId: string;
  cardWidth: string;
}) {
  if (firmwareResults.length) return <XStack gap="$3" flexWrap="wrap">{firmwareResults.map(result => <YStack key={result.stream} width={cardWidth}><FirmwareSlotCard result={result} /></YStack>)}</XStack>;
  if (slots.length) return <XStack gap="$3" flexWrap="wrap">{slots.map(slot => <YStack key={slot.stream} width={cardWidth}><SlotCard slot={slot} research={researchSlots.find(item => item.stream === slot.stream)} /></YStack>)}</XStack>;
  const streams = [...new Set(monitoring.map(item => item.stream))].filter((stream): stream is 'organic' | 'inorganic' | 'b3' => stream === 'organic' || stream === 'inorganic' || stream === 'b3');
  if (streams.length) return <MonitoringSlotCards samples={monitoring} deviceId={deviceId} cardWidth={cardWidth} streams={streams} />;
  return <EmptyState title="Belum ada data slot" description="Metrik slot akan muncul setelah device mengirimkan pembacaan terbaru." />;
}
