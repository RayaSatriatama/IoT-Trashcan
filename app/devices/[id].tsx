import { useLocalSearchParams, useRouter } from 'expo-router';
import { useCallback, useEffect, useMemo, useState, useSyncExternalStore } from 'react';
import { Image as RNImage } from 'react-native';
import { Button, Card, Text, XStack, YStack, useMedia } from 'tamagui';

import deviceArtwork from '../../assets/device_detail_three_bin_with_device_card.png';
import { AppShell } from '../../components/AppShell';
import { AppButton } from '../../components/AppButton';
import { ErrorState } from '../../components/ErrorState';
import { LoadingState } from '../../components/LoadingState';
import { DescriptionHint, SectionHeader } from '../../components/SectionHeader';
import { DeviceDecisionSummary } from '../../components/DeviceDecisionSummary';
import { ResearchPanel } from '../../components/ResearchPanel';
import { DeviceSlotCards } from '../../components/SlotCard';
import { StatusBadge } from '../../components/StatusBadge';
import { apiClient } from '../../lib/api/client';
import { usePollingResource } from '../../lib/api/hooks';
import { ArrowLeft, Bluetooth, Terminal, Wifi } from '../../lib/ui/icons';
import type { CanonicalStatus, DeviceDetail, ResearchResponse } from '../../lib/api/types';
import { formatTimestamp, optionalText } from '../../lib/format';
import { textStyles } from '../../lib/ui/typography';
import { getStatusBadgePresentation } from '../../components/StatusBadge';
import { WifiSettingsDialog } from '../../components/device/WifiSettingsDialog';
import { CalibrationWizard } from '../../components/device/CalibrationWizard';
import { SensorDiagnosticsPanel } from '../../components/device/SensorDiagnosticsPanel';
import { CALIBRATION_SENSOR_IDS, createCalibrationApi } from '../../lib/api/calibration';
import { isWebBluetoothAvailable } from '../../lib/ble/protocol';
import { DeviceBleSession } from '../../lib/ble/device-session';
import { WifiSettingsController } from '../../lib/ble/wifi-settings';
import { diagnosticFromResearch } from '../../lib/api/research';
import { DebugViewController } from '../../lib/debug/poller';
import { DeviceDebugPanel } from '../../components/device/DeviceDebugPanel';
import { SensorSelector } from '../../components/device/SensorSelector';
import { DeviceToolLauncher } from '../../components/device/DeviceToolLauncher';
import { useOrganicDecision } from '../../lib/ui/useSlotDecision';

export const DEVICE_STREAM_LABELS = { organic: 'Organik', inorganic: 'Anorganik', b3: 'B3' } as const;
export const DEVICE_UNAVAILABLE_COPY = 'Belum tersedia';
export function getDeviceStatusLabel(status: CanonicalStatus | null | undefined): string { return getStatusBadgePresentation(status).label; }

