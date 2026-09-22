import type { CanonicalStatus } from '../lib/api/types';
import { getStatusPresentation } from '../lib/status';
import { textStyles } from '../lib/ui/typography';
import { Text, XStack } from 'tamagui';
import { useReducedMotion } from '../lib/ui/motionContext';

export interface StatusBadgeProps {
  status: CanonicalStatus | null | undefined;
  compact?: boolean;
}

const unavailablePresentation = {
  label: 'Belum tersedia',
  token: 'statusOffline' as const,
};

export function getStatusBadgePresentation(status: CanonicalStatus | null | undefined) {
  return status ? getStatusPresentation(status) : unavailablePresentation;
}

export function StatusBadge({ status, compact = false }: StatusBadgeProps) {
  const reduceMotion = useReducedMotion();
  const presentation = getStatusBadgePresentation(status);
  const label = `Status: ${presentation.label}`;
  const backgroundToken = status ? `$${presentation.token}Soft` : '$surfaceMuted';

  return (
    <XStack
      alignItems="center"
      gap="$2"
      paddingHorizontal={compact ? '$2' : '$3'}
      paddingVertical={compact ? '$1' : '$2'}
      borderRadius="$6"
      backgroundColor={backgroundToken}
      role="status"
      aria-label={label}
      flexShrink={0}
      transition={reduceMotion ? undefined : 'standard'}
    >
      <XStack
        width={8}
        height={8}
        borderRadius={999}
        backgroundColor={`$${presentation.token}`}
        aria-hidden={true}
      />
      <Text color={`$${presentation.token}`} {...textStyles.caption} fontWeight="800" numberOfLines={1} ellipsizeMode="tail" flexShrink={0}>
        {presentation.label}
      </Text>
    </XStack>
  );
}
