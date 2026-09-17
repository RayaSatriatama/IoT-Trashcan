import { Text, YStack } from 'tamagui';
import { textStyles } from '../lib/ui/typography';

export function SectionHeader({ title, description }: { title: string; description?: string }) {
  return <YStack gap="$1"><Text color="$color" {...textStyles.sectionTitle}>{title}</Text>{description ? <Text color="$colorMuted" {...textStyles.bodySecondary}>{description}</Text> : null}</YStack>;
}
