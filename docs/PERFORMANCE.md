# CloudSeed/Terrarium Performance Optimizations

This document records the concrete performance and correctness fixes applied to the
CloudSeed reverb DSP running on the Daisy Seed (STM32H750, Cortex-M7 @ 480MHz, 48-sample
audio block, ~1ms budget). Each section names the exact pattern found, the file/line it
lived at, and the fix applied. No control mapping, preset, or audio topology changed.

## 1. FPU flush-to-zero (denormal stall elimination)

**Files**: `src/cloudseed.cpp`

Reverb feedback paths decay exponentially toward zero every tail: `DelayLine::Process`'s
feedback multiply (`CloudSeed/DelayLine.h:199`), `ModulatedAllpass::Process*`'s feedback
(`CloudSeed/ModulatedAllpass.h:90,128`), and `Biquad::Process`'s IIR state
(`CloudSeed/AudioLib/Biquad.h:57-61`). Once a value enters the subnormal range (~1e-38),
the Cortex-M7 FPU takes a multi-cycle microcoded slow path per operation instead of
single-cycle. The codebase already had three independent, incomplete, ad-hoc manual
guards for this (`ReverbChannel.h:373-380`, `AudioLib/Hp1.h:63-66`, `AudioLib/Lp1.h:59-62`)
that don't cover the delay-line/allpass feedback state where the problem originates.

**Fix**: set the FPU's Flush-to-Zero bit (FPSCR bit 24) once at boot, in `main()` before
`hw.Init()`:

```cpp
__set_FPSCR(__get_FPSCR() | (1u << 24)); // FZ: flush denormals to zero in hardware
```

Added `#include "cmsis_gcc.h"` for the `__set_FPSCR`/`__get_FPSCR` CMSIS intrinsics. The
three existing manual guards were left in place (harmless, now redundant).

## 2. Replace `std::map<Parameter,float>` with a flat array

**File**: `CloudSeed/ReverbChannel.h`

`parameters` was declared `map<Parameter, float>` — every `SetParameter` call and every
`UpdateLines()` read did a red-black-tree lookup (pointer-chasing, cache-unfriendly) for a
dense enum with 45 values (`CloudSeed/Parameter.h`). The sibling class
`ReverbController` already used the correct pattern: `float parameters[(int)Parameter::Count]`.

**Fix**: changed `parameters` to `float parameters[(int)Parameter::Count]`, removed
`#include <map>`, and updated every access site (constructor init loop, `SetSamplerate`'s
lambda, `SetParameter`, and the six `UpdateLines()` reads) to index with `(int)Parameter::X`.

## 3. Cache `perLineGain` instead of recomputing per block

**File**: `CloudSeed/ReverbChannel.h`

`GetPerLineGain()` computed `1.0 / std::sqrt(lineCount)` in double precision on every
`Process()` call (~1000×/sec at a 48-sample block), even though `lineCount` only changes
when the user toggles a delay-line switch. A dead commented-out line at the `LineCount`
parameter case (`//perLineGain = GetPerLineGain(); // In original Cloud Seed`) showed this
was the intended fix.

**Fix**: added a `float perLineGain` member, initialized in the constructor and
recomputed only in the `Parameter::LineCount` case of `SetParameter`. `Process()` now
reads the member directly instead of calling `GetPerLineGain()` (deleted). This also
fixes an incidental double-precision promotion: the cached value is computed with
`std::sqrt((float)lineCount)`, resolving to the single-precision `sqrtf` overload.

## 4. Remove per-tap integer modulo in `MultitapDiffuser::Process`

**File**: `CloudSeed/MultitapDiffuser.h:135` (pre-fix line)

Inside the per-sample tap loop (up to `MaxTaps = 50` taps):
`auto idx = (index + tapPos[j]) % len;`. `len` is a one-second delay buffer size in
samples (not a power of two), so this compiled to a full integer division per tap per
sample. `index` is kept in `[0, len)` and `tapPos[j] < length <= len` is guaranteed by
`Update()`, so `index + tapPos[j]` never reaches `2*len` — a single conditional
subtraction suffices, matching the wraparound technique already used elsewhere
(`ModulatedDelay.h`, `ModulatedAllpass.h`).

**Fix**:
```cpp
auto idx = index + tapPos[j];
if (idx >= len) idx -= len;
```

