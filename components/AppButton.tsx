import type { ReactNode } from 'react';
import { Button } from 'tamagui';

export type AppButtonVariant = 'primary' | 'secondary' | 'ghost' | 'danger';
export type AppButtonSize = 'sm' | 'md' | 'lg';

export interface AppButtonProps {
  children: ReactNode;
  variant?: AppButtonVariant;
  size?: AppButtonSize;
  onPress?: () => void;
  disabled?: boolean;
  ariaLabel?: string;
  fullWidth?: boolean;
}

const sizeStyles: Record<AppButtonSize, { minHeight: number; paddingHorizontal: string; borderRadius: string }> = {
  sm: { minHeight: 36, paddingHorizontal: '$3', borderRadius: '$3' },
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
  fullWidth = false,
}: AppButtonProps) {
  const styles = sizeStyles[size];
  const variantStyles = {
    primary: { backgroundColor: '$primary', color: '$white', borderWidth: 0 },
    secondary: { backgroundColor: '$surface', color: '$color', borderWidth: 1, borderColor: '$borderColor' },
    ghost: { backgroundColor: 'transparent', color: '$primary', borderWidth: 0 },
    danger: { backgroundColor: '$statusSafetyAlert', color: '$white', borderWidth: 0 },
  }[variant];

  return (
    <Button
      {...styles}
      {...variantStyles}
      width={fullWidth ? '100%' : undefined}
      fontSize={size === 'sm' ? 13 : 14}
      fontWeight="700"
      disabled={disabled}
      opacity={disabled ? 0.5 : 1}
      onPress={onPress}
      aria-label={ariaLabel}
      type="button"
    >
      {children}
    </Button>
  );
}
