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
import { Activity, Box, ChevronRight, ShieldAlert, Truck } from '../../lib/ui/icons';
import { textStyles } from '../../lib/ui/typography';

export interface HistoryFilterValues {
  from?: string;
  to?: string;
  location_id?: string;
  device_id?: string;
  status?: CanonicalStatus;
}

export const HISTORY_PAGE_SIZE = 10;
export const HISTORY_PAGE_SIZES = [10, 20, 50] as const;
const HISTORY_POLL_INTERVAL_MS = 60_000;

export const HISTORY_TABLE_COLUMNS = [
  'PERANGKAT',
  'LOKASI',
  'STREAM',
  'STATUS',
  'KETERISIAN',
  'BERAT',
  'WAKTU',
  'AKSI',
] as const;

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

export function totalHistoryPages(totalCount: number, pageSize: number): number {
  if (totalCount <= 0 || pageSize <= 0) return 1;
  return Math.max(1, Math.ceil(totalCount / pageSize));
}

export function formatHistoryScore(value: number | null | undefined): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return '—';
  return value.toFixed(2);
}

export function formatHistoryCount(value: number): string {
  return new Intl.NumberFormat('id-ID').format(Math.max(0, Math.trunc(value)));
}

export function historyRangeLabel(pageNumber: number, pageSize: number, totalCount: number): string {
  if (totalCount <= 0) return 'Tidak ada data pada filter ini';
  const start = Math.min(Math.max(1, (pageNumber - 1) * pageSize + 1), totalCount);
  const end = Math.min(Math.max(start, pageNumber * pageSize), totalCount);
  return `${formatHistoryCount(start)}–${formatHistoryCount(end)} dari ${formatHistoryCount(totalCount)} data`;
}

export function formatHistoryTimestamp(value: string | null | undefined): { time: string; date: string } {
  if (!value) return { time: '—', date: '—' };
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return { time: '—', date: '—' };
  const parts = new Intl.DateTimeFormat('id-ID', { timeZone: 'Asia/Jakarta', hour: '2-digit', minute: '2-digit', hour12: false }).formatToParts(date);
  const hour = parts.find((part) => part.type === 'hour')?.value ?? '—';
  const minute = parts.find((part) => part.type === 'minute')?.value ?? '—';
  return {
    time: `${hour}:${minute}`,
    date: new Intl.DateTimeFormat('id-ID', { timeZone: 'Asia/Jakarta', day: '2-digit', month: 'short', year: 'numeric' }).format(date),
  };
}

function compactMetric(value: number | null | undefined, suffix = ''): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return '—';
  return `${Number.isInteger(value) ? value : value.toFixed(1)}${suffix}`;
}

function historyStreamLabel(stream: string): string {
  if (stream === 'organic') return 'Organik';
  if (stream === 'inorganic') return 'Anorganik';
  if (stream === 'b3') return 'B3';
  return stream;
}

