import { Link, useRouter } from 'expo-router';
import { useCallback, useMemo, useState } from 'react';
import { ScrollView } from 'react-native';
import { Button, Card, Input, Label, Text, XStack, YStack } from 'tamagui';

import { AppShell } from '../../components/AppShell';
import { EmptyState } from '../../components/EmptyState';
import { ErrorState } from '../../components/ErrorState';
import { KpiCard } from '../../components/KpiCard';
import { LoadingState } from '../../components/LoadingState';
import { LocationCard } from '../../components/LocationCard';
import { StatusBadge } from '../../components/StatusBadge';
import { apiClient } from '../../lib/api/client';
import { usePollingResource } from '../../lib/api/hooks';
import { Filter, MapPin, Search, ShieldAlert, TriangleAlert } from '../../lib/ui/icons';
import type { CanonicalStatus, LocationListResponse } from '../../lib/api/types';

export const SEARCH_INPUT_LABEL = 'Cari lokasi';
export const LOCATION_STATUS_FILTERS: ReadonlyArray<{ label: string; value: CanonicalStatus | '' }> = [
  { label: 'Semua status', value: '' }, { label: 'Normal', value: 'Normal' }, { label: 'Waspada', value: 'Waspada' },
  { label: 'Diangkat', value: 'Angkat' }, { label: 'Perhatian', value: 'Safety Alert' }, { label: 'Offline', value: 'Offline' }, { label: 'Data tidak valid', value: 'Data Invalid' },
];

function StatusFilterChip({ active, label, onPress }: { active: boolean; label: string; onPress: () => void }) {
  return <Button minHeight={38} paddingHorizontal="$3" borderRadius="$6" borderWidth={1} borderColor={active ? '$primary' : '$borderColor'} backgroundColor={active ? '$primarySoft' : '$surface'} color={active ? '$primary' : '$colorMuted'} fontSize="$2" fontWeight={active ? '800' : '600'} onPress={onPress}>{label}</Button>;
}

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

  return <AppShell title="Lokasi" subtitle="Kelola area, pantau status, dan buka detail perangkat." activePath="/locations" onNavigate={handleNavigate}><YStack gap="$5" paddingBottom="$6">
    <YStack gap="$1"><Text color="$color" fontSize="$7" fontWeight="800" letterSpacing={-0.8}>Lokasi dan device</Text><Text color="$colorMuted" fontSize="$3">Pilih lokasi untuk melihat kondisi perangkat secara terpusat.</Text></YStack>
    <XStack gap="$3" flexWrap="wrap"><YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Total lokasi" value={data?.locations.length ?? 0} icon={MapPin} tone="green" /></YStack><YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Status normal" value={statusCounts.Normal} icon={MapPin} tone="blue" /></YStack><YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Perlu perhatian" value={statusCounts.Waspada + statusCounts.Angkat} icon={TriangleAlert} tone="orange" /></YStack><YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Peringatan keselamatan" value={statusCounts.Safety} icon={ShieldAlert} tone="red" /></YStack></XStack>

    <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3"><XStack alignItems="center" gap="$2"><Search size={17} color="#5D7397" strokeWidth={2.2} aria-hidden={true} /><Label htmlFor="location-search" color="$color" fontSize="$2" fontWeight="800">{SEARCH_INPUT_LABEL}</Label></XStack><XStack gap="$3" alignItems="center" flexWrap="wrap"><XStack flex={1} minWidth={220} alignItems="center" gap="$2" borderWidth={1} borderColor="$borderColor" borderRadius="$3" backgroundColor="$background" paddingHorizontal="$3"><Search size={17} color="#5D7397" strokeWidth={2} aria-hidden={true} /><Input id="location-search" flex={1} value={query} onChangeText={setQuery} placeholder="Cari nama atau alamat lokasi" aria-label={SEARCH_INPUT_LABEL} height={44} borderWidth={0} backgroundColor="transparent" paddingHorizontal="$1" returnKeyType="search" /></XStack><ScrollView horizontal showsHorizontalScrollIndicator={false} style={{ flexGrow: 0, flexShrink: 1 }} contentContainerStyle={{ alignItems: 'center', gap: 8, paddingVertical: 1 }}><Filter size={16} color="#5D7397" strokeWidth={2.2} aria-hidden={true} />{LOCATION_STATUS_FILTERS.map((filter) => <StatusFilterChip key={filter.value || 'all'} active={status === filter.value || (!status && !filter.value)} label={filter.label} onPress={() => setStatus(filter.value)} />)}</ScrollView></XStack></Card>

    {stale && data ? <StaleNotice onRetry={retry} /> : null}{loading && !data ? <LoadingState label="Memuat lokasi" rows={4} /> : null}{!data && !loading && error ? <ErrorState onRetry={retry} /> : null}
    {data ? data.locations.length ? <YStack gap="$3"><XStack alignItems="center" justifyContent="space-between"><YStack gap="$1"><Text color="$color" fontSize="$6" fontWeight="800">Daftar lokasi</Text><Text color="$colorMuted" fontSize="$2">{data.locations.length} lokasi ditemukan</Text></YStack>{status ? <StatusBadge status={status} compact /> : <Text color="$colorMuted" fontSize="$1">Semua status</Text>}</XStack><XStack gap="$3" flexWrap="wrap">{data.locations.map((location) => <YStack key={location.id} width="100%" $wide={{ width: 'calc(33.333% - 8px)' }}><Link href={`/locations/${encodeURIComponent(location.id)}`} asChild><LocationCard location={location} /></Link></YStack>)}</XStack></YStack> : <EmptyState title={query.trim() || status ? 'Lokasi tidak ditemukan' : 'Belum ada lokasi'} description={query.trim() || status ? 'Coba ubah kata kunci atau filter status.' : 'Lokasi akan muncul setelah perangkat mengirimkan data.'} /> : null}
  </YStack></AppShell>;
}
