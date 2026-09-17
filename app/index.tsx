import { Link, useRouter } from 'expo-router';
import { useCallback } from 'react';
import { Image as RNImage } from 'react-native';
import { Button, Card, Text, XStack, YStack } from 'tamagui';

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

export const HOME_KPI_LABELS = [
  'Total lokasi',
  'Total perangkat',
  'Perlu diangkat',
  'Peringatan keselamatan',
] as const;

export const RECENT_ACTIVITY_LIMIT = 3;

export const PRIORITY_EMPTY_COPY = {
  title: 'Tidak ada lokasi prioritas',
  description: 'Semua lokasi dalam kondisi yang tidak memerlukan tindakan saat ini.',
} as const;

export function recentActivityItems<T>(items: T[]): T[] {
  return items.slice(0, RECENT_ACTIVITY_LIMIT);
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
          Data terbaru belum tersedia. Menampilkan data terakhir yang berhasil dimuat.
        </Text>
        <Button
          color="$primary"
          fontSize="$2"
          fontWeight="700"
          minHeight={44}
          paddingHorizontal="$2"
          backgroundColor="transparent"
          borderWidth={0}
          onPress={onRetry}
          aria-label="Coba perbarui data"
        >
          Coba perbarui
        </Button>
      </XStack>
    </Card>
  );
}

function ActivityItem({ event }: { event: EventDTO }) {
  return (
    <Card
      borderWidth={1}
      borderColor="$borderColor"
      backgroundColor="$surface"
      borderRadius="$3"
      padding="$4"
      gap="$2"
      role="group"
      aria-label={`Aktivitas ${event.event_type}`}
    >
      <XStack justifyContent="space-between" alignItems="flex-start" gap="$3">
        <YStack flex={1} gap="$1">
          <Text color="$color" fontSize="$3" fontWeight="700">
            {event.event_type}
          </Text>
          <Text color="$colorMuted" fontSize="$2">
            Device {event.device_id}
          </Text>
        </YStack>
        <StatusBadge status={null} compact />
      </XStack>
      <XStack justifyContent="space-between" gap="$3" flexWrap="wrap">
        <Text color="$colorMuted" fontSize="$2">
          {event.severity}
        </Text>
        <Text color="$colorMuted" fontSize="$2">
          {formatTimestamp(event.started_at)}
        </Text>
      </XStack>
    </Card>
  );
}

export default function HomeRoute() {
  const router = useRouter();
  const loader = useCallback((signal: AbortSignal) => apiClient.fetchOverview(signal), []);
  const { data, loading, error, stale, retry } = usePollingResource<OverviewResponse>(loader);

  const handleNavigate = useCallback((path: string) => {
    router.push(path as never);
  }, [router]);

  return (
    <AppShell
      title="Ringkasan"
      subtitle="Pantau kondisi lokasi dan perangkat SIGAP secara berkala."
      activePath="/"
      onNavigate={handleNavigate}
    >
      <YStack gap="$6" paddingBottom="$6">
        <Card
          borderWidth={1}
          borderColor="$borderColor"
          backgroundColor="$surface"
          borderRadius="$4"
          padding="$4"
          gap="$4"
        >
          <XStack alignItems="center" gap="$5" flexWrap="wrap">
            <YStack flex={1} minWidth={240} gap="$2">
              <Text color="$primary" fontSize="$2" fontWeight="700">
                SIGAP MONITORING
              </Text>
              <Text color="$color" fontSize="$6" lineHeight="$6" fontWeight="700">
                Satu pandangan untuk semua lokasi.
              </Text>
              <Text color="$colorMuted" fontSize="$3" lineHeight="$4">
                Pantau perangkat, temukan prioritas, dan tindak lanjuti kondisi terbaru dari satu dashboard.
              </Text>
            </YStack>
            <RNImage
              source={require('../assets/home_hero_smart_bin_network.png')}
              accessibilityLabel="Ilustrasi jaringan tempat sampah pintar"
              resizeMode="contain"
              style={{ width: 260, height: 160 }}
            />
          </XStack>
        </Card>

        {stale && data ? <StaleNotice onRetry={retry} /> : null}

        {loading && !data ? <LoadingState label="Memuat ringkasan" rows={4} /> : null}

        {!data && !loading && error ? <ErrorState onRetry={retry} /> : null}

        {data ? (
          <>
            <YStack gap="$3">
              <Text color="$color" fontSize="$5" fontWeight="700">
                Ringkasan operasi
              </Text>
              <XStack gap="$3" flexWrap="wrap">
                <YStack flex={1} minWidth={220}>
                  <StatCard label={HOME_KPI_LABELS[0]} value={data.total_locations} description="Lokasi terhubung" />
                </YStack>
                <YStack flex={1} minWidth={220}>
                  <StatCard label={HOME_KPI_LABELS[1]} value={data.total_devices} description="Device terpantau" />
                </YStack>
                <YStack flex={1} minWidth={220}>
                  <StatCard label={HOME_KPI_LABELS[2]} value={data.pickup_required} description="Membutuhkan pengangkatan" accent="$statusAngkat" />
                </YStack>
                <YStack flex={1} minWidth={220}>
                  <StatCard label={HOME_KPI_LABELS[3]} value={data.safety_alerts} description="Perlu perhatian segera" accent="$statusSafetyAlert" />
                </YStack>
              </XStack>
            </YStack>

            <YStack gap="$3">
              <XStack alignItems="center" justifyContent="space-between" gap="$3">
                <YStack gap="$1">
                  <Text color="$color" fontSize="$5" fontWeight="700">
                    Lokasi prioritas
                  </Text>
                  <Text color="$colorMuted" fontSize="$2">
                    Lokasi yang perlu diperiksa lebih dulu.
                  </Text>
                </YStack>
                <Button
                  color="$primary"
                  fontSize="$2"
                  fontWeight="700"
                  minHeight={44}
                  paddingHorizontal="$2"
                  backgroundColor="transparent"
                  borderWidth={0}
                  onPress={() => handleNavigate('/locations')}
                  aria-label="Lihat semua lokasi"
                >
                  Lihat semua
                </Button>
              </XStack>
              {data.priority_locations.length ? (
                <XStack gap="$3" flexWrap="wrap">
                  {data.priority_locations.map((location) => (
                    <YStack key={location.id} width="100%" $wide={{ width: 'calc(50% - 6px)' }}>
                      <Link href={`/locations/${encodeURIComponent(location.id)}`} asChild>
                        <LocationCard location={location} />
                      </Link>
                    </YStack>
                  ))}
                </XStack>
              ) : (
                <EmptyState title={PRIORITY_EMPTY_COPY.title} description={PRIORITY_EMPTY_COPY.description} />
              )}
            </YStack>

            <YStack gap="$3">
              <YStack gap="$1">
                <Text color="$color" fontSize="$5" fontWeight="700">
                  Aktivitas terbaru
                </Text>
                <Text color="$colorMuted" fontSize="$2">
                  Peristiwa terbaru dari perangkat yang terhubung.
                </Text>
              </YStack>
              {recentActivityItems(data.recent_activity).length ? (
                <YStack gap="$3">
                  {recentActivityItems(data.recent_activity).map((event) => (
                    <ActivityItem key={event.id} event={event} />
                  ))}
                </YStack>
              ) : (
                <EmptyState title="Belum ada aktivitas" description="Aktivitas perangkat akan muncul setelah data tersedia." />
              )}
            </YStack>
          </>
        ) : null}
      </YStack>
    </AppShell>
  );
}
