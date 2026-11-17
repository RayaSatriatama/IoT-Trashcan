import { Fragment, useEffect, useMemo, useRef, useState } from 'react';
import { Card, Text, XStack, YStack } from 'tamagui';
import { Circle, Line, Polyline, Svg, Text as SvgText } from 'react-native-svg';
import { AppButton } from '../AppButton';
import { SensorSelector } from './SensorSelector';
import { SensorStateBadge } from '../SensorStateBadge';
import { apiClient } from '../../lib/api/client';
import type { FirmwareMonitoringSample, SensorHistoryPage, SensorHistoryPoint } from '../../lib/api/types';
import { SENSOR_METRICS, buildSensorPlot, sensorMetricValue, type SensorMetric } from '../../lib/ui/sensorHistory';
import { calibrationSensorLabel } from '../../lib/ui/deviceTools';
import { formatFirmwareMetric } from '../../lib/ui/firmwarePresentation';
import { firmwareReason } from '../../lib/ui/firmwarePresentation';
import { formatResearchTimestamp } from '../../lib/ui/slotPresentation';
import type { CalibrationSensorId } from '../../lib/api/calibration';
import { textStyles } from '../../lib/ui/typography';
import { describeLiveSample } from '../../lib/ui/liveSensorState';
import { buildHistoryCsv, csvFilename, downloadCsv } from '../../lib/ui/csvExport';

const MAX_POINTS = 1000;
type Sensor = { id: CalibrationSensorId; boot: string };
type State = { page?: SensorHistoryPage; points: SensorHistoryPoint[]; loading: boolean; olderLoading: boolean; error?: string; olderError?: string; cap: boolean };
type RequestContext = { active: boolean; deviceId: string; signature: string; inFlight: Map<string, AbortController>; older: Map<string, AbortController>; fetched: Map<string, string> };
export type SensorHistorySnapshot = { deviceId: string; bySensor: Readonly<Record<string, readonly SensorHistoryPoint[]>> };
const empty = (): State => ({ points: [], loading: true, olderLoading: false, cap: false });
const identity = (point: SensorHistoryPoint) => `${point.boot_id}\0${point.sequence}\0${point.sample_origin}`;

function mergePoints(prior: SensorHistoryPoint[], incoming: SensorHistoryPoint[]) {
  const seen = new Set(prior.map(identity));
  const merged = [...prior];
  for (const point of incoming) {
    if (seen.has(identity(point))) continue;
    seen.add(identity(point));
    merged.push(point);
  }
  return merged.sort((a, b) => {
    if (a.boot_id !== b.boot_id) return a.boot_id.localeCompare(b.boot_id);
    const left = BigInt(a.monotonic_us || '0');
    const right = BigInt(b.monotonic_us || '0');
    return a.sequence === b.sequence ? 0 : BigInt(a.sequence) < BigInt(b.sequence) ? -1 : 1;
  });
}

/** UI kit #39 geometry: 800x294 viewBox, plot 47..779 by 14..247. */
const CHART = { width: 800, height: 294, x0: 47, x1: 779, y0: 14, y1: 247 } as const;

const axisLabel = (value: number, span: number) => {
  const step = span / 4;
  const decimals = step >= 1 ? (Number.isInteger(step) ? 0 : 1) : Math.min(4, Math.max(1, Math.ceil(-Math.log10(step))));
  return value.toFixed(decimals).replace('.', ',');
};

const clockLabel = (ms: number) => new Date(ms).toLocaleTimeString('id-ID', { hour: '2-digit', minute: '2-digit' });

/** Maps the plot's own 16..304 / 16..104 space onto the kit's 800x294 canvas. */
const toCanvasX = (x: number) => CHART.x0 + (x - 16) / 288 * (CHART.x1 - CHART.x0);
const toCanvasY = (y: number) => CHART.y1 - (104 - y) / 88 * (CHART.y1 - CHART.y0);

/** Two significant decimals is enough for a 160 px tooltip. */
const tooltipValue = (value: number, unit: string) => {
  const abs = Math.abs(value);
  const shown = abs >= 100 ? Math.round(value) : abs >= 1 ? value.toFixed(2) : abs >= 0.01 ? value.toFixed(3) : value.toFixed(4);
  return `${String(shown).replace('.', ',')}${unit ? ` ${unit}` : ''}`;
};

