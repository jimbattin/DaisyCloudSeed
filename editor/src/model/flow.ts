import type { Preset } from './bank';
import { PAGES, type PageId } from './schema';

/**
 * Blocks of the CloudSeed signal path as the pedal runs it: CloudSeed/ReverbChannel.h Process(),
 * CloudSeed/DelayLine.h Process(), and the reverse voice and makeup gain in src/cloudseed.cpp.
 */
export type BlockId =
  | 'in'
  | 'hpf'
  | 'lpf'
  | 'predelay'
  | 'taps'
  | 'ediff'
  | 'lines'
  | 'delay'
  | 'ldiff'
  | 'lowshelf'
  | 'highshelf'
  | 'cutoff'
  | 'decay'
  | 'mix'
  | 'revInto'
  | 'revDirect'
  | 'makeup'
  | 'out';

export interface BlockInfo {
  /** Explainer heading and aria-label. */
  title: string;
  /** Text inside the SVG block. */
  short: string;
  /** Page a click opens. */
  page: PageId;
  /** PARAM_KEYS entries this block owns, in explainer row order. */
  keys: string[];
  /** Param key whose value >= 0.5 turns the block on. */
  enable?: string;
  /** Explainer paragraph. */
  text: string;
}

const REVERSE_KEYS = ['reverse.delay', 'reverse.enabled', 'reverse.direct_mix'];

