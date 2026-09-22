#ifndef REVERSEDELAY
#define REVERSEDELAY

#include <cmath>

namespace CloudSeed
{
    // Classic reverse delay. Records `input` forward into a circular buffer and
    // reads it back in overlapping grains of length W, each grain sweeping a fixed
    // W-sample window backwards in time so the recording plays in reverse. A new
    // grain is launched every (W - Xf) samples; adjacent grains overlap by the
    // crossfade length Xf and are blended with a complementary equal-power
    // (sin/cos) window, so grain boundaries are click-free while a single grain
    // reads cleanly backwards the rest of the time (the recognizable backward swell).
    class ReverseDelay
    {
    private:
        static const int MaxGrains = 2;

        struct Grain
        {
            bool active;
            int  phase;  // 0 .. W-1 within this grain
            int  anchor; // buffer index of the newest sample at grain launch
        };

        float* buffer = nullptr;
        int size = 0;       // buffer length in samples
        int W = 2;          // grain window length in samples (reverse time)
        int Xf = 1;         // crossfade length in samples (Xf < W/2)
        int hop = 1;        // samples between grain launches (W - Xf)
        int writeIndex = 0;
        int hopCounter = 0; // counts down to the next grain launch
        Grain grains[MaxGrains];

        // Equal-power window: fades 0->1 over the first Xf samples, holds at 1,
        // then fades 1->0 over the final Xf samples of the grain.
        float Window(int phase) const
        {
            if (phase < Xf)
                return std::sin(1.5707963267948966f * (float)phase / (float)Xf);
            if (phase >= W - Xf)
                return std::sin(1.5707963267948966f * (float)(W - phase) / (float)Xf);
            return 1.0f;
        }

    public:
        // buf: caller-owned storage of `bufferSamples` floats. grainSamples is the
        // reverse-window length; clamped to be >= 2 and <= bufferSamples/2 so the
        // backward read window never overlaps the region being overwritten.
        void Init(float* buf, int bufferSamples, int grainSamples)
        {
            buffer = buf;
            size = bufferSamples;
            int g = grainSamples;
            if (g > bufferSamples / 2) g = bufferSamples / 2;
            if (g < 2) g = 2;
            W = g;
            Xf = W / 8;
            if (Xf < 1) Xf = 1;
            if (Xf > W / 2) Xf = W / 2;
            hop = W - Xf;
            if (hop < 1) hop = 1;
            ClearBuffers();
        }

        void ClearBuffers()
        {
            if (buffer != nullptr)
                for (int i = 0; i < size; i++) buffer[i] = 0.0f;
            writeIndex = 0;
            hopCounter = 0;
            for (int i = 0; i < MaxGrains; i++)
            {
                grains[i].active = false;
                grains[i].phase = 0;
                grains[i].anchor = 0;
            }
        }

        void Process(const float* input, float* output, int sampleCount)
        {
            for (int i = 0; i < sampleCount; i++)
            {
                buffer[writeIndex] = input[i];

                // Launch a new grain anchored at the newest sample.
                if (hopCounter <= 0)
                {
                    int slot = -1;
                    for (int k = 0; k < MaxGrains; k++)
                        if (!grains[k].active) { slot = k; break; }
                    if (slot < 0)
                    {
                        // No free slot (should not happen while hop >= W/2); reuse
                        // the oldest grain rather than dropping the launch.
                        slot = 0;
                        for (int k = 1; k < MaxGrains; k++)
                            if (grains[k].phase > grains[slot].phase) slot = k;
                    }
                    grains[slot].active = true;
                    grains[slot].phase = 0;
                    grains[slot].anchor = writeIndex;
                    hopCounter += hop;
                }

                float sum = 0.0f;
                for (int k = 0; k < MaxGrains; k++)
                {
                    if (!grains[k].active) continue;
                    int r = grains[k].anchor - grains[k].phase;
                    if (r < 0) r += size;
                    sum += buffer[r] * Window(grains[k].phase);
                    grains[k].phase++;
                    if (grains[k].phase >= W) grains[k].active = false;
                }
                output[i] = sum;

                hopCounter--;
                writeIndex++;
                if (writeIndex >= size) writeIndex -= size;
            }
        }
    };
}

#endif
