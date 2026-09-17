import { useRouter } from 'expo-router';
import { useCallback, useMemo, useState } from 'react';
import { Image as RNImage } from 'react-native';
import { Button, Card, Input, Label, Select, Text, XStack, YStack, useMedia } from 'tamagui';

import historyArtwork from '../../assets/history_report_analytics_illustration.png';
import { AppShell } from '../../components/AppShell';
import { EmptyState } from '../../components/EmptyState';
import { ErrorState } from '../../components/ErrorState';
import { LoadingState } from '../../components/LoadingState';
import { StatusBadge } from '../../components/StatusBadge';
import { apiClient } from '../../lib/api/client';
import { usePollingResource } from '../../lib/api/hooks';
import type { CanonicalStatus, HistoryItem, HistoryResponse, LocationListResponse } from '../../lib/api/types';
import { formatNullableMetric, formatTimestamp } from '../../lib/format';

export interface HistoryFilterValues {
  from?: string;
  to?: string;
  location_id?: string;
  device_id?: string;
  status?: CanonicalStatus;
}

export const HISTORY_STATUS_FILTERS: ReadonlyArray<{ label: string; value: CanonicalStatus | '' }> = [
  { label: 'Semua status', value: '' },
  { label: 'Normal', value: 'Normal' },
  { label: 'Waspada', value: 'Waspada' },
  { label: 'Perlu diangkat', value: 'Angkat' },
  { label: 'Peringatan keselamatan', value: 'Safety Alert' },
  { label: 'Data tidak valid', value: 'Data Invalid' },
];

export function serializeHistoryFilters(filters: HistoryFilterValues): string {
  const query = new URLSearchParams();
  if (filters.from) query.set('from', filters.from);
  if (filters.to) query.set('to', filters.to);
  if (filters.location_id) query.set('location_id', filters.location_id);
  if (filters.device_id) query.set('device_id', filters.device_id);
  if (filters.status) query.set('status', filters.status);
  return query.toString();
}

export function buildHistoryExportHref(filters: HistoryFilterValues): string {
  const query = serializeHistoryFilters(filters);
  return query ? `/api/export.csv?${query}` : '/api/export.csv';
}

export function dateInputToUtcRange(fromDate: string, toDate: string): Pick<HistoryFilterValues, 'from' | 'to'> {
  const from = fromDate ? new Date(`${fromDate}T00:00:00.000Z`).toISOString() : undefined;
  let to: string | undefined;
  if (toDate) {
    const end = new Date(`${toDate}T00:00:00.000Z`);
    end.setUTCDate(end.getUTCDate() + 1);
    to = end.toISOString();
  }
  return { from, to };
}

export const HISTORY_EMPTY_COPY = {
  title: 'Belum ada riwayat pembacaan',
  description: 'Coba ubah rentang tanggal atau filter untuk melihat data audit yang tersedia.',
} as const;

export const HISTORY_MISSING_VALUE = 'Belum tersedia';

export function effectiveHistoryStatus(
  item: Pick<HistoryItem, 'fis_status' | 'safety_state'>,
): CanonicalStatus | null {
  if (item.safety_state === 'Safety Alert') return 'Safety Alert';
  return item.fis_status;
}

function StaleNotice({ onRetry }: { onRetry: () => void }) {
  return (
    <Card
      borderWidth={1}
      borderColor="$statusWaspada"
      backgroundColor="$statusWaspadaSoft"
      borderRadius="$4"
      paddingHorizontal="$4"
      paddingVertical="$3"
      role="status"
      aria-label="Data mungkin sudah lama"
    >
      <XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap">
        <Text color="$color" fontSize="$2" flex={1}>
          Data terbaru belum tersedia. Menampilkan hasil riwayat terakhir yang berhasil dimuat.
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
          aria-label="Coba perbarui riwayat"
        >
          Coba perbarui
        </Button>
      </XStack>
    </Card>
  );
}

function HistorySummary({ item, compact }: { item: HistoryItem; compact: boolean }) {
  return (
    <XStack gap="$4" flexWrap="wrap" flexDirection={compact ? 'column' : 'row'} flex={1}>
      <YStack minWidth={160} flex={1} gap="$1">
        <Text color="$colorMuted" fontSize="$1">Waktu</Text>
        <Text color="$color" fontSize="$2" fontWeight="600">{formatTimestamp(item.recorded_at)}</Text>
      </YStack>
      <YStack minWidth={160} flex={1} gap="$1">
        <Text color="$colorMuted" fontSize="$1">Lokasi</Text>
        <Text color="$color" fontSize="$2" fontWeight="600">{item.location_name || HISTORY_MISSING_VALUE}</Text>
      </YStack>
      <YStack minWidth={160} flex={1} gap="$1">
        <Text color="$colorMuted" fontSize="$1">Device</Text>
        <Text color="$color" fontSize="$2" fontWeight="600">{item.device_code || HISTORY_MISSING_VALUE}</Text>
        <Text color="$colorMuted" fontSize="$1">ID: {item.device_id}</Text>
      </YStack>
      <YStack minWidth={100} gap="$1">
        <Text color="$colorMuted" fontSize="$1">Stream</Text>
        <Text color="$color" fontSize="$2" fontWeight="600">{item.stream}</Text>
      </YStack>
    </XStack>
  );
}

