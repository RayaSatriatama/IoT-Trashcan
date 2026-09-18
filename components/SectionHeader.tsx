import type { ReactNode } from 'react';
import { Button, Text, Tooltip, XStack } from 'tamagui';
import { textStyles } from '../lib/ui/typography';

export function DescriptionHint({
  description,
  ariaLabel = 'Lihat informasi',
}: {
  description: string;
  ariaLabel?: string;
}) {
  return (
    <Tooltip delay={0} restMs={120} placement="bottom-end" allowFlip zIndex={1000} focus={{ enabled: true }}>
      <Tooltip.Trigger asChild>
        <Button
          minWidth={32}
          minHeight={32}
          width={32}
          height={32}
          padding={0}
          borderRadius="$6"
          borderWidth={1}
          borderColor="$borderColor"
          backgroundColor="$surface"
          color="$colorMuted"
          aria-label={ariaLabel}
          hoverStyle={{ borderColor: '$primary', backgroundColor: '$primarySoft' }}
          focusStyle={{ borderColor: '$primary', outlineColor: '$primary', outlineWidth: 2, outlineStyle: 'solid' }}
          type="button"
        >
          <Text color="$colorMuted" fontSize={13} lineHeight={17} fontWeight="800">i</Text>
        </Button>
      </Tooltip.Trigger>
      <Tooltip.Content
        enterStyle={{ opacity: 0, scale: 0.98 }}
        exitStyle={{ opacity: 0, scale: 0.98 }}
        elevate
        bordered
        borderColor="$borderColor"
        backgroundColor="$surface"
        padding="$3"
        width={280}
        maxWidth="calc(100vw - 24px)"
        role="tooltip"
      >
        <Text color="$color" {...textStyles.bodySecondary}>{description}</Text>
      </Tooltip.Content>
    </Tooltip>
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
        <Text color="$color" {...textStyles.sectionTitle} $compact={textStyles.sectionTitleCompact}>
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
