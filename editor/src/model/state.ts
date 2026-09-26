import {
  MAX_NAME_BYTES,
  MAX_PRESETS,
  TOTAL_LINE_COUNT,
  deletePreset,
  docText,
  duplicatePreset,
  formatNorm,
  loadBank,
  setField,
  type BankDoc,
  type Preset,
} from './bank';
import { KNOB_TARGETS, TOGGLE_TARGETS, type PageId } from './schema';

export type Source = 'project' | 'file' | 'pedal';

export interface EditorState {
  doc: BankDoc | null;
  presets: Preset[];
  selected: number;
  page: PageId;
  /** Text as last loaded or saved; the bank is dirty when it differs. */
  savedText: string;
  source: Source | null;
  fileName: string | null;
  error: string | null;
}

export type ScalarField =
  | 'blinks'
  | 'led_on_ms'
  | 'led_off_ms'
  | 'led_pause_ms'
  | 'default_delay_lines'
  | 'max_delay_lines';

export type Action =
  | { type: 'load'; text: string; source: Source; fileName: string | null }
  | { type: 'select'; preset: number }
  | { type: 'page'; page: PageId }
  | { type: 'setParam'; key: string; value: number }
  | { type: 'setName'; name: string }
  | { type: 'setScalar'; field: ScalarField; value: number }
  | { type: 'setKnob'; bank: 0 | 1; knob: number; target: string }
  | { type: 'setToggle'; bank: 0 | 1; toggle: number; target: string }
  | { type: 'duplicate' }
  | { type: 'delete' }
  | { type: 'saved'; text: string; fileName: string | null }
  | { type: 'dismissError' };

export const initialState: EditorState = {
  doc: null,
  presets: [],
  selected: 0,
  page: 'input',
  savedText: '',
  source: null,
  fileName: null,
  error: null,
};

export const isDirty = (s: EditorState): boolean => s.doc !== null && docText(s.doc) !== s.savedText;

/** Blocks save and upload: the only rule the editor itself can break. */
export const problems = (s: EditorState): string[] =>
  s.presets.flatMap((p, i) => (p.name === '' ? [`preset ${i}: empty name`] : []));

/** Printable ASCII without `"` and `\`, so a name is always a plain TOML basic string. */
export const sanitizeName = (name: string): string => name.replace(/[^\x20-\x7e]|["\\]/g, '').slice(0, MAX_NAME_BYTES);

const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));

const SCALAR_PROP: Record<ScalarField, keyof Preset> = {
  blinks: 'blinks',
  led_on_ms: 'ledOnMs',
  led_off_ms: 'ledOffMs',
  led_pause_ms: 'ledPauseMs',
  default_delay_lines: 'defaultDelayLines',
  max_delay_lines: 'maxDelayLines',
};

/** Patches one field of the selected preset in both the text and the parsed copy. */
function edit(s: EditorState, field: string, literal: string, update: Partial<Preset>): EditorState {
  const doc = setField(s.doc!, s.selected, field, literal);
  const presets = [...s.presets];
  presets[s.selected] = { ...presets[s.selected], ...update };
  return { ...s, doc, presets };
}

/** Reparses text the editor produced itself; failure would be an editor bug. */
function reload(text: string) {
  const r = loadBank(text);
  if (!r.ok) throw new Error(`editor produced an invalid bank: ${r.error}`);
  return r;
}

export function reducer(s: EditorState, a: Action): EditorState {
  if (a.type === 'load') {
    const r = loadBank(a.text);
    if (!r.ok) return { ...s, error: r.error };
    return {
      ...s,
      doc: r.doc,
      presets: r.presets,
      selected: 0,
      savedText: a.text,
      source: a.source,
      fileName: a.fileName,
      error: null,
    };
  }
  if (a.type === 'page') return { ...s, page: a.page };
  if (a.type === 'dismissError') return { ...s, error: null };
  if (a.type === 'saved') return { ...s, savedText: a.text, fileName: a.fileName };
  if (!s.doc) return s;

  const preset = s.presets[s.selected];
  switch (a.type) {
    case 'select':
      return a.preset >= 0 && a.preset < s.presets.length ? { ...s, selected: a.preset } : s;

    case 'setParam': {
      if (!(a.key in preset.params)) return s;
      const v = Number(formatNorm(clamp(a.value, 0, 1)));
      return edit(s, `params.${a.key}`, formatNorm(v), { params: { ...preset.params, [a.key]: v } });
    }

    case 'setName': {
      const name = sanitizeName(a.name);
      return edit(s, 'name', `"${name}"`, { name });
    }

    case 'setScalar': {
      const n = Math.round(a.value);
      if (!Number.isFinite(n)) return s;
      let v: number;
      switch (a.field) {
        case 'blinks':
          v = clamp(n, 1, 20);
          break;
        case 'default_delay_lines':
          v = clamp(n, 1, Math.min(TOTAL_LINE_COUNT, preset.maxDelayLines));
          break;
        case 'max_delay_lines':
          v = clamp(n, Math.max(1, preset.defaultDelayLines), TOTAL_LINE_COUNT);
          break;
        default:
          v = clamp(n, 0, 60000);
      }
      const literal = a.field.endsWith('delay_lines') ? `${v}.0` : String(v);
      const next = edit(s, a.field, literal, { [SCALAR_PROP[a.field]]: v });
      // Inserting an absent LED timing adds a line, so the parsed copy is rebuilt from the text.
      return next.doc!.lines.length === s.doc.lines.length ? next : { ...next, presets: reload(docText(next.doc!)).presets };
    }

    case 'setKnob': {
      if (!KNOB_TARGETS.includes(a.target) || a.knob < 0 || a.knob > 5) return s;
      const knobMap: [string[], string[]] = [[...preset.knobMap[0]], [...preset.knobMap[1]]];
      knobMap[a.bank][a.knob] = a.target;
      return edit(s, `knob_map.knob${a.knob + 1}_${a.bank ? 'b' : 'a'}`, `"${a.target}"`, { knobMap });
    }

    case 'setToggle': {
      if (!TOGGLE_TARGETS.includes(a.target) || a.toggle < 0 || a.toggle > 3) return s;
      const toggleMap: [string[], string[]] = [[...preset.toggleMap[0]], [...preset.toggleMap[1]]];
      toggleMap[a.bank][a.toggle] = a.target;
      return edit(s, `toggle_map.toggle${a.toggle + 1}_${a.bank ? 'b' : 'a'}`, `"${a.target}"`, { toggleMap });
    }

    case 'duplicate': {
      if (s.presets.length >= MAX_PRESETS) return s;
      const index = s.presets.length;
      let doc = duplicatePreset(s.doc, s.selected);
      doc = setField(doc, index, 'name', `"${preset.name.slice(0, MAX_NAME_BYTES - 5)} copy"`);
      doc = setField(doc, index, 'blinks', String(Math.min(index + 1, 20)));
      return { ...s, doc, presets: reload(docText(doc)).presets, selected: index };
    }

    case 'delete': {
      if (s.presets.length <= 1) return s;
      const { doc, presets } = reload(docText(deletePreset(s.doc, s.selected)));
      return { ...s, doc, presets, selected: Math.min(s.selected, presets.length - 1) };
    }
  }
}