function historyStreamColor(stream: string): string {
  if (stream === 'organic') return '#4FA768';
  if (stream === 'inorganic') return '#2F80ED';
  if (stream === 'b3') return '#F04E4E';
  return '#5D7397';
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

function HistoryRowDetail({ item }: { item: HistoryItem }) {
  const details = [
    ['Status efektif', effectiveHistoryStatus(item) || '—'],
    ['Keselamatan', item.safety_state || '—'],
    ['Alasan', item.dominant_reason || '—'],
    ['FIS score', formatHistoryScore(item.fis_score)],
    ['ID perangkat', item.device_id || '—'],
    ['OGI', compactMetric(item.ogi)],
    ['Utilisasi berat', compactMetric(item.weight_utilization, '%')],
    ['Suhu', compactMetric(item.temperature, ' °C')],
  ];
  return (
    <XStack flexWrap="wrap" gap="$4" paddingHorizontal="$4" paddingVertical="$3" backgroundColor="$surfaceMuted" borderBottomWidth={1} borderBottomColor="$borderColor">
      {details.map(([label, value]) => (
        <YStack key={label} minWidth={150} flex={1} gap="$1">
          <Text color="$colorMuted" {...textStyles.caption}>{label}</Text>
          <Text color="$color" {...textStyles.bodySecondary}>{value}</Text>
        </YStack>
      ))}
    </XStack>
  );
}

function HistoryRow({ item, compact }: { item: HistoryItem; compact: boolean }) {
  const [expanded, setExpanded] = useState(false);
  const status = effectiveHistoryStatus(item);
  const streamColor = historyStreamColor(item.stream);
  const timestamp = formatHistoryTimestamp(item.recorded_at);
  if (compact) {
    return (
      <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3" role="article" aria-label={`Riwayat ${item.device_code} ${historyStreamLabel(item.stream)}`}>
        <XStack justifyContent="space-between" alignItems="flex-start" gap="$3">
          <YStack flex={1} minWidth={0} gap="$1">
            <Text color="$color" {...textStyles.cardTitle} numberOfLines={1}>{item.device_code || '—'}</Text>
            <Text color="$colorMuted" {...textStyles.bodySecondary} numberOfLines={1}>{item.location_name || '—'}</Text>
            <Text color="$colorMuted" {...textStyles.caption} numberOfLines={1}>{item.placement_label || '—'}</Text>
          </YStack>
          <StatusBadge status={status} compact />
        </XStack>
        <XStack alignItems="center" gap="$2"><YStack width={8} height={8} borderRadius={99} backgroundColor={streamColor} /><Text color="$color" {...textStyles.bodySecondary}>{historyStreamLabel(item.stream)}</Text></XStack>
        <Text color="$colorMuted" {...textStyles.metadata}>{timestamp.time} · {timestamp.date}</Text>
        <XStack gap="$4" flexWrap="wrap" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$3">
          <YStack flex={1} minWidth={100} gap="$1"><Text color="$colorMuted" {...textStyles.caption}>Keterisian</Text><Text color="$color" {...textStyles.metricValue}>{compactMetric(item.fill, '%')}</Text></YStack>
          <YStack flex={1} minWidth={100} gap="$1"><Text color="$colorMuted" {...textStyles.caption}>Berat</Text><Text color="$color" {...textStyles.metricValue}>{compactMetric(item.weight, ' kg')}</Text></YStack>
        </XStack>
        {expanded ? <HistoryRowDetail item={item} /> : null}
        <AppButton size="sm" variant="ghost" onPress={() => setExpanded((value) => !value)} ariaLabel={`${expanded ? 'Tutup' : 'Buka'} detail riwayat ${item.device_code}`} ariaExpanded={expanded}>
          {expanded ? 'Tutup detail' : 'Detail'} <ChevronRight size={15} color="#0A9B62" strokeWidth={2.2} aria-hidden={true} />
        </AppButton>
      </Card>
    );
  }

  return (
    <YStack role="row">
      <XStack alignItems="center" gap="$3" paddingHorizontal="$4" paddingVertical="$3" minHeight={64} borderBottomWidth={1} borderBottomColor="$borderColor" hoverStyle={{ backgroundColor: '$surfaceMuted' }}>
        <YStack width={210} minWidth={180} gap="$1"><Text color="$color" {...textStyles.metadata} fontWeight="700" numberOfLines={1}>{item.device_code || '—'}</Text><Text color="$colorMuted" {...textStyles.caption} numberOfLines={1}>{item.placement_label || '—'}</Text></YStack>
        <YStack flex={1} minWidth={145}><Text color="$color" {...textStyles.metadata} numberOfLines={1}>{item.location_name || '—'}</Text></YStack>
        <XStack width={105} minWidth={95} alignItems="center" gap="$2"><YStack width={8} height={8} borderRadius={99} backgroundColor={streamColor} /><Text color="$color" {...textStyles.bodySecondary}>{historyStreamLabel(item.stream)}</Text></XStack>
        <YStack width={200} minWidth={190} alignItems="flex-start"><StatusBadge status={status} compact /></YStack>
        <Text color="$color" {...textStyles.metadata} width={80} textAlign="right">{compactMetric(item.fill, '%')}</Text>
        <Text color="$color" {...textStyles.metadata} width={80} textAlign="right">{compactMetric(item.weight, ' kg')}</Text>
        <YStack width={115} gap="$1"><Text color="$color" {...textStyles.metadata} fontWeight="700">{timestamp.time}</Text><Text color="$colorMuted" {...textStyles.caption}>{timestamp.date}</Text></YStack>
        <XStack width={68} justifyContent="flex-end"><AppButton size="sm" variant="ghost" onPress={() => setExpanded((value) => !value)} ariaLabel={`${expanded ? 'Tutup' : 'Buka'} detail riwayat ${item.device_code}`} ariaExpanded={expanded}>{expanded ? 'Tutup' : 'Detail'} <ChevronRight size={15} color="#0A9B62" strokeWidth={2.2} aria-hidden={true} /></AppButton></XStack>
      </XStack>
      {expanded ? <HistoryRowDetail item={item} /> : null}
    </YStack>
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
    <XStack alignItems="center" gap="$3" paddingHorizontal="$4" paddingVertical="$3" backgroundColor="$surfaceMuted" role="row">
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={210}>{HISTORY_TABLE_COLUMNS[0]}</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" flex={1} minWidth={145}>{HISTORY_TABLE_COLUMNS[1]}</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={105}>{HISTORY_TABLE_COLUMNS[2]}</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={200}>{HISTORY_TABLE_COLUMNS[3]}</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={80} textAlign="right">{HISTORY_TABLE_COLUMNS[4]}</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={80} textAlign="right">{HISTORY_TABLE_COLUMNS[5]}</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={115}>{HISTORY_TABLE_COLUMNS[6]}</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={68} textAlign="right">{HISTORY_TABLE_COLUMNS[7]}</Text>
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

type HistoryDraft = HistoryFilterValues & { fromDate: string; toDate: string };

function FilterFields({ draft, onChange }: { draft: HistoryDraft; onChange: (patch: Partial<HistoryDraft>) => void }) {
  return (
    <XStack gap="$3" flexWrap="wrap">
      <YStack width="100%" $wide={{ width: 'calc(40% - 10px)' }} gap="$2">
        <Text color="$color" {...textStyles.caption} fontWeight="700">Periode</Text>
        <XStack gap="$2">
          <YStack flex={1} gap="$1">
            <Label htmlFor="history-from" color="$colorMuted" {...textStyles.caption}>Dari tanggal</Label>
            <Input id="history-from" type="date" value={draft.fromDate} onChangeText={(value) => onChange({ fromDate: value })} minHeight={44} fontSize={16} borderColor="$borderColor" backgroundColor="$background" />
          </YStack>
          <YStack flex={1} gap="$1">
            <Label htmlFor="history-to" color="$colorMuted" {...textStyles.caption}>Sampai tanggal</Label>
            <Input id="history-to" type="date" value={draft.toDate} onChangeText={(value) => onChange({ toDate: value })} minHeight={44} fontSize={16} borderColor="$borderColor" backgroundColor="$background" />
          </YStack>
        </XStack>
      </YStack>
      <LocationFilter value={draft.location_id ?? ''} onChange={(location_id) => onChange({ location_id })} />
      <YStack width="100%" $tablet={{ width: 'calc(50% - 8px)' }} $wide={{ width: 'calc(20% - 13px)' }} gap="$2">
        <Label htmlFor="history-device" color="$color" {...textStyles.caption} fontWeight="700">Perangkat</Label>
        <Input id="history-device" value={draft.device_id ?? ''} onChangeText={(value) => onChange({ device_id: value })} placeholder="Nama atau ID perangkat" minHeight={44} fontSize={16} borderColor="$borderColor" backgroundColor="$background" />
      </YStack>
      <YStack width="100%" $tablet={{ width: 'calc(50% - 8px)' }} $wide={{ width: 'calc(20% - 13px)' }} gap="$2">
        <Text color="$color" {...textStyles.caption} fontWeight="700">Status</Text>
        <Select value={draft.status || 'all'} onValueChange={(value) => onChange({ status: value === 'all' ? undefined : value as CanonicalStatus })} name="history-status">
          <Select.Trigger aria-label="Filter status" minHeight={44} width="100%" borderColor="$borderColor" backgroundColor="$background"><Select.Value placeholder="Semua status" /></Select.Trigger>
          <Select.Content><Select.Viewport minWidth={280}>{HISTORY_STATUS_FILTERS.map((filter, index) => <Select.Item key={filter.value || 'all'} value={filter.value || 'all'} index={index}><Select.ItemText>{filter.label}</Select.ItemText><Select.ItemIndicator /></Select.Item>)}</Select.Viewport></Select.Content>
        </Select>
      </YStack>
    </XStack>
  );
}

function Filters({ draft, onChange, onApply, onReset, onExport, compact }: { draft: HistoryDraft; onChange: (patch: Partial<HistoryDraft>) => void; onApply: () => void; onReset: () => void; onExport: () => void; compact: boolean }) {
  const [open, setOpen] = useState(!compact);
  return (
    <YStack gap="$3">
      <XStack gap="$3" justifyContent="space-between" flexWrap="wrap">
        <AppButton size="md" variant="secondary" onPress={() => setOpen((value) => !value)} ariaLabel="Buka filter riwayat" ariaExpanded={open}>{open ? 'Tutup filter' : 'Filter'}</AppButton>
        <AppButton size="md" variant="primary" onPress={onExport} ariaLabel="Unduh CSV">Unduh CSV</AppButton>
      </XStack>
      {open ? <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3"><FilterFields draft={draft} onChange={onChange} /><XStack gap="$3" justifyContent="flex-end" flexWrap="wrap"><AppButton size="md" variant="secondary" onPress={onReset} ariaLabel="Reset filter">Reset</AppButton><AppButton size="md" variant="primary" onPress={onApply} ariaLabel="Terapkan filter">Terapkan filter</AppButton></XStack></Card> : null}
    </YStack>
  );
}

function HistoryPager({ pageNumber, totalPages, canGoNewer, canGoOlder, onNewer, onOlder }: { pageNumber: number; totalPages: number; canGoNewer: boolean; canGoOlder: boolean; onNewer: () => void; onOlder: () => void }) {
  return <XStack alignItems="center" gap="$2"><AppButton size="sm" variant="secondary" disabled={!canGoNewer} onPress={onNewer} ariaLabel="Halaman sebelumnya">‹</AppButton><Text color="$color" {...textStyles.metadata} fontWeight="700">{pageNumber} / {totalPages}</Text><AppButton size="sm" variant="secondary" disabled={!canGoOlder} onPress={onOlder} ariaLabel="Halaman berikutnya">›</AppButton></XStack>;
}

export default function HistoryRoute() {
  const router = useRouter();
  const media = useMedia();
  const [draft, setDraft] = useState<HistoryDraft>({ fromDate: '', toDate: '' });
  const [applied, setApplied] = useState<HistoryDraft>({ fromDate: '', toDate: '' });
  const [pageSize, setPageSize] = useState<number>(HISTORY_PAGE_SIZE);
  const [cursorStack, setCursorStack] = useState<HistoryCursor[]>([]);

  const dateRange = dateInputToUtcRange(applied.fromDate, applied.toDate);
  const filters = useMemo<HistoryFilterValues>(() => ({
    from: dateRange.from,
    to: dateRange.to,
    location_id: applied.location_id || undefined,
    device_id: applied.device_id?.trim() || undefined,
    status: applied.status,
  }), [dateRange.from, dateRange.to, applied]);
  const activeCursor = cursorStack.length ? cursorStack[cursorStack.length - 1] : undefined;
  const loader = useCallback(
    (signal: AbortSignal) => apiClient.fetchHistory(filters, { limit: pageSize, before: activeCursor }, signal),
    [activeCursor, filters, pageSize],
  );
  const { data, loading, error, stale, retry } = usePollingResource<HistoryResponse>(loader, HISTORY_POLL_INTERVAL_MS);
  const pageNumber = cursorStack.length + 1;
  const totalPages = totalHistoryPages(data?.page.total_count ?? 0, pageSize);

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
  const handleFilterChange = useCallback((patch: Partial<HistoryDraft>) => setDraft((current) => ({ ...current, ...patch })), []);
  const handleApply = useCallback(() => { setApplied(draft); setCursorStack([]); }, [draft]);
  const handleReset = useCallback(() => {
    const empty: HistoryDraft = { fromDate: '', toDate: '' };
    setDraft(empty);
    setApplied(empty);
    setCursorStack([]);
  }, []);
  const handleExport = useCallback(() => {
    const url = apiClient.buildExportURL(filters);
    if (typeof window !== 'undefined') window.open(url, '_blank', 'noopener,noreferrer');
  }, [filters]);
  const handlePageSize = useCallback((value: string) => { setPageSize(Number(value)); setCursorStack([]); }, []);
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
      <YStack gap="$5" paddingBottom="$6">
        <Card borderWidth={1} borderColor="$primarySoft" backgroundColor="$surface" borderRadius="$5" padding="$5" overflow="hidden">
          <XStack alignItems="center" gap="$5" flexWrap="wrap">
            <YStack flex={1} minWidth={250} gap="$2">
              <Text color="$primary" fontSize="$2" fontWeight="800">AUDIT DAN LAPORAN</Text>
              <XStack alignItems="center" gap="$2" flexWrap="wrap">
                <Text color="$color" {...textStyles.pageTitle} $compact={textStyles.pageTitleCompact} letterSpacing={-0.8}>Riwayat pembacaan yang mudah ditindaklanjuti.</Text>
                <DescriptionHint
                  description="Gunakan filter untuk menelusuri data sensor dan keputusan operasional dari semua lokasi."
                  ariaLabel="Lihat informasi tentang riwayat dan laporan"
                />
              </XStack>
            </YStack>
            <RNImage source={historyArtwork} accessibilityLabel="Ilustrasi analisis riwayat" resizeMode="contain" style={{ width: 210, height: 112, maxWidth: '100%' }} />
          </XStack>
        </Card>

        <Filters draft={draft} onChange={handleFilterChange} onApply={handleApply} onReset={handleReset} onExport={handleExport} compact={media.compact} />

        <XStack gap="$3" flexWrap="wrap">
          <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Ditampilkan" value={summary ? summary.total : '—'} icon={Activity} tone="blue" /></YStack>
          <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Normal" value={summary ? summary.normal : '—'} icon={Box} tone="green" /></YStack>
          <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Perlu diangkat" value={summary ? summary.pickup : '—'} icon={Truck} tone="orange" /></YStack>
          <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Safety Alert" value={summary ? summary.safety : '—'} icon={ShieldAlert} tone="red" /></YStack>
        </XStack>

        {stale && data ? <StaleNotice onRetry={retry} /> : null}
        {loading && !data ? <LoadingState label="Memuat riwayat" rows={4} /> : null}
        {!data && !loading && error ? <ErrorState onRetry={retry} /> : null}

        {data && data.items.length ? <HistoryTrend items={data.items} /> : null}
        {data ? <YStack gap="$3">
          <XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap">
            <Text color="$color" {...textStyles.sectionTitle} $compact={textStyles.sectionTitleCompact}>Riwayat pembacaan</Text>
            <XStack alignItems="center" gap="$3" flexWrap="wrap" width={media.compact ? '100%' : undefined} justifyContent={media.compact ? 'flex-end' : 'space-between'}>
              <Text color="$colorMuted" {...textStyles.metadata}>{historyRangeLabel(pageNumber, pageSize, data.page.total_count)}</Text>
              <XStack alignItems="center" gap="$3" flexWrap="wrap" width={media.compact ? '100%' : undefined} justifyContent={media.compact ? 'space-between' : undefined}>
                <XStack alignItems="center" gap="$2"><Text color="$colorMuted" {...textStyles.metadata}>Tampilkan</Text><Select value={String(pageSize)} onValueChange={handlePageSize} name="history-page-size"><Select.Trigger aria-label="Jumlah baris per halaman" minHeight={40} width={68} borderColor="$borderColor" backgroundColor="$surface"><Select.Value /></Select.Trigger><Select.Content><Select.Viewport minWidth={100}>{HISTORY_PAGE_SIZES.map((size, index) => <Select.Item key={size} value={String(size)} index={index}><Select.ItemText>{size}</Select.ItemText><Select.ItemIndicator /></Select.Item>)}</Select.Viewport></Select.Content></Select><Text color="$colorMuted" {...textStyles.metadata}>baris</Text></XStack>
                <HistoryPager pageNumber={pageNumber} totalPages={totalPages} canGoNewer={cursorStack.length > 0} canGoOlder={data.page.has_more && data.page.next !== null} onNewer={handleNewer} onOlder={handleOlder} />
              </XStack>
            </XStack>
          </XStack>
          {data.items.length ? (media.wide ? <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" overflow="hidden" role="table"><HistoryTableHeader />{data.items.map((item) => <HistoryRow key={`${item.reading_id}-${item.slot_id}`} item={item} compact={false} />)}</Card> : <YStack gap="$3">{data.items.map((item) => <HistoryRow key={`${item.reading_id}-${item.slot_id}`} item={item} compact />)}</YStack>) : <EmptyState title={HISTORY_EMPTY_COPY.title} description={HISTORY_EMPTY_COPY.description} />}
        </YStack> : null}
      </YStack>
    </AppShell>
  );
}
