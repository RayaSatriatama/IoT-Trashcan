import { useRouter } from 'expo-router';
import { useCallback, useMemo, useState } from 'react';
import { Image as RNImage } from 'react-native';
import { AnimatePresence, Card, Select, Text, XStack, YStack, useMedia } from 'tamagui';

import historyArtwork from '../../assets/history_report_analytics_illustration.png';
import { AppShell } from '../../components/AppShell';
import { EmptyState } from '../../components/EmptyState';
import { ErrorState } from '../../components/ErrorState';
import { AppButton } from '../../components/AppButton';
import { appliedHistoryDraft, HistoryFilters, type HistoryFilterDraft } from '../../components/history/HistoryFilters';
import { HistoryAnalytics } from '../../components/history/HistoryAnalytics';
import { LoadingState } from '../../components/LoadingState';
import { StatusBadge } from '../../components/StatusBadge';
import { apiClient } from '../../lib/api/client';
import { usePollingResource } from '../../lib/api/hooks';
import type {
  CanonicalStatus,
  HistoryCursor,
  HistoryItem,
  HistoryResponse,
} from '../../lib/api/types';
import { ChevronDown, ChevronRight } from '../../lib/ui/icons';
import { textStyles } from '../../lib/ui/typography';
import { useReducedMotion } from '../../lib/ui/motionContext';
import { useAnimatedNumber } from '../../lib/ui/useAnimatedNumber';

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

function jakartaDateStartToUtc(value: string, dayOffset = 0): string {
  const [year, month, day] = value.split('-').map(Number);
  return new Date(Date.UTC(year, month - 1, day + dayOffset, -7)).toISOString();
}

export function dateInputToUtcRange(fromDate: string, toDate: string): Pick<HistoryFilterValues, 'from' | 'to'> {
  return {
    from: fromDate ? jakartaDateStartToUtc(fromDate) : undefined,
    to: toDate ? jakartaDateStartToUtc(toDate, 1) : undefined,
  };
}

export function validateHistoryPeriod(fromDate: string, toDate: string): string | null {
  if (Boolean(fromDate) !== Boolean(toDate)) return 'Lengkapi tanggal awal dan akhir.';
  if (fromDate && toDate && fromDate > toDate) return 'Tanggal awal tidak boleh melewati tanggal akhir.';
  return null;
}

