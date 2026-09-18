import { useRouter } from 'expo-router';
import { useCallback, useEffect, useMemo, useState } from 'react';
import { Image as RNImage } from 'react-native';
import { Card, Text, XStack, YStack, useMedia } from 'tamagui';

import homeHero from '../assets/home_hero_smart_bin_network.png';
import { AppButton } from '../components/AppButton';
import { AppShell } from '../components/AppShell';
import { EmptyState } from '../components/EmptyState';
import { ErrorState } from '../components/ErrorState';
import { HomeDeviceWorkspace } from '../components/HomeDeviceWorkspace';
import { HomeMonitoringWorkspace } from '../components/HomeMonitoringWorkspace';
import { HomeLocationSelector } from '../components/HomeLocationSelector';
import { KpiCard } from '../components/KpiCard';
import { LoadingState } from '../components/LoadingState';
import { SectionHeader } from '../components/SectionHeader';
import { StatusBadge } from '../components/StatusBadge';
import { apiClient } from '../lib/api/client';
import { usePollingResource } from '../lib/api/hooks';
import type { DeviceSummaryResponse, EventDTO, LocationListResponse, OverviewResponse } from '../lib/api/types';
import { formatTimestamp } from '../lib/format';
import { Activity, ArrowRight, Box, MapPin, ShieldAlert, Truck } from '../lib/ui/icons';
import { paginate } from '../lib/ui/pagination';
import { textStyles } from '../lib/ui/typography';
import type { HomeMonitoringMode, HomeOperationalFilter } from '../lib/ui/homeMonitoring';
import { getAffectedLocations, groupDeviceSummaries } from '../lib/ui/homeMonitoring';

export const HOME_KPI_LABELS = ['Total lokasi', 'Total perangkat', 'Perlu diangkat', 'Peringatan keselamatan'] as const;
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
    <Card borderWidth={1} borderColor="$statusWaspada" backgroundColor="$statusWaspadaSoft" borderRadius="$4" paddingHorizontal="$4" paddingVertical="$3" role="status">
      <XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap">
        <Text color="$color" {...textStyles.bodySecondary} flex={1}>Data terbaru belum tersedia. Menampilkan data terakhir yang berhasil dimuat.</Text>
        <AppButton variant="ghost" size="sm" onPress={onRetry} ariaLabel="Coba perbarui data">Coba perbarui</AppButton>
      </XStack>
    </Card>
  );
}

function ActivityItem({ event }: { event: EventDTO }) {
  return (
    <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3" role="group" aria-label={`Aktivitas ${event.event_type}`}>
      <XStack alignItems="center" gap="$3">
        <YStack width={42} height={42} borderRadius="$6" backgroundColor="$primarySoft" alignItems="center" justifyContent="center">
          <Activity size={20} color="#0A9B62" strokeWidth={2.2} aria-hidden={true} />
        </YStack>
        <YStack flex={1} gap="$1">
          <Text color="$color" {...textStyles.cardTitle}>{event.event_type}</Text>
          <Text color="$colorMuted" {...textStyles.bodySecondary}>Device {event.device_id}</Text>
        </YStack>
        <StatusBadge status={null} compact />
      </XStack>
      <XStack justifyContent="space-between" gap="$3" flexWrap="wrap" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$3">
        <Text color="$colorMuted" {...textStyles.bodySecondary}>{event.severity}</Text>
        <Text color="$colorMuted" {...textStyles.metadata}>{formatTimestamp(event.started_at)}</Text>
      </XStack>
    </Card>
  );
}

