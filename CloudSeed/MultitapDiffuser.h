#ifndef MULTITAPDIFFUSER
#define MULTITAPDIFFUSER

#include <algorithm>
#include <cmath>
#include "MultitapDiffuser.h"
#include "Utils.h"
#include "AudioLib/ShaRandom.h"
extern void* custom_pool_allocate(size_t size);

namespace CloudSeed
{
	using namespace std;

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
		MultitapDiffuser(int delayBufferSize)
		{
			len = delayBufferSize;
			buffer = new (custom_pool_allocate(sizeof(float) * delayBufferSize)) float[delayBufferSize];
			output = new (custom_pool_allocate(sizeof(float) * delayBufferSize)) float[delayBufferSize];
			index = 0;
			count = 1;
			length = 1;
			gain = 1.0;
			decay = 0.0;
			isReverse = false;
			seeds.SetSeed(0);
			Update();
		}

		~MultitapDiffuser()
		{
			// buffer/output are placement-new'd into the SDRAM bump-allocator pool
			// (custom_pool_allocate) and are never freed; nothing to delete here.
		}


		void SetSeed(int seed)
		{
			seeds.SetSeed(seed);
			Update();
		}

		void SetCrossSeed(float crossSeed)
		{
			seeds.SetCrossSeed(crossSeed);
			Update();
		}

		float* GetOutput()
		{
			return output;
		}

		void SetTapCount(int tapCount)
		{
			count = tapCount;
			Update();
		}

		void SetTapLength(int tapLength)
		{
			length = tapLength;
			Update();
		}

		void SetTapDecay(float tapDecay)
		{
			decay = tapDecay;
			Update();
		}

		void SetTapGain(float tapGain)
		{
			gain = tapGain;
			Update();
		}

		void SetReverseDecay(bool reverseDecay)
		{
			this->isReverse = reverseDecay;
			Update();
		}

		void Process(float* input, int sampleCount)
		{
			// prevents race condition when parameters are updated from Gui
			if (isDirty)
			{
				std::copy(tapGains, tapGains + count, tapGainsTemp);
				std::copy(tapPosition, tapPosition + count, tapPositionTemp);
				countTemp = count;
				isDirty = false;
			}

			int* const tapPos = tapPositionTemp;
			float* const tapGain = tapGainsTemp;
			const int cnt = countTemp;

			for (int i = 0; i < sampleCount; i++)
			{
				if (index < 0) index += len;
				buffer[index] = input[i];
				output[i] = 0.0;

				for (int j = 0; j < cnt; j++)
				{
					auto idx = index + tapPos[j];
					if (idx >= len) idx -= len;
					output[i] += buffer[idx] * tapGain[j];
				}

				index--;
			}
		}

		void ClearBuffers()
		{
			Utils::ZeroBuffer(buffer, len);
			Utils::ZeroBuffer(output, len);
		}


	private:
		void Update()
		{
			int s = 0;
			auto rand = [&]() {return seeds[s++]; };

			// The tap arrays hold MaxTaps; GetScaledParameter already keeps TapCount
			// within 1..MaxTaps.
			if (count < 1)
				count = 1;
			if (count > MaxTaps)
				count = MaxTaps;

			if (length < count)
				length = count;

			// used to adjust the volume of the overall output as it grows when we add more taps
			float tapCountFactor = 1.0 / (1 + std::sqrt(count / MaxTaps));

			float tapData[MaxTaps];

			auto sumLengths = 0.0;
			for (int i = 0; i < count; i++)
			{
				auto val = 0.1 + rand();
				tapData[i] = val;
				sumLengths += val;
			}

			auto scaleLength = length / sumLengths;
			tapPosition[0] = 0;

			for (int i = 1; i < count; i++)
			{
				tapPosition[i] = tapPosition[i - 1] + (int)(tapData[i] * scaleLength);
			}

			float lastTapPos = tapPosition[count - 1];
			int gainIndex = 0;

			for (int i = 0; i < count; i++)
			{
				// when decay set to 0, there is no decay, when set to 1, the gain at the last sample is 0.01 = -40dB
				auto g = std::pow(10, -decay * 2 * tapPosition[i] / (float)(lastTapPos + 1));
				auto tap = (2 * rand() - 1) * tapCountFactor;

				if (isReverse) 
					gainIndex = count - (i + 1);
				else
					gainIndex = i;
				tapGains[gainIndex] = tap * g * gain;
			}
			// Set the tap vs. clean mix
			if (isReverse)
				tapGains[count - 1] = (1 - gain);
			else
				tapGains[0] = (1 - gain);

			isDirty = true;
		}
	};
}

#endif
