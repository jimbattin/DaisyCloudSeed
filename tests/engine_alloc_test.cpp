// The audio callback applies knob and toggle targets with SetParameter() and then
// runs Process(), so neither may allocate: heap allocation time is unbounded on
// the pedal, and the SDRAM pool (src/sdram_pool.cpp) never frees, so a pool
// allocation after boot would leak until it hit FatalErrorLoop(). Construction
// and boot are allowed to allocate; only the running engine is checked.
#include <new>
#include <stdio.h>
#include <stdlib.h>

#include "check.h"
#include "CloudSeed/AudioLib/ValueTables.h"
#include "CloudSeed/FastSin.h"
#include "CloudSeed/ParameterNames.h"
#include "CloudSeed/ReverbController.h"

static bool gCounting = false;
static int  gHeapAllocs = 0;
static int  gPoolAllocs = 0;

// Counting replacements for the global allocation functions. GCC's
// -Wmismatched-new-delete misreads free() on memory from this replaced operator
// new once the two are inlined together; the pairing is correct.
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void* operator new(size_t size)
{
    if (gCounting)
        gHeapAllocs++;
    void* p = malloc(size ? size : 1);
    if (!p)
        throw std::bad_alloc();
    return p;
}
void* operator new[](size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

// The firmware's SDRAM bump allocator, backed by malloc on the host.
void* custom_pool_allocate(size_t size)
{
    if (gCounting)
        gPoolAllocs++;
    return malloc(size);
}

int main()
{
    AudioLib::ValueTables::Init();
    CloudSeed::FastSin::Init();
    CloudSeed::ReverbController* reverb = new CloudSeed::ReverbController(48000);

    // A mid-range preset with every stage enabled and all delay lines running.
    float preset[(int)Parameter::Count];
    for (float& value : preset)
        value = 0.5f;
    reverb->LoadPreset(preset);
    reverb->SetParameter(Parameter::LineCount, (float)CloudSeed::TotalLineCount);

    const int kBlock = 48;
    float input[kBlock] = {1.0f};
    float output[kBlock];

    // Every parameter swept bottom to top and back to mid-range, one write and
    // one block at a time, as a turning knob drives it.
    const float kSweep[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 0.5f};
    for (int p = 0; p < (int)Parameter::Count; p++)
    {
        gHeapAllocs = 0;
        gPoolAllocs = 0;
        gCounting = true;
        for (float value : kSweep)
        {
            reverb->SetParameter((Parameter)p, value);
            reverb->Process(input, output, kBlock);
        }
        gCounting = false;

        CHECK(gHeapAllocs == 0 && gPoolAllocs == 0);
        if (gHeapAllocs || gPoolAllocs)
            fprintf(stderr, "  %s: %d heap and %d pool allocations\n",
                    kParameterNames[p], gHeapAllocs, gPoolAllocs);
    }

    return CheckSummary("engine_alloc_test");
}
