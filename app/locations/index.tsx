import { Link, useRouter } from 'expo-router';
import { useCallback, useMemo, useState } from 'react';
import { Button, Card, Input, Label, Select, Text, XStack, YStack } from 'tamagui';

import { AppShell } from '../../components/AppShell';
import { EmptyState } from '../../components/EmptyState';
import { ErrorState } from '../../components/ErrorState';
import { LoadingState } from '../../components/LoadingState';
import { LocationCard } from '../../components/LocationCard';
import { StatusBadge } from '../../components/StatusBadge';
import { apiClient } from '../../lib/api/client';
import { usePollingResource } from '../../lib/api/hooks';
import type { CanonicalStatus, LocationListResponse } from '../../lib/api/types';

export const SEARCH_INPUT_LABEL = 'Cari lokasi';
export const LOCATION_STATUS_FILTERS: ReadonlyArray<{ label: string; value: CanonicalStatus | '' }> = [
  { label: 'Semua status', value: '' }, { label: 'Normal', value: 'Normal' }, { label: 'Waspada', value: 'Waspada' },
  { label: 'Perlu diangkat', value: 'Angkat' }, { label: 'Peringatan keselamatan', value: 'Safety Alert' }, { label: 'Offline', value: 'Offline' }, { label: 'Data tidak valid', value: 'Data Invalid' },
];

function StaleNotice({ onRetry }: { onRetry: () => void }) {
  return <Card borderWidth={1} borderColor="$statusWaspada" backgroundColor="$statusWaspadaSoft" borderRadius="$4" paddingHorizontal="$4" paddingVertical="$3" role="status"><XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap"><Text color="$color" fontSize="$2" flex={1}>Data terbaru belum tersedia. Menampilkan daftar lokasi terakhir.</Text><Button color="$statusWaspada" fontSize="$2" fontWeight="800" minHeight={44} paddingHorizontal="$2" backgroundColor="transparent" borderWidth={0} onPress={onRetry}>Coba perbarui</Button></XStack></Card>;
}

export default function LocationsRoute() {
  const router = useRouter();
  const [query, setQuery] = useState('');
  const [status, setStatus] = useState<CanonicalStatus | ''>('');
  const loader = useCallback((signal: AbortSignal) => apiClient.fetchLocations({ query, status: status || undefined }, signal), [query, status]);
  const { data, loading, error, stale, retry } = usePollingResource<LocationListResponse>(loader);
  const handleNavigate = useCallback((path: string) => router.push(path as never), [router]);
  const statusCounts = useMemo(() => {
    const counts = { Normal: 0, Waspada: 0, Angkat: 0, Safety: 0 };
    for (const location of data?.locations ?? []) {
      if (location.status === 'Normal') counts.Normal += 1;
      if (location.status === 'Waspada') counts.Waspada += 1;
      if (location.status === 'Angkat') counts.Angkat += 1;
      if (location.status === 'Safety Alert') counts.Safety += 1;
    }
    return counts;
  }, [data]);

  return <AppShell title="Lokasi" subtitle="Kelola area, pantau status, dan buka detail perangkat." activePath="/locations" onNavigate={handleNavigate}><YStack gap="$6" paddingBottom="$6">
    <YStack gap="$1"><Text color="$color" fontSize="$7" fontWeight="800" letterSpacing={-0.8}>Lokasi dan device</Text><Text color="$colorMuted" fontSize="$3">Pilih lokasi untuk melihat kondisi perangkat secara terpusat.</Text></YStack>
    <XStack gap="$3" flexWrap="wrap"><Card width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }} borderWidth={1} borderColor="$borderColor" backgroundColor="$primarySoft" borderRadius="$4" padding="$4"><Text color="$primary" fontSize="$7" fontWeight="800">{data?.locations.length ?? 0}</Text><Text color="$colorMuted" fontSize="$2">Total lokasi</Text></Card><Card width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }} borderWidth={1} borderColor="$borderColor" backgroundColor="$blueSoft" borderRadius="$4" padding="$4"><Text color="$blue" fontSize="$7" fontWeight="800">{statusCounts.Normal}</Text><Text color="$colorMuted" fontSize="$2">Status normal</Text></Card><Card width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }} borderWidth={1} borderColor="$borderColor" backgroundColor="$orangeSoft" borderRadius="$4" padding="$4"><Text color="$statusAngkat" fontSize="$7" fontWeight="800">{statusCounts.Waspada + statusCounts.Angkat}</Text><Text color="$colorMuted" fontSize="$2">Perlu perhatian operasional</Text></Card><Card width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }} borderWidth={1} borderColor="$borderColor" backgroundColor="$redSoft" borderRadius="$4" padding="$4"><Text color="$statusSafetyAlert" fontSize="$7" fontWeight="800">{statusCounts.Safety}</Text><Text color="$colorMuted" fontSize="$2">Peringatan keselamatan</Text></Card></XStack>

    <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$4"><XStack gap="$4" flexWrap="wrap" alignItems="flex-end"><YStack flex={1} minWidth={240} gap="$2"><Label htmlFor="location-search" color="$color" fontSize="$2" fontWeight="800">{SEARCH_INPUT_LABEL}</Label><Input id="location-search" value={query} onChangeText={setQuery} placeholder="Cari nama atau alamat lokasi" aria-label={SEARCH_INPUT_LABEL} minHeight={52} borderWidth={1} borderColor="$borderColor" backgroundColor="$background" paddingHorizontal="$4" returnKeyType="search" /></YStack><YStack flex={1} minWidth={240} gap="$2"><Text color="$color" fontSize="$2" fontWeight="800">Filter status</Text><Select value={status || 'all'} onValueChange={(value) => setStatus(value === 'all' ? '' : value as CanonicalStatus)} name="location-status"><Select.Trigger aria-label="Filter status" minHeight={52} width="100%" borderColor="$borderColor" backgroundColor="$background"><Select.Value placeholder="Semua status" /></Select.Trigger><Select.Content><Select.Viewport minWidth={280}>{LOCATION_STATUS_FILTERS.map((filter, index) => <Select.Item key={filter.value || 'all'} value={filter.value || 'all'} index={index}><Select.ItemText>{filter.label}</Select.ItemText><Select.ItemIndicator /></Select.Item>)}</Select.Viewport></Select.Content></Select></YStack></XStack></Card>

    {stale && data ? <StaleNotice onRetry={retry} /> : null}{loading && !data ? <LoadingState label="Memuat lokasi" rows={4} /> : null}{!data && !loading && error ? <ErrorState onRetry={retry} /> : null}
    {data ? data.locations.length ? <YStack gap="$3"><XStack alignItems="center" justifyContent="space-between"><YStack gap="$1"><Text color="$color" fontSize="$6" fontWeight="800">Daftar lokasi</Text><Text color="$colorMuted" fontSize="$2">{data.locations.length} lokasi ditemukan</Text></YStack><StatusBadge status={status || null} compact /></XStack><XStack gap="$3" flexWrap="wrap">{data.locations.map((location) => <YStack key={location.id} width="100%" $wide={{ width: 'calc(33.333% - 8px)' }}><Link href={`/locations/${encodeURIComponent(location.id)}`} asChild><LocationCard location={location} /></Link></YStack>)}</XStack></YStack> : <EmptyState title={query.trim() || status ? 'Lokasi tidak ditemukan' : 'Belum ada lokasi'} description={query.trim() || status ? 'Coba ubah kata kunci atau filter status.' : 'Lokasi akan muncul setelah perangkat mengirimkan data.'} /> : null}
  </YStack></AppShell>;
}
