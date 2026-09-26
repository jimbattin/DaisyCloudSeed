
#pragma once

#include <vector>
#include "ModulatedAllpass.h"
#include "AudioLib/ShaRandom.h"

using namespace std;

namespace CloudSeed
{
	class AllpassDiffuser
	{
	public:
		static const int MaxStageCount = 2;

	private:
		int samplerate;

		vector<ModulatedAllpass*> filters;  // filled once, in the constructor
		int delay;
		float modRate;
		AudioLib::SeedSeries<MaxStageCount * 3> seeds;  // delay, mod amount, mod rate per stage
		
	public:
		int Stages;

		AllpassDiffuser(int samplerate, int delayBufferLengthMillis)
		{
			auto delayBufferSize = samplerate * ((float)delayBufferLengthMillis / 1000.0);
			for (int i = 0; i < MaxStageCount; i++)
			{
				filters.push_back(new ModulatedAllpass((int)delayBufferSize, 100));
			}

			seeds.SetSeed(23456);
			Update();
			Stages = 1;

			SetSamplerate(samplerate);
		}

		~AllpassDiffuser()
		{
			for (auto filter : filters)
				delete filter;
		}

		int GetSamplerate()
		{
			return samplerate;
		}

		void SetSamplerate(int samplerate)
		{
			this->samplerate = samplerate;
			SetModRate(modRate);
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


		bool GetModulationEnabled()
		{
			return filters[0]->ModulationEnabled;
		}

		void SetModulationEnabled(bool value)
		{
			for (auto filter : filters)
				filter->ModulationEnabled = value;
		}

		void SetInterpolationEnabled(bool enabled)
		{
			for (auto filter : filters)
				filter->InterpolationEnabled = enabled;
		}

		float* GetOutput()
		{
			return filters[Stages - 1]->GetOutput();
		}

		
		void SetDelay(int delaySamples)
		{
			delay = delaySamples;
			Update();
		}

		void SetFeedback(float feedback)
		{
			for (auto filter : filters)
				filter->Feedback = feedback;
		}

		void SetModAmount(float amount)
		{
			for (size_t i = 0; i < filters.size(); i++)
			{
				filters[i]->ModAmount = amount * (0.85 + 0.3 * seeds[MaxStageCount + i]);
			}
		}

		void SetModRate(float rate)
		{
			modRate = rate;

			for (size_t i = 0; i < filters.size(); i++)
				filters[i]->ModRate = rate * (0.85 + 0.3 * seeds[MaxStageCount * 2 + i]) / samplerate;
		}

		void Process(float* input, int sampleCount)
		{
			ModulatedAllpass** filterPtr = &filters[0];

			filterPtr[0]->Process(input, sampleCount);

			for (int i = 1; i < Stages; i++)
			{
				filterPtr[i]->Process(filterPtr[i - 1]->GetOutput(), sampleCount);
			}
		}

		void ClearBuffers()
		{
			for (size_t i = 0; i < filters.size(); i++)
				filters[i]->ClearBuffers();
		}

	private:
		void Update()
		{
			for (size_t i = 0; i < filters.size(); i++)
			{
				auto r = seeds[i];
				auto d = std::pow(10, r) * 0.1; // 0.1 ... 1.0
				filters[i]->SampleDelay = (int)(delay * d);
			}
		}

	};
}