export default function HomeRoute() {
  const router = useRouter();
  const media = useMedia();
  const overviewLoader = useCallback((signal: AbortSignal) => apiClient.fetchOverview(signal), []);
  const locationsLoader = useCallback((signal: AbortSignal) => apiClient.fetchLocations({}, signal), []);
  const overview = usePollingResource<OverviewResponse>(overviewLoader);
  const locationsResource = usePollingResource<LocationListResponse>(locationsLoader);
  const devicesLoader = useCallback((signal: AbortSignal) => apiClient.fetchDevices(signal), []);
  const devicesResource = usePollingResource<DeviceSummaryResponse>(devicesLoader);
  const [selectedLocationId, setSelectedLocationId] = useState<string | null>(null);
  const [locationPage, setLocationPage] = useState(1);
  const [devicePage, setDevicePage] = useState(1);
  const [monitoringMode, setMonitoringMode] = useState<HomeMonitoringMode>('locations');
  const [operationalFilter, setOperationalFilter] = useState<HomeOperationalFilter>(null);
  const [selectedAttentionLocationId, setSelectedAttentionLocationId] = useState<string | null>(null);

  const locations = locationsResource.data?.locations ?? [];
  const directory = useMemo(() => groupDeviceSummaries(devicesResource.data?.devices ?? []), [devicesResource.data]);
  const locationPageSize = media.wide ? 4 : media.tablet ? 2 : 3;
  const devicePageSize = media.wide ? 3 : media.tablet ? 2 : 1;
  const locationPages = paginate(locations, locationPage, locationPageSize);
  const preferredLocationId = overview.data?.priority_locations.find((priority) => locations.some((location) => location.id === priority.id))?.id;
  const defaultLocationId = preferredLocationId ?? locations[0]?.id ?? null;
  const affectedLocations = useMemo(
    () => operationalFilter ? getAffectedLocations(directory, operationalFilter) : [],
    [directory, operationalFilter],
  );

  useEffect(() => {
    setSelectedLocationId((current) => {
      if (current && locations.some((location) => location.id === current)) return current;
      return defaultLocationId;
    });
  }, [defaultLocationId, locations]);

  useEffect(() => {
    setLocationPage((current) => Math.min(current, locationPages.totalPages));
  }, [locationPages.totalPages]);

  useEffect(() => {
    setSelectedAttentionLocationId((current) => {
      if (current && affectedLocations.some((entry) => entry.location.id === current)) return current;
      return affectedLocations[0]?.location.id ?? null;
    });
  }, [affectedLocations]);

  const handleNavigate = useCallback((path: string) => router.push(path as never), [router]);
  const handleLocationSelect = useCallback((id: string) => {
    setSelectedLocationId(id);
    setDevicePage(1);
  }, []);
  const handleMonitoringMode = useCallback((mode: HomeMonitoringMode, filter: HomeOperationalFilter) => {
    setMonitoringMode(mode);
    setOperationalFilter(filter);
    setDevicePage(1);
    setLocationPage(1);
    setSelectedAttentionLocationId(null);
  }, []);
  const handleOpenDevice = useCallback((id: string) => handleNavigate(`/devices/${encodeURIComponent(id)}`), [handleNavigate]);
  const activity = useMemo(() => recentActivityItems(overview.data?.recent_activity ?? []), [overview.data?.recent_activity]);

  return (
    <AppShell title="Beranda" subtitle="Pantau tempat sampah pintar di berbagai lokasi, dalam satu sistem." activePath="/" onNavigate={handleNavigate}>
      <YStack gap="$6" paddingBottom="$6">
        <Card borderWidth={1} borderColor="$primarySoft" backgroundColor="$surface" borderRadius="$5" padding="$4" overflow="hidden">
          <XStack alignItems="center" gap="$4" flexWrap="wrap">
            <YStack flex={1} minWidth={260} gap="$3">
              <Text color="$primary" {...textStyles.overline} letterSpacing={1}>SIGAP MONITORING</Text>
              <XStack alignItems="center" gap="$2" flexWrap="wrap">
                <Text color="$color" {...textStyles.pageTitle} $compact={textStyles.pageTitleCompact}>Satu pandangan untuk semua lokasi.</Text>
              </XStack>
              <Text color="$colorMuted" {...textStyles.bodySecondary}>
                Pantau perangkat, temukan prioritas, dan tindak lanjuti kondisi terbaru dari satu dashboard.
              </Text>
              <AppButton size="md" onPress={() => handleNavigate('/locations')} ariaLabel="Lihat semua lokasi">
                Lihat semua lokasi <ArrowRight size={17} color="#FFFFFF" strokeWidth={2.3} aria-hidden={true} />
              </AppButton>
            </YStack>
            <RNImage source={homeHero} accessibilityLabel="Ilustrasi jaringan tempat sampah pintar" resizeMode="contain" style={{ width: 300, height: 150, maxWidth: '100%' }} />
          </XStack>
        </Card>

        {overview.stale && overview.data ? <StaleNotice onRetry={overview.retry} /> : null}
        {overview.loading && !overview.data ? <LoadingState label="Memuat ringkasan" rows={4} /> : null}
        {!overview.data && !overview.loading && overview.error ? <ErrorState onRetry={overview.retry} /> : null}

        {overview.data ? (
          <>
            <YStack gap="$3">
              <SectionHeader title="Ringkasan operasi" description="Kondisi terkini dari semua lokasi terhubung." />
              <XStack gap="$3" flexWrap="wrap">
                <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label={HOME_KPI_LABELS[0]} value={overview.data.total_locations} description="Lokasi terhubung" icon={MapPin} tone="green" selected={monitoringMode === 'locations'} onPress={() => handleMonitoringMode('locations', null)} /></YStack>
                <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label={HOME_KPI_LABELS[1]} value={overview.data.total_devices} description="Device terpantau" icon={Box} tone="blue" selected={monitoringMode === 'devices'} onPress={() => handleMonitoringMode('devices', null)} /></YStack>
                <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label={HOME_KPI_LABELS[2]} value={overview.data.pickup_required} description="Membutuhkan pengangkatan" icon={Truck} tone="orange" selected={operationalFilter === 'Angkat'} onPress={() => handleMonitoringMode('attention', 'Angkat')} /></YStack>
                <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label={HOME_KPI_LABELS[3]} value={overview.data.safety_alerts} description="Perlu perhatian segera" icon={ShieldAlert} tone="red" selected={operationalFilter === 'Safety Alert'} onPress={() => handleMonitoringMode('attention', 'Safety Alert')} /></YStack>
              </XStack>
            </YStack>

            {locationsResource.loading && !locationsResource.data ? <LoadingState label="Memuat lokasi" rows={locationPageSize} /> : null}
            {!locationsResource.data && !locationsResource.loading && locationsResource.error ? <ErrorState title="Lokasi tidak dapat dimuat" onRetry={locationsResource.retry} /> : null}
            {locationsResource.data && monitoringMode === 'locations' ? (
              <HomeLocationSelector
                locations={locations}
                selectedLocationId={selectedLocationId}
                page={locationPages.page}
                pageSize={locationPageSize}
                onSelect={handleLocationSelect}
                onOpenLocation={(id) => handleNavigate(`/locations/${encodeURIComponent(id)}`)}
                onPrevious={() => setLocationPage((current) => Math.max(1, current - 1))}
                onNext={() => setLocationPage((current) => Math.min(locationPages.totalPages, current + 1))}
              />
            ) : null}

            {locationsResource.data && monitoringMode === 'locations' && selectedLocationId ? (
              <HomeDeviceWorkspace
                location={null}
                locationId={selectedLocationId}
                page={devicePage}
                pageSize={devicePageSize}
                onPageChange={setDevicePage}
                onOpenLocation={(id) => handleNavigate(`/locations/${encodeURIComponent(id)}`)}
                onOpenDevice={handleOpenDevice}
              />
            ) : null}

            {monitoringMode !== 'locations' ? (
              devicesResource.loading && !directory.length ? <LoadingState label="Memuat perangkat" rows={devicePageSize} /> : devicesResource.error && !directory.length ? <ErrorState title="Daftar perangkat tidak dapat dimuat" onRetry={devicesResource.retry} /> : <HomeMonitoringWorkspace
                mode={monitoringMode}
                operationalFilter={operationalFilter}
                locations={directory}
                locationPage={locationPage}
                devicePage={devicePage}
                locationPageSize={locationPageSize}
                devicePageSize={devicePageSize}
                selectedAttentionLocationId={selectedAttentionLocationId}
                onLocationPageChange={setLocationPage}
                onDevicePageChange={setDevicePage}
                onSelectAttentionLocation={(id) => { setSelectedAttentionLocationId(id); setDevicePage(1); }}
                onOpenLocation={(id) => handleNavigate(`/locations/${encodeURIComponent(id)}`)}
                onOpenDevice={handleOpenDevice}
              />
            ) : null}

            <YStack gap="$3">
              <SectionHeader title="Aktivitas terbaru" description="Informasi terbaru dari perangkat yang terhubung." />
              {activity.length ? <XStack gap="$3" flexWrap="wrap">{activity.map((event) => <YStack key={event.id} width="100%" $wide={{ width: 'calc(33.333% - 8px)' }}><ActivityItem event={event} /></YStack>)}</XStack> : <EmptyState title="Belum ada aktivitas" description="Aktivitas perangkat akan muncul setelah data tersedia." />}
            </YStack>
          </>
        ) : null}
      </YStack>
    </AppShell>
  );
}
