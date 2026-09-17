import { describe, expect, it } from 'vitest';

import { pageCount, paginate } from './pagination';

describe('client pagination', () => {
  it('keeps empty collections on a stable empty page', () => {
    expect(pageCount(0, 4)).toBe(1);
    expect(paginate([], 5, 4)).toEqual({ items: [], page: 1, totalPages: 1 });
  });

  it('returns exact and partial pages', () => {
    expect(paginate([1, 2, 3, 4, 5, 6, 7, 8], 2, 4)).toEqual({
      items: [5, 6, 7, 8],
      page: 2,
      totalPages: 2,
    });
    expect(paginate([1, 2, 3, 4, 5], 2, 4)).toEqual({
      items: [5],
      page: 2,
      totalPages: 2,
    });
  });

  it('clamps pages outside the available range', () => {
    expect(paginate(['a', 'b', 'c'], 99, 2)).toEqual({
      items: ['c'],
      page: 2,
      totalPages: 2,
    });
    expect(paginate(['a', 'b', 'c'], 0, 2)).toEqual({
      items: ['a', 'b'],
      page: 1,
      totalPages: 2,
    });
  });
});
