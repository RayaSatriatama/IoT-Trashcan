import type { ReactNode } from 'react';
import { Button } from 'tamagui';
import { useReducedMotion } from '../lib/ui/motionContext';

export type AppButtonVariant = 'primary' | 'secondary' | 'ghost' | 'danger';
export type AppButtonSize = 'sm' | 'md' | 'lg';

export interface AppButtonProps {
  children: ReactNode;
  variant?: AppButtonVariant;
  size?: AppButtonSize;
  onPress?: () => void;
  disabled?: boolean;
  ariaLabel?: string;
  ariaExpanded?: boolean;
  fullWidth?: boolean;
}

const sizeStyles: Record<AppButtonSize, { minHeight: number; paddingHorizontal: string; borderRadius: string }> = {
  sm: { minHeight: 44, paddingHorizontal: '$3', borderRadius: '$3' },
  md: { minHeight: 44, paddingHorizontal: '$4', borderRadius: '$3' },
  lg: { minHeight: 48, paddingHorizontal: '$5', borderRadius: '$3' },
};

export function AppButton({
  children,
  variant = 'primary',
  size = 'md',
  onPress,
  disabled = false,
  ariaLabel,
  ariaExpanded,
  fullWidth = false,
}: AppButtonProps) {
  const reduceMotion = useReducedMotion();
  const styles = sizeStyles[size];
  const variantStyles = {
    primary: { backgroundColor: '$actionPrimary', color: '$white', borderWidth: 0 },
    secondary: { backgroundColor: '$surface', color: '$color', borderWidth: 1, borderColor: '$borderColor' },
    ghost: { backgroundColor: 'transparent', color: '$actionPrimary', borderWidth: 0 },
    danger: { backgroundColor: '$actionDanger', color: '$white', borderWidth: 0 },
  }[variant];

  return (
    <Button
      {...styles}
      {...variantStyles}
      width={fullWidth ? '100%' : undefined}
      alignSelf={fullWidth ? 'stretch' : 'flex-start'}
      fontSize={size === 'sm' ? 13 : 14}
      fontWeight="700"
      disabled={disabled}
      opacity={disabled ? 0.5 : 1}
      hoverStyle={disabled ? undefined : { opacity: 0.95, ...(reduceMotion ? {} : { y: -1 }) }}
      pressStyle={disabled ? undefined : { opacity: 0.9, ...(reduceMotion ? {} : { scale: 0.985 }) }}
      focusStyle={disabled ? undefined : { outlineColor: '$focusRing', outlineWidth: 3, outlineStyle: 'solid' }}
      onPress={onPress}
      aria-label={ariaLabel}
      aria-expanded={ariaExpanded}
      type="button"
      transition={reduceMotion ? undefined : 'quick'}
    >
      {children}
    </Button>
  );
}
