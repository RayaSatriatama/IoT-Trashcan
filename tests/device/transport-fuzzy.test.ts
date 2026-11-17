import { describe, expect, it } from 'vitest';

import {
  ORGANIC_CAPACITY_KG,
  TRANSPORT_INPUTS,
  evaluateTransportPriority,
  ruleSeverity,
  transportInputMembership,
  weightUtilizationPercent,
} from '../../lib/ui/transportFuzzy';

const spec = (id: string) => TRANSPORT_INPUTS.find(item => item.id === id)!;

describe('transport fuzzy design table', () => {
  it('maps published inputs to their strongest declared set', () => {
    expect(transportInputMembership(spec('fill'), 10).set).toBe('Rendah');
    expect(transportInputMembership(spec('fill'), 55).set).toBe('Sedang');
    expect(transportInputMembership(spec('fill'), 95).set).toBe('Tinggi');
    expect(transportInputMembership(spec('weight_utilization'), 10).set).toBe('Ringan');
    expect(transportInputMembership(spec('weight_utilization'), 50).set).toBe('Sedang');
    expect(transportInputMembership(spec('weight_utilization'), 90).set).toBe('Berat');
    expect(transportInputMembership(spec('ammonia'), 5).set).toBe('Rendah');
    expect(transportInputMembership(spec('ammonia'), 25).set).toBe('Sedang');
    expect(transportInputMembership(spec('ammonia'), 100).set).toBe('Tinggi');
    expect(transportInputMembership(spec('accumulation'), 10).set).toBe('Singkat');
    expect(transportInputMembership(spec('accumulation'), 50).set).toBe('Sedang');
    expect(transportInputMembership(spec('accumulation'), 90).set).toBe('Lama');
    expect(transportInputMembership(spec('temperature'), 10).set).toBe('Dingin');
    expect(transportInputMembership(spec('temperature'), 30).set).toBe('Sedang');
    expect(transportInputMembership(spec('temperature'), 50).set).toBe('Panas');
  });

  it('classifies ammonia with the same three-set treatment as the other inputs', () => {
    const degrees = transportInputMembership(spec('ammonia'), 18).memberships;
    expect(degrees.map(item => item.set)).toEqual(['Rendah', 'Sedang', 'Tinggi']);
    // 18 ppm belongs to both Rendah and Sedang; the strongest wins.
    // Rendah = (20-18)/(20-10) = 0,2, Sedang = (18-10)/(25-10) = 0,533.
    expect(degrees.find(item => item.set === 'Rendah')!.degree).toBeCloseTo(0.2, 6);
    expect(degrees.find(item => item.set === 'Sedang')!.degree).toBeCloseTo(8 / 15, 6);
    expect(transportInputMembership(spec('ammonia'), 18).set).toBe('Sedang');
  });

  it('keeps a missing input unavailable instead of zero', () => {
    const result = transportInputMembership(spec('ammonia'), null);
    expect(result.set).toBeNull();
    expect(result.degree).toBe(0);
    const priority = evaluateTransportPriority({ fill: 40, weight_utilization: 20, ammonia: null, accumulation: 50, temperature: 30 });
    expect(priority.state).toBe('UNAVAILABLE');
    expect(priority.score).toBeNull();
    expect(priority.missingInputs).toEqual(['ammonia']);
  });

  it('treats a missing temperature as unavailable too', () => {
    const priority = evaluateTransportPriority({ fill: 40, weight_utilization: 20, ammonia: 5, accumulation: 50, temperature: null });
    expect(priority.state).toBe('UNAVAILABLE');
    expect(priority.missingInputs).toEqual(['temperature']);
  });

  it('derives a monotonic priority score once all inputs exist', () => {
    const low = evaluateTransportPriority({ fill: 5, weight_utilization: 5, ammonia: 5, accumulation: 5, temperature: 18 });
    const high = evaluateTransportPriority({ fill: 95, weight_utilization: 95, ammonia: 120, accumulation: 95, temperature: 55 });
    expect(low.state).toBe('READY');
    expect(high.state).toBe('READY');
    expect(low.score).not.toBeNull();
    expect(high.score!).toBeGreaterThan(low.score!);
    expect(low.label).toBe('Rendah');
    expect(high.label).toBe('Kritis');
  });

  it('lets a hot bin raise the score with every other input held constant', () => {
    const base = { fill: 40, weight_utilization: 30, ammonia: 12, accumulation: 10 };
    const cold = evaluateTransportPriority({ ...base, temperature: 15 });
    const hot = evaluateTransportPriority({ ...base, temperature: 50 });
    expect(hot.score!).toBeGreaterThan(cold.score!);
  });

  it('exposes the numbers behind the score', () => {
    const priority = evaluateTransportPriority({ fill: 53, weight_utilization: 53, ammonia: 59, accumulation: 9, temperature: 31 });
    expect(priority.memberships).toHaveLength(TRANSPORT_INPUTS.length);
    expect(priority.rules.length).toBeGreaterThan(0);
    // Rules come back strongest first so the UI can show the binding ones.
    const last = priority.rules[priority.rules.length - 1];
    expect(priority.rules[0].strength).toBeGreaterThanOrEqual(last.strength);
    const aggregate = priority.aggregate!;
    expect(aggregate.grid).toBe(101);
    expect(aggregate.area).toBeGreaterThan(0);
    // The published score is exactly the centroid of the aggregated curve.
    expect(aggregate.centroid).toBeCloseTo(priority.score!, 9);
    expect(aggregate.levels.reduce((sum, item) => sum + item.level, 0)).toBeGreaterThan(0);
  });

  it('keeps the four-input severity table and spreads it over five inputs', () => {
    // The owner table for four inputs maps 0..8 onto Rendah..Kritis.
    expect([0, 1, 2, 3, 4, 5, 6, 7, 8].map(sum => ruleSeverity(sum, 4))).toEqual([0, 0, 1, 1, 2, 2, 3, 3, 3]);
    // Five inputs must still reach every output set and stay inside 0..3.
    expect(ruleSeverity(0, 5)).toBe(0);
    expect(ruleSeverity(10, 5)).toBe(3);
    expect([...Array(11).keys()].every(sum => ruleSeverity(sum, 5) >= 0 && ruleSeverity(sum, 5) <= 3)).toBe(true);
  });

  it('computes weight utilization from absolute weight against the declared capacity', () => {
    expect(ORGANIC_CAPACITY_KG).toBe(2);
    expect(weightUtilizationPercent(-0.18)).toBeCloseTo(9, 5);
    expect(weightUtilizationPercent(1)).toBeCloseTo(50, 5);
    expect(weightUtilizationPercent(1, 4)).toBeCloseTo(25, 5);
    expect(weightUtilizationPercent(null)).toBeNull();
  });
});
