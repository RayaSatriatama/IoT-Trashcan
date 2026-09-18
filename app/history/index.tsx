import { useRouter } from 'expo-router';
import { useCallback, useMemo, useState } from 'react';
import { Image as RNImage } from 'react-native';
import { Card, Input, Label, Select, Text, XStack, YStack, useMedia } from 'tamagui';

import historyArtwork from '../../assets/history_report_analytics_illustration.png';
import { AppShell } from '../../components/AppShell';
import { EmptyState } from '../../components/EmptyState';
import { ErrorState } from '../../components/ErrorState';
import { AppButton } from '../../components/AppButton';
import { KpiCard } from '../../components/KpiCard';
import { LoadingState } from '../../components/LoadingState';
import { DescriptionHint, SectionHeader } from '../../components/SectionHeader';
import { StatusBadge } from '../../components/StatusBadge';
import { apiClient } from '../../lib/api/client';
import { usePollingResource } from '../../lib/api/hooks';
import type {
  CanonicalStatus,
  HistoryCursor,
  HistoryItem,
  HistoryResponse,
  LocationListResponse,
} from '../../lib/api/types';
import { formatNullableMetric, formatTimestamp } from '../../lib/format';
import { Activity, Box, ShieldAlert, Truck } from '../../lib/ui/icons';
import { textStyles } from '../../lib/ui/typography';

export interface HistoryFilterValues {
  from?: string;
  to?: string;
  location_id?: string;
  device_id?: string;
  status?: CanonicalStatus;
}

export const HISTORY_PAGE_SIZE = 30;
const HISTORY_POLL_INTERVAL_MS = 60_000;

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

export interface DailyStatusCount {
  day: string;
  normal: number;
  pickup: number;
  safety: number;
}

export function groupHistoryByDay(items: HistoryItem[]): DailyStatusCount[] {
  const grouped = new Map<string, DailyStatusCount>();
  for (const item of items) {
    const day = item.recorded_at.slice(0, 10);
    const entry = grouped.get(day) ?? { day, normal: 0, pickup: 0, safety: 0 };
    const statusValue = effectiveHistoryStatus(item);
    if (statusValue === 'Safety Alert') entry.safety += 1;
    else if (statusValue === 'Angkat') entry.pickup += 1;
    else if (statusValue === 'Normal') entry.normal += 1;
    grouped.set(day, entry);
  }
  return Array.from(grouped.values()).sort((a, b) => a.day.localeCompare(b.day));
}

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
        <Text color="$color" {...textStyles.bodySecondary} flex={1}>
          Data terbaru belum tersedia. Menampilkan hasil riwayat terakhir yang berhasil dimuat.
        </Text>
        <AppButton variant="ghost" size="sm" onPress={onRetry} ariaLabel="Coba perbarui riwayat">
          Coba perbarui
        </AppButton>
      </XStack>
    </Card>
  );
}

function HistorySummary({ item, compact }: { item: HistoryItem; compact: boolean }) {
  return (
    <XStack gap="$4" flexWrap="wrap" flexDirection={compact ? 'column' : 'row'} flex={1}>
      <YStack minWidth={160} flex={1} gap="$1">
        <Text color="$colorMuted" {...textStyles.caption}>Waktu</Text>
        <Text color="$color" {...textStyles.metadata}>{formatTimestamp(item.recorded_at)}</Text>
      </YStack>
      <YStack minWidth={160} flex={1} gap="$1">
        <Text color="$colorMuted" {...textStyles.caption}>Lokasi</Text>
        <Text color="$color" {...textStyles.metadata}>{item.location_name || HISTORY_MISSING_VALUE}</Text>
      </YStack>
      <YStack minWidth={160} flex={1} gap="$1">
        <Text color="$colorMuted" {...textStyles.caption}>Device</Text>
        <Text color="$color" {...textStyles.metadata}>{item.device_code || HISTORY_MISSING_VALUE}</Text>
        <Text color="$colorMuted" {...textStyles.caption}>ID: {item.device_id}</Text>
      </YStack>
      <YStack minWidth={100} gap="$1">
        <Text color="$colorMuted" {...textStyles.caption}>Stream</Text>
        <Text color="$color" {...textStyles.metadata}>{item.stream}</Text>
      </YStack>
    </XStack>
  );
}

