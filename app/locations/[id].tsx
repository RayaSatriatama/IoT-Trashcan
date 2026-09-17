import { Link, useLocalSearchParams, useRouter } from 'expo-router';
import { useCallback } from 'react';
import { Button, Card, Text, XStack, YStack } from 'tamagui';

import { AppShell } from '../../components/AppShell';
import { DeviceCard } from '../../components/DeviceCard';
import { EmptyState } from '../../components/EmptyState';
import { ErrorState } from '../../components/ErrorState';
import { LoadingState } from '../../components/LoadingState';
import { StatusBadge } from '../../components/StatusBadge';
import { apiClient } from '../../lib/api/client';
import { usePollingResource } from '../../lib/api/hooks';
import type { LocationDetail } from '../../lib/api/types';

export const LOCATION_EMPTY_DEVICES_COPY = {
  title: 'Belum ada device',
  description: 'Device akan muncul setelah perangkat terhubung dan mengirimkan data.',
} as const;

export function deviceDetailHref(id: string): string {
  return `/devices/${encodeURIComponent(id)}`;
}

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
          Data terbaru belum tersedia. Menampilkan detail lokasi terakhir yang berhasil dimuat.
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

function BackButton({ onPress }: { onPress: () => void }) {
  return (
    <Button
      alignSelf="flex-start"
      minHeight={48}
      paddingHorizontal="$3"
      backgroundColor="$surface"
      borderWidth={1}
      borderColor="$borderColor"
      color="$color"
      onPress={onPress}
      aria-label="Kembali ke lokasi"
      type="button"
    >
      Kembali ke lokasi
    </Button>
  );
}

export default function LocationDetailRoute() {
  const router = useRouter();
  const params = useLocalSearchParams<{ id: string | string[] }>();
  const id = Array.isArray(params.id) ? params.id[0] : params.id;
  const loader = useCallback(
    (signal: AbortSignal) => apiClient.fetchLocation(id ?? '', signal),
    [id],
  );
  const { data, loading, error, stale, retry } = usePollingResource<LocationDetail>(loader);

  const handleBack = useCallback(() => router.back(), [router]);
  const handleNavigate = useCallback((path: string) => {
    router.push(path as never);
  }, [router]);

  return (
    <AppShell
      title={data?.name ?? 'Detail lokasi'}
      subtitle="Identitas lokasi dan perangkat yang terhubung."
      activePath="/locations"
      onNavigate={handleNavigate}
    >
      <YStack gap="$5" paddingBottom="$6">
        <BackButton onPress={handleBack} />

        {stale && data ? <StaleNotice onRetry={retry} /> : null}
        {loading && !data ? <LoadingState label="Memuat detail lokasi" rows={3} /> : null}
        {!data && !loading && error ? <ErrorState onRetry={retry} /> : null}

        {data ? (
          <>
            <Card
              borderWidth={1}
              borderColor="$borderColor"
              backgroundColor="$surface"
              borderRadius="$3"
              padding="$4"
              gap="$3"
            >
              <XStack alignItems="flex-start" justifyContent="space-between" gap="$3" flexWrap="wrap">
                <YStack flex={1} minWidth={220} gap="$1">
                  <Text color="$color" fontSize="$6" lineHeight="$6" fontWeight="700">
                    {data.name}
                  </Text>
                  <Text color="$colorMuted" fontSize="$3">
                    {data.address_text ?? 'Alamat belum tersedia'}
                  </Text>
                </YStack>
                <StatusBadge status={data.status} />
              </XStack>
              <XStack gap="$4" flexWrap="wrap">
                <Text color="$colorMuted" fontSize="$2">
                  {data.devices.length} device terhubung
                </Text>
                <Text color="$colorMuted" fontSize="$2">
                  ID lokasi: {data.id}
                </Text>
              </XStack>
            </Card>

            <YStack gap="$3">
              <YStack gap="$1">
                <Text color="$color" fontSize="$5" fontWeight="700">
                  Device di lokasi ini
                </Text>
                <Text color="$colorMuted" fontSize="$2">
                  Pilih device untuk melihat tiga slot dan metrik operasionalnya.
                </Text>
              </YStack>
              {data.devices.length ? (
                <YStack gap="$3">
                  {data.devices.map((device) => (
                    <Link key={device.id} href={deviceDetailHref(device.id)} asChild>
                      <DeviceCard device={device} />
                    </Link>
                  ))}
                </YStack>
              ) : (
                <EmptyState
                  title={LOCATION_EMPTY_DEVICES_COPY.title}
                  description={LOCATION_EMPTY_DEVICES_COPY.description}
                />
              )}
            </YStack>
          </>
        ) : null}
      </YStack>
    </AppShell>
  );
}
