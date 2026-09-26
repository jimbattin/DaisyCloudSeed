import { useState } from 'preact/hooks';
import { TOTAL_LINE_COUNT, type Preset } from '../model/bank';
import {
  BLOCKS,
  EFFECT,
  badgesFor,
  blockOn,
  blocksOfKey,
  lineCount,
  pageBlocks,
  preferOn,
  type BlockId,
} from '../model/flow';
import { formatValue } from '../model/scale';
import { PAGES, PARAMS, type PageId } from '../model/schema';
import { Key } from './controls';

export interface FlowPanelProps {
  preset: Preset;
  page: PageId;
  /** Parameter hovered or focused in the page controls. */
  focusKey: string | null;
  /** Block last clicked in the diagram. */
  pinned: BlockId | null;
  onBlock: (id: BlockId) => void;
}

const STORAGE_KEY = 'cloudseed.flowOpen';

function storedOpen(): boolean {
  try {
    return localStorage.getItem(STORAGE_KEY) !== '0';
  } catch {
    return true;
  }
}

interface Rect {
  x: number;
  y: number;
  w: number;
  h: number;
}

/** Rect blocks at fixed positions; the frame is `lines`. */
const RECTS: Record<Exclude<BlockId, 'delay' | 'ldiff' | 'decay' | 'makeup'>, Rect> = {
  in: { x: 8, y: 72, w: 44, h: 36 },
  hpf: { x: 104, y: 72, w: 56, h: 36 },
  lpf: { x: 170, y: 72, w: 56, h: 36 },
  predelay: { x: 236, y: 72, w: 64, h: 36 },
  taps: { x: 324, y: 72, w: 72, h: 36 },
  ediff: { x: 408, y: 72, w: 72, h: 36 },
  lines: { x: 510, y: 48, w: 306, h: 192 },
  cutoff: { x: 578, y: 154, w: 56, h: 32 },
  highshelf: { x: 646, y: 154, w: 56, h: 32 },
  lowshelf: { x: 714, y: 154, w: 56, h: 32 },
  mix: { x: 836, y: 72, w: 50, h: 218 },
  revInto: { x: 116, y: 4, w: 96, h: 26 },
  revDirect: { x: 790, y: 4, w: 96, h: 26 },
  out: { x: 954, y: 72, w: 42, h: 36 },
};

/** The two in-loop slots; DelayLine::Process taps the line output after slot 1. */
const SLOTS: [Rect, Rect] = [
  { x: 552, y: 72, w: 72, h: 36 },
  { x: 650, y: 72, w: 72, h: 36 },
];

/** Blocks drawn after the frame, so they sit on top and receive clicks. */
const DRAW_ORDER: BlockId[] = [
  'in', 'hpf', 'lpf', 'predelay', 'taps', 'ediff', 'delay', 'ldiff', 'cutoff', 'highshelf',
  'lowshelf', 'decay', 'mix', 'revInto', 'revDirect', 'makeup', 'out',
];

interface Wire {
  points: string;
  arrow: boolean;
  dim: (p: Preset) => boolean;
}

const never = () => false;
const silent = (key: string) => (p: Preset) => p.params[key] === 0;
const unused = (id: BlockId) => (p: Preset) => !blockOn(id, p);

const WIRES: Wire[] = [
  { points: '52,90 77,90', arrow: true, dim: never },
  { points: '95,90 104,90', arrow: true, dim: never },
  { points: '160,90 170,90', arrow: true, dim: never },
  { points: '226,90 236,90', arrow: true, dim: never },
  { points: '300,90 324,90', arrow: true, dim: never },
  { points: '396,90 408,90', arrow: true, dim: never },
  { points: '480,90 523,90', arrow: true, dim: never },
  { points: '541,90 552,90', arrow: true, dim: never },
  { points: '624,90 650,90', arrow: true, dim: never },
  { points: '722,90 790,90 790,170 770,170', arrow: true, dim: never },
  { points: '714,170 702,170', arrow: true, dim: never },
  { points: '646,170 634,170', arrow: true, dim: never },
  { points: '578,170 572,170', arrow: false, dim: never },
  { points: '552,170 532,170 532,99', arrow: true, dim: never },
  { points: '637,90 637,66 806,66 806,220 836,220', arrow: true, dim: silent('output.MainOut') },
  { points: '494,90 494,252 836,252', arrow: true, dim: silent('output.EarlyOut') },
  { points: '312,90 312,266 836,266', arrow: true, dim: silent('output.PredelayOut') },
  { points: '64,90 64,280 836,280', arrow: true, dim: silent('output.DryOut') },
  { points: '886,90 903,90', arrow: true, dim: never },
  { points: '921,90 928,90', arrow: false, dim: never },
  { points: '948,90 954,90', arrow: true, dim: never },
  { points: '64,90 64,17 116,17', arrow: true, dim: unused('revInto') },
  { points: '212,17 224,17 224,50 86,50 86,81', arrow: true, dim: unused('revInto') },
  { points: '896,90 896,60 870,60 870,30', arrow: true, dim: unused('revDirect') },
  { points: '886,17 912,17 912,81', arrow: true, dim: unused('revDirect') },
];

