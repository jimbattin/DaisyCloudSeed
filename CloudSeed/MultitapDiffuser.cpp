#include <algorithm>
#include <cmath>
#include <new>
#include "MultitapDiffuser.h"
#include "Utils.h"

extern void* custom_pool_allocate(size_t size);

namespace CloudSeed
{
	MultitapDiffuser::MultitapDiffuser(int delayBufferSize)
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

	void MultitapDiffuser::Process(float* input, int sampleCount)
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

	void MultitapDiffuser::ClearBuffers()
	{
		Utils::ZeroBuffer(buffer, len);
		Utils::ZeroBuffer(output, len);
	}

	void MultitapDiffuser::Update()
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
}
