import type { ComponentChildren } from 'preact';

export function Led({ text, wide, small, off }: { text: string; wide?: boolean; small?: boolean; off?: boolean }) {
  const cls = ['led', wide && 'wide', small && 'small', off && 'off'].filter(Boolean).join(' ');
  return (
    <div class={cls} title={text}>
      {text}
    </div>
  );
}

export interface KeyProps {
  label: string;
  pressed?: boolean;
  disabled?: boolean;
  dark?: boolean;
  wide?: boolean;
  title?: string;
  /** Edit mark in the top-right corner: a dot for changed, a ring for added since load/save. */
  mark?: 'edited' | 'added';
  onClick: () => void;
}

export function Key({ label, pressed, disabled, dark, wide, title, mark, onClick }: KeyProps) {
  return (
    <button
      type="button"
      class={['key', dark && 'dark', wide && 'wide', mark].filter(Boolean).join(' ')}
      aria-pressed={pressed ? 'true' : 'false'}
      disabled={disabled}
      title={title}
      onClick={onClick}
    >
      <span class="key-led" />
      {mark && <span class="edit-mark" />}
      {label}
    </button>
  );
}

export interface NumberFieldProps {
  value: number;
  min: number;
  max: number;
  step: number;
  /** Text shown for `value`. */
  display: (v: number) => string;
  onCommit: (v: number) => void;
  label?: string;
  class?: string;
}

/**
 * A number input that commits on `change`. Input that is not a number or is outside min..max
 * reverts to the current value; a field with a whole-number step rounds.
 */
export function NumberField({ value, min, max, step, display, onCommit, label, class: cls }: NumberFieldProps) {
  return (
    <input
      type="number"
      class={cls}
      aria-label={label}
      min={min}
      max={max}
      step={step}
      value={display(value)}
      onChange={(e) => {
        const el = e.currentTarget;
        let n = el.value.trim() === '' ? NaN : Number(el.value);
        if (!Number.isFinite(n) || n < min || n > max) {
          el.value = display(value);
          return;
        }
        if (Number.isInteger(step)) n = Math.round(n);
        el.value = display(n);
        onCommit(n);
      }}
    />
  );
}

export interface ConfirmDialogProps {
  open: boolean;
  title: string;
  body: ComponentChildren;
  confirmLabel: string;
  onConfirm: () => void;
  onCancel: () => void;
}

export function ConfirmDialog({ open, title, body, confirmLabel, onConfirm, onCancel }: ConfirmDialogProps) {
  if (!open) return null;
  return (
    <dialog open>
      <article>
        <header>
          <strong>{title}</strong>
        </header>
        <p>{body}</p>
        <footer>
          <button type="button" class="secondary" onClick={onCancel}>
            Cancel
          </button>{' '}
          <button type="button" onClick={onConfirm} autoFocus>
            {confirmLabel}
          </button>
        </footer>
      </article>
    </dialog>
  );
}
