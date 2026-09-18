import { Card, Text, YStack } from 'tamagui';
import { AppButton } from './AppButton';
import { textStyles } from '../lib/ui/typography';

export interface ErrorStateProps {
  title?: string;
  message?: string;
  onRetry?: () => void;
}

export function ErrorState({
  title = 'Data tidak dapat dimuat',
  message = 'Periksa koneksi API lalu coba lagi.',
  onRetry,
}: ErrorStateProps) {
  return (
    <Card
      borderWidth={1}
      borderColor="$statusSafetyAlert"
      backgroundColor="$redSoft"
      borderRadius="$4"
      padding="$4"
      gap="$3"
      role="alert"
      aria-label={title}
    >
      <YStack gap="$3" alignItems="flex-start">
        <YStack gap="$1">
          <Text {...textStyles.cardTitle} $compact={textStyles.cardTitleCompact} color="$color">
          {title}
          </Text>
          <Text {...textStyles.bodySecondary} color="$colorMuted">
            {message}
          </Text>
        </YStack>
        {onRetry ? (
          <AppButton size="md" variant="primary" onPress={onRetry} ariaLabel="Coba muat ulang">
            Coba lagi
          </AppButton>
        ) : null}
      </YStack>
    </Card>
  );
}