export const HISTORY_EMPTY_COPY = {
  title: 'Belum ada riwayat pembacaan',
  description: 'Coba ubah rentang tanggal atau filter untuk melihat data audit yang tersedia.',
} as const;

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
  const reduceMotion = useReducedMotion();
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
    <XStack
      flexWrap="wrap"
      gap="$4"
      paddingHorizontal="$4"
      paddingVertical="$3"
      backgroundColor="$surfaceMuted"
      borderBottomWidth={1}
      borderBottomColor="$borderColor"
      transition={reduceMotion ? undefined : 'standard'}
      enterStyle={reduceMotion ? undefined : { opacity: 0, y: -4 }}
      exitStyle={reduceMotion ? undefined : { opacity: 0, y: -4 }}
    >
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
  const reduceMotion = useReducedMotion();
  const status = effectiveHistoryStatus(item);
  const streamColor = historyStreamColor(item.stream);
  const timestamp = formatHistoryTimestamp(item.recorded_at);
  const animatedFill = useAnimatedNumber(item.fill, { precision: 1 });
  const animatedWeight = useAnimatedNumber(item.weight, { precision: 1 });
  const fillLabel = compactMetric(animatedFill, '%');
  const weightLabel = compactMetric(animatedWeight, ' kg');
  if (compact) {
    return (
      <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3" role="article" aria-label={`Riwayat ${item.device_code} ${historyStreamLabel(item.stream)}`} transition={reduceMotion ? undefined : 'standard'} enterStyle={reduceMotion ? undefined : { opacity: 0, y: 4 }}>
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
          <YStack flex={1} minWidth={100} gap="$1"><Text color="$colorMuted" {...textStyles.caption}>Keterisian</Text><Text color="$color" {...textStyles.metricValue}>{fillLabel}</Text></YStack>
          <YStack flex={1} minWidth={100} gap="$1"><Text color="$colorMuted" {...textStyles.caption}>Berat</Text><Text color="$color" {...textStyles.metricValue}>{weightLabel}</Text></YStack>
        </XStack>
        <AnimatePresence>{expanded ? <HistoryRowDetail key="mobile-history-detail" item={item} /> : null}</AnimatePresence>
        <AppButton size="sm" variant="ghost" onPress={() => setExpanded((value) => !value)} ariaLabel={`${expanded ? 'Tutup' : 'Buka'} detail riwayat ${item.device_code}`} ariaExpanded={expanded}>
          {expanded ? 'Tutup detail' : 'Detail'} <ChevronRight size={15} color="#0A9B62" strokeWidth={2.2} aria-hidden={true} />
        </AppButton>
      </Card>
    );
  }

  return (
    <YStack role="row" transition={reduceMotion ? undefined : 'standard'} enterStyle={reduceMotion ? undefined : { opacity: 0, y: 3 }}>
      <XStack alignItems="center" gap="$3" paddingHorizontal="$4" paddingVertical="$3" minHeight={64} borderBottomWidth={1} borderBottomColor="$borderColor" hoverStyle={{ backgroundColor: '$surfaceMuted' }}>
        <YStack width={210} minWidth={180} gap="$1"><Text color="$color" {...textStyles.metadata} fontWeight="700" numberOfLines={1}>{item.device_code || '—'}</Text><Text color="$colorMuted" {...textStyles.caption} numberOfLines={1}>{item.placement_label || '—'}</Text></YStack>
        <YStack flex={1} minWidth={145}><Text color="$color" {...textStyles.metadata} numberOfLines={1}>{item.location_name || '—'}</Text></YStack>
        <XStack width={105} minWidth={95} alignItems="center" gap="$2"><YStack width={8} height={8} borderRadius={99} backgroundColor={streamColor} /><Text color="$color" {...textStyles.bodySecondary}>{historyStreamLabel(item.stream)}</Text></XStack>
        <YStack width={180} minWidth={170} alignItems="flex-start"><StatusBadge status={status} compact /></YStack>
        <Text color="$color" {...textStyles.metadata} width={80} textAlign="right" fontVariant={['tabular-nums']}>{fillLabel}</Text>
        <Text color="$color" {...textStyles.metadata} width={80} textAlign="right" fontVariant={['tabular-nums']}>{weightLabel}</Text>
        <YStack width={115} gap="$1"><Text color="$color" {...textStyles.metadata} fontWeight="700" fontVariant={['tabular-nums']}>{timestamp.time}</Text><Text color="$colorMuted" {...textStyles.caption}>{timestamp.date}</Text></YStack>
        <XStack width={68} justifyContent="flex-end"><AppButton size="sm" variant="ghost" onPress={() => setExpanded((value) => !value)} ariaLabel={`${expanded ? 'Tutup' : 'Buka'} detail riwayat ${item.device_code}`} ariaExpanded={expanded}><XStack alignItems="center" gap="$1">{expanded ? 'Tutup' : 'Detail'} <ChevronRight size={15} color="#0A9B62" strokeWidth={2.2} aria-hidden={true} /></XStack></AppButton></XStack>
      </XStack>
      <AnimatePresence>{expanded ? <HistoryRowDetail key="desktop-history-detail" item={item} /> : null}</AnimatePresence>
    </YStack>
  );
}

