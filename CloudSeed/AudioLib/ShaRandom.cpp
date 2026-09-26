
#include <climits>
#include <cstring>
#include "ShaRandom.h"
#include "../Utils/Sha256.h"

namespace AudioLib
{
	void ShaRandom::Generate(long long seed, float* out, int count)
	{
		// Each digest yields eight 32-bit values. The next digest hashes only the
		// first 8 bytes of the previous one, starting from the 8 bytes of the seed
		// itself: that is the original CloudSeed algorithm, kept bit for bit.
		static_assert(sizeof(seed) == 8, "the seed is hashed as 8 bytes");
		const int valuesPerDigest = SHA256::DIGEST_SIZE / sizeof(unsigned int);

		unsigned char input[8];
		unsigned char digest[SHA256::DIGEST_SIZE];
		memcpy(input, &seed, sizeof input);

		for (int i = 0; i < count; i++)
		{
			const int slot = i % valuesPerDigest;
			if (slot == 0)
			{
				sha256(input, sizeof input, digest);
				memcpy(input, digest, sizeof input);
			}

			unsigned int val;
			memcpy(&val, digest + slot * sizeof val, sizeof val);
			out[i] = val / (float)UINT_MAX;
		}
	}
}