function HistoryDetails({ item, compact }: { item: HistoryItem; compact: boolean }) {
  return (
    <XStack gap="$4" flexWrap="wrap" flexDirection={compact ? 'column' : 'row'} alignItems="flex-start">
      <YStack minWidth={130} gap="$1">
        <Text color="$colorMuted" fontSize="$1">Status</Text>
        <StatusBadge status={effectiveHistoryStatus(item)} compact />
      </YStack>
      <YStack minWidth={170} flex={1} gap="$1">
        <Text color="$colorMuted" fontSize="$1">Alasan</Text>
        <Text color="$color" fontSize="$2">{item.dominant_reason || HISTORY_MISSING_VALUE}</Text>
      </YStack>
      <YStack minWidth={130} gap="$1">
        <Text color="$colorMuted" fontSize="$1">Keselamatan</Text>
        <Text color="$color" fontSize="$2">{item.safety_state || HISTORY_MISSING_VALUE}</Text>
      </YStack>
      <YStack minWidth={130} gap="$1">
        <Text color="$colorMuted" fontSize="$1">FIS score</Text>
        <Text color="$color" fontSize="$2">{formatNullableMetric(item.fis_score)}</Text>
      </YStack>
    </XStack>
  );
}

function HistoryRow({ item, compact }: { item: HistoryItem; compact: boolean }) {
  return (
    <Card
      borderWidth={1}
      borderColor="$borderColor"
      backgroundColor="$surface"
      borderRadius="$4"
      padding="$4"
      gap="$4"
      role="group"
      aria-label={`Riwayat ${item.device_code} ${item.stream}`}
    >
      <XStack gap="$4" alignItems="flex-start" flexWrap="wrap">
        <HistorySummary item={item} compact={compact} />
        <YStack minWidth={130} alignItems={compact ? 'flex-start' : 'flex-end'} gap="$1">
          <Text color="$colorMuted" fontSize="$1">Status</Text>
          <StatusBadge status={effectiveHistoryStatus(item)} compact />
        </YStack>
      </XStack>
      <HistoryDetails item={item} compact={compact} />
      <XStack gap="$4" flexWrap="wrap" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$3">
        <Text color="$colorMuted" fontSize="$2">Fill: {formatNullableMetric(item.fill, '%')}</Text>
        <Text color="$colorMuted" fontSize="$2">Berat: {formatNullableMetric(item.weight, ' kg')}</Text>
        <Text color="$colorMuted" fontSize="$2">Utilisasi: {formatNullableMetric(item.weight_utilization, '%')}</Text>
        <Text color="$colorMuted" fontSize="$2">OGI: {formatNullableMetric(item.ogi)}</Text>
      </XStack>
    </Card>
  );
}

function LocationFilter({ value, onChange }: { value: string; onChange: (value: string) => void }) {
  const loader = useCallback((signal: AbortSignal) => apiClient.fetchLocations({}, signal), []);
  const { data } = usePollingResource<LocationListResponse>(loader, 60_000);
  const locations = data?.locations ?? [];
  return (
    <YStack flex={1} minWidth={220} gap="$2">
      <Text color="$color" fontSize="$2" fontWeight="700">Lokasi</Text>
      <Select value={value || 'all'} onValueChange={(next) => onChange(next === 'all' ? '' : next)} name="history-location">
        <Select.Trigger aria-label="Filter lokasi" minHeight={48} width="100%" borderColor="$borderColor" backgroundColor="$background">
          <Select.Value placeholder="Semua lokasi" />
        </Select.Trigger>
        <Select.Content>
          <Select.Viewport minWidth={280}>
            <Select.Item value="all" index={0}><Select.ItemText>Semua lokasi</Select.ItemText><Select.ItemIndicator /></Select.Item>
            {locations.map((location, index) => (
              <Select.Item key={location.id} value={location.id} index={index + 1}>
                <Select.ItemText>{location.name}</Select.ItemText>
                <Select.ItemIndicator />
              </Select.Item>
            ))}
          </Select.Viewport>
        </Select.Content>
      </Select>
    </YStack>
  );
}