export const BLOCKS: Record<BlockId, BlockInfo> = {
  in: {
    title: 'GUITAR IN',
    short: 'IN',
    page: 'input',
    keys: ['input.InputMix'],
    text: 'The guitar signal from the input jack (mono, left channel). It feeds three places: the reverb chain to the right, the DRY bus straight to the output mix, and - when the reverse voice is in into-reverb mode - the reverse buffer. INMIX blended the two stereo inputs in the original plugin; this mono pedal ignores it.',
  },
  hpf: {
    title: 'INPUT HIGH-PASS',
    short: 'HPF',
    page: 'input',
    keys: ['input.HiPassEnabled', 'input.HighPass'],
    enable: 'input.HiPassEnabled',
    text: "A one-pole high-pass on everything entering the reverb. Raising it keeps low notes and palm mutes from building a boomy tail. It only filters the reverb's input: the DRY bus is taken before it, so your direct tone is untouched. A knob mapped to HPFREQ switches it on the first time it moves, unless a switch owns the enable.",
  },
  lpf: {
    title: 'INPUT LOW-PASS',
    short: 'LPF',
    page: 'input',
    keys: ['input.LowPassEnabled', 'input.LowPass'],
    enable: 'input.LowPassEnabled',
    text: 'A one-pole low-pass on the reverb input. Lowering it darkens the whole reverb from the first reflection on, and keeps pick attack and string noise from exciting the tail. Like the high-pass it never touches the DRY bus, and a knob mapped to LPFREQ switches it on at first touch.',
  },
  predelay: {
    title: 'PRE-DELAY',
    short: 'PREDLY',
    page: 'input',
    keys: ['input.PreDelay'],
    text: 'A plain delay (0-1000 ms) between the input filters and the reflections. It sets the gap between your note and the start of the reverb, which keeps the attack clear of the wash; longer settings suggest a bigger space. Its output also feeds the PRE bus, so PredelayOut can mix in a clean slap-back echo.',
  },
  taps: {
    title: 'EARLY TAPS',
    short: 'TAPS',
    page: 'early',
    keys: [
      'early.TapCount',
      'early.TapLength',
      'early.TapGain',
      'early.TapDecay',
      'early.isReverse',
      'seeds.TapSeed',
      'seeds.CrossSeed',
    ],
    text: 'The early reflections: a multi-tap delay with up to 50 taps spread over TapLength. Tap times and gains come from TapSeed, so every seed is a different room shape. TapGain trades the clean pass-through for the taps, TapDecay makes later taps quieter (at 1 the last is 40 dB down), and BLOOM reverses the gain order so the taps swell instead of fade. This is the first sound of the room, before the dense tail.',
  },
  ediff: {
    title: 'EARLY DIFFUSER',
    short: 'E.DIFF',
    page: 'ediff',
    keys: [
      'early_diffusion.DiffusionEnabled',
      'early_diffusion.DiffusionStages',
      'early_diffusion.DiffusionDelay',
      'early_diffusion.DiffusionFeedback',
      'early_diffusion.EarlyDiffusionModAmount',
      'early_diffusion.EarlyDiffusionModRate',
      'seeds.DiffusionSeed',
      'seeds.CrossSeed',
    ],
    enable: 'early_diffusion.DiffusionEnabled',
    text: 'Allpass diffusers (1 or 2 stages in series) that smear the discrete taps into a smooth wash without changing the tone. More feedback and a longer stage delay give a denser, more blurred onset; modulation stops it ringing metallically. When off, the taps pass straight through. Its output is the EARLY bus and the input to the delay lines.',
  },
  lines: {
    title: 'DELAY LINES',
    short: 'DELAY LINES',
    page: 'late',
    keys: ['delay_lines.max', 'late.LateStageTap'],
    text: 'The reverb tail: identical recirculating delay lines running in parallel, how many set by the LINES switch (the preset\'s default or max, up to 5). Each line has its own seeded length, so their echoes interleave into a dense decay. The outputs are summed and scaled by 1/sqrt(N), so more lines thicken the tail without making it louder, at a CPU cost. LateStageTap moves each line\'s output: off, the tail is heard after the line\'s delay; on, before it (straight after the late diffuser), so the tail starts one LineDelay sooner.',
  },
  delay: {
    title: 'LINE DELAY',
    short: 'DELAY',
    page: 'late',
    keys: ['late.LineDelay', 'late.LineModAmount', 'late.LineModRate', 'seeds.DelaySeed'],
    text: 'Each line\'s main delay. LineDelay sets the base length, and each line scales it by a seeded 0.5-1.5x from DelaySeed; longer lengths give a bigger, slower-building space with wider echo spacing. LineMod amount and rate slowly wobble the length, which detunes the tail slightly and breaks up metallic resonance - at high settings, a chorus-like shimmer.',
  },
  ldiff: {
    title: 'LATE DIFFUSER',
    short: 'L.DIFF',
    page: 'ldiff',
    keys: [
      'late_diffusion.LateDiffusionEnabled',
      'late_diffusion.LateDiffusionStages',
      'late_diffusion.LateDiffusionDelay',
      'late_diffusion.LateDiffusionFeedback',
      'late_diffusion.LateDiffusionModAmount',
      'late_diffusion.LateDiffusionModRate',
      'late.Interpolation',
      'seeds.PostDiffusionSeed',
    ],
    enable: 'late_diffusion.LateDiffusionEnabled',
    text: "An allpass diffuser inside each line's loop. Because it sits in the feedback path, its smearing compounds on every pass, so the tail grows smoother and denser as it decays. Seeded per line from PostDiffusionSeed. Interpolation turns on fractional delays in these allpasses for smoother modulation, at some CPU cost.",
  },
  lowshelf: {
    title: 'LOW SHELF',
    short: 'LO SHLF',
    page: 'eq1',
    keys: ['late_eq.LowShelfEnabled', 'late_eq.PostLowShelfGain', 'late_eq.PostLowShelfFrequency'],
    enable: 'late_eq.LowShelfEnabled',
    text: 'A low shelf inside the feedback loop. It does not change the first pass of the tail: it is applied again on every recirculation, so a cut here makes the low end die away faster than the rest - the usual way to stop a long reverb getting muddy. The gain only cuts (0 dB = no change).',
  },
  highshelf: {
    title: 'HIGH SHELF',
    short: 'HI SHLF',
    page: 'eq1',
    keys: ['late_eq.HighShelfEnabled', 'late_eq.PostHighShelfGain', 'late_eq.PostHighShelfFrequency'],
    enable: 'late_eq.HighShelfEnabled',
    text: 'A high shelf inside the feedback loop. Every pass applies it again, so cutting highs makes the tail darken as it decays, the way air and soft surfaces absorb treble in a real room. The gain only cuts (0 dB = no change).',
  },
  cutoff: {
    title: 'LOOP LOW-PASS',
    short: 'CUTOFF',
    page: 'eq2',
    keys: ['late_eq.CutoffEnabled', 'late_eq.PostCutoffFrequency'],
    enable: 'late_eq.CutoffEnabled',
    text: 'A low-pass inside the feedback loop. Like the shelves it compounds on every pass: a low cutoff gives a warm tail whose brightness drains quickly; wide open keeps the tail bright to the end.',
  },
  decay: {
    title: 'FEEDBACK (DECAY)',
    short: 'DECAY',
    page: 'late',
    keys: ['late.LineDecay'],
    text: "The feedback gain from the end of each line back to its input. The engine derives it from LineDecay and each line's length so that the tail falls 60 dB in the LineDecay time (0.05-60 s). This is the main reverb-time control. Cuts in the loop EQ make the real decay shorter than this.",
  },
  mix: {
    title: 'OUTPUT MIX',
    short: 'MIX',
    page: 'output',
    keys: ['output.DryOut', 'output.PredelayOut', 'output.EarlyOut', 'output.MainOut'],
    text: 'Where four signals are blended: DRY (the untouched input), PRE (the pre-delay output), EARLY (the reflections after the early diffuser) and MAIN (the summed delay lines). Each level follows a 2-decade curve, and -INF removes that path entirely. Turn DRY down for a wet-only sound.',
  },
  revInto: {
    title: 'REVERSE INTO REVERB',
    short: 'REVERSE',
    page: 'rev',
    keys: REVERSE_KEYS,
    text: "Reverse voice in into-reverb mode (REVMIX off). The clean input is recorded in windows of REVWIN length and played backwards, then added to the reverb's input, so the reverse is heard only through the reverb as a swelling wash. The pedal subtracts it from the dry pass-through so your direct tone stays forward.",
  },
  revDirect: {
    title: 'REVERSE DIRECT',
    short: 'REVERSE',
    page: 'rev',
    keys: REVERSE_KEYS,
    text: "Reverse voice in direct mode (REVMIX on). The reverb's whole output is recorded in REVWIN-long windows and played backwards alongside it, so the reversed reverb is heard on its own. It fades in and out smoothly when switched.",
  },
  makeup: {
    title: 'MAKEUP GAIN',
    short: 'GAIN',
    page: 'output',
    keys: [],
    text: 'Automatic gain added by the pedal, not a preset parameter. It is computed from DRY, EARLY and MAIN: as the mix gets wetter the gain rises along an equal-power curve, so loudness stays roughly even as you move from dry to wet.',
  },
  out: {
    title: 'OUT',
    short: 'OUT',
    page: 'output',
    keys: [],
    text: 'The pedal output. In bypass the input is copied straight here, but the reverb keeps running. The diagram shows the preset as it sounds with the pedal on, in its stored state; the pedal\'s switches can change it live.',
  },
};

