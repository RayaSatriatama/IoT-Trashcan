import { useMemo } from 'react';
import type { FirmwareMonitoringSample } from '../api/types';
import { estimateNh3FromPinMv } from './mq135Nh3Estimate';
import { computeSlotDecision, type SlotDecision } from './slotDecision';
import { ORGANIC_CAPACITY_KG, weightUtilizationPercent } from './transportFuzzy';
import { formatElapsed, useAccumulationTimer, type AccumulationTimer } from './useAccumulationTimer';

export interface SlotDecisionState {
  decision: SlotDecision;
  inputs: {
    fill: number | null;
    weightUtilization: number | null;
    ammoniaPpm: number | null;
    accumulationHours: number | null;
    temperatureC: number | null;
  };
  accumulation: AccumulationTimer;
  accumulationLabel: string | null;
  nh3: ReturnType<typeof estimateNh3FromPinMv> | null;
}

const readyValue = (sample: FirmwareMonitoringSample | undefined, key: string): number | null => {
  const value = sample?.quality === 'READY' ? sample.converted_value?.[key] : null;
  return typeof value === 'number' && Number.isFinite(value) ? value : null;
};

/**
 * One organic-slot decision from the five configured inputs: fill (%),
 * weight utilization (%), NH3-equivalent (ppm), accumulation (hours) and
 * temperature (°C). The timer starts at the first reading and is reset only by
 * the operator. The capacity defaults to the declared 2 kg.
 */
export function useOrganicDecision(
  deviceId: string,
  samples: readonly FirmwareMonitoringSample[],
  capacityKg: number = ORGANIC_CAPACITY_KG,
): SlotDecisionState {
  const byId = (id: string) => samples.find(item => item.sensor_id === id);
  const fill = readyValue(byId('ultrasonic_organic'), 'fill_percent');
  const weightKg = readyValue(byId('hx711_organic'), 'kg');
  const nh3 = estimateNh3FromPinMv(byId('mq135')?.converted_value?.adc_pin_mv);
  const temperatureC = readyValue(byId('sht31_44'), 'temperature_c');
  const hasReading = samples.some(item => item.quality === 'READY');
  const accumulation = useAccumulationTimer(deviceId, hasReading);
  const accumulationHours = accumulation.elapsedMs === null ? null : accumulation.elapsedMs / 3600000;
  const weightUtilization = weightUtilizationPercent(weightKg === null ? null : Math.abs(weightKg), capacityKg);

  const decision = useMemo(() => computeSlotDecision({
    fill,
    weightUtilization,
    ammoniaPpm: nh3.ppm,
    accumulationHours,
    temperatureC,
  }), [fill, weightUtilization, nh3.ppm, accumulationHours, temperatureC]);

  return {
    decision,
    inputs: { fill, weightUtilization, ammoniaPpm: nh3.ppm, accumulationHours, temperatureC },
    accumulation,
    accumulationLabel: accumulation.elapsedMs === null ? null : formatElapsed(accumulation.elapsedMs),
    nh3: nh3.status === 'UNAVAILABLE' ? null : nh3,
  };
}
