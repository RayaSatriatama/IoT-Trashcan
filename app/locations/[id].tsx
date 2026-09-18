import { Link, useLocalSearchParams, useRouter } from 'expo-router';
import { useCallback } from 'react';
import { Image as RNImage } from 'react-native';
import { Card, Text, XStack, YStack } from 'tamagui';

import { AppShell } from '../../components/AppShell';
import { DeviceCard } from '../../components/DeviceCard';
import { AppButton } from '../../components/AppButton';
import { EmptyState } from '../../components/EmptyState';
import { ErrorState } from '../../components/ErrorState';
import { LoadingState } from '../../components/LoadingState';
import { SectionHeader } from '../../components/SectionHeader';
import { StatusBadge } from '../../components/StatusBadge';
import { apiClient } from '../../lib/api/client';
import { usePollingResource } from '../../lib/api/hooks';
import { ArrowLeft } from '../../lib/ui/icons';
import { resolveLocationImage } from '../../lib/ui/assets';
import type { LocationDetail } from '../../lib/api/types';

export const LOCATION_EMPTY_DEVICES_COPY = { title: 'Belum ada device', description: 'Device akan muncul setelah perangkat terhubung dan mengirimkan data.' } as const;
export function deviceDetailHref(id: string): `/devices/${string}` { return `/devices/${encodeURIComponent(id)}` as `/devices/${string}`; }

function StaleNotice({ onRetry }: { onRetry: () => void }) {
  return <Card borderWidth={1} borderColor="$statusWaspada" backgroundColor="$statusWaspadaSoft" borderRadius="$4" paddingHorizontal="$4" paddingVertical="$3" role="status"><XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap"><Text color="$color" fontSize="$2" flex={1}>Data terbaru belum tersedia. Menampilkan detail lokasi terakhir.</Text><AppButton variant="ghost" size="sm" onPress={onRetry} ariaLabel="Coba perbarui detail lokasi">Coba perbarui</AppButton></XStack></Card>;
}

export default function LocationDetailRoute() {
  const router = useRouter();
  const params = useLocalSearchParams<{ id: string | string[] }>();
  const id = Array.isArray(params.id) ? params.id[0] : params.id;
  const loader = useCallback((signal: AbortSignal) => apiClient.fetchLocation(id ?? '', signal), [id]);
  const { data, loading, error, stale, retry } = usePollingResource<LocationDetail>(loader);
  const handleNavigate = useCallback((path: string) => router.push(path as never), [router]);

  return <AppShell title={data?.name ?? 'Detail lokasi'} subtitle="Kelola perangkat dan kondisi slot di area ini." activePath="/locations" onNavigate={handleNavigate}><YStack gap="$6" paddingBottom="$6"><AppButton variant="ghost" size="sm" onPress={() => router.back()} ariaLabel="Kembali ke lokasi"><ArrowLeft size={17} color="#0A9B62" strokeWidth={2.3} aria-hidden={true} /> Kembali ke lokasi</AppButton>
    {stale && data ? <StaleNotice onRetry={retry} /> : null}{loading && !data ? <LoadingState label="Memuat detail lokasi" rows={3} /> : null}{!data && !loading && error ? <ErrorState onRetry={retry} /> : null}
    {data ? <>
      <Card borderWidth={1} borderColor="$primarySoft" backgroundColor="$surface" borderRadius="$5" padding="$6" overflow="hidden"><XStack alignItems="center" gap="$5" flexWrap="wrap"><YStack flex={1} minWidth={240} gap="$3"><Text color="$primary" fontSize="$2" fontWeight="800">RINGKASAN LOKASI</Text><Text color="$color" fontSize="$8" lineHeight="$8" fontWeight="800" letterSpacing={-1}>{data.name}</Text><Text color="$colorMuted" fontSize="$3">{data.address_text ?? 'Alamat belum tersedia'}</Text><XStack alignItems="center" gap="$3" flexWrap="wrap"><StatusBadge status={data.status} /><Text color="$colorMuted" fontSize="$2">{data.devices.length} device terhubung</Text></XStack></YStack><RNImage source={resolveLocationImage(data.name)} accessibilityLabel={`Ilustrasi ${data.name}`} resizeMode="contain" style={{ width: 300, height: 150, maxWidth: '100%' }} /></XStack></Card>
      <YStack gap="$3"><SectionHeader title="Perangkat terhubung" description={`Perangkat di ${data.name}, dengan tiga slot operasional.`} action={<Text color="$colorMuted" fontSize="$2">{data.devices.length} perangkat</Text>} />{data.devices.length ? <XStack gap="$3" flexWrap="wrap">{data.devices.map((device) => <YStack key={device.id} width="100%" $tablet={{ width: 'calc(50% - 6px)' }} $wide={{ width: 'calc(33.333% - 8px)' }}><Link href={deviceDetailHref(device.id)} asChild><DeviceCard device={device} locationName={data.name} /></Link></YStack>)}</XStack> : <EmptyState title={LOCATION_EMPTY_DEVICES_COPY.title} description={LOCATION_EMPTY_DEVICES_COPY.description} />}</YStack>
    </> : null}
  </YStack></AppShell>;
}
