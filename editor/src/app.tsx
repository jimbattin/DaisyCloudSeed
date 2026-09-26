import { useEffect, useMemo, useReducer, useState } from 'preact/hooks';
import { ConfirmDialog, Key, Led } from './components/controls';
import { DevicePanel } from './components/DevicePanel';
import { KnobMapPage, PageKeys, ParamPage, SetupPage, ToggleMapPage } from './components/pages';
import { ProgramKeys } from './components/ProgramKeys';
import { openTomlFile, projectPresets, saveTomlFile } from './io/files';
import { PresetLink } from './midi/client';
import { ProtocolError, fnv1a32, type InfoReply } from './midi/protocol';
import { connectPedal, isWebMidiSupported } from './midi/webMidi';
import { docText, loadBank } from './model/bank';
import { initialState, isDirty, problems, reducer, type Source } from './model/state';

interface Confirm {
  title: string;
  body: string;
  confirmLabel: string;
  resolve: (ok: boolean) => void;
}

const pct = (done: number, total: number) => Math.floor((done * 100) / Math.max(total, 1));

export function App() {
  const [state, dispatch] = useReducer(reducer, initialState);
  const [link, setLink] = useState<PresetLink | null>(null);
  const [connected, setConnected] = useState(false);
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState<string | null>(null);
  const [info, setInfo] = useState<InfoReply | null>(null);
  const [confirm, setConfirm] = useState<Confirm | null>(null);
  const supported = useMemo(isWebMidiSupported, []);

  const dirty = isDirty(state);
  const { doc, presets, selected, page } = state;
  const preset = presets[selected];

  const inSync = useMemo(
    () => !!info && !!doc && fnv1a32(new TextEncoder().encode(docText(doc))) === info.activeHash,
    [doc, info],
  );

  useEffect(() => {
    if (!dirty) return;
    const warn = (e: BeforeUnloadEvent) => e.preventDefault();
    window.addEventListener('beforeunload', warn);
    return () => window.removeEventListener('beforeunload', warn);
  }, [dirty]);

  function ask(title: string, body: string, confirmLabel: string): Promise<boolean> {
    const { promise, resolve } = Promise.withResolvers<boolean>();
    setConfirm({ title, body, confirmLabel, resolve });
    return promise;
  }

  /** Runs one operation: clears the previous status, reports any error, drops a dead link. */
  async function run(op: () => Promise<void>) {
    setStatus(null);
    dispatch({ type: 'dismissError' });
    setBusy(true);
    try {
      await op();
    } catch (e) {
      const message = e instanceof Error ? e.message : String(e);
      setStatus(message);
      if ((e instanceof ProtocolError && message.endsWith('no reply')) || message === 'pedal disconnected') {
        setConnected(false);
      }
    } finally {
      setBusy(false);
    }
  }

  const discardOk = () =>
    dirty ? ask('Unsaved edits', 'Discard unsaved edits?', 'Discard') : Promise.resolve(true);

  const load = (text: string, source: Source, fileName: string | null) =>
    dispatch({ type: 'load', text, source, fileName });

  /** The bank text, if the pedal's own rules accept it; otherwise reports why. */
  function checkedText(): string | null {
    if (!doc) {
      setStatus('NO BANK');
      return null;
    }
    const blocked = problems(state);
    if (blocked.length > 0) {
      setStatus(blocked[0]);
      return null;
    }
    const text = docText(doc);
    const r = loadBank(text);
    if (!r.ok) {
      setStatus(r.error);
      return null;
    }
    return text;
  }

  const onProject = () =>
    run(async () => {
      if (await discardOk()) load(projectPresets, 'project', null);
    });

  const onOpen = () =>
    run(async () => {
      if (!(await discardOk())) return;
      const file = await openTomlFile();
      if (file) load(file.text, 'file', file.name);
    });

  const onSave = () => {
    const text = checkedText();
    if (text === null) return;
    saveTomlFile(text, state.fileName ?? 'presets.toml');
    dispatch({ type: 'saved', text, fileName: state.fileName });
  };

  const onConnect = () =>
    run(async () => {
      setConnected(false);
      setInfo(null);
      const next = new PresetLink(await connectPedal());
      setInfo(await next.info());
      setLink(next);
      setConnected(true);
    });

  const onRead = () =>
    run(async () => {
      if (!link || !(await discardOk())) return;
      const text = await link.readAll((d, t) => setStatus(`READ ${pct(d, t)}%`));
      load(text, 'pedal', null);
      setInfo(await link.info());
      setStatus(null);
    });

  const onUpload = () =>
    run(async () => {
      const text = checkedText();
      if (!link || text === null) return;
      const ok = await ask(
        'Upload bank',
        'Upload to the pedal? The pedal reboots (about 3 s), the output is dry during the upload, and sounds saved on the pedal (FS1 hold) are erased.',
        'Upload',
      );
      if (!ok) return;
      setStatus('UPLOAD 0%');
      const after = await link.upload(text, (d, t) => setStatus(d < t ? `UPLOAD ${pct(d, t)}%` : 'REBOOT\u2026'));
      afterReboot(after, 'UPLOADED - PRESS CONNECT TO VERIFY');
    });

  const onRevert = () =>
    run(async () => {
      if (!link) return;
      const ok = await ask(
        'Revert pedal',
        'Revert the pedal to its built-in bank? It reboots and erases sounds saved on the pedal.',
        'Revert',
      );
      if (!ok) return;
      setStatus('REBOOT\u2026');
      afterReboot(await link.revert(), 'REVERTED - PRESS CONNECT TO VERIFY');
    });

  function afterReboot(after: InfoReply | null, unverified: string) {
    if (after) {
      setInfo(after);
      setStatus(null);
    } else {
      setConnected(false);
      setInfo(null);
      setStatus(unverified);
    }
  }

  const display =
    status ?? state.error ?? (preset ? `P${String(selected + 1).padStart(2, '0')} ${preset.name.toUpperCase()}` : '\u2014\u2014 NO BANK \u2014\u2014');
  const origin = state.fileName ?? (state.source ? state.source.toUpperCase() : '');

  return (
    <main class="larc">
      <div class="header">
        <div class="brand">CLOUDSEED</div>
        <div class="main-display">
          <Led wide text={display} />
          <Led small off={!dirty && !origin} text={[dirty ? 'EDITED' : '', origin].filter(Boolean).join('  ') || '-'} />
        </div>
        <DevicePanel
          supported={supported}
          connected={connected}
          busy={busy}
          info={info}
          inSync={inSync}
          onConnect={onConnect}
          onRead={onRead}
          onUpload={onUpload}
          onRevert={onRevert}
        />
      </div>

      <div class="row">
        <Key label="PROJECT" wide disabled={busy} onClick={onProject} />
        <Key label="OPEN" disabled={busy} onClick={onOpen} />
        <Key label="SAVE" disabled={busy || !doc} onClick={onSave} />
      </div>

      {doc && preset ? (
        <div class="body">
          <ProgramKeys
            presets={presets}
            selected={selected}
            onSelect={(i) => dispatch({ type: 'select', preset: i })}
            onDuplicate={() => dispatch({ type: 'duplicate' })}
            onDelete={() => dispatch({ type: 'delete' })}
          />
          <div>
            <PageKeys page={page} onPage={(p) => dispatch({ type: 'page', page: p })} />
            {page === 'knobs' ? (
              <KnobMapPage preset={preset} dispatch={dispatch} />
            ) : page === 'toggles' ? (
              <ToggleMapPage preset={preset} dispatch={dispatch} />
            ) : page === 'setup' ? (
              <SetupPage preset={preset} dispatch={dispatch} />
            ) : (
              <ParamPage page={page} preset={preset} dispatch={dispatch} />
            )}
          </div>
        </div>
      ) : (
        <p class="hint">Load the project presets.toml, open a file, or connect the pedal and READ.</p>
      )}

      <ConfirmDialog
        open={confirm !== null}
        title={confirm?.title ?? ''}
        body={confirm?.body ?? ''}
        confirmLabel={confirm?.confirmLabel ?? ''}
        onCancel={() => {
          confirm?.resolve(false);
          setConfirm(null);
        }}
        onConfirm={() => {
          confirm?.resolve(true);
          setConfirm(null);
        }}
      />
    </main>
  );
}
