import type { MetricBarTone } from './MetricBar';
import { textStyles } from '../lib/ui/typography';
import { Circle, Svg } from 'react-native-svg';
import { Text, YStack } from 'tamagui';
import { useAnimatedNumber } from '../lib/ui/useAnimatedNumber';

const SIZE = 116;
const STROKE = 11;
const RADIUS = (SIZE - STROKE) / 2;
const CIRCUMFERENCE = 2 * Math.PI * RADIUS;

const toneColor: Record<MetricBarTone, string> = {
  green: '#14A866',
  blue: '#2487E8',
  red: '#E4515D',
  neutral: '#7890AA',
};

export interface SlotFillGaugeProps {
  value: number | null | undefined;
  tone: MetricBarTone;
}

export function SlotFillGauge({ value, tone }: SlotFillGaugeProps) {
  const valid = typeof value === 'number' && Number.isFinite(value);
  const animatedValue = useAnimatedNumber(valid ? value : null, { precision: 0 });
  const visualValue = animatedValue ?? (valid ? value : null);
  const clamped = visualValue === null ? 0 : Math.max(0, Math.min(100, visualValue));
  const offset = CIRCUMFERENCE - (clamped / 100) * CIRCUMFERENCE;

  return (
    <YStack
      width={SIZE}
      height={SIZE}
      alignItems="center"
      justifyContent="center"
      aria-label={valid ? `Keterisian ${Math.round(value)} persen` : 'Keterisian belum tersedia'}
    >
      <Svg
        width={SIZE}
        height={SIZE}
        style={{ position: 'absolute', transform: [{ rotate: '-90deg' }] }}
      >
        <Circle
          cx={SIZE / 2}
          cy={SIZE / 2}
          r={RADIUS}
          stroke="#EDF4F8"
          strokeWidth={STROKE}
          fill="none"
        />
        {valid ? (
          <Circle
            cx={SIZE / 2}
            cy={SIZE / 2}
            r={RADIUS}
            stroke={toneColor[tone]}
            strokeWidth={STROKE}
            strokeLinecap="round"
            strokeDasharray={`${CIRCUMFERENCE} ${CIRCUMFERENCE}`}
            strokeDashoffset={offset}
            fill="none"
          />
        ) : null}
      </Svg>
      <Text {...textStyles.metricHero} color="$color" fontVariant={['tabular-nums']}>
        {visualValue !== null ? `${Math.round(visualValue)}%` : '—'}
      </Text>
    </YStack>
  );
}
