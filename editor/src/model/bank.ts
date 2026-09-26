// The preset bank as text. Validation mirrors ParsePresetBankText() in src/preset_bank.cpp,
// message for message; edits patch only the value token of one line, so an unedited bank
// serializes byte-identically (same pedal hash, minimal git diffs).

import { parse } from 'smol-toml';
import { GROUPS, PARAMETER_ORDER, RUNTIME_PARAMETER, TOGGLE_PARAMS } from './schema';

export const MAX_TEXT_BYTES = 98304; // PresetProtocol::kMaxTextBytes
export const MAX_PRESETS = 16; // kMaxPresets
export const MAX_NAME_BYTES = 31; // kMaxPresetNameLen - 1
export const TOTAL_LINE_COUNT = 5; // CloudSeed::TotalLineCount

export interface Preset {
  name: string;
  blinks: number;
  ledOnMs: number;
  ledOffMs: number;
  ledPauseMs: number;
  defaultDelayLines: number;
  maxDelayLines: number;
  /** Keyed 'group.Name', as schema PARAM_KEYS. */
  params: Record<string, number>;
  /** [0] = _a, [1] = _b; knob 0..5. */
  knobMap: [string[], string[]];
  /** [0] = _a, [1] = _b; toggle 0..3. */
  toggleMap: [string[], string[]];
}

/** Value token columns within a line. */
export interface FieldLoc {
  line: number;
  start: number;
  end: number;
}

export interface PresetBlock {
  /** First line of the block: the separator comment above [[preset]], else the header. */
  start: number;
  /** One past the last line. */
  end: number;
  header: number;
  fields: Map<string, FieldLoc>;
}

export interface BankDoc {
  lines: string[];
  eol: '\n' | '\r\n';
  presets: PresetBlock[];
}

export type LoadResult = { ok: true; doc: BankDoc; presets: Preset[] } | { ok: false; error: string };

// ---------------------------------------------------------------------------------------------
// Text layout

