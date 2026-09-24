import { useCallback, useState, type ReactNode } from 'react';
import { Adapt, Card, Input, Label, Select, ScrollView, Sheet, Text, XStack, YStack } from 'tamagui';

import { AppButton } from '../AppButton';
import { DescriptionHint } from '../SectionHeader';
import { apiClient } from '../../lib/api/client';
import { usePollingResource } from '../../lib/api/hooks';
import type { CanonicalStatus, HistoryFilters as HistoryFilterValues, LocationListResponse } from '../../lib/api/types';
import { ChevronDown, ChevronUp, Filter as FilterIcon } from '../../lib/ui/icons';
import { textStyles } from '../../lib/ui/typography';

export interface HistoryFilterDraft extends HistoryFilterValues {
  fromDate: string;
  toDate: string;
}

export interface HistoryFiltersProps {
  draft: HistoryFilterDraft;
  compact: boolean;
  periodError: string | null;
  onChange: (patch: Partial<HistoryFilterDraft>) => void;
  onApply: () => boolean;
  onReset: () => void;
  onExport: () => void;
}

export function appliedHistoryDraft(
  draft: HistoryFilterDraft,
  dateRange: Pick<HistoryFilterValues, 'from' | 'to'>,
): HistoryFilterValues {
  return {
    from: dateRange.from,
    to: dateRange.to,
    location_id: draft.location_id || undefined,
    device_id: draft.device_id?.trim() || undefined,
    status: draft.status,
  };
}

const statusFilters: ReadonlyArray<{ label: string; value: CanonicalStatus | '' }> = [
  { label: 'Semua status', value: '' },
  { label: 'Normal', value: 'Normal' },
  { label: 'Waspada', value: 'Waspada' },
  { label: 'Perlu diangkat', value: 'Angkat' },
  { label: 'Peringatan keselamatan', value: 'Safety Alert' },
  { label: 'Data tidak valid', value: 'Data Invalid' },
];

function SelectAdaptation() {
  return (
    <Select.Adapt when="compact">
      <Sheet modal dismissOnSnapToBottom snapPoints={[60]}>
        <Sheet.Frame padding="$4" backgroundColor="$surface" borderTopLeftRadius="$5" borderTopRightRadius="$5">
          <Sheet.Handle />
          <Sheet.ScrollView><Adapt.Contents /></Sheet.ScrollView>
        </Sheet.Frame>
        <Sheet.Overlay backgroundColor="rgba(10, 31, 70, 0.24)" />
      </Sheet>
    </Select.Adapt>
  );
}

function LocationFilter({ value, onChange }: { value: string; onChange: (value: string) => void }) {
  const loader = useCallback((signal: AbortSignal) => apiClient.fetchLocations({}, signal), []);
  const { data } = usePollingResource<LocationListResponse>(loader, 60_000);
  const locations = data?.locations ?? [];

  return (
    <YStack width="100%" $tablet={{ width: 'calc(50% - 8px)' }} $wide={{ width: 'calc(14% - 9px)' }} gap="$2">
      <Label htmlFor="history-location" color="$color" {...textStyles.caption} fontWeight="700">Lokasi</Label>
      <Select value={value || 'all'} onValueChange={(next) => onChange(next === 'all' ? '' : next)} name="history-location">
        <Select.Trigger id="history-location" aria-label="Filter lokasi" minHeight={44} $compact={{ minHeight: 48 }} width="100%" borderColor="$borderColor" backgroundColor="$background">
          <Select.Value placeholder="Semua lokasi" fontSize={14} $compact={{ fontSize: 16 }} />
          <Select.Icon><ChevronDown size={16} color="#60789F" aria-hidden /></Select.Icon>
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
        <SelectAdaptation />
      </Select>
    </YStack>
  );
}

function FilterFields({ draft, onChange, periodError, actions }: { draft: HistoryFilterDraft; onChange: (patch: Partial<HistoryFilterDraft>) => void; periodError: string | null; actions?: ReactNode }) {
  return (
    <XStack gap="$3" flexWrap="wrap" alignItems="flex-end">
      <YStack width="100%" $tablet={{ width: 'calc(50% - 8px)' }} $wide={{ width: 'calc(25% - 9px)' }} gap="$2">
        <Text color="$color" {...textStyles.caption} fontWeight="700">Periode</Text>
        <XStack gap="$2">
          <YStack flex={1} gap="$1">
            <Label htmlFor="history-from" color="$colorMuted" {...textStyles.caption}>Dari tanggal</Label>
            <Input id="history-from" type="date" value={draft.fromDate} onChangeText={(value) => onChange({ fromDate: value })} minHeight={44} $compact={{ minHeight: 48, fontSize: 16 }} fontSize={14} borderColor={periodError ? '$statusSafetyAlert' : '$borderColor'} backgroundColor="$background" />
          </YStack>
          <YStack flex={1} gap="$1">
            <Label htmlFor="history-to" color="$colorMuted" {...textStyles.caption}>Sampai tanggal</Label>
            <Input id="history-to" type="date" value={draft.toDate} onChangeText={(value) => onChange({ toDate: value })} minHeight={44} $compact={{ minHeight: 48, fontSize: 16 }} fontSize={14} borderColor={periodError ? '$statusSafetyAlert' : '$borderColor'} backgroundColor="$background" />
          </YStack>
        </XStack>
        {periodError ? <Text color="$statusSafetyAlert" {...textStyles.caption} role="alert" aria-live="polite">{periodError}</Text> : null}
      </YStack>
      <LocationFilter value={draft.location_id ?? ''} onChange={(location_id) => onChange({ location_id })} />
      <YStack width="100%" $tablet={{ width: 'calc(50% - 8px)' }} $wide={{ width: 'calc(20% - 9px)' }} gap="$2">
        <Label htmlFor="history-device" color="$color" {...textStyles.caption} fontWeight="700">Perangkat</Label>
        <Input id="history-device" value={draft.device_id ?? ''} onChangeText={(value) => onChange({ device_id: value })} placeholder="Nama atau ID perangkat" minHeight={44} $compact={{ minHeight: 48, fontSize: 16 }} fontSize={14} borderColor="$borderColor" backgroundColor="$background" />
      </YStack>
      <YStack width="100%" $tablet={{ width: 'calc(50% - 8px)' }} $wide={{ width: 'calc(14% - 9px)' }} gap="$2">
        <Label htmlFor="history-status" color="$color" {...textStyles.caption} fontWeight="700">Status</Label>
        <Select value={draft.status || 'all'} onValueChange={(value) => onChange({ status: value === 'all' ? undefined : value as CanonicalStatus })} name="history-status">
          <Select.Trigger id="history-status" aria-label="Filter status" minHeight={44} $compact={{ minHeight: 48 }} width="100%" borderColor="$borderColor" backgroundColor="$background"><Select.Value placeholder="Semua status" fontSize={14} $compact={{ fontSize: 16 }} /><Select.Icon><ChevronDown size={16} color="#60789F" aria-hidden /></Select.Icon></Select.Trigger>
          <Select.Content><Select.Viewport minWidth={280}>{statusFilters.map((filter, index) => <Select.Item key={filter.value || 'all'} value={filter.value || 'all'} index={index}><Select.ItemText>{filter.label}</Select.ItemText><Select.ItemIndicator /></Select.Item>)}</Select.Viewport></Select.Content>
          <SelectAdaptation />
        </Select>
      </YStack>
      {actions ? <YStack width="100%" $tablet={{ width: 'calc(50% - 8px)' }} $wide={{ width: 'calc(18% - 9px)' }} alignItems="flex-end">{actions}</YStack> : null}
    </XStack>
  );
}