export function DeviceDetailSessionView({ owner, controller, deviceCode, bleDeviceId, sensorId, onSensorChange, diagnostic, diagnosticLoading = false, diagnosticError = null, onDiagnosticRefresh }: { owner: DeviceBleSession; controller: WifiSettingsController; deviceCode: string; bleDeviceId: string | null; sensorId: typeof CALIBRATION_SENSOR_IDS[number]; onSensorChange: (sensor: typeof CALIBRATION_SENSOR_IDS[number]) => void; diagnostic: ReturnType<typeof diagnosticFromResearch>; diagnosticLoading?: boolean; diagnosticError?: string | null; onDiagnosticRefresh: () => void }) {
  const peer = useSyncExternalStore(owner.subscribe, owner.getSnapshot, owner.getSnapshot);
  const [transport, setTransport] = useState<'ble' | 'https'>(bleDeviceId ? 'ble' : 'https');
  const [debugVisible, setDebugVisible] = useState(false);
  const wifiView = useSyncExternalStore(controller.subscribe, controller.getSnapshot, controller.getSnapshot);
  const debug = useMemo(() => new DebugViewController({ deviceId: deviceCode, session: owner }), [deviceCode, owner]);
  useEffect(() => () => debug.dispose(), [debug]);
  const connectBle = useCallback(() => owner.connect(), [owner]);
  const beginActivity = useCallback((source: Parameters<DeviceBleSession['beginControlActivity']>[0]) => owner.beginControlActivity(source), [owner]);
  return <YStack role="region" aria-label="Alat perangkat" gap={16} minWidth={0}>
    <Text color="$color" {...textStyles.sectionTitle} fontSize={19} lineHeight={26.6}>Diagnostik dan kalibrasi sensor</Text>
    <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius={15} padding={19} $devicePhone={{ padding: 14 }}>
      <XStack gap={11} $deviceToolsMobile={{ flexDirection: 'column' }}>
        <DeviceToolLauncher label="Konfigurasi Wi-Fi" icon={<Wifi size={18} color="#087B50" aria-hidden />} expanded={wifiView.open} onPress={() => controller.open()} />
        <DeviceToolLauncher label="Debug" icon={<Terminal size={18} color="#155DB0" aria-hidden />} blue expanded={debugVisible} onPress={() => setDebugVisible(value => !value)} />
      </XStack>
    </Card>
    <WifiSettingsDialog deviceId={deviceCode} bleDeviceId={bleDeviceId} session={owner} controller={controller} hideTrigger />
    <DeviceDebugPanel key={`${deviceCode}:${bleDeviceId ?? ''}`} controller={debug} deviceId={deviceCode} busy={(peer.busy !== null && peer.busy !== 'debug') || peer.controlActive} trace={peer.trace} hideToggle visible={debugVisible} onVisibleChange={setDebugVisible} />
    <XStack gap={14} alignItems="stretch" minWidth={0} $deviceToolsMobile={{ flexDirection: 'column' }}>
      <Card role="group" aria-label="Sensor" flex={1} minWidth={0} borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius={15} padding={19} gap={15} $deviceToolsMobile={{ flexGrow: 0, flexShrink: 0, flexBasis: 'auto' }} $devicePhone={{ padding: 14 }}>
        <Text color="$color" {...textStyles.cardTitle} fontSize={16} lineHeight={22.4}>Sensor</Text>
        <SensorSelector value={sensorId} disabled={peer.controlActive} onChange={sensor => { if (!owner.getSnapshot().controlActive) onSensorChange(sensor); }} />
      </Card>
      <SensorDiagnosticsPanel diagnostic={diagnostic} source="legacy" loading={diagnosticLoading} error={diagnosticError} onRefresh={onDiagnosticRefresh} />
    </XStack>
    <Card role="group" aria-label="Kalibrasi" borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius={15} padding={19} gap={15} $devicePhone={{ padding: 14 }}>
      <Text color="$color" {...textStyles.cardTitle} fontSize={16} lineHeight={22.4}>Kalibrasi</Text>
      <XStack gap={8} flexWrap="wrap">
        <Button type="button" minHeight={44} paddingHorizontal={13} borderRadius={10} borderWidth={1} fontSize={12} fontWeight="700" backgroundColor={transport === 'ble' ? '$deviceSelected' : '$surface'} borderColor={transport === 'ble' ? '$actionPrimary' : '$borderColor'} color={transport === 'ble' ? '$actionPrimary' : '$color'} aria-label="Kalibrasi melalui BLE" aria-pressed={transport === 'ble'} disabled={!bleDeviceId || peer.controlActive} onPress={() => { if (bleDeviceId && !peer.controlActive) setTransport('ble'); }}><Bluetooth size={16} color={transport === 'ble' ? '#087B50' : '#10275B'} aria-hidden /> BLE lokal</Button>
        <Button type="button" minHeight={44} paddingHorizontal={13} borderRadius={10} borderWidth={1} fontSize={12} fontWeight="700" backgroundColor={transport === 'https' ? '$deviceSelected' : '$surface'} borderColor={transport === 'https' ? '$actionPrimary' : '$borderColor'} color={transport === 'https' ? '$actionPrimary' : '$color'} aria-label="Kalibrasi melalui HTTPS" aria-pressed={transport === 'https'} disabled={peer.controlActive} onPress={() => { if (!peer.controlActive) setTransport('https'); }}><Wifi size={16} color={transport === 'https' ? '#087B50' : '#10275B'} aria-hidden /> HTTPS Wi-Fi</Button>
      </XStack>
      {transport === 'ble' && !bleDeviceId ? <Text role="status" color="$colorMuted">Identitas BLE belum tersedia.</Text> : <CalibrationWizard key={`${deviceCode}:${transport}`} deviceId={transport === 'ble' ? bleDeviceId! : deviceCode} sensorId={sensorId} diagnostic={diagnostic} api={null} apiFactory={createCalibrationApi} transport={transport} bleClient={transport === 'ble' ? owner.calibrationAdapter : null} bleConnected={peer.connected} peerBusy={peer.busy !== null && peer.busy !== 'calibration'} onConnectBle={transport === 'ble' && isWebBluetoothAvailable() ? connectBle : undefined} onDiagnosticRefresh={onDiagnosticRefresh} onControlActivity={beginActivity} compactTrigger />}
    </Card>
  </YStack>;
}

function StaleNotice({ onRetry }: { onRetry: () => void }) {
  return <Card borderWidth={1} borderColor="$statusWaspada" backgroundColor="$statusWaspadaSoft" borderRadius="$4" paddingHorizontal="$4" paddingVertical="$3" role="status"><XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap"><Text color="$color" fontSize="$2" flex={1}>Data terbaru belum tersedia. Menampilkan detail device terakhir.</Text><AppButton variant="ghost" size="sm" onPress={onRetry} ariaLabel="Coba perbarui detail device">Coba perbarui</AppButton></XStack></Card>;
}

