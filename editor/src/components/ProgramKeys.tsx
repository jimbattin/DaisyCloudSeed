import { useState } from 'preact/hooks';
import type { Preset } from '../model/bank';
import { MAX_PRESETS } from '../model/bank';
import { ConfirmDialog, Key } from './controls';

export interface ProgramKeysProps {
  presets: Preset[];
  selected: number;
  onSelect: (i: number) => void;
  onDuplicate: () => void;
  onDelete: () => void;
}

const nn = (i: number) => String(i + 1).padStart(2, '0');

export function ProgramKeys({ presets, selected, onSelect, onDuplicate, onDelete }: ProgramKeysProps) {
  const [confirming, setConfirming] = useState(false);
  const current = presets[selected];
  return (
    <div class="panel">
      <div class="silk">Program</div>
      <div class="program-keys">
        {Array.from({ length: MAX_PRESETS }, (_, i) => (
          <Key
            key={i}
            label={nn(i)}
            title={presets[i]?.name}
            pressed={i === selected && i < presets.length}
            disabled={i >= presets.length}
            onClick={() => onSelect(i)}
          />
        ))}
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