function HistoryTableHeader() {
  return (
    <XStack alignItems="center" gap="$3" paddingHorizontal="$4" paddingVertical="$3" backgroundColor="$surfaceMuted" role="row" style={{ position: 'sticky', top: 0, zIndex: 2 }}>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={210}>{HISTORY_TABLE_COLUMNS[0]}</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" flex={1} minWidth={145}>{HISTORY_TABLE_COLUMNS[1]}</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={105}>{HISTORY_TABLE_COLUMNS[2]}</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={180}>{HISTORY_TABLE_COLUMNS[3]}</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={80} textAlign="right">{HISTORY_TABLE_COLUMNS[4]}</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={80} textAlign="right">{HISTORY_TABLE_COLUMNS[5]}</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={115}>{HISTORY_TABLE_COLUMNS[6]}</Text>
      <Text color="$colorMuted" {...textStyles.caption} fontWeight="800" width={68} textAlign="right">{HISTORY_TABLE_COLUMNS[7]}</Text>
    </XStack>
  );
}

function HistoryPager({ pageNumber, totalPages, canGoNewer, canGoOlder, onNewer, onOlder }: { pageNumber: number; totalPages: number; canGoNewer: boolean; canGoOlder: boolean; onNewer: () => void; onOlder: () => void }) {
  return <XStack alignItems="center" gap="$2"><AppButton size="sm" variant="secondary" disabled={!canGoNewer} onPress={onNewer} ariaLabel="Halaman sebelumnya">‹</AppButton><Text color="$color" {...textStyles.metadata} fontWeight="700">{pageNumber} / {totalPages}</Text><AppButton size="sm" variant="secondary" disabled={!canGoOlder} onPress={onOlder} ariaLabel="Halaman berikutnya">›</AppButton></XStack>;
}

