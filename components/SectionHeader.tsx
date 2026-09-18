import { useId, useState, type ReactNode } from 'react';
import { Button, Card, Text, XStack, YStack } from 'tamagui';
import { textStyles } from '../lib/ui/typography';

export function DescriptionHint({
  description,
  ariaLabel = 'Lihat informasi',
}: {
  description: string;
  ariaLabel?: string;
}) {
  const [open, setOpen] = useState(false);
  const tooltipId = useId();

  return (
    <YStack
      position="relative"
      zIndex={open ? 20 : 1}
    >
      <Button
        minWidth={34}
        minHeight={34}
        width={34}
        height={34}
        padding={0}
        borderRadius="$6"
        borderWidth={1}
        borderColor="$borderColor"
        backgroundColor="$surface"
        color="$colorMuted"
        aria-label={ariaLabel}
        aria-expanded={open}
        aria-controls={tooltipId}
        aria-describedby={open ? tooltipId : undefined}
        onMouseEnter={() => setOpen(true)}
        onMouseLeave={() => setOpen(false)}
        onFocus={() => setOpen(true)}
        onBlur={() => setOpen(false)}
        onPress={() => setOpen(true)}
        onClick={() => setOpen(true)}
        hoverStyle={{ borderColor: '$primary', backgroundColor: '$primarySoft' }}
        focusStyle={{ borderColor: '$primary' }}
        type="button"
      >
        <Text color="$colorMuted" fontSize={14} lineHeight={18} fontWeight="800">
          i
        </Text>
      </Button>

      {open ? (
        <Card
          id={tooltipId}
          position="absolute"
          top={40}
          right={0}
          width={280}
          maxWidth={280}
          padding="$3"
          borderWidth={1}
          borderColor="$borderColor"
          borderRadius="$3"
          backgroundColor="$surface"
          elevation="$3"
          role="tooltip"
        >
          <Text color="$color" {...textStyles.bodySecondary}>
            {description}
          </Text>
        </Card>
      ) : null}
    </YStack>
  );
}

export function SectionHeader({
  title,
  description,
  action,
}: {
  title: string;
  description?: string;
  action?: ReactNode;
}) {
  return (
    <XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap">
      <XStack alignItems="center" gap="$2" flex={1} minWidth={220}>
        <Text color="$color" {...textStyles.sectionTitle}>
          {title}
        </Text>
        {description ? (
          <DescriptionHint
            description={description}
            ariaLabel={`Lihat informasi tentang ${title}`}
          />
        ) : null}
      </XStack>
      {action ? <XStack alignItems="center" gap="$2">{action}</XStack> : null}
    </XStack>
  );
}