const DOTS: [number, number][] = [
  [64, 90],
  [312, 90],
  [494, 90],
  [637, 90],
  [896, 90],
];

const SUMS: [number, number][] = [
  [86, 90],
  [532, 90],
  [912, 90],
];

/** Value text shown inside a block ('' for none). */
function subText(id: BlockId, preset: Preset): string {
  if (!blockOn(id, preset)) return 'OFF';
  const fv = (k: string) => formatValue(k, preset.params[k], preset);
  switch (id) {
    case 'hpf':
      return fv('input.HighPass');
    case 'lpf':
      return fv('input.LowPass');
    case 'predelay':
      return fv('input.PreDelay');
    case 'taps':
      return `${fv('early.TapCount')}×${fv('early.TapLength')}`;
    case 'ediff':
      return fv('early_diffusion.DiffusionStages');
    case 'delay':
      return fv('late.LineDelay');
    case 'ldiff':
      return fv('late_diffusion.LateDiffusionStages');
    case 'lowshelf':
      return fv('late_eq.PostLowShelfGain');
    case 'highshelf':
      return fv('late_eq.PostHighShelfGain');
    case 'cutoff':
      return fv('late_eq.PostCutoffFrequency');
    case 'decay':
      return fv('late.LineDecay');
    case 'revInto':
    case 'revDirect':
      return fv('reverse.delay');
    default:
      return '';
  }
}

const blockBadges = (id: BlockId, preset: Preset) => [
  ...new Set(BLOCKS[id].keys.flatMap((k) => badgesFor(preset, k))),
];