const SEPARATOR = /^#\s*-{3,}\s*$/;
const TABLE = /^\s*\[preset\.(.+)\]\s*(#.*)?$/;
const KEY = /^\s*([A-Za-z0-9_]+)\s*=\s*/;

export function scanDoc(text: string): BankDoc {
  const eol = text.includes('\r\n') ? '\r\n' : '\n';
  const lines = text.split(/\r?\n/);
  const presets: PresetBlock[] = [];
  let path = '';

  lines.forEach((line, i) => {
    const trimmed = line.trim();
    if (trimmed === '[[preset]]') {
      let start = i;
      while (start > 0 && SEPARATOR.test(lines[start - 1].trim())) start--;
      const prev = presets.at(-1);
      if (prev) prev.end = start;
      presets.push({ start, end: lines.length, header: i, fields: new Map() });
      path = '';
      return;
    }
    if (trimmed === '' || trimmed.startsWith('#')) return;
    const table = TABLE.exec(line);
    if (table) {
      path = table[1].trim();
      return;
    }
    const key = KEY.exec(line);
    const block = presets.at(-1);
    if (!key || !block) return;
    const start = key[0].length;
    let end: number;
    if (line[start] === '"') {
      end = start + 1;
      while (end < line.length && !(line[end] === '"' && line[end - 1] !== '\\')) end++;
      end = Math.min(end + 1, line.length);
    } else {
      const hash = line.indexOf('#', start);
      end = hash < 0 ? line.length : hash;
      while (end > start && /\s/.test(line[end - 1])) end--;
    }
    block.fields.set(path ? `${path}.${key[1]}` : key[1], { line: i, start, end });
  });

  return { lines, eol, presets };
}

export const docText = (doc: BankDoc): string => doc.lines.join(doc.eol);

// ---------------------------------------------------------------------------------------------
// Validation (src/preset_bank.cpp)

type Table = Record<string, unknown>;

class Reject extends Error {}

const isTable = (v: unknown): v is Table =>
  v !== null && typeof v === 'object' && !Array.isArray(v) && !(v instanceof Date);

/** printf("%g"): 6 significant digits, trailing zeros removed, exponent form outside 1e-4..1e6. */
export function fmtG(v: number): string {
  if (Number.isNaN(v)) return 'nan';
  if (!Number.isFinite(v)) return v < 0 ? '-inf' : 'inf';
  if (v === 0) return Object.is(v, -0) ? '-0' : '0';
  const [mantissa, expText] = v.toExponential(5).split('e');
  const exp = Number(expText);
  const strip = (s: string) => (s.includes('.') ? s.replace(/0+$/, '').replace(/\.$/, '') : s);
  if (exp < -4 || exp >= 6) {
    return `${strip(mantissa)}e${exp < 0 ? '-' : '+'}${String(Math.abs(exp)).padStart(2, '0')}`;
  }
  return strip(v.toFixed(5 - exp));
}

/** toml_double_in, then toml_int_in: any TOML number. */
function readNumber(t: Table, key: string): number | undefined {
  const v = t[key];
  if (typeof v === 'number') return v;
  if (typeof v === 'bigint') return Number(v);
  return undefined;
}

function readWhole(t: Table, key: string, lo: number, hi: number): number | undefined {
  const v = readNumber(t, key);
  return v !== undefined && v >= lo && v <= hi && Number.isInteger(v) ? v : undefined;
}

/** rejectUnknownKeys: tomlc99 lists scalars, then arrays, then tables. */
function rejectUnknownKeys(t: Table, allowed: readonly string[], context: string): void {
  const keys = Object.keys(t);
  const scalars = keys.filter((k) => !Array.isArray(t[k]) && !isTable(t[k]));
  const arrays = keys.filter((k) => Array.isArray(t[k]));
  const tables = keys.filter((k) => isTable(t[k]));
  for (const key of [...scalars, ...arrays, ...tables]) {
    if (!allowed.includes(key)) throw new Reject(`${context}unknown key '${key}'`);
  }
}

const GROUP_NAMES = Object.keys(GROUPS);
const groupOf = (name: string) => GROUP_NAMES.find((g) => GROUPS[g].includes(name));

function readPseudo(t: Table, group: string, key: string, index: number): number {
  const v = readNumber(t, key);
  if (v === undefined) throw new Reject(`preset ${index}: missing or non-numeric '${group}.${key}'`);
  if (!(v >= 0 && v <= 1)) throw new Reject(`preset ${index}: '${group}.${key}' = ${fmtG(v)} out of range 0..1`);
  return v;
}

function parseParams(preset: Table, index: number, out: Record<string, number>): void {
  const params = preset.params;
  if (!isTable(params)) throw new Reject(`preset ${index}: missing [preset.params]`);
  rejectUnknownKeys(params, [...GROUP_NAMES, 'reverse', 'delay_lines'], `preset ${index}: [preset.params]: `);

  const seen = new Set<string>();
  for (const g of GROUP_NAMES) {
    const group = params[g];
    if (!isTable(group)) throw new Reject(`preset ${index}: missing [preset.params.${g}]`);
    const keys = Object.keys(group);
    if (keys.some((k) => Array.isArray(group[k]) || isTable(group[k]))) {
      throw new Reject(`preset ${index}: group '${g}' must contain only parameter values`);
    }
    for (const key of keys) {
      if (!(PARAMETER_ORDER as readonly string[]).includes(key)) {
        throw new Reject(`preset ${index}: unknown parameter '${key}'`);
      }
      if (key === RUNTIME_PARAMETER) {
        throw new Reject(`preset ${index}: '${key}' is runtime-controlled and must not appear`);
      }
      const owner = groupOf(key)!;
      if (owner !== g) {
        throw new Reject(`preset ${index}: parameter '${key}' belongs in group '${owner}', found in '${g}'`);
      }
      const v = readNumber(group, key);
      if (v === undefined) throw new Reject(`preset ${index}: '${key}' is not a number`);
      if (!(v >= 0 && v <= 1)) throw new Reject(`preset ${index}: '${key}' = ${fmtG(v)} out of range 0..1`);
      out[`${g}.${key}`] = v;
      seen.add(key);
    }
  }
  for (const name of PARAMETER_ORDER) {
    if (name !== RUNTIME_PARAMETER && !seen.has(name)) {
      throw new Reject(`preset ${index}: missing parameter '${name}'`);
    }
  }

  const reverse = params.reverse;
  if (!isTable(reverse)) throw new Reject(`preset ${index}: missing [preset.params.reverse]`);
  rejectUnknownKeys(reverse, ['delay', 'enabled', 'direct_mix'], `preset ${index}: [preset.params.reverse]: `);
  for (const key of ['delay', 'enabled', 'direct_mix']) out[`reverse.${key}`] = readPseudo(reverse, 'reverse', key, index);

  const delayLines = params.delay_lines;
  if (!isTable(delayLines)) throw new Reject(`preset ${index}: missing [preset.params.delay_lines]`);
  rejectUnknownKeys(delayLines, ['max'], `preset ${index}: [preset.params.delay_lines]: `);
  out['delay_lines.max'] = readPseudo(delayLines, 'delay_lines', 'max', index);
}

/** splitTarget + resolveParamTarget: "group.Parameter" naming a file-controlled parameter. */
function splitTarget(text: string, map: string, key: string, index: number): [string, string] {
  const dot = text.indexOf('.');
  if (dot <= 0 || dot === text.length - 1) {
    throw new Reject(`preset ${index}: ${map}: ${key}: '${text}' must be "group.Parameter"`);
  }
  return [text.slice(0, dot), text.slice(dot + 1)];
}

function resolveParamTarget(text: string, group: string, name: string, map: string, key: string, index: number): void {
  const where = `preset ${index}: ${map}: ${key}: `;
  if (!GROUP_NAMES.includes(group)) throw new Reject(`${where}unknown group in '${text}'`);
  if (!(PARAMETER_ORDER as readonly string[]).includes(name)) throw new Reject(`${where}unknown parameter '${name}'`);
  if (name === RUNTIME_PARAMETER) throw new Reject(`${where}'${name}' is runtime-controlled and cannot be mapped`);
  if (groupOf(name) !== group) throw new Reject(`${where}'${name}' is not in group '${group}'`);
}

function parseKnobTarget(text: string, key: string, index: number): void {
  const [group, name] = splitTarget(text, 'knob_map', key, index);
  if (group === 'reverse') {
    if (name !== 'delay') throw new Reject(`preset ${index}: knob_map: ${key}: 'reverse' has only 'delay'`);
    return;
  }
  resolveParamTarget(text, group, name, 'knob_map', key, index);
}

function parseToggleTarget(text: string, key: string, index: number): void {
  const [group, name] = splitTarget(text, 'toggle_map', key, index);
  if (group === 'delay_lines') {
    if (name !== 'max') throw new Reject(`preset ${index}: toggle_map: ${key}: 'delay_lines' has only 'max'`);
    return;
  }
  if (group === 'reverse') {
    if (name !== 'enabled' && name !== 'direct_mix') {
      throw new Reject(`preset ${index}: toggle_map: ${key}: 'reverse' toggles only 'enabled' or 'direct_mix'`);
    }
    return;
  }
  resolveParamTarget(text, group, name, 'toggle_map', key, index);
  if (!TOGGLE_PARAMS.includes(name)) {
    throw new Reject(`preset ${index}: toggle_map: ${key}: '${name}' is not an on/off parameter`);
  }
}

function parseMap(
  preset: Table,
  index: number,
  map: 'knob_map' | 'toggle_map',
  count: number,
  resolve: (text: string, key: string, index: number) => void,
): [string[], string[]] {
  const noun = map === 'knob_map' ? 'knob' : 'toggle';
  const table = preset[map];
  if (!isTable(table)) throw new Reject(`preset ${index}: missing [preset.${map}]`);
  const keys = ['a', 'b'].map((bank) => Array.from({ length: count }, (_, i) => `${noun}${i + 1}_${bank}`));
  rejectUnknownKeys(table, keys.flat(), `preset ${index}: [preset.${map}]: `);
  const out: [string[], string[]] = [[], []];
  keys.forEach((bankKeys, bank) =>
    bankKeys.forEach((key) => {
      const value = table[key];
      if (typeof value !== 'string') throw new Reject(`preset ${index}: ${map}: missing or non-string '${key}'`);
      resolve(value, key, index);
      out[bank].push(value);
    }),
  );
  return out;
}

const PRESET_KEYS = [
  'name', 'blinks', 'led_on_ms', 'led_off_ms', 'led_pause_ms',
  'default_delay_lines', 'max_delay_lines', 'params', 'knob_map', 'toggle_map',
];

function readOptionalMs(preset: Table, key: string, index: number, fallback: number): number {
  if (!(key in preset)) return fallback;
  const ms = readWhole(preset, key, 0, 60000);
  if (ms === undefined) throw new Reject(`preset ${index}: ${key} must be a whole number 0..60000`);
  return ms;
}

function readLineCount(preset: Table, key: string, index: number): number {
  const lines = readWhole(preset, key, 1, TOTAL_LINE_COUNT);
  if (lines === undefined) throw new Reject(`preset ${index}: ${key} must be a whole number 1..${TOTAL_LINE_COUNT}`);
  return lines;
}

function parsePreset(preset: Table, index: number): Preset {
  rejectUnknownKeys(preset, PRESET_KEYS, `preset ${index}: `);

  const name = preset.name;
  if (typeof name !== 'string') throw new Reject(`preset ${index}: missing name`);
  const nameBytes = new TextEncoder().encode(name).length;
  if (nameBytes === 0) throw new Reject(`preset ${index}: empty name`);
  if (nameBytes > MAX_NAME_BYTES) throw new Reject(`preset ${index}: name longer than ${MAX_NAME_BYTES} bytes`);

  // toml_int_in: an integer literal only, so `3.0` is rejected like the firmware does.
  const blinks = preset.blinks;
  if (typeof blinks !== 'bigint' || blinks < 1n || blinks > 20n) {
    throw new Reject(`preset ${index}: blinks out of range 1..20`);
  }

  const ledOnMs = readOptionalMs(preset, 'led_on_ms', index, 150);
  const ledOffMs = readOptionalMs(preset, 'led_off_ms', index, 150);
  const ledPauseMs = readOptionalMs(preset, 'led_pause_ms', index, 5000);

  const maxDelayLines = readLineCount(preset, 'max_delay_lines', index);
  const defaultDelayLines = readLineCount(preset, 'default_delay_lines', index);
  if (defaultDelayLines > maxDelayLines) {
    throw new Reject(`preset ${index}: default_delay_lines exceeds max_delay_lines`);
  }

  const params: Record<string, number> = {};
  parseParams(preset, index, params);
  const knobMap = parseMap(preset, index, 'knob_map', 6, parseKnobTarget);
  const toggleMap = parseMap(preset, index, 'toggle_map', 4, parseToggleTarget);

  return {
    name,
    blinks: Number(blinks),
    ledOnMs,
    ledOffMs,
    ledPauseMs,
    defaultDelayLines,
    maxDelayLines,
    params,
    knobMap,
    toggleMap,
  };
}

/** checkAscii: the first byte >= 0x80, by 1-based line and byte column. */
function checkAscii(bytes: Uint8Array): void {
  let line = 1;
  let column = 1;
  for (const b of bytes) {
    if (b >= 0x80) {
      throw new Reject(`line ${line}, column ${column}: non-ASCII byte 0x${b.toString(16).toUpperCase().padStart(2, '0')}`);
    }
    if (b === 0x0a) {
      line++;
      column = 1;
    } else {
      column++;
    }
  }
}

/** Every field key the TOML document defines for one preset, as scanDoc names them. */
function fieldKeys(preset: Table): string[] {
  const out: string[] = [];
  const walk = (t: Table, prefix: string) => {
    for (const [k, v] of Object.entries(t)) {
      if (isTable(v)) walk(v, `${prefix}${k}.`);
      else out.push(`${prefix}${k}`);
    }
  };
  walk(preset, '');
  return out;
}

function validate(text: string): { doc: BankDoc; presets: Preset[] } {
  const bytes = new TextEncoder().encode(text);
  if (bytes.length === 0) throw new Reject('empty preset text');
  if (bytes.includes(0)) throw new Reject('NUL byte in preset text');
  if (bytes.length > MAX_TEXT_BYTES) {
    throw new Reject(`text is ${bytes.length} bytes; the pedal accepts at most ${MAX_TEXT_BYTES}`);
  }
  checkAscii(bytes);

  let root: Table;
  try {
    root = parse(text, { integersAsBigInt: true }) as Table;
  } catch (e) {
    throw new Reject(`toml: ${String((e as Error).message).split('\n')[0]}`);
  }

  const array = root.preset;
  if (!Array.isArray(array)) throw new Reject('missing [[preset]] array');
  if (array.length < 1 || array.length > MAX_PRESETS) {
    throw new Reject(`preset count ${array.length} out of range 1..${MAX_PRESETS}`);
  }
  rejectUnknownKeys(root, ['preset'], '');

  const presets = array.map((p, i) => {
    if (!isTable(p)) throw new Reject(`preset ${i}: not a table`);
    return parsePreset(p, i);
  });

  const doc = scanDoc(text);
  if (doc.presets.length !== presets.length) {
    throw new Reject(`unsupported layout: found ${doc.presets.length} [[preset]] lines for ${presets.length} presets`);
  }
  (array as Table[]).forEach((p, i) => {
    for (const field of fieldKeys(p)) {
      if (!doc.presets[i].fields.has(field)) {
        throw new Reject(`preset ${i}: '${field}' must be on its own "key = value" line to be editable`);
      }
    }
  });
  return { doc, presets };
}

export function loadBank(text: string): LoadResult {
  try {
    return { ok: true, ...validate(text) };
  } catch (e) {
    if (e instanceof Reject) return { ok: false, error: e.message };
    throw e;
  }
}

// ---------------------------------------------------------------------------------------------
// Edits

/** A 0..1 value as a TOML float literal: at most 6 decimals, always with a decimal point. */
export function formatNorm(v: number): string {
  const s = Number(v.toFixed(6)).toString();
  return s.includes('.') ? s : `${s}.0`;
}

const OPTIONAL_MS = ['led_on_ms', 'led_off_ms', 'led_pause_ms'];

/** Replaces one field's value token; an absent optional LED timing is inserted after blinks. */
export function setField(doc: BankDoc, preset: number, field: string, literal: string): BankDoc {
  const block = doc.presets[preset];
  const loc = block.fields.get(field);
  if (!loc) {
    const blinks = block.fields.get('blinks');
    if (!OPTIONAL_MS.includes(field) || !blinks) throw new Error(`preset ${preset}: no field '${field}'`);
    const lines = [...doc.lines];
    lines.splice(blinks.line + 1, 0, `${field} = ${literal}`);
    return scanDoc(lines.join(doc.eol));
  }
  const lines = [...doc.lines];
  const line = lines[loc.line];
  lines[loc.line] = line.slice(0, loc.start) + literal + line.slice(loc.end);
  const fields = new Map(block.fields);
  fields.set(field, { ...loc, end: loc.start + literal.length });
  const presets = [...doc.presets];
  presets[preset] = { ...block, fields };
  return { ...doc, lines, presets };
}

function trimTrailingBlank(lines: string[]): string[] {
  let end = lines.length;
  while (end > 0 && lines[end - 1] === '') end--;
  return lines.slice(0, end);
}

/** Appends a copy of preset `i`'s block at the end of the bank. */
export function duplicatePreset(doc: BankDoc, i: number): BankDoc {
  const { start, end } = doc.presets[i];
  const block = trimTrailingBlank(doc.lines.slice(start, end));
  return scanDoc([...trimTrailingBlank(doc.lines), '', ...block, ''].join(doc.eol));
}

/** Removes preset `i`'s block; later presets move down one slot. */
export function deletePreset(doc: BankDoc, i: number): BankDoc {
  const { start, end } = doc.presets[i];
  const lines = [...doc.lines.slice(0, start), ...doc.lines.slice(end)];
  return scanDoc([...trimTrailingBlank(lines), ''].join(doc.eol));
}
