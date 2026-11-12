import { Text, YStack } from 'tamagui';
import type { DebugSource, DebugViewEvent } from '../../lib/debug/poller';
import { textStyles } from '../../lib/ui/typography';

export function DebugEventList({ events, advanced, source }: { events: readonly DebugViewEvent[]; advanced: boolean; source: DebugSource }) {
  return <YStack maxHeight={280} overflowY="auto" gap="$2" role="log" aria-label="Log firmware perangkat" tabIndex={0}>
    {!events.length ? <Text role="status" color="$colorMuted">{source === 'https' ? 'Belum ada riwayat server.' : 'Belum ada log perangkat.'}</Text> : events.map((item, index) => <YStack key={`${item.source}:${item.bootId}:${item.event?.sequence ?? index}`} paddingVertical="$2" borderTopWidth={1} borderColor="$borderColor" gap="$1">
      <Text color="$color" {...textStyles.bodySecondary}>{item.event?.level} · modul {item.event?.module} · kode {item.event?.event_code} · {item.event?.duration_us} µs</Text>
      {advanced ? <Text color="$colorMuted" {...textStyles.caption}>{item.source} · build {item.buildId ?? 'belum tersedia'} · boot {item.bootId} · sequence {item.event?.sequence}{item.receivedAt ? ` · diterima ${item.receivedAt}` : ''}{item.cursor ? ` · server cursor ${item.cursor}` : ''}{item.snapshotLevel ? ` · level capture ${item.snapshotLevel}` : ''}{item.capturedMonotonicUs ? ` · uptime capture ${item.capturedMonotonicUs} µs` : ''}{item.capturedMonotonicUs && item.event && BigInt(item.capturedMonotonicUs) >= BigInt(item.event.monotonic_us) ? ` · usia saat capture ${BigInt(item.capturedMonotonicUs) - BigInt(item.event.monotonic_us)} µs` : ''}</Text> : null}
    </YStack>)}
  </YStack>;
}