export function HistoryFilters({ draft, compact, periodError, onChange, onApply, onReset, onExport }: HistoryFiltersProps) {
  const [open, setOpen] = useState(!compact);
  const filterActions = (
    <XStack gap="$2" justifyContent="flex-end" flexWrap="wrap">
      <AppButton size="sm" variant="secondary" onPress={onReset} ariaLabel="Reset filter">Reset</AppButton>
      <AppButton size="sm" variant="primary" onPress={() => { if (onApply()) setOpen(false); }} ariaLabel="Terapkan filter">Terapkan filter</AppButton>
    </XStack>
  );
  const fields = <FilterFields draft={draft} onChange={onChange} periodError={periodError} />;

  if (compact) {
    return (
      <YStack gap="$3">
        <XStack gap="$3" justifyContent="space-between" alignItems="center">
          <AppButton size="md" variant="secondary" onPress={() => setOpen(true)} ariaLabel="Buka filter riwayat" ariaExpanded={open}>
            <FilterIcon size={16} color="#1D3566" aria-hidden /> Filter
          </AppButton>
          <AppButton size="md" variant="secondary" onPress={onExport} ariaLabel="Unduh CSV">Unduh CSV</AppButton>
        </XStack>
        <Sheet modal open={open} onOpenChange={setOpen} snapPoints={[88]} dismissOnSnapToBottom>
          <Sheet.Overlay backgroundColor="rgba(10, 31, 70, 0.24)" />
          <Sheet.Frame padding="$4" backgroundColor="$surface" borderTopLeftRadius="$5" borderTopRightRadius="$5">
            <Sheet.Handle />
            <ScrollView contentContainerStyle={{ paddingBottom: 24 }}>
              <YStack gap="$4">
                <XStack alignItems="center" justifyContent="space-between" gap="$3">
                  <XStack alignItems="center" gap="$2"><Text color="$color" {...textStyles.sectionTitle}>Filter riwayat</Text><DescriptionHint description="Gunakan periode, lokasi, perangkat, dan status untuk mempersempit audit pembacaan." ariaLabel="Lihat informasi tentang filter riwayat" /></XStack>
                  <AppButton size="sm" variant="ghost" onPress={() => setOpen(false)} ariaLabel="Tutup filter">Tutup</AppButton>
                </XStack>
                {fields}
                {filterActions}
              </YStack>
            </ScrollView>
          </Sheet.Frame>
        </Sheet>
      </YStack>
    );
  }

  return (
    <YStack gap="$2">
      <XStack gap="$3" justifyContent="space-between" alignItems="center" flexWrap="wrap">
        <XStack alignItems="center" gap="$2"><Text color="$color" {...textStyles.sectionTitle}>Filter riwayat</Text><DescriptionHint description="Gunakan periode, lokasi, perangkat, dan status untuk mempersempit audit pembacaan." ariaLabel="Lihat informasi tentang filter riwayat" /></XStack>
        <XStack gap="$2" alignItems="center" flexWrap="wrap">
          <AppButton size="sm" variant="ghost" onPress={() => setOpen((value) => !value)} ariaLabel={open ? 'Sembunyikan filter' : 'Tampilkan filter'} ariaExpanded={open}>
            {open ? <ChevronUp size={16} color="#0A9B62" aria-hidden /> : <ChevronDown size={16} color="#0A9B62" aria-hidden />} {open ? 'Sembunyikan filter' : 'Tampilkan filter'}
          </AppButton>
          <AppButton size="sm" variant="secondary" onPress={onExport} ariaLabel="Unduh CSV">Unduh CSV</AppButton>
        </XStack>
      </XStack>
      {open ? <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$3" gap="$3"><FilterFields draft={draft} onChange={onChange} periodError={periodError} actions={filterActions} /></Card> : null}
    </YStack>
  );
}
