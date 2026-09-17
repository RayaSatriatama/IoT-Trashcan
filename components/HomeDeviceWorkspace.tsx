import { useCallback, useEffect, useRef, useState } from 'react';
import type { DeviceDetail, LocationDetail } from '../lib/api/types';
import { apiClient } from '../lib/api/client';
import { paginate } from '../lib/ui/pagination';
import { AppButton } from './AppButton';
import { DeviceCard } from './DeviceCard';
import { EmptyState } from './EmptyState';
import { ErrorState } from './ErrorState';
import { LoadingState } from './LoadingState';
import { SectionHeader } from './SectionHeader';
import { SectionPager } from './SectionPager';
import { StatusBadge } from './StatusBadge';
import { ArrowRight } from '../lib/ui/icons';
import { Text, XStack, YStack } from 'tamagui';

export interface HomeDeviceWorkspaceProps {
  location: LocationDetail | null;
  locationId: string | null;
  page: number;
  pageSize: number;
  onPageChange: (page: number) => void;
  onOpenLocation: (id: string) => void;
  onOpenDevice: (id: string) => void;
}

export function HomeDeviceWorkspace({
  location,
  locationId,
  page,
  pageSize,
  onPageChange,
  onOpenLocation,
  onOpenDevice,
}: HomeDeviceWorkspaceProps) {
  const [data, setData] = useState<LocationDetail | null>(location);
  const [loading, setLoading] = useState(Boolean(locationId));
  const [error, setError] = useState<Error | null>(null);
  const abortRef = useRef<AbortController | null>(null);

  const load = useCallback(() => {
    if (!locationId) return;
    abortRef.current?.abort();
    const controller = new AbortController();
    abortRef.current = controller;
    setLoading(true);
    setError(null);
    apiClient.fetchLocation(locationId, controller.signal)
      .then((value) => {
        if (!controller.signal.aborted) setData(value);
      })
      .catch((reason: unknown) => {
        if (!controller.signal.aborted) setError(reason instanceof Error ? reason : new Error('Gagal memuat perangkat'));
      })
      .finally(() => {
        if (!controller.signal.aborted) setLoading(false);
      });
  }, [locationId]);

  useEffect(() => {
    if (!locationId) {
      abortRef.current?.abort();
      setData(null);
      setLoading(false);
      setError(null);
      return () => undefined;
    }
    setData(location && location.id === locationId ? location : null);
    load();
    return () => abortRef.current?.abort();
  }, [location, locationId, load]);

  useEffect(() => () => abortRef.current?.abort(), []);

  const current = paginate(data?.devices ?? [], page, pageSize);
  const title = data ? `Perangkat di ${data.name}` : 'Perangkat di lokasi';
  const description = data ? `${data.devices.length} perangkat terhubung` : 'Pilih lokasi untuk memuat perangkat.';

  return (
    <YStack gap="$3">
      <SectionHeader
        title={title}
        description={description}
        action={data ? (
          <XStack alignItems="center" gap="$2" flexWrap="wrap" justifyContent="flex-end">
            <StatusBadge status={data.status} compact />
            <SectionPager page={current.page} totalPages={current.totalPages} onPrevious={() => onPageChange(Math.max(1, current.page - 1))} onNext={() => onPageChange(Math.min(current.totalPages, current.page + 1))} />
            <AppButton variant="ghost" size="sm" onPress={() => onOpenLocation(data.id)} ariaLabel="Lihat detail lokasi">
              Lihat detail lokasi <ArrowRight size={15} color="#0A9B62" strokeWidth={2.2} aria-hidden={true} />
            </AppButton>
          </XStack>
        ) : null}
      />
      {loading && !data ? <LoadingState label="Memuat perangkat" rows={pageSize} /> : null}
      {!data && !loading && error ? <ErrorState title="Perangkat tidak dapat dimuat" onRetry={load} /> : null}
      {data && error ? <ErrorState title="Perangkat tidak dapat diperbarui" message="Data lokasi terakhir tetap ditampilkan." onRetry={load} /> : null}
      {data && !loading && current.items.length ? (
        <XStack gap="$3" flexWrap="wrap">
          {current.items.map((device: DeviceDetail) => (
            <YStack key={device.id} width="100%" $tablet={{ width: 'calc(50% - 6px)' }} $wide={{ width: 'calc(33.333% - 8px)' }}>
              <DeviceCard device={device} onPress={() => onOpenDevice(device.id)} />
            </YStack>
          ))}
        </XStack>
      ) : null}
      {data && !loading && !data.devices.length ? <EmptyState title="Belum ada device pada lokasi ini" description="Device akan muncul setelah terhubung ke lokasi." /> : null}
      {data && !loading && !current.items.length && data.devices.length ? <Text color="$colorMuted">Tidak ada perangkat pada halaman ini.</Text> : null}
    </YStack>
  );
}
