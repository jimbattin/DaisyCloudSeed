#!/usr/bin/env python3
"""Generate presets.toml (and the golden dump used by `make presets-check`) from
the hard-coded initFactory* methods in CloudSeed/ReverbController.h.

This is a one-way migration tool: once presets.toml is the source of truth it is
kept only so the extraction can be re-audited against git history.

Usage: python3 tools/gen_presets_toml.py [--check]
  --check  write to a temporary location and diff against the committed files
"""

import argparse
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(REPO, "CloudSeed", "ReverbController.h")
PARAM_H = os.path.join(REPO, "CloudSeed", "Parameter.h")

# Order is persisted to QSPI flash as Settings::currentPreset. Never reorder.
PRESET_META = [
    ("initFactoryChorus", "Chorus", 1, "5.0"),
    ("initFactoryDullEchos", "Dull Echos", 2, "5.0"),
    ("initFactoryHyperplane", "Hyperplane", 3, "5.0"),
    ("initFactoryMediumSpace", "Medium Space", 4, "5.0"),
    ("initFactoryNoiseInTheHallway", "Noise in the Hallway", 5, "5.0"),
    ("initFactoryRubiKaFields", "Rubi Ka Fields", 6, "5.0"),
    ("initFactorySmallRoom", "Small Room", 7, "5.0"),
    ("initFactory90sAreBack", "90s Are Back", 8, "5.0"),
    ("initFactoryThroughTheLookingGlass", "Through the Looking Glass", 9, "4.0"),
    ("initFactoryDarkPlate", "Dark Plate", 10, "5.0"),
]

LED_ON_MS = 150
LED_OFF_MS = 150
LED_PAUSE_MS = 5000

# Runtime-controlled: SWITCH_1 drives LineCount, SWITCH_4 drives isReverse.
RUNTIME_PARAMS = ("LineCount", "isReverse")

# Signal-flow grouping. Must stay in sync with kGroups in preset_bank.cpp.
GROUPS = [
    ("input",
     "Input stage: pre-delay and the input filters feeding the whole reverb.",
     ["InputMix", "PreDelay", "HiPassEnabled", "HighPass", "LowPassEnabled",
      "LowPass"]),
    ("early",
     "Early reflections: the multi-tap delay that follows the input stage.",
     ["TapCount", "TapLength", "TapGain", "TapDecay"]),
    ("early_diffusion",
     "Early diffusion: allpass chain that smears the early reflections.",
     ["DiffusionEnabled", "DiffusionStages", "DiffusionDelay",
      "DiffusionFeedback", "EarlyDiffusionModAmount", "EarlyDiffusionModRate"]),
    ("late",
     "Late reverb lines: the feedback delay network that generates the tail.",
     ["LineDelay", "LineDecay", "LineModAmount", "LineModRate", "LateStageTap",
      "Interpolation"]),
    ("late_diffusion",
     "Late diffusion: allpass chain inside each delay line.",
     ["LateDiffusionEnabled", "LateDiffusionStages", "LateDiffusionDelay",
      "LateDiffusionFeedback", "LateDiffusionModAmount",
      "LateDiffusionModRate"]),
    ("late_eq",
     "Late EQ: shelves and low-pass applied inside each line's feedback path.",
     ["LowShelfEnabled", "PostLowShelfGain", "PostLowShelfFrequency",
      "HighShelfEnabled", "PostHighShelfGain", "PostHighShelfFrequency",
      "CutoffEnabled", "PostCutoffFrequency"]),
    ("seeds",
     "Random seeds: fix the tap/delay/diffusion patterns so a preset is "
     "reproducible.",
     ["TapSeed", "DiffusionSeed", "DelaySeed", "PostDiffusionSeed",
      "CrossSeed"]),
    ("output",
     "Output mix: level of each stage in the final signal.",
     ["DryOut", "PredelayOut", "EarlyOut", "MainOut"]),
]

