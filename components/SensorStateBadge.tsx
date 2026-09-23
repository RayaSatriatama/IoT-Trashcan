import { Text, XStack } from 'tamagui';

import type { SensorState } from '../lib/api/types';
import { formatSensorState } from '../lib/ui/slotPresentation';
import { textStyles } from '../lib/ui/typography';

const tone: Record<SensorState, { color: string; background: string }> = {
  VALID: { color: '$statusNormal', background: '$statusNormalSoft' },
  WARMING_UP: { color: '$statusWaspada', background: '$statusWaspadaSoft' },
  STALE: { color: '$statusWaspada', background: '$statusWaspadaSoft' },
  INVALID: { color: '$statusSafetyAlert', background: '$statusSafetyAlertSoft' },
  DISABLED: { color: '$colorMuted', background: '$surfaceMuted' },
};

export function SensorStateBadge({ state }: { state: SensorState }) {
  const presentation = tone[state] ?? tone.DISABLED;
  return (
    <XStack
      alignItems="center"
      gap="$1"
      paddingHorizontal="$2"
      paddingVertical="$1"
      borderRadius="$6"
      backgroundColor={presentation.background}
      role="status"
      aria-label={`Status sensor: ${formatSensorState(state)}`}
    >
      <XStack width={6} height={6} borderRadius={999} backgroundColor={presentation.color} aria-hidden={true} />
      <Text {...textStyles.caption} fontWeight="700" color={presentation.color} numberOfLines={1}>
        {formatSensorState(state)}
      </Text>
    </XStack>
  );
}
