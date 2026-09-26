#ifndef DELAYLINECOUNT
#define DELAYLINECOUNT

namespace CloudSeed
{
	// Number of delay lines ReverbChannel builds, and so the largest LineCount it
	// can process. The single definition: ReverbChannel clamps LineCount to
	// 1..TotalLineCount, and the preset parser (src/preset_bank.cpp) rejects a
	// default_delay_lines / max_delay_lines above it.
	//
	// The original CloudSeed plugin uses 8 (or 12) lines, DaisyCloudSeed used 2 for
	// stereo on the Daisy Patch, and GuitarML's mono Terrarium fork used 4. Each line
	// costs SDRAM pool memory and CPU.
	constexpr int TotalLineCount = 5;
}

#endif