## 5. Fix double-precision `fmod` literal promotion

**Files**: `CloudSeed/ModulatedDelay.h:94`, `CloudSeed/ModulatedAllpass.h:151`

Both did `modPhase = std::fmod(modPhase, 1.0);` where `modPhase` is a `float` member. The
`1.0` literal is a `double`, forcing the `double` overload of `fmod` (promote, call,
narrow) instead of `fmodf`. This runs in `Update()`, called every 8 samples per instance,
across up to ~18 modulated delay/allpass instances at 5 active lines.

**Fix**: changed the literal to `1.0f` in both files, selecting the float-only `fmodf`
overload with no promotion/narrowing.

## 6. Fix undefined behavior in destructors mismatching `placement new`/`delete`

**Files**: `CloudSeed/ModulatedDelay.h`, `CloudSeed/ModulatedAllpass.h`,
`CloudSeed/MultitapDiffuser.h`, `CloudSeed/ReverbChannel.h`, `CloudSeed/DelayLine.h`

The custom SDRAM allocator (`src/sdram_pool.cpp`'s `custom_pool_allocate`) is a
bump allocator with no free function — by design, since the `ReverbController` is
constructed once and lives for the process lifetime. Most buffers are correctly
constructed with placement `new (custom_pool_allocate(...)) T[...]` into this pool, but
five destructors called plain `delete`/`delete[]` on those placement-new'd pointers
anyway — undefined behavior (can corrupt the pool's bump-allocation bookkeeping), latent
because these destructors are never invoked in the shipped firmware but a real bug for any
future code path that does destroy one of these objects.

**Fix**:
- `ModulatedDelay`, `ModulatedAllpass`, `MultitapDiffuser`: destructors no longer call
  `delete` on their pool-backed buffers (replaced with an explanatory comment).
- `ReverbChannel`: destructor now explicitly placement-destroys each `DelayLine`
  (`line->~DelayLine()`) instead of `delete line`, and no longer deletes the pool-backed
  `tempBuffer`/`lineOutBuffer`/`outBuffer`.
- `DelayLine`: `tempBuffer`/`mixedBuffer`/`filterOutputBuffer` are the one exception — they
  are real heap allocations (`new float[bufferSize]`, not pool-backed). The bug there was
  `delete` (singular) against a `new[]` allocation; fixed to `delete[]` to match.

`AllpassDiffuser.h`'s destructor (`delete filter` on plain `new ModulatedAllpass(...)`
pointers) was already correctly paired and left untouched.

## 7. Bump the application build to `-O3` + `-ffast-math`

**Files**: `./Makefile`, `CloudSeed/Makefile`

`src/cloudseed.cpp` built through `libdaisy/core/Makefile`'s default
`OPT ?= -O2`, one level below the `-O3` the `CloudSeed/` library itself already used.

**Fix**: `./Makefile` now sets `OPT = -O3` before including
`$(SYSTEM_FILES_DIR)/Makefile` (must precede the include since it uses a weak `?=`
default), and appends `CPPFLAGS += -ffast-math` after the include. `CloudSeed/Makefile`
adds `-ffast-math` to its existing `CPPFLAGS` list alongside `-fno-exceptions`, so the
library and application compile with matching floating-point semantics. This lets GCC
fuse the many multiply-accumulate patterns in `Biquad::Process`, `DelayLine::Process`'s
feedback mix, and the allpass/multitap taps into single-cycle Cortex-M7 VFMA instructions,
and drops `errno`-setting overhead from `pow`/`sqrt`/`log10` calls.

**Verified**: build succeeds with no errors; `.text`+`.data` = 140000 bytes (28.48% of the
480KB `BOOT_SRAM` region), comfortably within budget.

## 8. Remove redundant bypass-buffer copy in the audio callback

**File**: `src/cloudseed.cpp`

`audioBypassBuffer[AUDIO_BUFFER_SIZE]` existed purely to echo the input back out when
bypassed, filled by a full per-sample copy loop every block regardless of bypass state.
Since `in[0][i]` remains valid for the whole callback, the buffer and its copy were
unnecessary.

**Fix**: deleted `audioBypassBuffer` and its copy loop; both bypass output paths
(`state.bypass` branch and the preset-change-in-progress branch) now write
`out[0][i] = in[0][i];` directly.

## 9. Allocation-free parameter updates (seed series cached)

**Files**: `CloudSeed/AudioLib/ShaRandom.h`, `CloudSeed/AudioLib/ShaRandom.cpp`,
`CloudSeed/Utils/Sha256.h`, `CloudSeed/Utils/Sha256.cpp`, `CloudSeed/MultitapDiffuser.h`,
`CloudSeed/AllpassDiffuser.h`, `CloudSeed/ReverbChannel.h`; test `tests/engine_alloc_test.cpp`

Knob and toggle targets are applied inside the audio callback, and 17 of the 47 parameters
allocated on the heap when written. `ShaRandom::Generate()` built its result from
`std::vector`s, and each `sha256()` call returned a new one. `ReverbChannel::UpdateLines()`
regenerated the delay-line seed series (4 SHA-256 digests) on every `LineDelay`, `LineDecay`,
`LineModAmount`, `LineModRate` and `LateDiffusionMod*` write, although the series depends only
on `DelaySeed` and `CrossSeed`. `MultitapDiffuser::Update()` built three temporary vectors for
every `TapCount`, `TapLength`, `TapGain`, `TapDecay` and `isReverse` write, and `Process()`
copy-assigned them. Measured on the host with counting allocators, over six writes each:
`LineDecay` made 210 heap allocations, `TapDecay` 18, and `CrossSeed` 2508 (it re-hashed every
stage's series, and each `DelayLine` diffuser hashed twice via `SetSeed` then `SetCrossSeed`).

**Fix**:
- `sha256()` writes into a caller buffer, and `ShaRandom::Generate(seed, out, count)` fills a
  caller array. The algorithm is unchanged: each digest hashes the first 8 bytes of the
  previous one.
- New `AudioLib::SeedSeries<N>` (`ShaRandom.h`) holds the series of `seed` and of `~seed` in
  fixed arrays plus their cross-seed blend. It re-hashes only when the seed actually changes,
  and a `CrossSeed` write only re-blends. `MultitapDiffuser` (`2 * MaxTaps` values),
  `AllpassDiffuser` (`MaxStageCount * 3`) and `ReverbChannel` (`TotalLineCount * 3`, the line
  seeds) each own one. `UpdateLines()` reads the cached line seeds and never hashes.
- `MultitapDiffuser`'s tap gains, positions, their `*Temp` copies and `tapData` are
  `MaxTaps`-sized arrays. `Update()` clamps the tap count to 1..`MaxTaps`, a limit
  `GetScaledParameter` already keeps.

**Result**: `tests/engine_alloc_test.cpp` (part of `make test`) sweeps every parameter
through `SetParameter()` and `Process()` after boot and requires zero heap and zero pool
allocations; it failed for 17 parameters before this change. The output is unchanged:
rendering all ten presets while sweeping the seeds, `CrossSeed`, `LineDecay`, `TapDecay`,
`TapCount`, `LineModAmount` and `isReverse` is byte-identical to the old engine at host
`-O2`, and at `-O3 -ffast-math -mfma -fno-tree-vectorize`. With the host auto-vectorizer on, the
two differ only by float reassociation, at most 92 dB below peak; the Cortex-M7's
scalar-only FPU gives GCC nothing to vectorize with. Code size: `-O3` now inlines the smaller
`MultitapDiffuser::Update()` into each of its `SetParameter` cases, adding 7.5 KB of SRAM
(`ReverbChannel::SetParameter` 8,128 → 16,416 B).

## Verification performed

1. **Build**: `make libs && make clean && make` completes
   with no errors and produces `build/cloudseed.bin`/`.elf`/`.hex`. Memory usage:
   SDRAM 77.10% of 64MB, SRAM (`.text`+`.data`) 28.48% of 480KB `BOOT_SRAM`.
2. **Host-side behavioral smoke test**: a throwaway harness linked `ReverbController`
   against a `malloc`-backed stand-in for `custom_pool_allocate`, drove `LineCount`
   through 1-5, and processed an impulse-then-silence buffer through the pre-fix
   (steps 2-5 reverted) and post-fix code. Output was bit-identical across all 480
   samples, exit code 0, no NaN/Inf in either build.
