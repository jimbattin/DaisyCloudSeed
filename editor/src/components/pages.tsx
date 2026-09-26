import type { Dispatch } from 'preact/hooks';
import type { Preset } from '../model/bank';
import { MAX_NAME_BYTES, TOTAL_LINE_COUNT } from '../model/bank';
import { GROUPS, KNOB_TARGETS, PAGES, PARAMS, TOGGLE_TARGETS, type PageId } from '../model/schema';
import { sanitizeName, type Action, type ScalarField } from '../model/state';
import { Key, NumberField } from './controls';
import { Fader, SwitchStrip } from './Fader';

export function PageKeys({ page, onPage }: { page: PageId; onPage: (p: PageId) => void }) {
  return (
    <div class="page-keys">
      {PAGES.map((p) => (
        <Key key={p.id} label={p.label} wide pressed={p.id === page} dark onClick={() => onPage(p.id)} />
      ))}
    </div>
  );
}

/** Knob and toggle slots of `preset` that target `key`, e.g. 'K1A', 'S2B'. */
function badgesFor(preset: Preset, key: string): string[] {
  const out: string[] = [];
  preset.knobMap.forEach((bank, b) =>
    bank.forEach((t, k) => t === key && out.push(`K${k + 1}${b ? 'B' : 'A'}`)),
  );
  preset.toggleMap.forEach((bank, b) =>
    bank.forEach((t, k) => t === key && out.push(`S${k + 1}${b ? 'B' : 'A'}`)),
  );
  return out;
}

const BANK_SLOTS = 6;

export function ParamPage({ page, preset, dispatch }: { page: PageId; preset: Preset; dispatch: Dispatch<Action> }) {
  const keys = PAGES.find((p) => p.id === page)!.keys;
  return (
    <div class="fader-bank">
      {keys.map((key) => {
        const def = PARAMS[key];
        const Strip = def.kind === 'switch' ? SwitchStrip : Fader;
        return (
          <Strip
            key={key}
            def={def}
            value={preset.params[key]}
            preset={preset}
            badges={badgesFor(preset, key)}
            onChange={(value) => dispatch({ type: 'setParam', key, value })}
          />
        );
      })}
      {Array.from({ length: Math.max(0, BANK_SLOTS - keys.length) }, (_, i) => (
        <div key={`empty${i}`} class="strip empty" />
      ))}
    </div>
  );
}

function TargetSelect({
  value,
  options,
  label,
  onChange,
}: {
  value: string;
  options: string[];
  label: string;
  onChange: (target: string) => void;
}) {
  const groups = [...Object.keys(GROUPS), 'reverse', 'delay_lines'];
  return (
    <select
      class="led"
      aria-label={label}
      title={`${value}: ${PARAMS[value].description}`}
      value={value}
      onChange={(e) => onChange(e.currentTarget.value)}
    >
      {groups.map((g) => {
        const inGroup = options.filter((t) => t.startsWith(`${g}.`));
        return inGroup.length === 0 ? null : (
          <optgroup key={g} label={g}>
            {inGroup.map((t) => (
              <option key={t} value={t}>
                {t.slice(g.length + 1)}
              </option>
            ))}
          </optgroup>
        );
      })}
    </select>
  );
}

function MapPage({
  preset,
  kind,
  dispatch,
}: {
  preset: Preset;
  kind: 'knob' | 'toggle';
  dispatch: Dispatch<Action>;
}) {
  const map = kind === 'knob' ? preset.knobMap : preset.toggleMap;
  const options = kind === 'knob' ? KNOB_TARGETS : TOGGLE_TARGETS;
  const title = kind === 'knob' ? 'KNOB' : 'SW';
  return (
    <div>
      <div class={`map-grid ${kind}s`}>
        {map[0].map((_, i) => (
          <div key={i} class="map-col panel">
            <span class="silk">
              {title} {i + 1}
            </span>
            {([0, 1] as const).map((bank) => (
              <TargetSelect
                key={bank}
                label={`${title} ${i + 1} ${bank ? 'B' : 'A'}`}
                value={map[bank][i]}
                options={options}
                onChange={(target) =>
                  dispatch(
                    kind === 'knob'
                      ? { type: 'setKnob', bank, knob: i, target }
                      : { type: 'setToggle', bank, toggle: i, target },
                  )
                }
              />
            ))}
          </div>
        ))}
      </div>
      <p class="silk">A = normal, B = while FS2 held</p>
    </div>
  );
}

export const KnobMapPage = (props: { preset: Preset; dispatch: Dispatch<Action> }) => <MapPage {...props} kind="knob" />;
export const ToggleMapPage = (props: { preset: Preset; dispatch: Dispatch<Action> }) => (
  <MapPage {...props} kind="toggle" />
);

export function SetupPage({ preset, dispatch }: { preset: Preset; dispatch: Dispatch<Action> }) {
  const scalar = (field: ScalarField, label: string, value: number, min: number, max: number, step = 1) => (
    <label class="silk">
      {label}
      <NumberField
        class="led"
        label={label}
        value={value}
        min={min}
        max={max}
        step={step}
        display={String}
        onCommit={(v) => dispatch({ type: 'setScalar', field, value: v })}
      />
    </label>
  );
  return (
    <div class="setup-grid">
      <label class="silk">
        Name
        <input
          class="led"
          aria-label="Name"
          maxLength={MAX_NAME_BYTES}
          value={preset.name}
          onInput={(e) => {
            const el = e.currentTarget;
            const name = sanitizeName(el.value);
            // A stripped character leaves the state unchanged, so no re-render would remove it.
            if (name !== el.value) el.value = name;
            dispatch({ type: 'setName', name });
          }}
        />
      </label>
      {scalar('blinks', 'Blinks', preset.blinks, 1, 20)}
      {scalar('led_on_ms', 'LED on ms', preset.ledOnMs, 0, 60000, 10)}
      {scalar('led_off_ms', 'LED off ms', preset.ledOffMs, 0, 60000, 10)}
      {scalar('led_pause_ms', 'LED pause ms', preset.ledPauseMs, 0, 60000, 10)}
      {scalar('default_delay_lines', 'Default lines', preset.defaultDelayLines, 1, Math.min(TOTAL_LINE_COUNT, preset.maxDelayLines))}
      {scalar('max_delay_lines', 'Max lines', preset.maxDelayLines, Math.max(1, preset.defaultDelayLines), TOTAL_LINE_COUNT)}
    </div>
  );
}
