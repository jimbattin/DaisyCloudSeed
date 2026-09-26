import type { LineCounts } from '../model/scale';
import { formatValue } from '../model/scale';
import type { ParamDef } from '../model/schema';
import { Key, Led, NumberField } from './controls';

export interface StripProps {
  def: ParamDef;
  value: number;
  preset: LineCounts;
  /** Knob / toggle slots mapped to this parameter, e.g. ['K1A', 'S2B']. */
  badges: string[];
  onChange: (v: number) => void;
  /** Display text of the value as loaded or saved, present only when `value` differs from it. */
  original?: string;
  /** Reports this strip's key while pointed at or focused, null on leave. */
  onHover: (key: string | null) => void;
}

const SEED_SCALE = 1e6;

const tooltip = (def: ParamDef, original: string | undefined) =>
  original === undefined ? def.description : `${def.description}\nOriginal: ${original}`;

/** Pointer and keyboard focus handlers that report `key` to `onHover`. */
const hoverHandlers = (key: string, onHover: (key: string | null) => void) => ({
  onMouseEnter: () => onHover(key),
  onMouseLeave: () => onHover(null),
  onFocusIn: () => onHover(key),
  onFocusOut: () => onHover(null),
});

export function Fader({ def, value, preset, badges, onChange, original, onHover }: StripProps) {
  const seed = def.entry === 'seed';
  return (
    <div class="strip" title={tooltip(def, original)} {...hoverHandlers(def.key, onHover)}>
      {original !== undefined && <span class="edit-mark" />}
      <Led text={formatValue(def.key, value, preset)} />
      <input
        type="range"
        min={0}
        max={1}
        step={0.0001}
        value={value}
        aria-label={def.label}
        title={tooltip(def, original)}
        onInput={(e) => onChange(Number(e.currentTarget.value))}
      />
      <span class="silk">{def.label}</span>
      <span class="badges">{badges.join(' ')}</span>
      {seed ? (
        <NumberField
          label={`${def.label} seed`}
          value={value}
          min={0}
          max={SEED_SCALE}
          step={1}
          display={(v) => String(Math.floor(v * SEED_SCALE + 0.001))}
          onCommit={(n) => onChange(n / SEED_SCALE)}
        />
      ) : (
        <NumberField
          label={`${def.label} value`}
          value={value}
          min={0}
          max={1}
          step={0.0001}
          display={(v) => String(Number(v.toFixed(6)))}
          onCommit={onChange}
        />
      )}
    </div>
  );
}

export function SwitchStrip({ def, value, preset, badges, onChange, original, onHover }: StripProps) {
  const on = value >= 0.5;
  return (
    <div class="strip" title={tooltip(def, original)} {...hoverHandlers(def.key, onHover)}>
      {original !== undefined && <span class="edit-mark" />}
      <Led text={formatValue(def.key, value, preset)} />
      <div class="switch-slot">
        <Key label={on ? 'ON' : 'OFF'} pressed={on} title={tooltip(def, original)} onClick={() => onChange(on ? 0 : 1)} />
      </div>
      <span class="silk">{def.label}</span>
      <span class="badges">{badges.join(' ')}</span>
    </div>
  );
}
