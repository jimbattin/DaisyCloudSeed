import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { loadBank, type Preset } from './bank';
import { BLOCKS, EFFECT, blockOn, blocksOfKey, lineCount, pageBlocks, type BlockId } from './flow';
import { PARAM_KEYS } from './schema';

const fixture = readFileSync(new URL('../../../tests/fixtures/two_presets.toml', import.meta.url), 'utf8');
const loaded = loadBank(fixture);
if (!loaded.ok) throw new Error(loaded.error);
const chorus = loaded.presets[0];

const withParams = (p: Preset, params: Record<string, number>): Preset => ({
  ...p,
  params: { ...p.params, ...params },
});

describe('flow model', () => {
  it('gives every parameter a block and an effect line, and names only real parameters', () => {
    for (const key of PARAM_KEYS) {
      expect(blocksOfKey(key).length, key).toBeGreaterThanOrEqual(1);
      expect(EFFECT[key], key).toBeTruthy();
    }
    const known = Object.fromEntries(PARAM_KEYS.map((k) => [k, true]));
    for (const key of Object.keys(EFFECT)) expect(known[key], key).toBe(true);
    for (const [id, info] of Object.entries(BLOCKS)) {
      for (const key of info.keys) expect(known[key], `${id}: ${key}`).toBe(true);
    }
  });

  it('lights the blocks each page edits', () => {
    expect(pageBlocks('seeds', chorus)).toEqual(['taps', 'ediff', 'delay', 'ldiff']);
    expect(pageBlocks('rev', chorus)).toEqual(['revInto', 'revDirect', 'lines']);
    expect(pageBlocks('setup', chorus)).toEqual(['lines']);
    expect(pageBlocks('knobs', chorus)).toEqual([
      'mix', 'ldiff', 'taps', 'decay', 'predelay', 'hpf', 'lpf', 'delay', 'revInto', 'revDirect',
    ]);
  });

  it('reads the stored switch states of the preset', () => {
    const off: BlockId[] = ['hpf', 'lpf', 'lowshelf', 'highshelf', 'revInto', 'revDirect'];
    const on: BlockId[] = ['ediff', 'ldiff', 'cutoff'];
    for (const id of off) expect(blockOn(id, chorus), id).toBe(false);
    for (const id of on) expect(blockOn(id, chorus), id).toBe(true);
    expect(lineCount(chorus)).toBe(2);
  });

  it('routes the reverse voice by reverse.direct_mix', () => {
    const into = withParams(chorus, { 'reverse.enabled': 1 });
    expect(blockOn('revInto', into)).toBe(true);
    expect(blockOn('revDirect', into)).toBe(false);
    const direct = withParams(chorus, { 'reverse.enabled': 1, 'reverse.direct_mix': 1 });
    expect(blockOn('revInto', direct)).toBe(false);
    expect(blockOn('revDirect', direct)).toBe(true);
  });

  it('takes the max line count when delay_lines.max is on', () => {
    expect(lineCount(withParams(chorus, { 'delay_lines.max': 1 }))).toBe(5);
  });
});
