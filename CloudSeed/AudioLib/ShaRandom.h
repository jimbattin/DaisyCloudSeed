#ifndef SHARANDOM
#define SHARANDOM

namespace AudioLib
{
	class ShaRandom
	{
	public:
		// Writes `count` pseudo-random values in [0, 1] derived from `seed` into `out`.
		// The series is part of every preset's sound, so the algorithm is frozen; the
		// first N values do not depend on `count`. No allocation.
		static void Generate(long long seed, float* out, int count);
	};

	// The seed series every CloudSeed stage draws its delays, gains and modulation
	// from: the series of `seed` cross-faded with the series of its complement,
	//     value[i] = a[i] * (1 - crossSeed) + b[i] * crossSeed
	// where a = Generate(seed) and b = Generate(~seed). Both raw series are kept, so
	// a new cross-seed only re-blends and a repeated seed is not re-hashed. Knob
	// writes reach this from the audio callback, so nothing here allocates.
	template <int N>
	class SeedSeries
	{
	public:
		void SetSeed(long long seed)
		{
			if (hashed && seed == this->seed)
				return;
			this->seed = seed;
			hashed = true;
			ShaRandom::Generate(seed, a, N);
			ShaRandom::Generate(~seed, b, N);
			Blend();
		}

		void SetCrossSeed(float crossSeed)
		{
			this->crossSeed = crossSeed;
			Blend();
		}

		float operator[](int i) const { return mixed[i]; }

	private:
		void Blend()
		{
			for (int i = 0; i < N; i++)
				mixed[i] = a[i] * (1 - crossSeed) + b[i] * crossSeed;
		}

		long long seed = 0;
		bool hashed = false;   // a and b hold the series of `seed`
		float crossSeed = 0.0f;
		float a[N] = {};
		float b[N] = {};
		float mixed[N] = {};
	};
}

#endif
