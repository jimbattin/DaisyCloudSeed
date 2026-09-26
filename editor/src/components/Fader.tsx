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
}

const SEED_SCALE = 1e6;

export function Fader({ def, value, preset, badges, onChange }: StripProps) {
  const seed = def.entry === 'seed';
  return (
    <div class="strip" title={def.description}>
      <Led text={formatValue(def.key, value, preset)} />
      <input
        type="range"
        min={0}
        max={1}
        step={0.0001}
        value={value}
        aria-label={def.label}
        title={def.description}
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

export function SwitchStrip({ def, value, preset, badges, onChange }: StripProps) {
  const on = value >= 0.5;
  return (
    <div class="strip" title={def.description}>
      <Led text={formatValue(def.key, value, preset)} />
      <div class="switch-slot">
        <Key label={on ? 'ON' : 'OFF'} pressed={on} title={def.description} onClick={() => onChange(on ? 0 : 1)} />
      </div>
      <span class="silk">{def.label}</span>
      <span class="badges">{badges.join(' ')}</span>
    </div>
  );
}
