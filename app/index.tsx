import { Link, useRouter } from 'expo-router';
import { useCallback } from 'react';
import { Image as RNImage } from 'react-native';
import { Button, Card, Text, XStack, YStack } from 'tamagui';

import b3Artwork from '../assets/bin_b3_single_faithful.png';
import homeHero from '../assets/home_hero_smart_bin_network.png';
import logoArtwork from '../assets/logo_mark_ui_generated.png';
import { AppShell } from '../components/AppShell';
import { EmptyState } from '../components/EmptyState';
import { ErrorState } from '../components/ErrorState';
import { LoadingState } from '../components/LoadingState';
import { LocationCard } from '../components/LocationCard';
import { StatCard } from '../components/StatCard';
import { StatusBadge } from '../components/StatusBadge';
import { apiClient } from '../lib/api/client';
import { usePollingResource } from '../lib/api/hooks';
import type { EventDTO, OverviewResponse } from '../lib/api/types';
import { formatTimestamp } from '../lib/format';

export const HOME_KPI_LABELS = ['Total lokasi', 'Total perangkat', 'Perlu diangkat', 'Peringatan keselamatan'] as const;
export const RECENT_ACTIVITY_LIMIT = 3;
export const PRIORITY_EMPTY_COPY = {
  title: 'Tidak ada lokasi prioritas',
  description: 'Semua lokasi dalam kondisi yang tidak memerlukan tindakan saat ini.',
} as const;

export function recentActivityItems<T>(items: T[]): T[] { return items.slice(0, RECENT_ACTIVITY_LIMIT); }

function StaleNotice({ onRetry }: { onRetry: () => void }) {
  return <Card borderWidth={1} borderColor="$statusWaspada" backgroundColor="$statusWaspadaSoft" borderRadius="$4" paddingHorizontal="$4" paddingVertical="$3" role="status"><XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap"><Text color="$color" fontSize="$2" flex={1}>Data terbaru belum tersedia. Menampilkan data terakhir yang berhasil dimuat.</Text><Button color="$statusWaspada" fontSize="$2" fontWeight="800" minHeight={44} paddingHorizontal="$2" backgroundColor="transparent" borderWidth={0} onPress={onRetry} aria-label="Coba perbarui data">Coba perbarui</Button></XStack></Card>;
}

function ActivityItem({ event }: { event: EventDTO }) {
  return <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3" role="group" aria-label={`Aktivitas ${event.event_type}`}><XStack alignItems="center" gap="$3"><YStack width={42} height={42} borderRadius="$6" backgroundColor="$primarySoft" alignItems="center" justifyContent="center"><RNImage source={logoArtwork} resizeMode="contain" accessibilityElementsHidden style={{ width: 28, height: 28 }} /></YStack><YStack flex={1} gap="$1"><Text color="$color" fontSize="$3" fontWeight="800">{event.event_type}</Text><Text color="$colorMuted" fontSize="$2">Device {event.device_id}</Text></YStack><StatusBadge status={null} compact /></XStack><XStack justifyContent="space-between" gap="$3" flexWrap="wrap" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$3"><Text color="$colorMuted" fontSize="$2">{event.severity}</Text><Text color="$colorMuted" fontSize="$2">{formatTimestamp(event.started_at)}</Text></XStack></Card>;
}