function HistoryDetails({ item, compact }: { item: HistoryItem; compact: boolean }) {
  return (
    <XStack gap="$4" flexWrap="wrap" flexDirection={compact ? 'column' : 'row'} alignItems="flex-start">
      <YStack minWidth={130} gap="$1">
        <Text color="$colorMuted" {...textStyles.caption}>Status</Text>
        <StatusBadge status={effectiveHistoryStatus(item)} compact />
      </YStack>
      <YStack minWidth={170} flex={1} gap="$1">
        <Text color="$colorMuted" {...textStyles.caption}>Alasan</Text>
        <Text color="$color" {...textStyles.bodySecondary}>{item.dominant_reason || HISTORY_MISSING_VALUE}</Text>
      </YStack>
      <YStack minWidth={130} gap="$1">
        <Text color="$colorMuted" {...textStyles.caption}>Keselamatan</Text>
        <Text color="$color" {...textStyles.bodySecondary}>{item.safety_state || HISTORY_MISSING_VALUE}</Text>
      </YStack>
      <YStack minWidth={130} gap="$1">
        <Text color="$colorMuted" {...textStyles.caption}>FIS score</Text>
        <Text color="$color" {...textStyles.bodySecondary}>{formatNullableMetric(item.fis_score)}</Text>
      </YStack>
    </XStack>
  );
}

function HistoryRow({ item, compact }: { item: HistoryItem; compact: boolean }) {
  if (!compact) {
    return (
      <XStack
        alignItems="center"
        gap="$4"
        paddingHorizontal="$4"
        paddingVertical="$3"
        borderBottomWidth={1}
        borderBottomColor="$borderColor"
        role="group"
        aria-label={`Riwayat ${item.device_code} ${item.stream}`}
      >
        <YStack width={178} gap="$1">
          <Text color="$color" {...textStyles.metadata} fontWeight="700">{item.device_code}</Text>
          <Text color="$colorMuted" {...textStyles.caption}>{item.placement_label || HISTORY_MISSING_VALUE}</Text>
        </YStack>
        <YStack flex={1} minWidth={150} gap="$1">
          <Text color="$color" {...textStyles.metadata} fontWeight="600">{item.location_name || HISTORY_MISSING_VALUE}</Text>
          <Text color="$colorMuted" {...textStyles.caption}>{item.stream}</Text>
        </YStack>
        <YStack width={142} gap="$1">
          <StatusBadge status={effectiveHistoryStatus(item)} compact />
          <Text color="$colorMuted" {...textStyles.caption}>{item.safety_state || HISTORY_MISSING_VALUE}</Text>
        </YStack>
        <YStack width={156} gap="$1">
          <Text color="$color" {...textStyles.bodySecondary}>{formatTimestamp(item.recorded_at)}</Text>
          <Text color="$colorMuted" {...textStyles.caption}>FIS {formatNullableMetric(item.fis_score)}</Text>
        </YStack>
        <Text color="$colorMuted" {...textStyles.caption} width={190} numberOfLines={2}>
          {item.dominant_reason || HISTORY_MISSING_VALUE}
        </Text>
      </XStack>
    );
  }

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
        <HistorySummary item={item} compact />
        <YStack minWidth={130} alignItems="flex-start" gap="$1">
          <Text color="$colorMuted" {...textStyles.caption}>Status</Text>
          <StatusBadge status={effectiveHistoryStatus(item)} compact />
        </YStack>
      </XStack>
      <HistoryDetails item={item} compact />
      <XStack gap="$4" flexWrap="wrap" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$3">
        <Text color="$colorMuted" {...textStyles.bodySecondary}>Fill: {formatNullableMetric(item.fill, '%')}</Text>
        <Text color="$colorMuted" {...textStyles.bodySecondary}>Berat: {formatNullableMetric(item.weight, ' kg')}</Text>
        <Text color="$colorMuted" {...textStyles.bodySecondary}>Utilisasi: {formatNullableMetric(item.weight_utilization, '%')}</Text>
        <Text color="$colorMuted" {...textStyles.bodySecondary}>OGI: {formatNullableMetric(item.ogi)}</Text>
      </XStack>
    </Card>
  );
}

