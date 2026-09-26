// @vitest-environment happy-dom
import fixture from '../../../tests/fixtures/two_presets.toml?raw';
import { cleanup, fireEvent, render, screen } from '@testing-library/preact';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { loadBank, type Preset } from '../model/bank';
import { FlowPanel } from './FlowPanel';

const loaded = loadBank(fixture);
if (!loaded.ok) throw new Error(loaded.error);
const chorus = loaded.presets[0];

afterEach(() => {
  cleanup();
  localStorage.clear();
});

const show = (preset: Preset = chorus, onBlock = vi.fn()) =>
  render(<FlowPanel preset={preset} page="input" focusKey={null} pinned={null} onBlock={onBlock} />);

const blockEl = (container: Element, id: string) => container.querySelector(`g[data-block=${id}]`)!;

describe('FlowPanel', () => {
  it('lights the page blocks and marks switched-off blocks', () => {
    const { container } = show();
    expect(blockEl(container, 'hpf').classList.contains('off')).toBe(true);
    for (const id of ['hpf', 'lpf', 'predelay', 'in']) {
      expect(blockEl(container, id).classList.contains('hl'), id).toBe(true);
    }
    expect(blockEl(container, 'taps').classList.contains('hl')).toBe(false);
  });

  it('orders the loop slots by LateStageTap', () => {
    const { container, rerender } = show();
    expect(container.querySelector('g[data-block=ldiff] rect')!.getAttribute('x')).toBe('552');
    expect(container.querySelector('g[data-block=delay] rect')!.getAttribute('x')).toBe('650');
    const off = { ...chorus, params: { ...chorus.params, 'late.LateStageTap': 0 } };
    rerender(<FlowPanel preset={off} page="input" focusKey={null} pinned={null} onBlock={vi.fn()} />);
    expect(container.querySelector('g[data-block=delay] rect')!.getAttribute('x')).toBe('552');
    expect(container.querySelector('g[data-block=ldiff] rect')!.getAttribute('x')).toBe('650');
  });

  it('shows the line count and the unused reverse wires', () => {
    const { container } = show();
    expect(blockEl(container, 'lines').textContent).toContain('DELAY LINES ×2');
    expect(container.querySelectorAll('rect.stack')).toHaveLength(1);
    const wires = [...container.querySelectorAll('polyline.wire')];
    expect(wires.slice(21).map((w) => w.classList.contains('dim'))).toEqual([true, true, true, true]);
  });

  it('reports a clicked block', () => {
    const onBlock = vi.fn();
    const { container } = show(chorus, onBlock);
    fireEvent.click(blockEl(container, 'taps'));
    expect(onBlock).toHaveBeenCalledWith('taps');
  });

  it('collapses with FLOW and stays collapsed on the next render', () => {
    const { container } = show();
    fireEvent.click(screen.getByRole('button', { name: 'FLOW' }));
    expect(container.querySelector('svg')).toBeNull();
    expect(localStorage.getItem('cloudseed.flowOpen')).toBe('0');
    cleanup();
    const again = show();
    expect(again.container.querySelector('svg')).toBeNull();
  });
});
