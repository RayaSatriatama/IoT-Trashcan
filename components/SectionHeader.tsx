import { useRef, useState, type ReactNode } from 'react';
import { Button, Text, Tooltip, XStack } from 'tamagui';
import { Info } from '../lib/ui/icons';
import { textStyles } from '../lib/ui/typography';
import { useReducedMotion } from '../lib/ui/motionContext';

export function DescriptionHint({
  description,
  ariaLabel = 'Lihat informasi',
}: {
  description: string;
  ariaLabel?: string;
}) {
  const reduceMotion = useReducedMotion();
  const [open, setOpen] = useState(false);
  const pressedOpen = useRef<boolean | null>(null);
  return (
    <Tooltip open={open} onOpenChange={setOpen} delay={0} restMs={120} placement="bottom-end" allowFlip zIndex={1000} focus={{ enabled: true, visibleOnly: false }}>
      <Tooltip.Trigger asChild>
        <Button
          minWidth={44}
          minHeight={44}
          width={44}
          height={44}
          padding={0}
          borderRadius="$6"
          borderWidth={0}
          backgroundColor="transparent"
          alignItems="center"
          justifyContent="center"
          aria-label={ariaLabel}
          onFocus={() => { pressedOpen.current = false; setOpen(true); }}
          onBlur={() => { pressedOpen.current = null; setOpen(false); }}
          onPressIn={() => { pressedOpen.current ??= open; }}
          onPress={() => {
            // Pointer focus can open the hint before the matching press fires.
            setOpen(!(pressedOpen.current ?? open));
            pressedOpen.current = null;
          }}
          hoverStyle={{ backgroundColor: '$surfaceMuted' }}
          pressStyle={{ backgroundColor: '$primarySoft' }}
          focusStyle={{ backgroundColor: '$primarySoft', outlineColor: '$primary', outlineWidth: 2, outlineStyle: 'solid' }}
          type="button"
          transition={reduceMotion ? undefined : 'quick'}
        >
          <Info size={17} strokeWidth={2} color="#60789F" aria-hidden />
        </Button>
      </Tooltip.Trigger>
      <Tooltip.Content
        onEscapeKeyDown={() => setOpen(false)}
        transition={reduceMotion ? undefined : 'quick'}
        enterStyle={{ opacity: 0, scale: 0.98 }}
        exitStyle={{ opacity: 0, scale: 0.98 }}
        borderWidth={1}
        shadowColor="$shadowColor"
        shadowRadius={10}
        shadowOffset={{ height: 5, width: 0 }}
        elevationAndroid={10}
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
        <Text color="$color" {...textStyles.sectionTitle} $compact={textStyles.sectionTitleCompact} flexShrink={1}>
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
