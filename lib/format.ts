export function formatNullableMetric(value: number | null | undefined, suffix = ''): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return 'Belum tersedia';
  const nearestInteger = Math.round(value);
  const displayValue = Math.abs(value - nearestInteger) < 1e-9 ? nearestInteger : value;
  return `${displayValue}${suffix}`;
}

export function formatTimestamp(value: string | null | undefined): string {
  if (!value) return 'Belum tersedia';
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return 'Belum tersedia';
  return new Intl.DateTimeFormat('id-ID', { dateStyle: 'medium', timeStyle: 'short' }).format(date);
}