function Filters({ filters, onChange, onReset, onExport }: {
  filters: HistoryFilterValues & { fromDate: string; toDate: string };
  onChange: (patch: Partial<HistoryFilterValues & { fromDate: string; toDate: string }>) => void;
  onReset: () => void;
  onExport: () => void;
}) {
  return (
    <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$4">
      <XStack gap="$4" flexWrap="wrap">
        <YStack flex={1} minWidth={180} gap="$2">
          <Label htmlFor="history-from" color="$color" fontSize="$2" fontWeight="700">Dari tanggal</Label>
          <Input id="history-from" type="date" value={filters.fromDate} onChangeText={(value) => onChange({ fromDate: value })} minHeight={48} borderColor="$borderColor" backgroundColor="$background" />
        </YStack>
        <YStack flex={1} minWidth={180} gap="$2">
          <Label htmlFor="history-to" color="$color" fontSize="$2" fontWeight="700">Sampai tanggal</Label>
          <Input id="history-to" type="date" value={filters.toDate} onChangeText={(value) => onChange({ toDate: value })} minHeight={48} borderColor="$borderColor" backgroundColor="$background" />
        </YStack>
        <LocationFilter value={filters.location_id ?? ''} onChange={(location_id) => onChange({ location_id })} />
        <YStack flex={1} minWidth={220} gap="$2">
          <Label htmlFor="history-device" color="$color" fontSize="$2" fontWeight="700">Device ID (internal)</Label>
          <Input id="history-device" value={filters.device_id ?? ''} onChangeText={(value) => onChange({ device_id: value })} placeholder="Contoh: dev-a" minHeight={48} borderColor="$borderColor" backgroundColor="$background" />
        </YStack>
        <YStack flex={1} minWidth={220} gap="$2">
          <Text color="$color" fontSize="$2" fontWeight="700">Status</Text>
          <Select value={filters.status || 'all'} onValueChange={(value) => onChange({ status: value === 'all' ? undefined : value as CanonicalStatus })} name="history-status">
            <Select.Trigger aria-label="Filter status" minHeight={48} width="100%" borderColor="$borderColor" backgroundColor="$background">
              <Select.Value placeholder="Semua status" />
            </Select.Trigger>
            <Select.Content>
              <Select.Viewport minWidth={280}>
                {HISTORY_STATUS_FILTERS.map((filter, index) => (
                  <Select.Item key={filter.value || 'all'} value={filter.value || 'all'} index={index}>
                    <Select.ItemText>{filter.label}</Select.ItemText>
                    <Select.ItemIndicator />
                  </Select.Item>
                ))}
              </Select.Viewport>
            </Select.Content>
          </Select>
        </YStack>
      </XStack>
      <XStack gap="$3" flexWrap="wrap" justifyContent="flex-end">
        <Button minHeight={48} borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" color="$color" onPress={onReset} aria-label="Reset filter">Reset filter</Button>
        <Button minHeight={48} backgroundColor="$primary" color="$white" onPress={onExport} aria-label="Unduh CSV">Unduh CSV</Button>
      </XStack>
    </Card>
  );
}

