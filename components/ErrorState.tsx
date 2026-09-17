import { Button, Card, Text, YStack } from 'tamagui';

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
      backgroundColor="$surface"
      borderRadius="$3"
      padding="$5"
      gap="$3"
      role="alert"
      aria-label={title}
    >
      <YStack gap="$2">
        <Text fontSize="$4" color="$color" fontWeight="700">
          {title}
        </Text>
        <Text fontSize="$2" color="$colorMuted">
          {message}
        </Text>
        {onRetry ? (
          <Button
            alignSelf="flex-start"
            minHeight={48}
            paddingHorizontal="$4"
            backgroundColor="$primary"
            color="$white"
            onPress={onRetry}
            aria-label="Coba muat ulang"
            type="button"
          >
            Coba lagi
          </Button>
        ) : null}
      </YStack>
    </Card>
  );
}