export default function DeviceDetailRoute() {
  const router = useRouter();
  const media = useMedia();
  const params = useLocalSearchParams<{ id: string | string[] }>();
  const id = Array.isArray(params.id) ? params.id[0] : params.id;
  const loader = useCallback((signal: AbortSignal) => apiClient.fetchDevice(id ?? '', signal), [id]);
  const { data, loading, error, stale, retry } = usePollingResource<DeviceDetail>(loader, 1_000);
  const researchLoader = useCallback((signal: AbortSignal) => apiClient.fetchResearch(id ?? '', signal), [id]);
  const { data: researchData, loading: researchLoading, error: researchError, retry: retryResearch } = usePollingResource<ResearchResponse>(researchLoader);
  const [selectedSensorIndex, setSelectedSensorIndex] = useState(0);
  const selectedSensor = CALIBRATION_SENSOR_IDS[selectedSensorIndex] ?? CALIBRATION_SENSOR_IDS[0];
  const selectedDiagnostic = useMemo(() => diagnosticFromResearch(selectedSensor, researchData), [researchData, selectedSensor]);
  const owner = useMemo(() => new DeviceBleSession(data?.ble_device_id ?? null), [id, data?.ble_device_id]);
  const wifiController = useMemo(() => new WifiSettingsController(owner), [owner]);
  const peer = useSyncExternalStore(owner.subscribe, owner.getSnapshot, owner.getSnapshot);
  useEffect(() => () => { wifiController.dispose(); owner.dispose(); }, [wifiController, owner]);
  const firmwareResults = data?.firmware_results ?? [];
  // The four configured inputs (fill, weight utilization, NH3 ppm, accumulation)
  // drive the decision whenever the MCU publishes none of its own.
  const organicDecision = useOrganicDecision(id ?? '', data?.id === id ? data?.firmware_monitoring ?? [] : []);

  const handleNavigate = useCallback((path: string) => router.push(path as never), [router]);

  return <AppShell title={optionalText(data?.placement_label) ?? 'Detail device'} subtitle="Pantau kondisi perangkat dan metrik per slot." activePath="/locations" onNavigate={handleNavigate}><YStack gap="$6" paddingBottom="$6"><AppButton variant="ghost" size="sm" onPress={() => router.back()} ariaLabel="Kembali ke lokasi"><ArrowLeft size={17} color="#0A9B62" strokeWidth={2.3} aria-hidden={true} /> Kembali</AppButton>
    {stale && data ? <StaleNotice onRetry={retry} /> : null}{loading && !data ? <LoadingState label="Memuat detail device" rows={4} /> : null}{!data && !loading && error ? <ErrorState onRetry={retry} /> : null}
    {data ? <>
      <Card borderWidth={1} borderColor="$primarySoft" backgroundColor="$surface" borderRadius="$5" padding={media.tablet ? '$6' : '$4'} overflow="hidden"><XStack alignItems="center" gap={media.tablet ? '$5' : '$4'} flexWrap="wrap"><YStack flex={1} minWidth={250} gap="$3"><Text color="$primary" fontSize="$2" fontWeight="800">DETAIL PERANGKAT</Text><Text color="$color" {...textStyles.pageTitle} $compact={textStyles.pageTitleCompact} letterSpacing={-1}>{optionalText(data.placement_label) ?? 'Perangkat'}</Text><XStack gap="$2" flexWrap="wrap" alignItems="center">{data.status ? <StatusBadge status={data.status} /> : null}{data.online_state === 'Offline' ? <StatusBadge status="Offline" /> : null}{data.last_seen ? <Text color="$colorMuted" fontSize="$2">Terakhir terlihat: {formatTimestamp(data.last_seen)}</Text> : null}</XStack></YStack><DeviceDecisionSummary summary={researchData?.decision_summary} firmwareResults={firmwareResults} webScore={organicDecision.decision.score} webStatus={organicDecision.decision.status} />{media.tablet ? <RNImage source={deviceArtwork} accessibilityLabel="Ilustrasi tiga jenis tempat sampah" resizeMode="contain" style={{ width: 260, height: 140, maxWidth: '100%' }} /> : null}</XStack></Card>
      <YStack gap="$3">
        <SectionHeader title="Kondisi slot" />
        <DeviceSlotCards firmwareResults={firmwareResults} slots={data.slots} researchSlots={researchData?.slots} monitoring={data.id === id ? data.firmware_monitoring ?? [] : []} deviceId={id ?? ''} cardWidth={media.wide ? 'calc(33.333% - 8px)' : media.tablet ? 'calc(50% - 6px)' : '100%'} />
      </YStack>
      <ResearchPanel key={id ?? 'research'} data={researchData} firmwareResults={data.firmware_results} deviceId={id ?? ''} monitoring={data.id === id ? data.firmware_monitoring ?? [] : []} monitoringError={stale || Boolean(error)} loading={researchLoading} error={researchError} onRetry={retryResearch} />
      <DeviceDetailSessionView owner={owner} controller={wifiController} deviceCode={data.device_code} bleDeviceId={data.ble_device_id ?? null} sensorId={selectedSensor} onSensorChange={sensor => { if (!owner.getSnapshot().controlActive) setSelectedSensorIndex(CALIBRATION_SENSOR_IDS.indexOf(sensor)); }} diagnostic={selectedDiagnostic} diagnosticLoading={researchLoading} diagnosticError={researchError?.message ?? null} onDiagnosticRefresh={retryResearch} />
    </> : null}
  </YStack></AppShell>;
}