FILE_HEADER = '''\
# CloudSeed reverb presets for the Terrarium pedal.
#
# This file is compiled into the firmware image (see presets_toml.s) and parsed
# once at boot. Edit values here, then rebuild and reflash:
#     make presets-check && make && make program-dfu
#
# Rules enforced by the parser (a violation stops boot and blinks both LEDs):
#   * Preset order is the order below. The index is stored in QSPI flash, so
#     reordering or deleting presets invalidates saved settings.
#   * Every [preset.params.*] group must contain exactly the keys listed for it;
#     a key in the wrong group, an unknown key, or a missing key is an error.
#   * LineCount and isReverse must NOT appear: they are driven live by SWITCH_1
#     (delay line count) and SWITCH_4 (bloom).
#
# Every parameter value is normalized 0.0-1.0. The engine maps it to the real
# range noted in the comments below.
#
# PARAMETER REFERENCE (values are normalized 0.0-1.0; the engine maps them to the
# real ranges shown here). Each preset repeats these groups with its own values.
#
#   [preset.params.input]  Input stage: pre-delay and the input filters feeding the whole reverb.
#     InputMix        stereo input blend; unused in this mono fork, kept so the value set stays complete
#     PreDelay        delay before the reverb, 0-1000 ms
#     HiPassEnabled   on when >= 0.5: enables the input high-pass
#     HighPass        input high-pass cutoff, 20-1000 Hz
#     LowPassEnabled  on when >= 0.5: enables the input low-pass
#     LowPass         input low-pass cutoff, 400-20000 Hz
#
#   [preset.params.early]  Early reflections: the multi-tap delay that follows the input stage.
#     TapCount   number of early-reflection taps, 1-50
#     TapLength  time spread of the taps, 0-500 ms
#     TapGain    gain of the tap bank, 2-decade curve
#     TapDecay   how fast tap gain falls across the bank, 0-1
#
#   [preset.params.early_diffusion]  Early diffusion: allpass chain that smears the early reflections.
#     DiffusionEnabled         on when >= 0.5: runs the early allpass diffuser
#     DiffusionStages          allpass stages in series, 1-2
#     DiffusionDelay           delay per allpass stage, 10-100 ms
#     DiffusionFeedback        allpass feedback, 0-1
#     EarlyDiffusionModAmount  allpass delay modulation depth, 0-2.5 ms
#     EarlyDiffusionModRate    allpass delay modulation rate, 0-5 Hz
#
#   [preset.params.late]  Late reverb lines: the feedback delay network that generates the tail.
#     LineDelay       delay per line, 20-1000 ms (line count comes from SWITCH_1, not this file)
#     LineDecay       tail decay time, 0.05-60 s
#     LineModAmount   line delay modulation depth, 0-2.5 ms
#     LineModRate     line delay modulation rate, 0-5 Hz
#     LateStageTap    on when >= 0.5: tap the line after diffusion instead of before
#     Interpolation   on when >= 0.5: fractional-delay interpolation (smoother modulation, more CPU)
#
#   [preset.params.late_diffusion]  Late diffusion: allpass chain inside each delay line.
#     LateDiffusionEnabled    on when >= 0.5: runs the per-line allpass diffuser
#     LateDiffusionStages     allpass stages per line, 1-2
#     LateDiffusionDelay      delay per allpass stage, 10-100 ms
#     LateDiffusionFeedback   allpass feedback, 0-1
#     LateDiffusionModAmount  allpass delay modulation depth, 0-2.5 ms
#     LateDiffusionModRate    allpass delay modulation rate, 0-5 Hz
#
#   [preset.params.late_eq]  Late EQ: shelves and low-pass applied inside each line's feedback path.
#     LowShelfEnabled         on when >= 0.5: enables the low shelf
#     PostLowShelfGain        low shelf gain, 2-decade curve
#     PostLowShelfFrequency   low shelf corner, 20-1000 Hz
#     HighShelfEnabled        on when >= 0.5: enables the high shelf
#     PostHighShelfGain       high shelf gain, 2-decade curve
#     PostHighShelfFrequency  high shelf corner, 400-20000 Hz
#     CutoffEnabled           on when >= 0.5: enables the in-loop low-pass
#     PostCutoffFrequency     in-loop low-pass cutoff, 400-20000 Hz
#
#   [preset.params.seeds]  Random seeds: fix the tap/delay/diffusion patterns so a preset is reproducible.
#     TapSeed            early tap pattern seed, value x 1000000
#     DiffusionSeed      early diffuser seed, value x 1000000
#     DelaySeed          delay line length seed, value x 1000000
#     PostDiffusionSeed  late diffuser seed, value x 1000000
#     CrossSeed          stereo seed offset; no audible effect in this mono fork
#
#   [preset.params.output]  Output mix: level of each stage in the final signal.
#     DryOut       dry signal level, 2-decade curve
#     PredelayOut  pre-delay tap level, 2-decade curve
#     EarlyOut     early reflection level, 2-decade curve
#     MainOut      late reverb level, 2-decade curve
#
'''

SEPARATOR = "# " + "-" * 74

ASSIGN_RE = re.compile(
    r"^\s*parameters\[\(int\)Parameter::([A-Za-z0-9_]+)\]\s*=\s*([^;]+);")


def parse_enum_order(text):
    body = text.split("enum class Parameter", 1)[1]
    body = body.split("{", 1)[1].split("};", 1)[0]
    names = []
    for raw in body.splitlines():
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        for item in line.split(","):
            item = item.strip()
            if not item:
                continue
            name = item.split("=", 1)[0].strip()
            if not name:
                continue
            if name in ("Count", "Unused"):
                return names
            names.append(name)
    return names


