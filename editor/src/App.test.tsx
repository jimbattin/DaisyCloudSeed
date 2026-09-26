// @vitest-environment happy-dom
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/preact';
import { afterEach, describe, expect, it } from 'vitest';
import { App } from './app';

afterEach(cleanup);

const mainLed = (container: Element) => container.querySelector('.led.wide')!.textContent ?? '';

describe('App', () => {
  it('loads the project bank and edits a fader', async () => {
    const { container } = render(<App />);
    expect(screen.getByText(/Load the project presets.toml/)).toBeTruthy();

    fireEvent.click(screen.getByRole('button', { name: 'PROJECT' }));
    await waitFor(() => expect(mainLed(container)).toMatch(/^P01 /));
    expect(mainLed(container)).toBe('P01 CHORUS');

    fireEvent.click(screen.getByRole('button', { name: 'OUTPUT' }));
    const labels = [...container.querySelectorAll('.fader-bank .silk')].map((el) => el.textContent);
    expect(labels).toEqual(['DRY', 'PREDLY', 'EARLY', 'MAIN']);

    expect(screen.queryByText(/EDITED/)).toBeNull();
    expect(container.querySelector('.edit-mark')).toBeNull();
    fireEvent.input(screen.getByRole('slider', { name: 'DRY' }), { target: { value: '0.5' } });
    expect(screen.getByText(/EDITED/)).toBeTruthy();
    // The DRY strip, the OUTPUT page key and program key 01 are marked; nothing else.
    const marked = [...container.querySelectorAll('.edit-mark')].map((m) => m.parentElement!.textContent);
    expect(marked).toEqual(['01', 'OUTPUT', expect.stringContaining('DRY')]);
    expect(screen.getByRole('slider', { name: 'DRY' }).title).toMatch(/\nOriginal: (-INF|-?\d+\.\ddB) \([\d.]+\)$/);
  });

  it('reports a browser without Web MIDI', () => {
    render(<App />);
    expect(screen.getByText(/NO WEB MIDI/)).toBeTruthy();
    expect((screen.getByRole('button', { name: 'CONNECT' }) as HTMLButtonElement).disabled).toBe(true);
  });
});
