// Real-unit display of normalized values: a port of ReverbController::GetScaledParameter
// (CloudSeed/ReverbController.h) and AudioLib::ValueTables (CloudSeed/AudioLib/ValueTables.cpp).

import { PARAMS } from './schema';

type Curve = '2dec' | '3dec' | '4oct' | '3oct';

const RAW: Record<Curve, (x: number) => number> = {
  '2dec': (x) => 100 ** x / 100,
  '3dec': (x) => 1000 ** x / 1000,
  '4oct': (x) => (16 ** x - 1) / 16 + 0.0625,
  '3oct': (x) => (8 ** x - 1) / 8 + 0.125,
};

/** ValueTables::Get: a 4001-entry float table normalized to 0..1, indexed by truncation. */
export function table(norm: number, curve: Curve): number {
  const idx = Math.trunc(norm * 4000.999);
  if (idx === 0) return 0;
  const f = Math.fround;
  const r0 = f(RAW[curve](0));
  return f(f(f(RAW[curve](idx / 4000)) - r0) / f(1 - r0));
}

const hz = (f: number) => (f < 1000 ? `${Math.round(f)}Hz` : `${(f / 1000).toFixed(2)}k`);

const db = (norm: number) => {
  const g = table(norm, '2dec');
  return g === 0 ? '-INF' : `${(20 * Math.log10(g)).toFixed(1)}dB`;
};

export interface LineCounts {
  defaultDelayLines: number;
  maxDelayLines: number;
}

/** The engine's reading of `norm` for parameter `key` ('group.Name'), as panel text. */
export function formatValue(key: string, v: number, preset: LineCounts): string {
  if (key === 'delay_lines.max') return `${v >= 0.5 ? preset.maxDelayLines : preset.defaultDelayLines}LN`;
  if (key === 'reverse.delay') return `${Math.round(20 + table(v, '3oct') * 1980)}ms`;
  if (PARAMS[key]?.kind === 'switch') return v >= 0.5 ? 'ON' : 'OFF';

  switch (key.slice(key.indexOf('.') + 1)) {
    case 'PreDelay':
      return `${Math.trunc(v * 1000)}ms`;
    case 'TapLength':
      return `${Math.trunc(v * 500)}ms`;
    case 'DiffusionDelay':
    case 'LateDiffusionDelay':
      return `${Math.trunc(10 + v * 90)}ms`;
    case 'LineDelay':
      return `${Math.trunc(20 + table(v, '2dec') * 980)}ms`;
    case 'HighPass':
    case 'PostLowShelfFrequency':
      return hz(20 + table(v, '4oct') * 980);
    case 'LowPass':
    case 'PostHighShelfFrequency':
    case 'PostCutoffFrequency':
      return hz(400 + table(v, '4oct') * 19600);
    case 'LineDecay': {
      const s = 0.05 + table(v, '3dec') * 59.95;
      return s < 10 ? `${s.toFixed(2)}s` : `${s.toFixed(1)}s`;
    }
    case 'TapGain':
    case 'PostLowShelfGain':
    case 'PostHighShelfGain':
    case 'DryOut':
    case 'PredelayOut':
    case 'EarlyOut':
    case 'MainOut':
      return db(v);
    case 'EarlyDiffusionModRate':
    case 'LineModRate':
    case 'LateDiffusionModRate':
      return `${(table(v, '2dec') * 5).toFixed(2)}Hz`;
    case 'EarlyDiffusionModAmount':
    case 'LineModAmount':
    case 'LateDiffusionModAmount':
      return `${(v * 2.5).toFixed(2)}ms`;
    case 'TapCount':
      return String(1 + Math.trunc(v * 49));
    case 'DiffusionStages':
    case 'LateDiffusionStages':
      return `${1 + Math.trunc(v * 1.999)}STG`;
    case 'TapSeed':
    case 'DiffusionSeed':
    case 'DelaySeed':
    case 'PostDiffusionSeed':
      return String(Math.floor(v * 1e6 + 0.001));
    default:
      return v.toFixed(2);
  }
}
