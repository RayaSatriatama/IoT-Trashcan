import { describe, expect, it } from 'vitest';
import { formatCapacity } from '../../lib/ui/useOrganicCapacity';

describe('organic capacity formatting', () => {
  it('renders whole kilograms without decimals', () => {
    expect(formatCapacity(5)).toBe('5');
    expect(formatCapacity(12)).toBe('12');
  });

  it('uses an Indonesian decimal comma for fractional kilograms', () => {
    expect(formatCapacity(4.35)).toBe('4,35');
    expect(formatCapacity(0.5)).toBe('0,5');
  });
});