/** What turning each parameter does to the sound, one line per PARAM_KEYS key. */
export const EFFECT: Record<string, string> = {
  'input.InputMix': 'Nothing on this mono pedal.',
  'input.PreDelay': 'Longer: a wider gap between the note and the reverb.',
  'input.HiPassEnabled': 'On: the reverb input loses its low end (HPFREQ).',
  'input.HighPass': 'Higher: a thinner, less boomy reverb; the dry tone is unchanged.',
  'input.LowPassEnabled': 'On: the reverb input is darkened (LPFREQ).',
  'input.LowPass': 'Lower: a darker reverb from the first reflection on.',
  'early.TapCount': 'More: denser, smoother early reflections.',
  'early.TapLength': 'Longer: reflections spread over more time, a larger room.',
  'early.TapGain': 'Higher: more reflections, less clean signal through the tap stage.',
  'early.TapDecay': 'Higher: later taps fade more (at 1 the last tap is 40 dB down).',
  'early.isReverse': 'On (Bloom): the taps swell toward the end instead of fading.',
  'early_diffusion.DiffusionEnabled': 'On: the taps are smeared into a smooth wash.',
  'early_diffusion.DiffusionStages': '2 stages: a denser smear than 1.',
  'early_diffusion.DiffusionDelay': 'Longer: a slower, more blurred onset.',
  'early_diffusion.DiffusionFeedback': 'Higher: more smearing, less distinct taps.',
  'early_diffusion.EarlyDiffusionModAmount': 'Deeper: less metallic ringing, a slight pitch wobble.',
  'early_diffusion.EarlyDiffusionModRate': 'Faster: the wobble moves quicker.',
  'late.LineDelay': 'Longer: a bigger space with wider echo spacing in the tail.',
  'late.LineDecay': 'Longer: the tail takes longer to fall 60 dB.',
  'late.LineModAmount': 'Deeper: more detuning and shimmer in the tail.',
  'late.LineModRate': 'Faster: quicker chorus-like movement in the tail.',
  'late.LateStageTap': 'On: the tail is heard one LineDelay sooner, straight after the late diffuser.',
  'late.Interpolation': 'On: smoother late-diffuser modulation, more CPU.',
  'late_diffusion.LateDiffusionEnabled': 'On: the tail gets smoother and denser on every pass.',
  'late_diffusion.LateDiffusionStages': '2 stages: denser than 1.',
  'late_diffusion.LateDiffusionDelay': 'Longer: a more blurred, less echoey tail.',
  'late_diffusion.LateDiffusionFeedback': 'Higher: more smearing on every pass.',
  'late_diffusion.LateDiffusionModAmount': 'Deeper: less ringing, more movement in the tail.',
  'late_diffusion.LateDiffusionModRate': 'Faster: quicker movement.',
  'late_eq.LowShelfEnabled': 'On: lows are cut on every pass through the loop.',
  'late_eq.PostLowShelfGain': 'Lower: the low end of the tail dies faster (0 dB = no cut).',
  'late_eq.PostLowShelfFrequency': 'Higher: the cut reaches further up from the bass.',
  'late_eq.HighShelfEnabled': 'On: highs are cut on every pass through the loop.',
  'late_eq.PostHighShelfGain': 'Lower: the tail darkens faster as it decays (0 dB = no cut).',
  'late_eq.PostHighShelfFrequency': 'Lower: the cut reaches further down from the treble.',
  'late_eq.CutoffEnabled': 'On: a low-pass is applied on every pass.',
  'late_eq.PostCutoffFrequency': 'Lower: a warmer tail whose brightness drains quickly.',
  'seeds.TapSeed': 'Change: a different set of tap times and gains, a different room.',
  'seeds.DiffusionSeed': 'Change: different early allpass delays.',
  'seeds.DelaySeed': 'Change: different line lengths and modulation, a different tail texture.',
  'seeds.PostDiffusionSeed': 'Change: different late allpass delays.',
  'seeds.CrossSeed': 'Higher: the early seed series blend toward their complements.',
  'output.DryOut': 'Higher: more of the untouched guitar.',
  'output.PredelayOut': 'Higher: a clean echo at the pre-delay time.',
  'output.EarlyOut': 'Higher: more early reflections.',
  'output.MainOut': 'Higher: more reverb tail.',
  'reverse.delay': 'Longer: longer reversed phrases.',
  'reverse.enabled': 'On: the reverse voice plays.',
  'reverse.direct_mix': 'On: the reversed reverb is heard directly; off: the reverse feeds the reverb.',
  'delay_lines.max': 'On: the MAX line count from SETUP instead of DEFAULT - a denser tail, more CPU.',
};