export default function HomeRoute() {
  const router = useRouter();
  const loader = useCallback((signal: AbortSignal) => apiClient.fetchOverview(signal), []);
  const { data, loading, error, stale, retry } = usePollingResource<OverviewResponse>(loader);
  const handleNavigate = useCallback((path: string) => router.push(path as never), [router]);

  return <AppShell title="Beranda" subtitle="Pantau tempat sampah pintar di berbagai lokasi, dalam satu sistem." activePath="/" onNavigate={handleNavigate}><YStack gap="$7" paddingBottom="$6">
    <Card borderWidth={1} borderColor="$primarySoft" backgroundColor="$surface" borderRadius="$5" padding="$6" overflow="hidden"><XStack alignItems="center" gap="$6" flexWrap="wrap"><YStack flex={1} minWidth={260} gap="$3"><Text color="$primary" fontSize="$2" fontWeight="800" letterSpacing={1}>SIGAP MONITORING</Text><Text color="$color" fontSize="$8" lineHeight="$8" fontWeight="800" letterSpacing={-1}>Satu pandangan untuk semua lokasi.</Text><Text color="$colorMuted" fontSize="$3" lineHeight="$4">Pantau perangkat, temukan prioritas, dan tindak lanjuti kondisi terbaru dari satu dashboard.</Text><Button alignSelf="flex-start" minHeight={48} paddingHorizontal="$5" borderRadius="$6" backgroundColor="$primary" color="$white" fontWeight="800" onPress={() => handleNavigate('/locations')} aria-label="Lihat semua lokasi">Lihat semua lokasi  →</Button></YStack><RNImage source={homeHero} accessibilityLabel="Ilustrasi jaringan tempat sampah pintar" resizeMode="contain" style={{ width: 360, height: 220, maxWidth: '100%' }} /></XStack></Card>

    {stale && data ? <StaleNotice onRetry={retry} /> : null}
    {loading && !data ? <LoadingState label="Memuat ringkasan" rows={4} /> : null}
    {!data && !loading && error ? <ErrorState onRetry={retry} /> : null}

    {data ? <>
      <YStack gap="$3"><YStack gap="$1"><Text color="$color" fontSize="$6" fontWeight="800">Ringkasan operasi</Text><Text color="$colorMuted" fontSize="$2">Kondisi terkini dari semua lokasi terhubung.</Text></YStack><XStack gap="$3" flexWrap="wrap"><YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><StatCard label={HOME_KPI_LABELS[0]} value={data.total_locations} description="Lokasi terhubung" icon={logoArtwork} /></YStack><YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><StatCard label={HOME_KPI_LABELS[1]} value={data.total_devices} description="Device terpantau" accent="$blue" icon={b3Artwork} /></YStack><YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><StatCard label={HOME_KPI_LABELS[2]} value={data.pickup_required} description="Membutuhkan pengangkatan" accent="$statusAngkat" icon={b3Artwork} /></YStack><YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><StatCard label={HOME_KPI_LABELS[3]} value={data.safety_alerts} description="Perlu perhatian segera" accent="$statusSafetyAlert" icon={b3Artwork} /></YStack></XStack></YStack>

      <YStack gap="$3"><XStack alignItems="flex-end" justifyContent="space-between" gap="$3" flexWrap="wrap"><YStack gap="$1"><Text color="$color" fontSize="$6" fontWeight="800">Lokasi prioritas</Text><Text color="$colorMuted" fontSize="$2">Lokasi yang perlu diperiksa lebih dulu.</Text></YStack><Button color="$primary" fontSize="$2" fontWeight="800" minHeight={44} paddingHorizontal="$2" backgroundColor="transparent" borderWidth={0} onPress={() => handleNavigate('/locations')} aria-label="Lihat semua lokasi">Lihat semua lokasi  →</Button></XStack>{data.priority_locations.length ? <XStack gap="$3" flexWrap="wrap">{data.priority_locations.map((location) => <YStack key={location.id} width="100%" $wide={{ width: 'calc(33.333% - 8px)' }}><Link href={`/locations/${encodeURIComponent(location.id)}`} asChild><LocationCard location={location} /></Link></YStack>)}</XStack> : <EmptyState title={PRIORITY_EMPTY_COPY.title} description={PRIORITY_EMPTY_COPY.description} />}</YStack>

      <YStack gap="$3"><YStack gap="$1"><Text color="$color" fontSize="$6" fontWeight="800">Aktivitas terbaru</Text><Text color="$colorMuted" fontSize="$2">Informasi terbaru dari perangkat yang terhubung.</Text></YStack>{recentActivityItems(data.recent_activity).length ? <XStack gap="$3" flexWrap="wrap">{recentActivityItems(data.recent_activity).map((event) => <YStack key={event.id} width="100%" $wide={{ width: 'calc(33.333% - 8px)' }}><ActivityItem event={event} /></YStack>)}</XStack> : <EmptyState title="Belum ada aktivitas" description="Aktivitas perangkat akan muncul setelah data tersedia." />}</YStack>
    </> : null}
  </YStack></AppShell>;
}
