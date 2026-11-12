export type DebugLevel = 'ERROR' | 'WARN' | 'INFO' | 'DEBUG';
export type DebugCursor = { boot_id: string; after: string };
export type DebugEvent = { sequence: string; monotonic_us: string; level: DebugLevel; module: number; event_code: number; request_id: number | null; duration_us: number; arg0: number; arg1: number; related_sequence: string | null };
export type DebugMeta = { boot_id: string; build_id: string; level: DebugLevel; oldest_sequence: string; latest_sequence: string; overwritten: string; delivery_lost: string; usb_lost: string; captured_monotonic_us: string; sequence_exhausted: boolean };
export type DebugPage = { boot_id: string; level: DebugLevel; oldest_sequence: string; next_after: string; overwritten: string; gap: boolean; boot_changed: boolean; event: DebugEvent | null };
export type DebugResponse = { kind: 'meta'; meta: DebugMeta } | { kind: 'page'; page: DebugPage } | { kind: 'level'; level: DebugLevel };
