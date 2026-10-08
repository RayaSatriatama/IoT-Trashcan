import { useLocalSearchParams, useRouter } from 'expo-router';
import { useCallback } from 'react';
import { Image as RNImage } from 'react-native';
import { Card, Text, XStack, YStack, useMedia } from 'tamagui';

import deviceArtwork from '../../assets/device_detail_three_bin_with_device_card.png';
import { AppShell } from '../../components/AppShell';
import { AppButton } from '../../components/AppButton';
import { EmptyState } from '../../components/EmptyState';
import { ErrorState } from '../../components/ErrorState';
import { LoadingState } from '../../components/LoadingState';
import { SectionHeader } from '../../components/SectionHeader';
import { DeviceDecisionSummary } from '../../components/DeviceDecisionSummary';
import { ResearchPanel } from '../../components/ResearchPanel';
import { SlotCard } from '../../components/SlotCard';
import { StatusBadge } from '../../components/StatusBadge';
import { apiClient } from '../../lib/api/client';
import { usePollingResource } from '../../lib/api/hooks';
import { ArrowLeft } from '../../lib/ui/icons';
import type { CanonicalStatus, DeviceDetail, ResearchResponse } from '../../lib/api/types';
import { formatTimestamp, optionalText } from '../../lib/format';
import { textStyles } from '../../lib/ui/typography';
import { getStatusBadgePresentation } from '../../components/StatusBadge';
import { WifiSettingsDialog } from '../../components/device/WifiSettingsDialog';

export const DEVICE_STREAM_LABELS = { organic: 'Organik', inorganic: 'Anorganik', b3: 'B3' } as const;
export const DEVICE_UNAVAILABLE_COPY = 'Belum tersedia';
export function getDeviceStatusLabel(status: CanonicalStatus | null | undefined): string { return getStatusBadgePresentation(status).label; }

function StaleNotice({ onRetry }: { onRetry: () => void }) {
  return <Card borderWidth={1} borderColor="$statusWaspada" backgroundColor="$statusWaspadaSoft" borderRadius="$4" paddingHorizontal="$4" paddingVertical="$3" role="status"><XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap"><Text color="$color" fontSize="$2" flex={1}>Data terbaru belum tersedia. Menampilkan detail device terakhir.</Text><AppButton variant="ghost" size="sm" onPress={onRetry} ariaLabel="Coba perbarui detail device">Coba perbarui</AppButton></XStack></Card>;
}

export default function DeviceDetailRoute() {
  const router = useRouter();
  const media = useMedia();
  const params = useLocalSearchParams<{ id: string | string[] }>();
  const id = Array.isArray(params.id) ? params.id[0] : params.id;
  const loader = useCallback((signal: AbortSignal) => apiClient.fetchDevice(id ?? '', signal), [id]);
  const { data, loading, error, stale, retry } = usePollingResource<DeviceDetail>(loader);
  const researchLoader = useCallback((signal: AbortSignal) => apiClient.fetchResearch(id ?? '', signal), [id]);
  const { data: researchData, loading: researchLoading, error: researchError, retry: retryResearch } = usePollingResource<ResearchResponse>(researchLoader);
  const handleNavigate = useCallback((path: string) => router.push(path as never), [router]);

  return <AppShell title={data?.device_code ?? 'Detail device'} subtitle="Pantau kondisi perangkat dan metrik per slot." activePath="/locations" onNavigate={handleNavigate}><YStack gap="$6" paddingBottom="$6"><AppButton variant="ghost" size="sm" onPress={() => router.back()} ariaLabel="Kembali ke lokasi"><ArrowLeft size={17} color="#0A9B62" strokeWidth={2.3} aria-hidden={true} /> Kembali</AppButton>
    {stale && data ? <StaleNotice onRetry={retry} /> : null}{loading && !data ? <LoadingState label="Memuat detail device" rows={4} /> : null}{!data && !loading && error ? <ErrorState onRetry={retry} /> : null}
    {data ? <>
      <Card borderWidth={1} borderColor="$primarySoft" backgroundColor="$surface" borderRadius="$5" padding={media.tablet ? '$6' : '$4'} overflow="hidden"><XStack alignItems="center" gap={media.tablet ? '$5' : '$4'} flexWrap="wrap"><YStack flex={1} minWidth={250} gap="$3"><Text color="$primary" fontSize="$2" fontWeight="800">DETAIL PERANGKAT</Text><Text color="$color" {...textStyles.pageTitle} $compact={textStyles.pageTitleCompact} letterSpacing={-1}>{data.device_code}</Text>{optionalText(data.placement_label) ? <Text color="$colorMuted" fontSize="$3">{optionalText(data.placement_label)}</Text> : null}<XStack gap="$2" flexWrap="wrap" alignItems="center"><StatusBadge status={data.status ?? (data.online_state === 'Offline' ? 'Offline' : null)} />{data.online_state === 'Offline' && data.status && data.status !== 'Offline' ? <StatusBadge status="Offline" /> : null}<Text color="$colorMuted" fontSize="$2">Terakhir terlihat: {formatTimestamp(data.last_seen)}</Text></XStack><WifiSettingsDialog deviceId={data.device_code} /></YStack><DeviceDecisionSummary summary={researchData?.decision_summary} />{media.tablet ? <RNImage source={deviceArtwork} accessibilityLabel="Ilustrasi tiga jenis tempat sampah" resizeMode="contain" style={{ width: 260, height: 140, maxWidth: '100%' }} /> : null}</XStack></Card>
      <YStack gap="$3"><SectionHeader title="Kondisi slot" description="Pantau keterisian, kapasitas, dan kondisi setiap jenis sampah." />{data.slots.length ? <XStack gap="$3" flexWrap="wrap">{data.slots.map((slot) => <YStack key={slot.stream} width={media.wide ? 'calc(33.333% - 8px)' : media.tablet ? 'calc(50% - 6px)' : '100%'}><SlotCard slot={slot} research={researchData?.slots.find((item) => item.stream === slot.stream)} /></YStack>)}</XStack> : <EmptyState title="Belum ada data slot" description="Metrik slot akan muncul setelah device mengirimkan pembacaan terbaru." />}</YStack>
      <ResearchPanel key={id ?? 'research'} data={researchData} loading={researchLoading} error={researchError} onRetry={retryResearch} />
    </> : null}
  </YStack></AppShell>;
}
