import { useLocalSearchParams, useRouter } from 'expo-router';
import { useCallback, useEffect, useRef, useState } from 'react';
import { Image as RNImage } from 'react-native';
import { Card, Text, XStack, YStack, useMedia } from 'tamagui';

import deviceArtwork from '../../assets/device_detail_three_bin_with_device_card.png';
import { AppShell } from '../../components/AppShell';
import { AppButton } from '../../components/AppButton';
import { EmptyState } from '../../components/EmptyState';
import { ErrorState } from '../../components/ErrorState';
import { LoadingState } from '../../components/LoadingState';
import { SectionHeader } from '../../components/SectionHeader';
import { ResearchPanel } from '../../components/ResearchPanel';
import { SlotCard } from '../../components/SlotCard';
import { StatusBadge } from '../../components/StatusBadge';
import { apiClient } from '../../lib/api/client';
import { usePollingResource } from '../../lib/api/hooks';
import { ArrowLeft } from '../../lib/ui/icons';
import type { CanonicalStatus, DeviceDetail, ResearchResponse } from '../../lib/api/types';
import { formatTimestamp } from '../../lib/format';
import { getStatusBadgePresentation } from '../../components/StatusBadge';

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
  const [researchData, setResearchData] = useState<ResearchResponse | null>(null);
  const [researchLoading, setResearchLoading] = useState(false);
  const [researchError, setResearchError] = useState<Error | null>(null);
  const [researchStarted, setResearchStarted] = useState(false);
  const researchAbort = useRef<AbortController | null>(null);

  useEffect(() => { researchAbort.current?.abort(); researchAbort.current = null; setResearchData(null); setResearchLoading(false); setResearchError(null); setResearchStarted(false); return () => researchAbort.current?.abort(); }, [id]);
  const loadResearch = useCallback(() => { if (!id || researchLoading) return; researchAbort.current?.abort(); const controller = new AbortController(); researchAbort.current = controller; setResearchStarted(true); setResearchLoading(true); setResearchError(null); apiClient.fetchResearch(id, controller.signal).then(setResearchData).catch((cause: unknown) => { if (!controller.signal.aborted) setResearchError(cause instanceof Error ? cause : new Error('Gagal memuat data riset')); }).finally(() => { if (!controller.signal.aborted) setResearchLoading(false); }); }, [id, researchLoading]);
  const handleNavigate = useCallback((path: string) => router.push(path as never), [router]);

  return <AppShell title={data?.device_code ?? 'Detail device'} subtitle="Pantau kondisi perangkat dan metrik per slot." activePath="/locations" onNavigate={handleNavigate}><YStack gap="$6" paddingBottom="$6"><AppButton variant="ghost" size="sm" onPress={() => router.back()} ariaLabel="Kembali ke lokasi"><ArrowLeft size={17} color="#0A9B62" strokeWidth={2.3} aria-hidden={true} /> Kembali</AppButton>
    {stale && data ? <StaleNotice onRetry={retry} /> : null}{loading && !data ? <LoadingState label="Memuat detail device" rows={4} /> : null}{!data && !loading && error ? <ErrorState onRetry={retry} /> : null}
    {data ? <>
      <Card borderWidth={1} borderColor="$primarySoft" backgroundColor="$surface" borderRadius="$5" padding="$6" overflow="hidden"><XStack alignItems="center" gap="$5" flexWrap="wrap"><YStack flex={1} minWidth={250} gap="$3"><Text color="$primary" fontSize="$2" fontWeight="800">DETAIL PERANGKAT</Text><Text color="$color" fontSize="$8" lineHeight="$8" fontWeight="800" letterSpacing={-1}>{data.device_code}</Text><Text color="$colorMuted" fontSize="$3">{data.placement_label ?? DEVICE_UNAVAILABLE_COPY}</Text><XStack gap="$2" flexWrap="wrap" alignItems="center"><StatusBadge status={data.status ?? (data.online_state === 'Offline' ? 'Offline' : null)} />{data.online_state === 'Offline' && data.status && data.status !== 'Offline' ? <StatusBadge status="Offline" /> : null}<Text color="$colorMuted" fontSize="$2">Terakhir terlihat: {formatTimestamp(data.last_seen)}</Text></XStack></YStack><RNImage source={deviceArtwork} accessibilityLabel="Ilustrasi tiga jenis tempat sampah" resizeMode="contain" style={{ width: 340, height: 170, maxWidth: '100%' }} /></XStack></Card>
      <YStack gap="$3"><SectionHeader title="Kondisi slot" description="Pantau keterisian, kapasitas, dan kondisi setiap jenis sampah." />{data.slots.length ? <XStack gap="$3" flexWrap="wrap">{data.slots.map((slot) => <YStack key={slot.stream} width={media.wide ? 'calc(33.333% - 8px)' : media.tablet ? 'calc(50% - 6px)' : '100%'}><SlotCard slot={slot} /></YStack>)}</XStack> : <EmptyState title="Belum ada data slot" description="Metrik slot akan muncul setelah device mengirimkan pembacaan terbaru." />}</YStack>
      <ResearchPanel key={id ?? 'research'} data={researchData} loading={researchLoading} error={researchError} onRetry={loadResearch} onOpen={researchStarted ? undefined : loadResearch} />
    </> : null}
  </YStack></AppShell>;
}
