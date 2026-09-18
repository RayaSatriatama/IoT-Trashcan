import { describe, expect, it } from 'vitest';
import { textStyles } from '../../lib/ui/typography';

describe('SIGAP compact typography scale', () => {
  it('keeps section titles below the hero scale', () => {
    expect(textStyles.sectionTitle).toMatchObject({
      fontSize: 18,
      lineHeight: 24,
      fontWeight: '700',
    });
  });

  it('uses compact card, body, and metadata scales', () => {
    expect(textStyles.cardTitle).toMatchObject({ fontSize: 14, lineHeight: 20, fontWeight: '700' });
    expect(textStyles.bodySecondary).toMatchObject({ fontSize: 13, lineHeight: 18 });
    expect(textStyles.metadata).toMatchObject({ fontSize: 12, lineHeight: 17, fontWeight: '500' });
  });

  it('keeps KPI labels compact without shrinking KPI numbers too far', () => {
    expect(textStyles.kpiLabel).toMatchObject({ fontSize: 12, lineHeight: 17, fontWeight: '600' });
    expect(textStyles.numericKpi).toMatchObject({ fontSize: 32, fontWeight: '800' });
  });
});