export default function HistoryRoute() {
  const router = useRouter();
  const media = useMedia();
  const [draft, setDraft] = useState<HistoryFilterDraft>({ fromDate: '', toDate: '' });
  const [applied, setApplied] = useState<HistoryFilterDraft>({ fromDate: '', toDate: '' });
  const [periodError, setPeriodError] = useState<string | null>(null);
  const [pageSize, setPageSize] = useState<number>(HISTORY_PAGE_SIZE);
  const [cursorStack, setCursorStack] = useState<HistoryCursor[]>([]);

  const filters = useMemo<HistoryFilterValues>(() => appliedHistoryDraft(applied, dateInputToUtcRange(applied.fromDate, applied.toDate)), [applied]);
  const activeCursor = cursorStack.length ? cursorStack[cursorStack.length - 1] : undefined;
  const loader = useCallback(
    (signal: AbortSignal) => apiClient.fetchHistory(filters, { limit: pageSize, before: activeCursor }, signal),
    [activeCursor, filters, pageSize],
  );
  const { data, loading, error, stale, retry } = usePollingResource<HistoryResponse>(loader, HISTORY_POLL_INTERVAL_MS);
  const pageNumber = cursorStack.length + 1;
  const totalPages = totalHistoryPages(data?.page.total_count ?? 0, pageSize);

  const handleNavigate = useCallback((path: string) => router.push(path as never), [router]);
  const handleFilterChange = useCallback((patch: Partial<HistoryFilterDraft>) => setDraft((current) => ({ ...current, ...patch })), []);
  const handleApply = useCallback(() => {
    const validationError = validateHistoryPeriod(draft.fromDate, draft.toDate);
    if (validationError) {
      setPeriodError(validationError);
      return false;
    }
    setPeriodError(null);
    setApplied(draft);
    setCursorStack([]);
    return true;
  }, [draft]);
  const handleReset = useCallback(() => {
    const empty: HistoryFilterDraft = { fromDate: '', toDate: '' };
    setDraft(empty);
    setApplied(empty);
    setPeriodError(null);
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
      <YStack gap="$4" paddingBottom="$6">
        <Card borderWidth={1} borderColor="$primarySoft" backgroundColor="$surface" borderRadius="$5" padding="$4" overflow="hidden">
          <XStack alignItems="center" gap="$3" flexWrap="wrap">
            <YStack flex={1} minWidth={220} gap="$1">
              <Text color="$primary" fontSize="$2" fontWeight="800">AUDIT DAN LAPORAN</Text>
              <YStack gap="$2">
                <Text color="$color" fontSize={30} lineHeight={36} fontWeight="800" $compact={{ fontSize: 28, lineHeight: 34 }} letterSpacing={-0.6}>Riwayat pembacaan yang mudah ditindaklanjuti.</Text>
                <Text color="$colorMuted" {...textStyles.bodySecondary}>
                  Gunakan filter untuk menelusuri data sensor dan keputusan operasional dari semua lokasi.
                </Text>
              </YStack>
            </YStack>
            <RNImage source={historyArtwork} accessibilityLabel="Ilustrasi analisis riwayat" resizeMode="contain" style={{ width: 160, height: 86, maxWidth: '100%' }} />
          </XStack>
        </Card>

        <HistoryFilters draft={draft} compact={media.compact} periodError={periodError} onChange={handleFilterChange} onApply={handleApply} onReset={handleReset} onExport={handleExport} />

        {data ? <HistoryAnalytics summary={data.summary} trend={data.trend} compact={media.compact} /> : null}

        {stale && data ? <StaleNotice onRetry={retry} /> : null}
        {loading && !data ? <LoadingState label="Memuat riwayat" rows={4} /> : null}
        {!data && !loading && error ? <ErrorState onRetry={retry} /> : null}

        {data ? <YStack gap="$3">
          <XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap">
            <Text color="$color" {...textStyles.sectionTitle} $compact={textStyles.sectionTitleCompact}>Riwayat pembacaan</Text>
            <XStack alignItems="center" gap="$3" flexWrap="wrap" width={media.compact ? '100%' : undefined} justifyContent={media.compact ? 'flex-end' : 'space-between'}>
              <Text color="$colorMuted" {...textStyles.metadata}>{historyRangeLabel(pageNumber, pageSize, data.page.total_count)}</Text>
              <XStack alignItems="center" gap="$3" flexWrap="wrap" width={media.compact ? '100%' : undefined} justifyContent={media.compact ? 'space-between' : undefined}>
                <XStack alignItems="center" gap="$2"><Text color="$colorMuted" {...textStyles.metadata}>Tampilkan</Text><Select value={String(pageSize)} onValueChange={handlePageSize} name="history-page-size"><Select.Trigger aria-label="Jumlah baris per halaman" minHeight={40} width={72} borderColor="$borderColor" backgroundColor="$surface"><Select.Value /><Select.Icon><ChevronDown size={15} color="#60789F" aria-hidden /></Select.Icon></Select.Trigger><Select.Content><Select.Viewport minWidth={100}>{HISTORY_PAGE_SIZES.map((size, index) => <Select.Item key={size} value={String(size)} index={index}><Select.ItemText>{size}</Select.ItemText><Select.ItemIndicator /></Select.Item>)}</Select.Viewport></Select.Content></Select><Text color="$colorMuted" {...textStyles.metadata}>baris</Text></XStack>
                <HistoryPager pageNumber={pageNumber} totalPages={totalPages} canGoNewer={cursorStack.length > 0} canGoOlder={data.page.has_more && data.page.next !== null} onNewer={handleNewer} onOlder={handleOlder} />
              </XStack>
            </XStack>
          </XStack>
          {data.items.length ? (media.wide ? <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" overflow="visible" role="table"><HistoryTableHeader />{data.items.map((item) => <HistoryRow key={`${item.reading_id}-${item.slot_id}`} item={item} compact={false} />)}</Card> : <YStack gap="$3">{data.items.map((item) => <HistoryRow key={`${item.reading_id}-${item.slot_id}`} item={item} compact />)}</YStack>) : <EmptyState title={HISTORY_EMPTY_COPY.title} description={HISTORY_EMPTY_COPY.description} />}
        </YStack> : null}
      </YStack>
    </AppShell>
  );
}