function HistoryTrend({ items }: { items: HistoryItem[] }) {
  const days = groupHistoryByDay(items).slice(-7);
  const maximum = Math.max(...days.map((day) => day.normal + day.pickup + day.safety), 1);

  return (
    <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$4">
      <XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap">
        <SectionHeader title="Trend status" description="Distribusi status pada halaman yang sedang ditampilkan." />
        <XStack gap="$3" flexWrap="wrap">
          <XStack alignItems="center" gap="$1"><YStack width={8} height={8} borderRadius="$6" backgroundColor="$statusNormal" /><Text color="$colorMuted" {...textStyles.caption}>Normal</Text></XStack>
          <XStack alignItems="center" gap="$1"><YStack width={8} height={8} borderRadius="$6" backgroundColor="$statusAngkat" /><Text color="$colorMuted" {...textStyles.caption}>Diangkat</Text></XStack>
          <XStack alignItems="center" gap="$1"><YStack width={8} height={8} borderRadius="$6" backgroundColor="$statusSafetyAlert" /><Text color="$colorMuted" {...textStyles.caption}>Safety</Text></XStack>
        </XStack>
      </XStack>
      <XStack alignItems="flex-end" justifyContent="space-between" gap="$3" height={142}>
        {days.map((day) => {
          const total = day.normal + day.pickup + day.safety;
          return (
            <YStack key={day.day} flex={1} alignItems="center" justifyContent="flex-end" gap="$2">
              <YStack width="70%" maxWidth={36} height={Math.max(10, (total / maximum) * 104)} borderRadius="$3" overflow="hidden" backgroundColor="$surfaceMuted">
                <YStack flex={day.safety} backgroundColor="$statusSafetyAlert" />
                <YStack flex={day.pickup} backgroundColor="$statusAngkat" />
                <YStack flex={day.normal} backgroundColor="$statusNormal" />
              </YStack>
              <Text color="$colorMuted" {...textStyles.caption}>{day.day.slice(5)}</Text>
            </YStack>
          );
        })}
      </XStack>
    </Card>
  );
}

function HistoryTableHeader() {
  return (
    <XStack alignItems="center" gap="$4" paddingHorizontal="$4" paddingVertical="$2" backgroundColor="$surfaceMuted">
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={178}>DEVICE</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" flex={1} minWidth={150}>LOKASI / STREAM</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={142}>STATUS</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={156}>WAKTU</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={190}>DETAIL</Text>
    </XStack>
  );
}

function LocationFilter({ value, onChange }: { value: string; onChange: (value: string) => void }) {
  const loader = useCallback((signal: AbortSignal) => apiClient.fetchLocations({}, signal), []);
  const { data } = usePollingResource<LocationListResponse>(loader, 60_000);
  const locations = data?.locations ?? [];

  return (
    <YStack width="100%" $tablet={{ width: 'calc(50% - 8px)' }} $wide={{ width: 'calc(20% - 13px)' }} gap="$2">
      <Text color="$color" {...textStyles.caption} fontWeight="700">Lokasi</Text>
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
    <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3">
      <XStack gap="$3" flexWrap="wrap">
        <YStack width="100%" $tablet={{ width: 'calc(50% - 8px)' }} $wide={{ width: 'calc(20% - 13px)' }} gap="$2">
          <Label htmlFor="history-from" color="$color" {...textStyles.caption} fontWeight="700">Dari tanggal</Label>
          <Input id="history-from" type="date" value={filters.fromDate} onChangeText={(value) => onChange({ fromDate: value })} minHeight={48} fontSize={16} borderColor="$borderColor" backgroundColor="$background" />
        </YStack>
        <YStack width="100%" $tablet={{ width: 'calc(50% - 8px)' }} $wide={{ width: 'calc(20% - 13px)' }} gap="$2">
          <Label htmlFor="history-to" color="$color" {...textStyles.caption} fontWeight="700">Sampai tanggal</Label>
          <Input id="history-to" type="date" value={filters.toDate} onChangeText={(value) => onChange({ toDate: value })} minHeight={48} fontSize={16} borderColor="$borderColor" backgroundColor="$background" />
        </YStack>
        <LocationFilter value={filters.location_id ?? ''} onChange={(location_id) => onChange({ location_id })} />
        <YStack width="100%" $tablet={{ width: 'calc(50% - 8px)' }} $wide={{ width: 'calc(20% - 13px)' }} gap="$2">
          <Label htmlFor="history-device" color="$color" {...textStyles.caption} fontWeight="700">Device ID (internal)</Label>
          <Input id="history-device" value={filters.device_id ?? ''} onChangeText={(value) => onChange({ device_id: value })} placeholder="Contoh: dev-a" minHeight={48} fontSize={16} borderColor="$borderColor" backgroundColor="$background" />
        </YStack>
        <YStack width="100%" $tablet={{ width: 'calc(50% - 8px)' }} $wide={{ width: 'calc(20% - 13px)' }} gap="$2">
          <Text color="$color" {...textStyles.caption} fontWeight="700">Status</Text>
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
      <XStack gap="$3" flexWrap="wrap" justifyContent="flex-end" paddingTop="$1">
        <AppButton size="md" variant="secondary" onPress={onReset} ariaLabel="Reset filter">Reset filter</AppButton>
        <AppButton size="md" variant="primary" onPress={onExport} ariaLabel="Unduh CSV">Unduh CSV</AppButton>
      </XStack>
    </Card>
  );
}

