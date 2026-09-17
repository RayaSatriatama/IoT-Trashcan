import { Text, YStack } from 'tamagui';

export function SectionHeader({ title, description }: { title: string; description?: string }) {
  return <YStack gap="$1"><Text color="$color" fontSize="$6" fontWeight="800">{title}</Text>{description ? <Text color="$colorMuted" fontSize="$2">{description}</Text> : null}</YStack>;
}