type HoverEvent = { nativeEvent: { clientX: number }; currentTarget: { getBoundingClientRect?: () => DOMRect } };

/** Pointer and mouse handlers together, because react-native-web maps both. */
const hoverHandlers = (move: (clientX: number, target: HoverEvent['currentTarget']) => void, leave: () => void) => {
  const handle = (event: HoverEvent) => move(event.nativeEvent.clientX, event.currentTarget);
  return { onPointerMove: handle, onMouseMove: handle, onPointerDown: handle, onPointerLeave: leave, onMouseLeave: leave } as Record<string, unknown>;
};

/** Interactive trend chart: hover, drag or arrow keys reveal the nearest sample. */
function TrendChart({ plot, metric }: { plot: ReturnType<typeof buildSensorPlot>; metric: SensorMetric }) {
  const [hover, setHover] = useState<number | null>(null);
  const [width, setWidth] = useState(0);
  const span = plot.domain ? plot.domain.max - plot.domain.min : 0;
  const ticks = [0, 1, 2, 3, 4].map(index => ({
    y: CHART.y0 + (CHART.y1 - CHART.y0) * index / 4,
    value: plot.domain ? plot.domain.max - span * index / 4 : null,
  }));
  const marks = plot.segments.flat();
  const last = marks[marks.length - 1];
  const stamps = plot.window
    ? [0, 0.25, 0.5, 0.75, 1].map(fraction => ({ fraction, ms: plot.window!.from + (plot.window!.to - plot.window!.from) * fraction }))
    : [];
  const active = hover !== null && marks[hover] ? marks[hover] : null;
  const activeX = active ? toCanvasX(active.x) : 0;
  const activeY = active ? toCanvasY(active.y) : 0;
  // The canvas keeps the kit's 800:294 ratio at every width, so the drawn area
  // fills the box exactly and pointer pixels map 1:1 onto viewBox units.
  const height = width > 0 ? Math.round(width * CHART.height / CHART.width) : 230;

  const pick = (clientX: number, target: { getBoundingClientRect?: () => DOMRect }) => {
    if (!marks.length) return;
    const rect = target.getBoundingClientRect?.();
    if (!rect || rect.width <= 0) return;
    const fraction = Math.min(1, Math.max(0, (clientX - rect.left) / rect.width));
    const canvasX = fraction * CHART.width;
    let best = 0;
    let bestDistance = Infinity;
    marks.forEach((point, index) => {
      const distance = Math.abs(toCanvasX(point.x) - canvasX);
      if (distance < bestDistance) { bestDistance = distance; best = index; }
    });
    setHover(best);
  };

  return <YStack gap="$2">
    <XStack alignItems="center" gap="$2" flexWrap="wrap" paddingTop="$1">
      <YStack width={15} height={3} borderRadius={5} backgroundColor="#087B50" />
      <Text {...textStyles.caption} color="$colorMuted">{metric.label} · {metric.unit}</Text>
      {/* The readout lives in the header so the floating card never sits under the
          pointer, which made the crosshair flicker at the plot edges. */}
      {active ? <XStack alignItems="center" gap="$2" marginLeft="auto" testID={`chart-readout-${metric.key}`}>
        <Text {...textStyles.caption} color="$colorMuted" fontVariant={['tabular-nums']}>{new Date(active.t).toLocaleString('id-ID', { day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit', second: '2-digit', timeZone: 'Asia/Jakarta' })}</Text>
        <Text {...textStyles.caption} color="$color" fontWeight="700" fontVariant={['tabular-nums']}>{tooltipValue(active.value, metric.unit)}</Text>
      </XStack> : null}
    </XStack>
    <YStack width="100%" position="relative"
        onLayout={event => setWidth(event.nativeEvent.layout.width)}
        tabIndex={0} role="img" aria-label={`Grafik tren ${metric.label}. Gunakan tombol panah untuk memeriksa titik.`}
        onKeyDown={(event: { key?: string; preventDefault?: () => void }) => {
          if (!marks.length) return;
          if (event.key === 'ArrowRight' || event.key === 'ArrowLeft') {
            event.preventDefault?.();
            setHover(current => {
              const base = current === null ? marks.length - 1 : current;
              return event.key === 'ArrowRight' ? Math.min(marks.length - 1, base + 1) : Math.max(0, base - 1);
            });
          } else if (event.key === 'Escape') setHover(null);
        }}>
        <Svg height={height} width="100%" viewBox={`0 0 ${CHART.width} ${CHART.height}`} role="img"
          aria-label={`Grafik tren ${metric.label} dalam ${metric.unit}`}>
          {ticks.map(tick => <Fragment key={`tick-${tick.y}`}>
            <Line x1={CHART.x0} y1={tick.y} x2={CHART.x1} y2={tick.y} stroke="#E7EDF4" strokeWidth={1} strokeDasharray="3 5" />
            {tick.value !== null && (span > 0 || tick.y === CHART.y0 || tick.y === CHART.y1)
              ? <SvgText x={CHART.x0 - 12} y={tick.y + 4} fontSize={12} fill="#536987" textAnchor="end">{axisLabel(tick.value, span)}</SvgText>
              : null}
          </Fragment>)}
          {plot.segments.map((segment, index) => <Fragment key={`segment-${index}`}>
            {segment.length > 1 ? <Polyline points={segment.map(point => `${toCanvasX(point.x).toFixed(2)},${toCanvasY(point.y).toFixed(2)}`).join(' ')} fill="none" stroke="#087B50" strokeWidth={3} strokeLinecap="round" strokeLinejoin="round" /> : null}
            {segment.length === 1 ? <Circle cx={toCanvasX(segment[0].x)} cy={toCanvasY(segment[0].y)} r={3} fill="#087B50" /> : null}
          </Fragment>)}
          {last ? <Circle cx={toCanvasX(last.x)} cy={toCanvasY(last.y)} r={4} fill="#087B50" stroke="#FFFFFF" strokeWidth={2} /> : null}
          {active ? <Fragment>
            <Line x1={activeX} y1={CHART.y0} x2={activeX} y2={CHART.y1} stroke="#7184A4" strokeWidth={1} strokeDasharray="4 4" />
            <Circle cx={activeX} cy={activeY} r={5} fill="#FFFFFF" stroke="#087B50" strokeWidth={2.5} />
          </Fragment> : null}
          {stamps.map((stamp, index) => <SvgText key={`stamp-${stamp.ms}`} x={CHART.x0 + (CHART.x1 - CHART.x0) * stamp.fraction} y={CHART.height - 19} fontSize={12} fill="#536987"
            textAnchor={index === 0 ? 'start' : index === stamps.length - 1 ? 'end' : 'middle'}>{clockLabel(stamp.ms)}</SvgText>)}
        </Svg>
        <YStack position="absolute" top={0} left={0} right={0} bottom={0} aria-hidden style={{ cursor: 'crosshair' }}
          {...hoverHandlers(pick, () => setHover(null))} />
    </YStack>
  </YStack>;
}

function DataTable({ points, metric }: { points: readonly SensorHistoryPoint[]; metric: SensorMetric }) {
  return <YStack borderTopWidth={1} borderTopColor="$borderColor" marginTop="$3" paddingTop="$2" gap="$1">
    <Text {...textStyles.caption} color="$colorMuted">Waktu</Text>
    {points.map(point => {
      const value = sensorMetricValue(point.converted, metric);
      return <XStack key={`${point.boot_id}-${point.sequence}`} justifyContent="space-between" gap="$2" paddingVertical="$1" borderBottomWidth={1} borderBottomColor="$borderColor">
        <Text {...textStyles.caption} color="$colorMuted">{point.recorded_at ? formatResearchTimestamp(point.recorded_at) : 'Waktu tidak tersedia'}</Text>
        <Text {...textStyles.caption} color="$color" fontVariant={['tabular-nums']} fontWeight="700">{formatFirmwareMetric(typeof value === 'number' && Number.isFinite(value) ? value : null, metric.unit)}</Text>
      </XStack>;
    })}
  </YStack>;
}

/** One chart card matching the UI kit #39 specimen: head, legend, plot, footer
 * and a collapsible data table. */
function ChartCard({ plot, metric, points }: { plot: ReturnType<typeof buildSensorPlot>; metric: SensorMetric; points: readonly SensorHistoryPoint[] }) {
  const [open, setOpen] = useState(false);
  const stamps = plot.window ? `${clockLabel(plot.window.from)} – ${clockLabel(plot.window.to)}` : 'Waktu tidak tersedia';
  return <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3">
    <XStack justifyContent="space-between" alignItems="flex-start" gap="$3" flexWrap="wrap">
      <YStack gap="$1" minWidth={0}>
        <Text {...textStyles.cardTitle} color="$color">Tren {metric.label}</Text>
        <Text {...textStyles.caption} color="$colorMuted">{metric.label} ({metric.unit})</Text>
      </YStack>
      <XStack backgroundColor="$surfaceMuted" paddingHorizontal="$2" paddingVertical="$1" borderRadius={100}>
        <Text {...textStyles.caption} color="$colorMuted" fontWeight="700">Data perangkat</Text>
      </XStack>
    </XStack>
    {metric.source === 'web_estimate' ? <Text {...textStyles.caption} color="$colorMuted">TEST_ONLY · Dihitung dari ADC sensor; konstanta referensi bukan kalibrasi gas.</Text> : null}
    {!plot.segments.length ? <Text color="$colorMuted">Nilai engineering tepercaya belum tersedia.</Text> : <>
      <TrendChart plot={plot} metric={metric} />
      <XStack justifyContent="space-between" gap="$2" flexWrap="wrap" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$3">
        <Text {...textStyles.caption} color="$colorMuted">{points.length} titik · {stamps} · WIB</Text>
        <Text {...textStyles.caption} color="$colorMuted">Geser grafik ↔</Text>
      </XStack>
      <YStack borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$2">
        <XStack alignItems="center" gap="$2" onPress={() => setOpen(value => !value)} role="button" aria-label={open ? 'Sembunyikan tabel data' : 'Lihat tabel data'} paddingVertical="$2">
          <Text {...textStyles.caption} color="$actionPrimary" fontWeight="700">{open ? 'Sembunyikan tabel data' : 'Lihat tabel data'}</Text>
        </XStack>
        {open ? <DataTable points={points.slice(-8)} metric={metric} /> : null}
      </YStack>
    </>}

  </Card>;
}

export function SensorHistoryCharts({ deviceId, monitoring = [], onHistoryChange }: { deviceId: string; monitoring?: FirmwareMonitoringSample[]; onHistoryChange?: (snapshot: SensorHistorySnapshot) => void }) {
  const signature = JSON.stringify([...new Map(monitoring.filter(sample => sample.sensor_id && sample.boot_id).map(sample => [sample.sensor_id, sample.boot_id] as const))].filter(([id]) => Boolean(SENSOR_METRICS[id])).sort(([a], [b]) => a.localeCompare(b)));
  const sensors = useMemo<Sensor[]>(() => JSON.parse(signature).map(([id, boot]: [CalibrationSensorId, string]) => ({ id, boot })), [signature]);
  const [selectedId, setSelectedId] = useState<CalibrationSensorId | null>(null);
  const [states, setStates] = useState<Record<string, State>>({});
  const [refresh, setRefresh] = useState(0);
  const ownerRef = useRef<RequestContext | null>(null);
  const latestRef = useRef(monitoring);
  latestRef.current = monitoring;
  const callbackRef = useRef(onHistoryChange);
  callbackRef.current = onHistoryChange;
  const publishedRef = useRef('');
  const selected = sensors.find(sensor => sensor.id === selectedId) ?? sensors[0];
  const options = sensors.map(sensor => ({ value: sensor.id, label: calibrationSensorLabel(sensor.id) }));
  const selectedSample = selected ? monitoring.find(sample => sample.sensor_id === selected.id) : undefined;
  const selectedView = selectedSample ? describeLiveSample(selectedSample) : undefined;
  const selectedState = !selectedView ? 'DISABLED' : selectedView.state === 'VALID' && (selectedView.recencyLabel === 'Tertunda (>30 detik)' || selectedSample?.fresh === false) ? 'STALE' : selectedView.state;

  useEffect(() => {
    const context: RequestContext = { active: true, deviceId, signature, inFlight: new Map(), older: new Map(), fetched: new Map() };
    ownerRef.current = context;
    const owns = () => context.active && ownerRef.current === context;
    setStates(Object.fromEntries(sensors.map(sensor => [sensor.id, empty()])));
    const loadHead = ({ id, boot }: Sensor) => {
      if (!owns() || context.inFlight.has(id)) return;
      const latest = latestRef.current.find(sample => sample.sensor_id === id && sample.boot_id === boot);
      const marker = `${boot}:${latest?.last_sequence ?? ''}`;
      if (context.fetched.get(id) === marker) return;
      const controller = new AbortController();
      context.inFlight.set(id, controller);
      apiClient.fetchSensorHistory(deviceId, id, undefined, controller.signal, boot).then(page => {
        if (!owns() || controller.signal.aborted) return;
        if (page.device_id !== deviceId || page.sensor_id !== id || page.points.some(point => point.boot_id !== boot)) throw new Error('Respons riwayat tidak cocok dengan snapshot sensor.');
        context.fetched.set(id, marker);
        setStates(previous => {
          const current = previous[id] ?? empty();
          if (!owns()) return previous;
          const points = mergePoints(current.points, page.points);
          return { ...previous, [id]: { ...current, page: { ...page, next_cursor: current.page ? current.page.next_cursor : page.next_cursor }, points: points.slice(-MAX_POINTS), loading: false, error: undefined, cap: current.cap || points.length >= MAX_POINTS } };
        });
      }).catch(error => {
        if (owns() && !controller.signal.aborted) setStates(previous => { const current = previous[id] ?? empty(); return { ...previous, [id]: { ...current, loading: false, error: error instanceof Error ? error.message : 'Gagal memuat riwayat' } }; });
      }).finally(() => { if (context.inFlight.get(id) === controller) context.inFlight.delete(id); });
    };
    sensors.forEach(loadHead);
    const poll = setInterval(() => sensors.forEach(loadHead), 2_500);
    return () => {
      clearInterval(poll);
      context.inFlight.forEach(controller => controller.abort());
      context.active = false;
      if (ownerRef.current === context) ownerRef.current = null;
      for (const controller of context.older.values()) controller.abort();
      context.older.clear();
    };
  }, [deviceId, signature, refresh]);

  useEffect(() => {
    if (!callbackRef.current) return;
    const bySensor = Object.fromEntries(sensors.map(sensor => [sensor.id, (states[sensor.id]?.points ?? []).filter(point => point.boot_id === sensor.boot)]));
    const key = JSON.stringify([deviceId, signature, Object.entries(bySensor).map(([id, points]) => [id, points.length, points[0]?.sequence, points[points.length - 1]?.sequence])]);
    if (publishedRef.current === key) return;
    publishedRef.current = key;
    callbackRef.current({ deviceId, bySensor });
  }, [states, deviceId, signature]);

  const loadOlder = (context: RequestContext, sensor: Sensor, cursor: string) => {
    if (!context.active || ownerRef.current !== context) return;
    const current = states[sensor.id];
    if (!current || current.page?.next_cursor !== cursor || current.cap || current.olderLoading || context.older.has(sensor.id)) return;
    const controller = new AbortController();
    context.older.set(sensor.id, controller);
    setStates(previous => ({ ...previous, [sensor.id]: { ...previous[sensor.id], olderLoading: true, olderError: undefined } }));
    apiClient.fetchSensorHistory(context.deviceId, sensor.id, cursor, controller.signal, sensor.boot).then(page => {
      if (!context.active || ownerRef.current !== context || controller.signal.aborted || context.older.get(sensor.id) !== controller) return;
      if (page.device_id !== context.deviceId || page.sensor_id !== sensor.id || page.points.some(point => point.boot_id !== sensor.boot)) {
        setStates(previous => ({ ...previous, [sensor.id]: { ...previous[sensor.id], olderLoading: false, olderError: 'Respons riwayat tidak cocok dengan snapshot sensor.' } }));
        return;
      }
      setStates(previous => {
        const latest = previous[sensor.id];
        if (!latest || latest.page?.next_cursor !== cursor || !context.active || ownerRef.current !== context) return previous;
        const points = mergePoints(latest.points, page.points);
        return { ...previous, [sensor.id]: { ...latest, page, points: points.slice(-MAX_POINTS), olderLoading: false, cap: latest.cap || points.length >= MAX_POINTS } };
      });
    }).catch(error => {
      if (context.active && ownerRef.current === context && !controller.signal.aborted) setStates(previous => ({ ...previous, [sensor.id]: { ...previous[sensor.id], olderLoading: false, olderError: error instanceof Error ? error.message : 'Gagal memuat halaman lama' } }));
    }).finally(() => {
      if (context.older.get(sensor.id) === controller) context.older.delete(sensor.id);
    });
  };

  const renderedContext = ownerRef.current?.deviceId === deviceId && ownerRef.current?.signature === signature ? ownerRef.current : null;
  const state = selected ? states[selected.id] ?? empty() : undefined;
  const exportPoints = sensors.map(sensor => [sensor.id, (states[sensor.id]?.points ?? []).filter(point => point.boot_id === sensor.boot)] as const)
    .filter(([, points]) => points.length > 0);
  const exportCount = exportPoints.reduce((total, [, points]) => total + points.length, 0);
  const downloadAll = () => {
    const csv = buildHistoryCsv(Object.fromEntries(exportPoints));
    downloadCsv(csvFilename(deviceId), csv);
  };
  return <YStack gap="$3" role="region" aria-label="Sensor fisik">
    <XStack alignItems="center" justifyContent="space-between" gap="$2" flexWrap="wrap">
      <Text {...textStyles.cardTitle} color="$color">Sensor fisik</Text>
      <AppButton variant="secondary" size="sm" disabled={exportCount === 0} onPress={downloadAll}
        ariaLabel={`Unduh ${exportCount} sampel riwayat semua sensor sebagai CSV`}>Unduh CSV</AppButton>
    </XStack>
    {selected && options.length ? <SensorSelector id="research-sensor-select" label="Pilih sensor" value={selected.id} options={options} onChange={setSelectedId} /> : null}
    {!selected ? <Text color="$colorMuted">Belum ada data engineering sensor yang dapat ditampilkan.</Text> : null}
    {selected && state ? <YStack gap="$2">
      {selectedSample ? <YStack gap="$2" padding="$3" borderWidth={1} borderColor="$borderColor" borderRadius="$3">
        <XStack alignItems="center" justifyContent="space-between" gap="$2"><Text {...textStyles.caption} color="$colorMuted">Pembacaan terakhir{state.points[state.points.length - 1]?.recorded_at ? ` · ${formatResearchTimestamp(state.points[state.points.length - 1].recorded_at!)}` : ''}</Text><SensorStateBadge state={selectedState} /></XStack>
        {SENSOR_METRICS[selected.id].map(metric => {
          const value = selectedSample.quality === 'READY' ? sensorMetricValue(selectedSample.converted_value, metric) : null;
          return <Text key={metric.key} {...textStyles.bodySecondary} color="$color">{metric.label}: {formatFirmwareMetric(typeof value === 'number' && Number.isFinite(value) ? value : null, metric.unit)}</Text>;
        })}
        {selectedSample.quality === 'INVALID' || selectedSample.quality === 'UNCALIBRATED' ? <Text {...textStyles.caption} color="$colorMuted">{firmwareReason(selectedSample.reason ?? '')}</Text> : null}
      </YStack> : <Text color="$colorMuted">Pembacaan terkini sensor belum tersedia.</Text>}
      {state.loading ? <Text color="$colorMuted">Memuat riwayat…</Text> : null}
      {state.error ? <Text role="alert" color="$colorMuted">Riwayat gagal dimuat: {state.error}</Text> : null}
      {state.page?.unavailable_reason ? <Text color="$colorMuted">Tidak tersedia: {state.page.unavailable_reason}</Text> : null}
      {!state.loading && !state.error && state.points.length === 0 ? <Text color="$colorMuted">Belum ada sampel engineering.</Text> : null}
      {SENSOR_METRICS[selected.id].map(metric => (
        <ChartCard key={metric.key} plot={buildSensorPlot(state.points, metric)} metric={metric} points={state.points} />
      ))}
      {state.cap ? <Text color="$colorMuted">Batas tampilan {MAX_POINTS} sampel tercapai.</Text> : null}
      {state.olderError ? <Text role="alert" color="$colorMuted">Halaman lama gagal dimuat: {state.olderError}</Text> : null}
      {state.page?.next_cursor && !state.cap ? <AppButton variant="secondary" size="sm" disabled={state.olderLoading} onPress={() => {
        if (renderedContext) loadOlder(renderedContext, selected, state.page!.next_cursor!);
      }} ariaLabel={`Muat riwayat ${calibrationSensorLabel(selected.id)} yang lebih lama`}>{state.olderLoading ? 'Memuat…' : 'Muat lebih lama'}</AppButton> : null}
    </YStack> : null}
    <AppButton variant="ghost" size="sm" onPress={() => setRefresh(value => value + 1)} ariaLabel="Perbarui snapshot riwayat sensor">Perbarui</AppButton>
  </YStack>;
}