def extract_presets(text):
    """Return {init_function_name: {param: literal}} for every initFactory*."""
    lines = text.splitlines()
    starts = []
    for i, line in enumerate(lines):
        m = re.match(r"\s*void (initFactory[A-Za-z0-9_]+)\(\)", line)
        if m:
            starts.append((i, m.group(1)))

    out = {}
    for idx, (start, name) in enumerate(starts):
        end = starts[idx + 1][0] if idx + 1 < len(starts) else len(lines)
        values = {}
        for line in lines[start:end]:
            stripped = line.lstrip()
            if stripped.startswith("//"):
                continue  # commented-out assignment (LineCount / isReverse)
            m = ASSIGN_RE.match(line)
            if not m:
                continue
            key, literal = m.group(1), m.group(2).strip()
            if key in values:
                raise SystemExit("%s: duplicate assignment of %s" % (name, key))
            values[key] = literal
        out[name] = values
    return out


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def build_toml(presets):
    chunks = [FILE_HEADER]
    for func, name, blinks, max_lines in PRESET_META:
        values = presets[func]
        body = [SEPARATOR,
                "[[preset]]",
                'name = "%s"' % name,
                "blinks = %d" % blinks,
                "led_on_ms = %d" % LED_ON_MS,
                "led_off_ms = %d" % LED_OFF_MS,
                "led_pause_ms = %d" % LED_PAUSE_MS,
                "max_delay_lines = %s" % max_lines]
        for group, comment, keys in GROUPS:
            width = max(len(k) for k in keys)
            body.append("")
            body.append("# " + comment)
            body.append("[preset.params.%s]" % group)
            for key in keys:
                body.append("%-*s = %s" % (width, key, values[key]))
        chunks.append("\n".join(body) + "\n")
    return "\n".join(chunks)


def build_expected(presets, enum_order):
    lines = []
    for index, (func, name, blinks, max_lines) in enumerate(PRESET_META):
        values = presets[func]
        lines.append(
            'preset %d name="%s" blinks=%d on=%d off=%d pause=%d '
            "max_delay_lines=%.6g"
            % (index, name, blinks, LED_ON_MS, LED_OFF_MS, LED_PAUSE_MS,
               f32(float(max_lines))))
        for key in enum_order:
            if key in RUNTIME_PARAMS:
                continue
            lines.append("  %s = %.9g" % (key, f32(float(values[key]))))
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="compare against the committed files instead of "
                         "rewriting them")
    ap.add_argument("--source", default=HEADER,
                    help="header to extract initFactory* bodies from; point it "
                         "at a copy from git history once the methods are gone "
                         "(git show <rev>:CloudSeed/ReverbController.h)")
    args = ap.parse_args()

    source = args.source
    with open(source, "r") as fp:
        header_text = fp.read()
    with open(PARAM_H, "r") as fp:
        enum_order = parse_enum_order(fp.read())

    presets = extract_presets(header_text)
    if not presets:
        raise SystemExit(
            "no initFactory* methods found in %s; presets.toml is already the "
            "source of truth and this generator has nothing left to read"
            % source)

    expected_params = [p for p in enum_order if p not in RUNTIME_PARAMS]
    grouped = [k for _, _, keys in GROUPS for k in keys]
    if sorted(grouped) != sorted(expected_params):
        raise SystemExit("GROUPS does not cover exactly the %d non-runtime "
                         "parameters" % len(expected_params))

    for func, _, _, _ in PRESET_META:
        if func not in presets:
            raise SystemExit("missing %s in %s" % (func, source))
        missing = [k for k in expected_params if k not in presets[func]]
        extra = [k for k in presets[func] if k not in expected_params]
        if missing:
            raise SystemExit("%s: missing %s" % (func, ", ".join(missing)))
        if extra:
            raise SystemExit("%s: unexpected %s" % (func, ", ".join(extra)))

    toml_text = build_toml(presets)
    expected_text = build_expected(presets, enum_order)

    toml_path = os.path.join(REPO, "presets.toml")
    expected_path = os.path.join(REPO, "tools", "presets_expected.txt")

    if args.check:
        ok = True
        for path, text in ((toml_path, toml_text),
                           (expected_path, expected_text)):
            try:
                with open(path, "r") as fp:
                    current = fp.read()
            except IOError:
                print("%s: missing" % path, file=sys.stderr)
                ok = False
                continue
            if current != text:
                print("%s: differs from generated content" % path,
                      file=sys.stderr)
                ok = False
        return 0 if ok else 1

    os.makedirs(os.path.dirname(expected_path), exist_ok=True)
    with open(toml_path, "w") as fp:
        fp.write(toml_text)
    with open(expected_path, "w") as fp:
        fp.write(expected_text)
    print("wrote %s (%d bytes)" % (toml_path, len(toml_text)))
    print("wrote %s (%d bytes)" % (expected_path, len(expected_text)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
