// Web MIDI transport for PresetLink. One code path for Chrome and Firefox: reconnection after
// the pedal reboots polls requestMIDIAccess() instead of relying on MIDIAccess.onstatechange,
// which Firefox has historically not fired for device hot-plug (Mozilla bug 836897).

import { RebootTimeoutError, type MidiTransport } from './client';
import { createSysexAssembler } from './protocol';

// The pedal resets 100 ms after its reply, then spends ~2.5 s in the bootloader without a
// MIDI port; polling earlier could find the old ports or the old firmware.
const REBOOT_SETTLE_MS = 1500;
const REBOOT_POLL_MS = 500;
const REBOOT_DEADLINE_MS = 20000;
// Time for the re-enumerated firmware to start servicing its USB link.
const REOPEN_SETTLE_MS = 300;
// Bound on one reboot poll (list + open), and on opening the ports at CONNECT.
const POLL_ATTEMPT_MS = 2000;
const CONNECT_TIMEOUT_MS = 5000;
const NOT_FOUND = 'pedal not found: check the USB cable, then reload the page';

function sleep(ms: number): Promise<void> {
  const { promise, resolve } = Promise.withResolvers<void>();
  setTimeout(resolve, ms);
  return promise;
}

export const isWebMidiSupported = (): boolean =>
  typeof navigator !== 'undefined' && typeof navigator.requestMIDIAccess === 'function';

const isDaisy = (port: MIDIPort) => /daisy/i.test(port.name ?? '') || /daisy/i.test(port.manufacturer ?? '');

interface Ports {
  input: MIDIInput;
  output: MIDIOutput;
}

async function findPorts(): Promise<Ports | null> {
  let access: MIDIAccess;
  try {
    access = await navigator.requestMIDIAccess({ sysex: true });
  } catch {
    throw new Error('MIDI access denied');
  }
  const input = [...access.inputs.values()].find((p) => isDaisy(p) && p.state === 'connected');
  const output = [...access.outputs.values()].find((p) => isDaisy(p) && p.state === 'connected');
  return input && output ? { input, output } : null;
}

export class WebMidiTransport implements MidiTransport {
  private readonly subscribers = new Set<(msg: Uint8Array) => void>();
  private readonly feed = createSysexAssembler((msg) => this.subscribers.forEach((cb) => cb(msg)));

  constructor(private ports: Ports) {
    this.attach();
  }

  private attach() {
    this.ports.input.onmidimessage = (e) => {
      if (e.data) this.feed(e.data);
    };
  }

  send(msg: Uint8Array): void {
    try {
      this.ports.output.send(msg);
    } catch {
      throw new Error('pedal disconnected');
    }
  }

  subscribe(cb: (msg: Uint8Array) => void): () => void {
    this.subscribers.add(cb);
    return () => void this.subscribers.delete(cb);
  }

  async waitForReboot(): Promise<void> {
    const deadline = Date.now() + REBOOT_DEADLINE_MS;
    await sleep(REBOOT_SETTLE_MS);
    this.ports.input.onmidimessage = null;
    while (Date.now() < deadline) {
      const ports = await within(
        findPorts().then((p) => p && openPorts(p)),
        Math.min(POLL_ATTEMPT_MS, deadline - Date.now()),
      );
      if (ports) {
        this.ports = ports;
        this.attach();
        await sleep(REOPEN_SETTLE_MS);
        return;
      }
      await sleep(REBOOT_POLL_MS);
    }
    throw new RebootTimeoutError('pedal did not come back after reboot');
  }
}

async function openPorts(ports: Ports): Promise<Ports> {
  await ports.input.open();
  await ports.output.open();
  return ports;
}

/**
 * `promise`'s value, or null once `ms` pass or it rejects. Web MIDI calls on a port that
 * vanished during a reboot can stay pending forever (seen in Firefox), so every attempt is
 * bounded.
 */
function within<T>(promise: Promise<T | null>, ms: number): Promise<T | null> {
  const { promise: timeout, resolve } = Promise.withResolvers<null>();
  const timer = setTimeout(() => resolve(null), ms);
  return Promise.race([promise.catch(() => null), timeout]).finally(() => clearTimeout(timer));
}

export async function connectPedal(): Promise<WebMidiTransport> {
  const found = await findPorts();
  if (!found) throw new Error(NOT_FOUND);
  const ports = await within(openPorts(found), CONNECT_TIMEOUT_MS);
  if (!ports) throw new Error(NOT_FOUND);
  return new WebMidiTransport(ports);
}
