import { Link, useRouter } from 'expo-router';
import { useCallback, useState } from 'react';
import { Card, Input, Label, Select, Text, XStack, YStack } from 'tamagui';

import { AppShell } from '../../components/AppShell';
import { EmptyState } from '../../components/EmptyState';
import { ErrorState } from '../../components/ErrorState';
import { LoadingState } from '../../components/LoadingState';
import { LocationCard } from '../../components/LocationCard';
import { apiClient } from '../../lib/api/client';
import { usePollingResource } from '../../lib/api/hooks';
import type { CanonicalStatus, LocationListResponse } from '../../lib/api/types';

export const SEARCH_INPUT_LABEL = 'Cari lokasi';

export const LOCATION_STATUS_FILTERS: ReadonlyArray<{ label: string; value: CanonicalStatus | '' }> = [
  { label: 'Semua status', value: '' },
  { label: 'Normal', value: 'Normal' },
  { label: 'Waspada', value: 'Waspada' },
  { label: 'Perlu diangkat', value: 'Angkat' },
  { label: 'Peringatan keselamatan', value: 'Safety Alert' },
  { label: 'Offline', value: 'Offline' },
  { label: 'Data tidak valid', value: 'Data Invalid' },
];

function StaleNotice({ onRetry }: { onRetry: () => void }) {
  return (
    <Card
      borderWidth={1}
      borderColor="$statusWaspada"
      backgroundColor="$surface"
      borderRadius="$3"
      paddingHorizontal="$4"
      paddingVertical="$3"
      role="status"
      aria-label="Data mungkin sudah lama"
    >
      <XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap">
        <Text color="$color" fontSize="$2" flex={1}>
          Data terbaru belum tersedia. Menampilkan daftar lokasi terakhir yang berhasil dimuat.
        </Text>
        <Text
          color="$primary"
          fontSize="$2"
          fontWeight="700"
          cursor="pointer"
          onPress={onRetry}
          role="button"
          aria-label="Coba perbarui data"
        >
          Coba perbarui
        </Text>
      </XStack>
    </Card>
  );
}

export default function LocationsRoute() {
  const router = useRouter();
  const [query, setQuery] = useState('');
  const [status, setStatus] = useState<CanonicalStatus | ''>('');
  const loader = useCallback(
    (signal: AbortSignal) => apiClient.fetchLocations({ query, status: status || undefined }, signal),
    [query, status],
  );
  const { data, loading, error, stale, retry } = usePollingResource<LocationListResponse>(loader);

  const handleNavigate = useCallback((path: string) => {
    router.push(path as never);
  }, [router]);

  return (
    <AppShell
      title="Lokasi"
      subtitle="Cari lokasi dan tinjau status operasionalnya."
      activePath="/locations"
      onNavigate={handleNavigate}
    >
      <YStack gap="$5" paddingBottom="$6">
        <Card
          borderWidth={1}
          borderColor="$borderColor"
          backgroundColor="$surface"
          borderRadius="$3"
          padding="$4"
          gap="$4"
        >
          <YStack gap="$2">
            <Label htmlFor="location-search" color="$color" fontSize="$2" fontWeight="700">
              {SEARCH_INPUT_LABEL}
            </Label>
            <Input
              id="location-search"
              value={query}
              onChangeText={setQuery}
              placeholder="Nama atau alamat lokasi"
              aria-label={SEARCH_INPUT_LABEL}
              minHeight={48}
              borderWidth={1}
              borderColor="$borderColor"
              backgroundColor="$background"
              paddingHorizontal="$3"
              returnKeyType="search"
            />
          </YStack>
          <YStack gap="$2">
            <Text color="$color" fontSize="$2" fontWeight="700">
              Filter status
            </Text>
            <Select
              value={status || 'all'}
              onValueChange={(value) => setStatus(value === 'all' ? '' : value as CanonicalStatus)}
              name="location-status"
            >
              <Select.Trigger aria-label="Filter status" minHeight={48} width="100%" borderColor="$borderColor" backgroundColor="$background">
                <Select.Value placeholder="Semua status" />
              </Select.Trigger>
              <Select.Content>
                <Select.Viewport minWidth={280}>
                  {LOCATION_STATUS_FILTERS.map((filter, index) => (
                    <Select.Item key={filter.value || 'all'} value={filter.value || 'all'} index={index}>
                      <Select.ItemText>{filter.label}</Select.ItemText>
                      <Select.ItemIndicator />
                    </Select.Item>
                  ))}
                </Select.Viewport>
              </Select.Content>
            </Select>
          </YStack>
        </Card>

        {stale && data ? <StaleNotice onRetry={retry} /> : null}
        {loading && !data ? <LoadingState label="Memuat lokasi" rows={4} /> : null}
        {!data && !loading && error ? <ErrorState onRetry={retry} /> : null}

        {data ? (
          data.locations.length ? (
            <YStack gap="$3">
              <Text color="$color" fontSize="$5" fontWeight="700">
                {data.locations.length} lokasi ditemukan
              </Text>
              <XStack gap="$3" flexWrap="wrap">
                {data.locations.map((location) => (
                  <YStack key={location.id} width="100%" $wide={{ width: 'calc(50% - 6px)' }}>
                    <Link href={`/locations/${encodeURIComponent(location.id)}`} asChild>
                      <LocationCard location={location} />
                    </Link>
                  </YStack>
                ))}
              </XStack>
            </YStack>
          ) : (
            <EmptyState
              title={query.trim() || status ? 'Lokasi tidak ditemukan' : 'Belum ada lokasi'}
              description={query.trim() || status ? 'Coba ubah kata kunci atau filter status.' : 'Lokasi akan muncul setelah perangkat mengirimkan data.'}
            />
          )
        ) : null}
      </YStack>
    </AppShell>
  );
}
