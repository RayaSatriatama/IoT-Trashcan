import { Card, Text, YStack } from 'tamagui';

export interface LoadingStateProps {
  label?: string;
  rows?: number;
}

export function LoadingState({ label = 'Memuat data', rows = 3 }: LoadingStateProps) {
  return (
    <YStack gap="$3" role="status" aria-label={label}>
      {Array.from({ length: rows }, (_, index) => (
        <Card
          key={index}
          borderWidth={1}
          borderColor="$borderColor"
          backgroundColor="$surface"
          borderRadius="$4"
          padding="$4"
          gap="$3"
        >
          <YStack height={20} width="42%" borderRadius="$2" backgroundColor="$surfaceMuted" />
          <YStack height={14} width="76%" borderRadius="$2" backgroundColor="$surfaceMuted" />
          <YStack height={14} width="58%" borderRadius="$2" backgroundColor="$surfaceMuted" />
        </Card>
      ))}
      <Text color="$colorMuted" fontSize="$2">
        {label}
      </Text>
    </YStack>
  );
}
