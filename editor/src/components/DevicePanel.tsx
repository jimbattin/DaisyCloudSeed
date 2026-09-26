import type { InfoReply } from '../midi/protocol';
import { Key, Led } from './controls';

export interface DevicePanelProps {
  supported: boolean;
  connected: boolean;
  busy: boolean;
  info: InfoReply | null;
  inSync: boolean;
  onConnect: () => void;
  onRead: () => void;
  onUpload: () => void;
  onRevert: () => void;
}

export function DevicePanel(p: DevicePanelProps) {
  const { info } = p;
  const idle = !p.supported || p.busy;
  const offline = idle || !p.connected;
  return (
    <div class="device panel">
      <div class="silk">
        <span class={p.connected ? 'status-led on' : 'status-led'} />
        Pedal
      </div>
      {!p.supported ? (
        <Led small text="NO WEB MIDI - USE CHROME OR FIREFOX" />
      ) : info && p.connected ? (
        <>
          <Led small text={`SRC ${info.source === 1 ? 'UPLOAD' : 'BUILT-IN'}  PRE ${info.presetCount}`} />
          <Led small text={`LEN ${info.activeLength}  HASH 0x${info.activeHash.toString(16).padStart(8, '0')}`} />
          <Led small text={p.inSync ? 'SYNC' : 'DIFF'} />
        </>
      ) : (
        <Led small off text="NOT CONNECTED" />
      )}
      <div class="keys">
        <Key label="CONNECT" wide dark pressed={p.connected} disabled={idle} onClick={p.onConnect} />
        <Key label="READ" dark disabled={offline} onClick={p.onRead} />
        <Key label="UPLOAD" wide dark disabled={offline} onClick={p.onUpload} />
        <Key label="REVERT" wide dark disabled={offline} onClick={p.onRevert} />
      </div>
    </div>
  );
}
