import type { CanonicalStatus } from '../lib/api/types';
import { getStatusPresentation } from '../lib/status';
import { Text, XStack } from 'tamagui';

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
  const presentation = getStatusBadgePresentation(status);
  const label = `Status: ${presentation.label}`;

  return (
    <XStack
      alignItems="center"
      gap="$2"
      paddingHorizontal={compact ? '$2' : '$3'}
      paddingVertical="$1"
      borderRadius="$6"
      backgroundColor="$surfaceMuted"
      role="status"
      aria-label={label}
    >
      <XStack
        width={8}
        height={8}
        borderRadius={999}
        backgroundColor={`$${presentation.token}`}
        aria-hidden={true}
      />
      <Text color="$color" fontSize="$2" fontWeight="600">
        {presentation.label}
      </Text>
    </XStack>
  );
}