function PaginationControls({
  pageNumber,
  canGoNewer,
  canGoOlder,
  onNewer,
  onOlder,
}: {
  pageNumber: number;
  canGoNewer: boolean;
  canGoOlder: boolean;
  onNewer: () => void;
  onOlder: () => void;
}) {
  return (
    <XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap">
      <Text color="$colorMuted" {...textStyles.bodySecondary}>Halaman {pageNumber} · maksimum {HISTORY_PAGE_SIZE} pembacaan</Text>
      <XStack gap="$2">
        <AppButton size="md" variant="secondary" disabled={!canGoNewer} onPress={onNewer} ariaLabel="Tampilkan riwayat yang lebih baru">
          Lebih baru
        </AppButton>
        <AppButton size="md" variant="primary" disabled={!canGoOlder} onPress={onOlder} ariaLabel="Tampilkan riwayat yang lebih lama">
          Lebih lama
        </AppButton>
      </XStack>
    </XStack>
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
  const [cursorStack, setCursorStack] = useState<HistoryCursor[]>([]);

  const dateRange = dateInputToUtcRange(fromDate, toDate);
  const filters = useMemo<HistoryFilterValues>(() => ({
    from: dateRange.from,
    to: dateRange.to,
    location_id: locationId || undefined,
    device_id: deviceId.trim() || undefined,
    status,
  }), [dateRange.from, dateRange.to, locationId, deviceId, status]);
  const activeCursor = cursorStack.length ? cursorStack[cursorStack.length - 1] : undefined;
  const loader = useCallback(
    (signal: AbortSignal) => apiClient.fetchHistory(filters, { limit: HISTORY_PAGE_SIZE, before: activeCursor }, signal),
    [activeCursor, filters],
  );
  const { data, loading, error, stale, retry } = usePollingResource<HistoryResponse>(loader, HISTORY_POLL_INTERVAL_MS);
  const displayFilters = useMemo(() => ({ ...filters, fromDate, toDate }), [filters, fromDate, toDate]);
  const pageNumber = cursorStack.length + 1;

  const summary = useMemo(() => {
    if (!data) return null;
    const items = data.items;
    return {
      total: items.length,
      normal: items.filter((item) => effectiveHistoryStatus(item) === 'Normal').length,
      pickup: items.filter((item) => effectiveHistoryStatus(item) === 'Angkat').length,
      safety: items.filter((item) => effectiveHistoryStatus(item) === 'Safety Alert').length,
    };
  }, [data]);

  const handleNavigate = useCallback((path: string) => router.push(path as never), [router]);
  const resetPage = useCallback(() => setCursorStack([]), []);
  const handleFilterChange = useCallback((patch: Partial<HistoryFilterValues & { fromDate: string; toDate: string }>) => {
    resetPage();
    if (patch.fromDate !== undefined) setFromDate(patch.fromDate);
    if (patch.toDate !== undefined) setToDate(patch.toDate);
    if (patch.location_id !== undefined) setLocationId(patch.location_id ?? '');
    if (patch.device_id !== undefined) setDeviceId(patch.device_id ?? '');
    if (patch.status !== undefined || 'status' in patch) setStatus(patch.status);
  }, [resetPage]);
  const handleReset = useCallback(() => {
    setFromDate('');
    setToDate('');
    setLocationId('');
    setDeviceId('');
    setStatus(undefined);
    resetPage();
  }, [resetPage]);
  const handleExport = useCallback(() => {
    const url = apiClient.buildExportURL(filters);
    if (typeof window !== 'undefined') window.open(url, '_blank', 'noopener,noreferrer');
  }, [filters]);
  const handleOlder = useCallback(() => {
    if (!data?.page.has_more || !data.page.next) return;
    setCursorStack((current) => [...current, data.page.next as HistoryCursor]);
  }, [data]);
  const handleNewer = useCallback(() => {
    setCursorStack((current) => current.slice(0, -1));
  }, []);

  return (
    <AppShell
      title="Riwayat"
      subtitle="Telusuri pembacaan dan keputusan untuk audit operasional."
      activePath="/history"
      onNavigate={handleNavigate}
    >
      <YStack gap="$6" paddingBottom="$6">
        <Card borderWidth={1} borderColor="$primarySoft" backgroundColor="$surface" borderRadius="$5" padding="$5" overflow="hidden">
          <XStack alignItems="center" gap="$5" flexWrap="wrap">
            <YStack flex={1} minWidth={250} gap="$2">
              <Text color="$primary" fontSize="$2" fontWeight="800">AUDIT DAN LAPORAN</Text>
              <XStack alignItems="center" gap="$2" flexWrap="wrap">
                <Text color="$color" fontSize="$7" lineHeight="$7" fontWeight="800" letterSpacing={-0.8}>Riwayat pembacaan yang mudah ditindaklanjuti.</Text>
                <DescriptionHint
                  description="Gunakan filter untuk menelusuri data sensor dan keputusan operasional dari semua lokasi."
                  ariaLabel="Lihat informasi tentang riwayat dan laporan"
                />
              </XStack>
            </YStack>
            <RNImage source={historyArtwork} accessibilityLabel="Ilustrasi analisis riwayat" resizeMode="contain" style={{ width: 210, height: 112, maxWidth: '100%' }} />
          </XStack>
        </Card>

        <XStack gap="$3" flexWrap="wrap">
          <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Ditampilkan" value={summary ? summary.total : '—'} icon={Activity} tone="blue" /></YStack>
          <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Normal" value={summary ? summary.normal : '—'} icon={Box} tone="green" /></YStack>
          <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Perlu diangkat" value={summary ? summary.pickup : '—'} icon={Truck} tone="orange" /></YStack>
          <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Safety Alert" value={summary ? summary.safety : '—'} icon={ShieldAlert} tone="red" /></YStack>
        </XStack>

        {data && data.items.length ? <HistoryTrend items={data.items} /> : null}
        <Filters filters={displayFilters} onChange={handleFilterChange} onReset={handleReset} onExport={handleExport} />

        {stale && data ? <StaleNotice onRetry={retry} /> : null}
        {loading && !data ? <LoadingState label="Memuat riwayat" rows={4} /> : null}
        {!data && !loading && error ? <ErrorState onRetry={retry} /> : null}

        {data ? (
          data.items.length ? (
            <YStack gap="$3">
              <XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap">
                <XStack alignItems="center" gap="$2" flexWrap="wrap">
                  <Text color="$color" fontSize="$5" fontWeight="700">{data.items.length} pembacaan · halaman {pageNumber}</Text>
                  <DescriptionHint
                    description="Riwayat interaktif dibatasi per halaman agar tetap responsif. CSV tetap mengekspor seluruh hasil filter."
                    ariaLabel="Lihat informasi tentang pagination riwayat"
                  />
                </XStack>
              </XStack>

              {media.wide ? (
                <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" overflow="hidden">
                  <HistoryTableHeader />
                  {data.items.map((item) => <HistoryRow key={`${item.reading_id}-${item.slot_id}`} item={item} compact={false} />)}
                </Card>
              ) : (
                data.items.map((item) => <HistoryRow key={`${item.reading_id}-${item.slot_id}`} item={item} compact />)
              )}

              <PaginationControls
                pageNumber={pageNumber}
                canGoNewer={cursorStack.length > 0}
                canGoOlder={data.page.has_more && data.page.next !== null}
                onNewer={handleNewer}
                onOlder={handleOlder}
              />
            </YStack>
          ) : (
            <EmptyState title={HISTORY_EMPTY_COPY.title} description={HISTORY_EMPTY_COPY.description} />
          )
        ) : null}
      </YStack>
    </AppShell>
  );
}