export function FlowPanel({ preset, page, focusKey, pinned, onBlock }: FlowPanelProps) {
  const [open, setOpen] = useState(storedOpen);
  const [hoverBlock, setHoverBlock] = useState<BlockId | null>(null);

  const toggle = () => {
    const next = !open;
    setOpen(next);
    try {
      localStorage.setItem(STORAGE_KEY, next ? '1' : '0');
    } catch {
      // Storage unavailable (private mode, quota): the panel still toggles for this session.
    }
  };

  const onPage = pageBlocks(page, preset);
  const highlighted = focusKey ? [...onPage, ...blocksOfKey(focusKey)] : onPage;
  const explained: BlockId =
    hoverBlock ??
    (focusKey ? preferOn(blocksOfKey(focusKey), preset) : null) ??
    pinned ??
    preferOn(onPage, preset) ??
    'in';

  const lines = lineCount(preset);
  const stageTap = preset.params['late.LateStageTap'] >= 0.5;
  const rectOf = (id: BlockId): Rect | undefined =>
    id === 'delay' ? SLOTS[stageTap ? 1 : 0] : id === 'ldiff' ? SLOTS[stageTap ? 0 : 1] : RECTS[id as keyof typeof RECTS];
  const fv = (k: string) => formatValue(k, preset.params[k], preset);

  function block(id: BlockId) {
    const info = BLOCKS[id];
    const on = blockOn(id, preset);
    const sub = subText(id, preset);
    const badges = blockBadges(id, preset).join(' ');
    const pageLabel = PAGES.find((p) => p.id === info.page)!.label;
    const cls = [
      'blk',
      highlighted.includes(id) && 'hl',
      id === explained && 'focus',
      !on && 'off',
    ]
      .filter(Boolean)
      .join(' ');
    const r = rectOf(id);
    const cx = r ? r.x + r.w / 2 : 0;
    const short = id === 'taps' && preset.params['early.isReverse'] >= 0.5 ? 'BLOOM' : info.short;

    let shape;
    let texts;
    if (id === 'decay') {
      shape = <polygon points="572,158 572,182 552,170" />;
      texts = (
        <>
          <text class="t" x={518} y={204}>{`DECAY ${sub}`}</text>
          {badges && <text class="bdg" x={518} y={216}>{badges}</text>}
        </>
      );
    } else if (id === 'makeup') {
      shape = <polygon points="928,78 928,102 948,90" />;
      texts = <text class="t" x={938} y={116} text-anchor="middle">GAIN</text>;
    } else if (id === 'lines') {
      shape = <rect x={r!.x} y={r!.y} width={r!.w} height={r!.h} />;
      texts = (
        <>
          <text class="t" x={518} y={62}>{`DELAY LINES ×${lines}`}</text>
          {badges && <text class="bdg" x={518} y={232}>{badges}</text>}
        </>
      );
    } else if (id === 'mix') {
      shape = <rect x={r!.x} y={r!.y} width={r!.w} height={r!.h} />;
      texts = (
        <>
          <text class="t" x={861} y={88} text-anchor="middle">MIX</text>
          {blockBadges(id, preset).map((b, i) => (
            <text key={b} class="bdg" x={841} y={104 + 11 * i}>
              {b}
            </text>
          ))}
        </>
      );
    } else {
      const { x, y, w, h } = r!;
      const [ty, vy] = h === 26 ? [11, 22] : h === 32 ? [13, 26] : sub ? [15, 28] : [22, 0];
      shape = <rect x={x} y={y} width={w} height={h} />;
      texts = (
        <>
          <text class="t" x={cx} y={y + ty} text-anchor="middle">{short}</text>
          {sub && <text class="v" x={cx} y={y + vy} text-anchor="middle">{sub}</text>}
          {badges &&
            (id === 'revDirect' ? (
              // Left of the block: below it, the badges would cross the reverse wires.
              <text class="bdg" x={x - 4} y={y + 16} text-anchor="end">{badges}</text>
            ) : (
              <text class="bdg" x={x} y={y + h + 10}>{badges}</text>
            ))}
        </>
      );
    }

    return (
      <g
        key={id}
        class={cls}
        data-block={id}
        role="button"
        tabindex={0}
        aria-label={`${info.title} (open ${pageLabel} page)`}
        onClick={() => onBlock(id)}
        onKeyDown={(e) => {
          if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            onBlock(id);
          }
        }}
        onMouseEnter={() => setHoverBlock(id)}
        onMouseLeave={() => setHoverBlock(null)}
        onFocus={() => setHoverBlock(id)}
        onBlur={() => setHoverBlock(null)}
      >
        {shape}
        <title>{sub ? `${info.title}: ${sub}` : info.title}</title>
        {texts}
      </g>
    );
  }

  const info = BLOCKS[explained];
  const explainedOn = blockOn(explained, preset);
  const stateChip = info.enable
    ? explainedOn
      ? 'ON'
      : 'OFF - BYPASSED'
    : explained === 'revInto' || explained === 'revDirect'
      ? explainedOn
        ? 'IN USE'
        : 'NOT IN USE'
      : null;

  return (
    <section class="flow panel">
      <div class="flow-head">
        <span class="silk">Signal flow</span>
        {open && <span class="flow-note">as stored in this preset · click a block to open its page</span>}
        <Key label="FLOW" pressed={open} dark onClick={toggle} />
      </div>
      {open && (
        <div class="flow-body">
          <svg viewBox="0 0 1000 300" width="100%" role="img" aria-label="CloudSeed signal flow">
            <defs>
              <marker id="flow-arrow" viewBox="0 0 6 6" refX="6" refY="3" markerWidth="6" markerHeight="6" orient="auto">
                <path d="M0,0 L6,3 L0,6 z" />
              </marker>
            </defs>
            {/* One outline per extra line, back to front, then a mask so only their offset edges show. */}
            {Array.from({ length: lines - 1 }, (_, i) => lines - 1 - i).map((k) => (
              <rect key={k} class="stack" x={510 + 3 * k} y={48 - 3 * k} width={306} height={192} />
            ))}
            <rect class="stack-mask" x={510} y={48} width={306} height={192} />
            {WIRES.map((w) => (
              <polyline
                key={w.points}
                class={w.dim(preset) ? 'wire dim' : 'wire'}
                points={w.points}
                marker-end={w.arrow ? 'url(#flow-arrow)' : undefined}
              />
            ))}
            {DOTS.map(([x, y]) => (
              <circle key={`${x},${y}`} class="dot" cx={x} cy={y} r={2.5} />
            ))}
            {SUMS.map(([x, y]) => (
              <g key={`${x},${y}`}>
                <circle class="sum" cx={x} cy={y} r={9} />
                <text class="sum-plus" x={x} y={y + 4}>
                  +
                </text>
              </g>
            ))}
            <text class="v" x={700} y={232}>{`MAIN ${fv('output.MainOut')} ×1/√${lines}`}</text>
            <text class="v" x={700} y={249}>{`EARLY ${fv('output.EarlyOut')}`}</text>
            <text class="v" x={700} y={263}>{`PRE ${fv('output.PredelayOut')}`}</text>
            <text class="v" x={700} y={277}>{`DRY ${fv('output.DryOut')}`}</text>
            {block('lines')}
            {DRAW_ORDER.map(block)}
          </svg>
          <aside class="flow-explain">
            <h4>{info.title}</h4>
            {stateChip && <span class="flow-state">{stateChip}</span>}
            <p>{info.text}</p>
            {explained === 'lines' && (
              <p>{`This preset: ${lines} of ${TOTAL_LINE_COUNT} lines (default ${preset.defaultDelayLines}, max ${preset.maxDelayLines}), LateStageTap ${stageTap ? 'on' : 'off'}.`}</p>
            )}
            {info.keys.length > 0 && (
              <table class="flow-params">
                <tbody>
                  {info.keys.map((k) => (
                    <tr key={k} class={k === focusKey ? 'focus' : undefined}>
                      <td>{PARAMS[k].label}</td>
                      <td>{formatValue(k, preset.params[k], preset)}</td>
                      <td>{badgesFor(preset, k).join(' ')}</td>
                      <td>{EFFECT[k]}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            )}
          </aside>
        </div>
      )}
    </section>
  );
}
