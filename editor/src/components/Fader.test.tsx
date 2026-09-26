// @vitest-environment happy-dom
import { cleanup, fireEvent, render, screen } from '@testing-library/preact';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { PARAMS } from '../model/schema';
import { Fader } from './Fader';

afterEach(cleanup);

const lines = { defaultDelayLines: 2, maxDelayLines: 5 };

describe('Fader', () => {
  it('shows the real value and reports range moves', () => {
    const onChange = vi.fn();
    render(<Fader def={PARAMS['input.PreDelay']} value={0.070000000298023224} preset={lines} badges={['K1B']} onChange={onChange} onHover={() => {}} />);
    expect(screen.getByText('70ms')).toBeTruthy();
    expect(screen.getByText('K1B')).toBeTruthy();
    fireEvent.input(screen.getByRole('slider'), { target: { value: '0.5' } });
    expect(onChange).toHaveBeenCalledWith(0.5);
  });

  it('reverts out-of-range typed values and commits valid ones', () => {
    const onChange = vi.fn();
    render(<Fader def={PARAMS['input.PreDelay']} value={0.25} preset={lines} badges={[]} onChange={onChange} onHover={() => {}} />);
    const field = screen.getByLabelText('PREDLY value') as HTMLInputElement;
    fireEvent.change(field, { target: { value: '1.5' } });
    expect(onChange).not.toHaveBeenCalled();
    expect(field.value).toBe('0.25');
    fireEvent.change(field, { target: { value: '0.75' } });
    expect(onChange).toHaveBeenCalledWith(0.75);
  });

  it('enters seeds as the engine integer', () => {
    const onChange = vi.fn();
    render(<Fader def={PARAMS['seeds.TapSeed']} value={0.00115} preset={lines} badges={[]} onChange={onChange} onHover={() => {}} />);
    const field = screen.getByLabelText('TAPSD seed') as HTMLInputElement;
    expect(field.value).toBe('1150');
    fireEvent.change(field, { target: { value: '2000' } });
    expect(onChange).toHaveBeenCalledWith(0.002);
  });
});
