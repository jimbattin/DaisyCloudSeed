#ifndef MULTITAPDIFFUSER
#define MULTITAPDIFFUSER

#include "AudioLib/ShaRandom.h"

namespace CloudSeed
{
	class MultitapDiffuser
	{
	public:
		static const int MaxTaps = 50;

	private:
		float* buffer;
		float* output;
		int len;

		int index;
		// Update() fills these, Process() plays the *Temp copies (swapped in on isDirty).
		// Fixed MaxTaps size: a knob write must not allocate.
		float tapGains[MaxTaps];
		int tapPosition[MaxTaps];
		AudioLib::SeedSeries<2 * MaxTaps> seeds;  // Update() draws two values per tap
		int count;
		float length;
		float gain;
		float decay;

		bool isDirty;
		bool isReverse;
		float tapGainsTemp[MaxTaps];
		int tapPositionTemp[MaxTaps];
		int countTemp;

	public:
		// buffer/output are placement-new'd into the SDRAM bump-allocator pool
		// (custom_pool_allocate) and are never freed, so there is no destructor.
		MultitapDiffuser(int delayBufferSize);

		float* GetOutput() { return output; }

		void SetSeed(int seed)                  { seeds.SetSeed(seed); Update(); }
		void SetCrossSeed(float crossSeed)      { seeds.SetCrossSeed(crossSeed); Update(); }
		void SetTapCount(int tapCount)          { count = tapCount; Update(); }
		void SetTapLength(int tapLength)        { length = tapLength; Update(); }
		void SetTapDecay(float tapDecay)        { decay = tapDecay; Update(); }
		void SetTapGain(float tapGain)          { gain = tapGain; Update(); }
		void SetReverseDecay(bool reverseDecay) { isReverse = reverseDecay; Update(); }

		void Process(float* input, int sampleCount);
		void ClearBuffers();

	private:
		// Recomputes the tap positions and gains. Out of line on purpose: every
		// setter calls it, and inlining it into each ReverbChannel::SetParameter case
		// cost 7.5 KB of SRAM for code that runs once per knob write.
		void Update();
	};
}

#endif
