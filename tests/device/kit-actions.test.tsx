import { afterEach, expect, it, vi } from 'vitest';
const mode = vi.hoisted(() => ({ reduced: false }));
vi.mock('tamagui', () => ({ Button: 'button' }));
vi.mock('../../lib/ui/motionContext', () => ({ useReducedMotion: () => mode.reduced }));
import { AppButton } from '../../components/AppButton';
afterEach(() => { mode.reduced = false; });
it('uses kit accessible action tokens and a 44px small target', () => {
  const button = AppButton({ children: 'TEST', size: 'sm' });
  expect(button.props.minHeight).toBe(44);
  expect(button.props.backgroundColor).toBe('$actionPrimary');
  expect(AppButton({ children: 'TEST', variant: 'danger' }).props.backgroundColor).toBe('$actionDanger');
});
it('respects reduced motion without removing focus or action feedback', () => {
  mode.reduced = true;
  const button = AppButton({ children: 'TEST' });
  expect(button.props.pressStyle.scale).toBeUndefined();
  expect(button.props.focusStyle.outlineColor).toBe('$focusRing');
  expect(button.props.transition).toBeUndefined();
});