export default function HistoryRoute() {
  const router = useRouter();
  const media = useMedia();
  const [fromDate, setFromDate] = useState('');
  const [toDate, setToDate] = useState('');
  const [locationId, setLocationId] = useState('');
  const [deviceId, setDeviceId] = useState('');
  const [status, setStatus] = useState<CanonicalStatus | undefined>();
  const dateRange = dateInputToUtcRange(fromDate, toDate);
  const filters = useMemo<HistoryFilterValues>(() => ({
    from: dateRange.from,
    to: dateRange.to,
    location_id: locationId || undefined,
    device_id: deviceId.trim() || undefined,
    status,
  }), [dateRange.from, dateRange.to, locationId, deviceId, status]);
  const loader = useCallback((signal: AbortSignal) => apiClient.fetchHistory(filters, signal), [filters]);
  const { data, loading, error, stale, retry } = usePollingResource<HistoryResponse>(loader);
  const displayFilters = useMemo(() => ({ ...filters, fromDate, toDate }), [filters, fromDate, toDate]);
  const summary = useMemo(() => {
    const items = data?.items ?? [];
    return {
      total: items.length,
      normal: items.filter((item) => effectiveHistoryStatus(item) === 'Normal').length,
      pickup: items.filter((item) => effectiveHistoryStatus(item) === 'Angkat').length,
      safety: items.filter((item) => effectiveHistoryStatus(item) === 'Safety Alert').length,
    };
  }, [data]);

  const handleNavigate = useCallback((path: string) => router.push(path as never), [router]);
  const handleReset = useCallback(() => {
    setFromDate('');
    setToDate('');
    setLocationId('');
    setDeviceId('');
    setStatus(undefined);
  }, []);
  const handleExport = useCallback(() => {
    const url = apiClient.buildExportURL(filters);
    if (typeof window !== 'undefined') window.open(url, '_blank', 'noopener,noreferrer');
  }, [filters]);

  return (
    <AppShell
      title="Riwayat"
      subtitle="Telusuri pembacaan dan keputusan untuk audit operasional."
      activePath="/history"
      onNavigate={handleNavigate}
    >
      <YStack gap="$6" paddingBottom="$6">
        <Card borderWidth={1} borderColor="$primarySoft" backgroundColor="$surface" borderRadius="$5" padding="$6" overflow="hidden"><XStack alignItems="center" gap="$5" flexWrap="wrap"><YStack flex={1} minWidth={250} gap="$3"><Text color="$primary" fontSize="$2" fontWeight="800">AUDIT DAN LAPORAN</Text><Text color="$color" fontSize="$7" lineHeight="$7" fontWeight="800" letterSpacing={-0.8}>Riwayat pembacaan yang mudah ditindaklanjuti.</Text><Text color="$colorMuted" fontSize="$3">Gunakan filter untuk menelusuri data sensor dan keputusan operasional dari semua lokasi.</Text></YStack><RNImage source={historyArtwork} accessibilityLabel="Ilustrasi analisis riwayat" resizeMode="contain" style={{ width: 240, height: 140, maxWidth: '100%' }} /></XStack></Card>
        <XStack gap="$3" flexWrap="wrap"><Card width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }} borderWidth={1} borderColor="$borderColor" backgroundColor="$blueSoft" borderRadius="$4" padding="$4"><Text color="$blue" fontSize="$7" fontWeight="800">{summary.total}</Text><Text color="$colorMuted" fontSize="$2">Total pembacaan</Text></Card><Card width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }} borderWidth={1} borderColor="$borderColor" backgroundColor="$primarySoft" borderRadius="$4" padding="$4"><Text color="$primary" fontSize="$7" fontWeight="800">{summary.normal}</Text><Text color="$colorMuted" fontSize="$2">Normal</Text></Card><Card width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }} borderWidth={1} borderColor="$borderColor" backgroundColor="$orangeSoft" borderRadius="$4" padding="$4"><Text color="$statusAngkat" fontSize="$7" fontWeight="800">{summary.pickup}</Text><Text color="$colorMuted" fontSize="$2">Perlu diangkat</Text></Card><Card width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }} borderWidth={1} borderColor="$borderColor" backgroundColor="$redSoft" borderRadius="$4" padding="$4"><Text color="$statusSafetyAlert" fontSize="$7" fontWeight="800">{summary.safety}</Text><Text color="$colorMuted" fontSize="$2">Safety Alert</Text></Card></XStack>
        <Filters filters={displayFilters} onChange={(patch) => {
          if (patch.fromDate !== undefined) setFromDate(patch.fromDate);
          if (patch.toDate !== undefined) setToDate(patch.toDate);
          if (patch.location_id !== undefined) setLocationId(patch.location_id ?? '');
          if (patch.device_id !== undefined) setDeviceId(patch.device_id ?? '');
          if (patch.status !== undefined || 'status' in patch) setStatus(patch.status);
        }} onReset={handleReset} onExport={handleExport} />

        {stale && data ? <StaleNotice onRetry={retry} /> : null}
        {loading && !data ? <LoadingState label="Memuat riwayat" rows={4} /> : null}
        {!data && !loading && error ? <ErrorState onRetry={retry} /> : null}

        {data ? (
          data.items.length ? (
            <YStack gap="$3">
              <XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap">
                <YStack gap="$1">
                  <Text color="$color" fontSize="$5" fontWeight="700">{data.items.length} pembacaan</Text>
                  <Text color="$colorMuted" fontSize="$2">Urut terbaru berdasarkan waktu perekaman.</Text>
                </YStack>
              </XStack>
              {data.items.map((item) => <HistoryRow key={`${item.reading_id}-${item.slot_id}`} item={item} compact={!media.wide} />)}
            </YStack>
          ) : (
            <EmptyState title={HISTORY_EMPTY_COPY.title} description={HISTORY_EMPTY_COPY.description} />
          )
        ) : null}
      </YStack>
    </AppShell>
  );
}
