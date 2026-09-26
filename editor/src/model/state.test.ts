import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { docText } from './bank';
import { initialState, isDirty, presetChanges, problems, reducer, type Action, type EditorState } from './state';

const fixture = readFileSync(new URL('../../../tests/fixtures/two_presets.toml', import.meta.url), 'utf8');

const run = (s: EditorState, ...actions: Action[]) => actions.reduce(reducer, s);
const loaded = run(initialState, { type: 'load', text: fixture, source: 'file', fileName: 'two.toml' });
const line = (s: EditorState, prefix: string) =>
  docText(s.doc!).split('\n').find((l) => l.startsWith(prefix));

describe('reducer', () => {
  it('loads a bank', () => {
    expect(loaded.presets).toHaveLength(2);
    expect(loaded.error).toBeNull();
    expect(isDirty(loaded)).toBe(false);
  });

  it.each([
    [0.5, 'PreDelay       = 0.5'],
    [1, 'PreDelay       = 1.0'],
    [0.123456789, 'PreDelay       = 0.123457'],
    [1.7, 'PreDelay       = 1.0'],
  ])('setParam %f writes %s', (value, expected) => {
    const s = run(loaded, { type: 'setParam', key: 'input.PreDelay', value });
    expect(line(s, 'PreDelay')).toBe(expected);
    expect(s.presets[0].params['input.PreDelay']).toBe(Number(expected.split('= ')[1]));
  });

  it('clamps default_delay_lines to the preset max', () => {
    const s = run(loaded, { type: 'select', preset: 1 }, { type: 'setScalar', field: 'default_delay_lines', value: 5 });
    expect(s.presets[1].defaultDelayLines).toBe(4);
  });

  it('duplicates to the end with a copy name and the next blink count', () => {
    const s = run(loaded, { type: 'duplicate' });
    expect(s.selected).toBe(2);
    expect(s.presets[2].name).toBe('Chorus copy');
    expect(s.presets[2].blinks).toBe(3);
  });

  it('never grows past 16 presets or deletes the last one', () => {
    const full = run(loaded, ...Array<Action>(14).fill({ type: 'duplicate' }));
    expect(full.presets).toHaveLength(16);
    expect(run(full, { type: 'duplicate' })).toBe(full);
    const single = run(loaded, { type: 'delete' });
    expect(single.presets.map((p) => p.name)).toEqual(['Through the Looking Glass']);
    expect(run(single, { type: 'delete' })).toBe(single);
  });

  it('strips quotes, backslashes and overflow from names', () => {
    const s = run(loaded, { type: 'setName', name: 'A"b\\c' + 'x'.repeat(40) });
    expect(s.presets[0].name).toHaveLength(31);
    expect(s.presets[0].name).not.toMatch(/["\\]/);
    expect(line(s, 'name = ')).toBe(`name = "${s.presets[0].name}"`);
  });

  it('blocks an empty name', () => {
    expect(problems(run(loaded, { type: 'setName', name: '' }))).toEqual(['preset 0: empty name']);
  });

  it('tracks dirtiness against the last save', () => {
    const edited = run(loaded, { type: 'setParam', key: 'input.PreDelay', value: 0.5 });
    expect(isDirty(edited)).toBe(true);
    expect(isDirty(run(edited, { type: 'saved', text: docText(edited.doc!), fileName: 'two.toml' }))).toBe(false);
  });

  it('keeps the current bank when a load fails', () => {
    const s = run(loaded, { type: 'load', text: fixture.replace('blinks = 1', 'blinks = 99'), source: 'file', fileName: 'x' });
    expect(s.error).toBe('preset 0: blinks out of range 1..20');
    expect(s.doc).toBe(loaded.doc);
  });

  it('only accepts known map targets', () => {
    const ok = run(loaded, { type: 'setKnob', bank: 1, knob: 5, target: 'late.LineDecay' });
    expect(ok.presets[0].knobMap[1][5]).toBe('late.LineDecay');
    expect(line(ok, 'knob6_b')).toBe('knob6_b = "late.LineDecay"');
    expect(run(loaded, { type: 'setToggle', bank: 0, toggle: 0, target: 'late.LineDecay' })).toBe(loaded);
  });

  describe('presetChanges', () => {
    const fields = (s: EditorState, i: number) => Object.keys(presetChanges(s, i).fields);

    it('marks exactly the edited fields, and nothing after a save', () => {
      const s = run(
        loaded,
        { type: 'setParam', key: 'input.PreDelay', value: 0.5 },
        { type: 'setKnob', bank: 1, knob: 5, target: 'late.LineDecay' },
        { type: 'setScalar', field: 'blinks', value: 4 },
      );
      expect(fields(s, 0).sort()).toEqual(['blinks', 'knob.1.5', 'params.input.PreDelay']);
      expect(fields(s, 1)).toEqual([]);
      expect(presetChanges(s, 0).original.params['input.PreDelay']).toBe(loaded.presets[0].params['input.PreDelay']);
      expect(fields(run(s, { type: 'saved', text: docText(s.doc!), fileName: null }), 0)).toEqual([]);
    });

    it('compares a preset that moved down after a delete with its own original', () => {
      const s = run(loaded, { type: 'delete' });
      expect(s.presets[0].name).toBe('Through the Looking Glass');
      expect(presetChanges(s, 0)).toMatchObject({ added: false, fields: {} });
    });

    it('flags a duplicate as added and compares it with its source', () => {
      const s = run(loaded, { type: 'select', preset: 1 }, { type: 'duplicate' });
      const c = presetChanges(s, 2);
      expect(c.added).toBe(true);
      expect(c.original.name).toBe('Through the Looking Glass');
      expect(Object.keys(c.fields).sort()).toEqual(['blinks', 'name']);
      expect(presetChanges(run(s, { type: 'saved', text: docText(s.doc!), fileName: null }), 2).added).toBe(false);
    });
  });
});
