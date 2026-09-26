import { describe, expect, it } from 'vitest';
import { formatValue } from './scale';

const lines = { defaultDelayLines: 2, maxDelayLines: 5 };

describe('formatValue matches the engine scaling', () => {
  it.each([
    ['reverse.delay', 0.4771, '500ms'],
    ['late.LineDecay', 0.68000012636184692, '6.57s'],
    ['input.LowPass', 0.29000008106231689, '2.01k'],
    ['output.DryOut', 0.94499987363815308, '-2.2dB'],
    ['output.DryOut', 0, '-INF'],
    ['late.LineDelay', 0.68499988317489624, '242ms'],
    ['input.PreDelay', 0.070000000298023224, '70ms'],
    ['seeds.TapSeed', 0.0011500000255182385, '1150'],
    ['early_diffusion.DiffusionStages', 0.4285714328289032, '1STG'],
    ['late.LineModRate', 0.46999993920326233, '0.39Hz'],
    ['early.TapCount', 0.36499997973442078, '18'],
    ['delay_lines.max', 0, '2LN'],
    ['delay_lines.max', 1, '5LN'],
    ['input.HiPassEnabled', 0.5, 'ON'],
  ])('%s = %f -> %s', (key, v, expected) => {
    expect(formatValue(key, v, lines)).toBe(expected);
  });
});
