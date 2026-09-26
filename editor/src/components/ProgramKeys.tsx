import { useState } from 'preact/hooks';
import type { Preset } from '../model/bank';
import { MAX_PRESETS } from '../model/bank';
import type { PresetChanges } from '../model/state';
import { ConfirmDialog, Key } from './controls';

export interface ProgramKeysProps {
  presets: Preset[];
  /** One per preset, from `presetChanges()`. */
  changes: PresetChanges[];
  selected: number;
  onSelect: (i: number) => void;
  onDuplicate: () => void;
  onDelete: () => void;
}

const nn = (i: number) => String(i + 1).padStart(2, '0');

function markOf(c: PresetChanges | undefined): 'edited' | 'added' | undefined {
  if (!c) return undefined;
  if (c.added) return 'added';
  return Object.keys(c.fields).length > 0 ? 'edited' : undefined;
}

const MARK_NOTE = { edited: ' (edited)', added: ' (new)' };

export function ProgramKeys({ presets, changes, selected, onSelect, onDuplicate, onDelete }: ProgramKeysProps) {
  const [confirming, setConfirming] = useState(false);
  const current = presets[selected];
  return (
    <div class="panel">
      <div class="silk">Program</div>
      <div class="program-keys">
        {Array.from({ length: MAX_PRESETS }, (_, i) => {
          const mark = markOf(changes[i]);
          return (
            <Key
              key={i}
              label={nn(i)}
              title={presets[i] && presets[i].name + (mark ? MARK_NOTE[mark] : '')}
              mark={mark}
              pressed={i === selected && i < presets.length}
              disabled={i >= presets.length}
              onClick={() => onSelect(i)}
            />
          );
        })}
        <Key label="DUP" dark disabled={presets.length >= MAX_PRESETS} onClick={onDuplicate} />
        <Key label="DEL" dark disabled={presets.length <= 1} onClick={() => setConfirming(true)} />
      </div>
      <ConfirmDialog
        open={confirming}
        title="Delete preset"
        body={`Delete preset ${nn(selected)} ${current?.name ?? ''}? Presets after it move down one slot.`}
        confirmLabel="Delete"
        onCancel={() => setConfirming(false)}
        onConfirm={() => {
          setConfirming(false);
          onDelete();
        }}
      />
    </div>
  );
}
