import { useLocalSearchParams, useRouter } from 'expo-router';
import { useCallback, useEffect, useRef, useState } from 'react';
import { Button, Card, Text, XStack, YStack } from 'tamagui';

import { AppShell } from '../../components/AppShell';
import { EmptyState } from '../../components/EmptyState';
import { ErrorState } from '../../components/ErrorState';
import { LoadingState } from '../../components/LoadingState';
import { ResearchPanel } from '../../components/ResearchPanel';
import { SlotCard } from '../../components/SlotCard';
import { StatusBadge } from '../../components/StatusBadge';
import { apiClient } from '../../lib/api/client';
import { usePollingResource } from '../../lib/api/hooks';
import type { CanonicalStatus, DeviceDetail, ResearchResponse } from '../../lib/api/types';
import { formatTimestamp } from '../../lib/format';
import { getStatusBadgePresentation } from '../../components/StatusBadge';

export const DEVICE_STREAM_LABELS = {
  organic: 'Organik',
  inorganic: 'Anorganik',
  b3: 'B3',
} as const;

export const DEVICE_UNAVAILABLE_COPY = 'Belum tersedia';

export function getDeviceStatusLabel(status: CanonicalStatus | null | undefined): string {
  return getStatusBadgePresentation(status).label;
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
          Data terbaru belum tersedia. Menampilkan detail device terakhir yang berhasil dimuat.
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

export default function DeviceDetailRoute() {
  const router = useRouter();
  const params = useLocalSearchParams<{ id: string | string[] }>();
  const id = Array.isArray(params.id) ? params.id[0] : params.id;
  const loader = useCallback(
    (signal: AbortSignal) => apiClient.fetchDevice(id ?? '', signal),
    [id],
  );
  const { data, loading, error, stale, retry } = usePollingResource<DeviceDetail>(loader);
  const [researchData, setResearchData] = useState<ResearchResponse | null>(null);
  const [researchLoading, setResearchLoading] = useState(false);
  const [researchError, setResearchError] = useState<Error | null>(null);
  const [researchStarted, setResearchStarted] = useState(false);
  const researchAbort = useRef<AbortController | null>(null);

  useEffect(() => () => researchAbort.current?.abort(), []);

  const loadResearch = useCallback(() => {
    if (!id || researchLoading) return;
    researchAbort.current?.abort();
    const controller = new AbortController();
    researchAbort.current = controller;
    setResearchStarted(true);
    setResearchLoading(true);
    setResearchError(null);
    apiClient.fetchResearch(id, controller.signal)
      .then((result) => setResearchData(result))
      .catch((cause: unknown) => {
        if (!controller.signal.aborted) {
          setResearchError(cause instanceof Error ? cause : new Error('Gagal memuat data riset'));
        }
      })
      .finally(() => {
        if (!controller.signal.aborted) setResearchLoading(false);
      });
  }, [id, researchLoading]);

  const handleBack = useCallback(() => router.back(), [router]);
  const handleNavigate = useCallback((path: string) => {
    router.push(path as never);
  }, [router]);

  return (
    <AppShell
      title={data?.device_code ?? 'Detail device'}
      subtitle="Status koneksi dan metrik operasional per slot."
      activePath="/locations"
      onNavigate={handleNavigate}
    >
      <YStack gap="$5" paddingBottom="$6">
        <BackButton onPress={handleBack} />

        {stale && data ? <StaleNotice onRetry={retry} /> : null}
        {loading && !data ? <LoadingState label="Memuat detail device" rows={4} /> : null}
        {!data && !loading && error ? <ErrorState onRetry={retry} /> : null}

        {data ? (
          <>
            <Card
              borderWidth={1}
              borderColor="$borderColor"
              backgroundColor="$surface"
              borderRadius="$3"
              padding="$4"
              gap="$4"
            >
              <XStack alignItems="flex-start" justifyContent="space-between" gap="$3" flexWrap="wrap">
                <YStack flex={1} minWidth={220} gap="$1">
                  <Text color="$color" fontSize="$6" lineHeight="$6" fontWeight="700">
                    {data.device_code}
                  </Text>
                  <Text color="$colorMuted" fontSize="$3">
                    {data.placement_label ?? DEVICE_UNAVAILABLE_COPY}
                  </Text>
                </YStack>
                <XStack gap="$2" flexWrap="wrap" justifyContent="flex-end">
                  <StatusBadge status={data.status ?? (data.online_state === 'Offline' ? 'Offline' : null)} />
                  {data.online_state === 'Offline' && data.status && data.status !== 'Offline' ? <StatusBadge status="Offline" /> : null}
                </XStack>
              </XStack>
              <XStack gap="$4" flexWrap="wrap">
                <Text color="$colorMuted" fontSize="$2">
                  Koneksi: {data.online_state ?? DEVICE_UNAVAILABLE_COPY}
                </Text>
                <Text color="$colorMuted" fontSize="$2">
                  Terakhir terlihat: {formatTimestamp(data.last_seen)}
                </Text>
                <Text color="$colorMuted" fontSize="$2">
                  ID lokasi: {data.location_id}
                </Text>
              </XStack>
            </Card>

            <YStack gap="$3">
              <YStack gap="$1">
                <Text color="$color" fontSize="$5" fontWeight="700">
                  Kondisi slot
                </Text>
                <Text color="$colorMuted" fontSize="$2">
                  Metrik hanya menampilkan slot yang dikirim API.
                </Text>
              </YStack>
              {data.slots.length ? (
                <YStack gap="$3">
                  {data.slots.map((slot) => <SlotCard key={slot.stream} slot={slot} />)}
                </YStack>
              ) : (
                <EmptyState
                  title="Belum ada data slot"
                  description="Metrik slot akan muncul setelah device mengirimkan pembacaan terbaru."
                />
              )}
            </YStack>

            <ResearchPanel
              data={researchData}
              loading={researchLoading}
              error={researchError}
              onRetry={loadResearch}
              onOpen={researchStarted ? undefined : loadResearch}
            />
          </>
        ) : null}
      </YStack>
    </AppShell>
  );
}