const BLOCK_IDS = Object.keys(BLOCKS) as BlockId[];

/** Knob and toggle slots of `preset` that target `key`, e.g. 'K1A', 'S2B'. */
export function badgesFor(preset: Preset, key: string): string[] {
  const out: string[] = [];
  preset.knobMap.forEach((bank, b) =>
    bank.forEach((t, k) => t === key && out.push(`K${k + 1}${b ? 'B' : 'A'}`)),
  );
  preset.toggleMap.forEach((bank, b) =>
    bank.forEach((t, k) => t === key && out.push(`S${k + 1}${b ? 'B' : 'A'}`)),
  );
  return out;
}

/** Every block that owns `key`, in BLOCKS declaration order. */
export function blocksOfKey(key: string): BlockId[] {
  return BLOCK_IDS.filter((id) => BLOCKS[id].keys.includes(key));
}

/** Whether the block is in the signal path with the preset's stored switch states. */
export function blockOn(id: BlockId, preset: Preset): boolean {
  const p = preset.params;
  if (id === 'revInto') return p['reverse.enabled'] >= 0.5 && p['reverse.direct_mix'] < 0.5;
  if (id === 'revDirect') return p['reverse.enabled'] >= 0.5 && p['reverse.direct_mix'] >= 0.5;
  const enable = BLOCKS[id].enable;
  return enable === undefined || p[enable] >= 0.5;
}

/** Active delay lines, as updateEngineControls() in src/cloudseed.cpp selects them. */
export function lineCount(preset: Preset): number {
  return preset.params['delay_lines.max'] >= 0.5 ? preset.maxDelayLines : preset.defaultDelayLines;
}

/** Blocks a page edits, unique, in order of first appearance. */
export function pageBlocks(page: PageId, preset: Preset): BlockId[] {
  const keys =
    page === 'knobs'
      ? [...preset.knobMap[0], ...preset.knobMap[1]]
      : page === 'toggles'
        ? [...preset.toggleMap[0], ...preset.toggleMap[1]]
        : page === 'setup'
          ? null
          : PAGES.find((p) => p.id === page)!.keys;
  if (keys === null) return ['lines'];
  return [...new Set(keys.flatMap(blocksOfKey))];
}

/** The first block of `ids` that is on, else the first of `ids`. */
export function preferOn(ids: BlockId[], preset: Preset): BlockId | null {
  return ids.find((id) => blockOn(id, preset)) ?? ids[0] ?? null;
}
