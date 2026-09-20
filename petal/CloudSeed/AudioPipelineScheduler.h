#pragma once
#include "util/ringbuffer.h"
#include <cstdint>
#include <cstddef>

// Decouples SDRAM-heavy per-sample audio processing from a hard-real-time
// audio ISR. The ISR only pushes fresh input / pulls delayed output through
// two ring buffers (PushInput/PullOutput, non-blocking, ISR-safe); a
// lower-priority context (e.g. a bare-metal main loop) drains input and
// produces output in small ChunkSize-sample slices, each gated by a
// jittered, block-relative deadline computed from an externally supplied
// clock (Process's nowUs argument). This spreads the processing load evenly
// across each BlockSize-sample period instead of bursting it all at the top,
// at the cost of a fixed LatencyBlocks*BlockSize samples of added pipeline
// latency.
template <size_t BlockSize, size_t ChunkSize, size_t LatencyBlocks>
class AudioPipelineScheduler
{
  public:
    static_assert(BlockSize % ChunkSize == 0,
                  "BlockSize must be an exact multiple of ChunkSize");
    static constexpr size_t ChunksPerBlock = BlockSize / ChunkSize;
    // +2 blocks of margin: LatencyBlocks primed blocks, plus one full block
    // the main loop may accumulate before the ISR drains it, plus the one
    // slot daisy::RingBuffer reserves to disambiguate full vs. empty.
    static constexpr size_t RingCapacity = (LatencyBlocks + 2) * BlockSize;

    /** (Re-)initializes the pipeline: clears both ring buffers, pre-fills
     *  the output ring with LatencyBlocks of silence to establish the fixed
     *  round-trip latency, and resets the chunk-pacing schedule to start at
     *  nowUs. Call once at startup and again on every preset change (so no
     *  stale pre-change audio lingers in the buffers). */
    void Init(uint32_t nowUs, uint32_t blockPeriodUs, uint32_t jitterRangeUs)
    {
        inputRing_.Init();
        outputRing_.Init();
        float silence[BlockSize] = {};
        for(size_t i = 0; i < LatencyBlocks; i++)
            outputRing_.Overwrite(silence, BlockSize);
        blockPeriodUs_ = blockPeriodUs;
        jitterRangeUs_ = jitterRangeUs;
        for(size_t c = 0; c < ChunksPerBlock; c++)
            chunkOffsetUs_[c]
                = (uint32_t)((uint64_t)c * blockPeriodUs_ / ChunksPerBlock);
        blockBaseUs_ = nowUs;
        chunkIndex_  = 0;
    }

    /** ISR side: pushes a fresh block of BlockSize input samples.
     *  Returns false if the input ring had no room (main loop lagging on the
     *  input side); the write still happens, overwriting the oldest unread
     *  samples, so the ISR never blocks. */
    bool PushInput(const float* block)
    {
        bool ok = inputRing_.writable() >= BlockSize;
        inputRing_.Overwrite(block, BlockSize);
        return ok;
    }

    /** ISR side: pulls the next delayed BlockSize output samples.
     *  Returns false (and fills block with silence) if the output ring
     *  underran (main loop fell behind schedule). */
    bool PullOutput(float* block)
    {
        if(outputRing_.readable() >= BlockSize)
        {
            outputRing_.ImmediateRead(block, BlockSize);
            return true;
        }
        for(size_t i = 0; i < BlockSize; i++)
            block[i] = 0.0f;
        return false;
    }

    /** Main-loop side: advances at most one ChunkSize-sample slice if enough
     *  input is buffered AND this slice's (jittered) deadline has arrived.
     *  processBlock(float* in, float* out, size_t n) must behave like
     *  CloudSeed::ReverbController::Process. Returns true if a slice ran
     *  (the caller should still run its own idle-filler work regardless of
     *  the return value, every main-loop iteration). */
    template <typename ProcessFn>
    bool Process(uint32_t nowUs, ProcessFn&& processBlock)
    {
        if(inputRing_.readable() < ChunkSize)
            return false;

        uint32_t deadline = blockBaseUs_ + chunkOffsetUs_[chunkIndex_]
                             + (jitterRangeUs_ ? NextJitter() : 0);
        if((int32_t)(nowUs - deadline) < 0)
            return false;

        float inChunk[ChunkSize];
        float outChunk[ChunkSize];
        inputRing_.ImmediateRead(inChunk, ChunkSize);
        processBlock(inChunk, outChunk, ChunkSize);
        outputRing_.Overwrite(outChunk, ChunkSize);

        chunkIndex_++;
        if(chunkIndex_ == ChunksPerBlock)
        {
            chunkIndex_ = 0;
            blockBaseUs_ += blockPeriodUs_;
        }
        return true;
    }

  private:
    // xorshift32: fast, deterministic-per-seed PRNG used only to dither
    // chunk timing so any residual periodic current ripple spreads into
    // broadband noise instead of concentrating at one tone. Returns a
    // two's-complement bit pattern representing a signed offset in
    // [-jitterRangeUs_/2, +jitterRangeUs_/2); adding it to a uint32_t
    // deadline via unsigned wraparound arithmetic is well-defined and
    // equivalent to signed addition.
    uint32_t NextJitter()
    {
        xorshiftState_ ^= xorshiftState_ << 13;
        xorshiftState_ ^= xorshiftState_ >> 17;
        xorshiftState_ ^= xorshiftState_ << 5;
        int32_t signedJitter = (int32_t)(xorshiftState_ % jitterRangeUs_)
                                - (int32_t)(jitterRangeUs_ / 2);
        return (uint32_t)signedJitter;
    }

    daisy::RingBuffer<float, RingCapacity> inputRing_;
    daisy::RingBuffer<float, RingCapacity> outputRing_;
    uint32_t chunkOffsetUs_[ChunksPerBlock] = {};
    uint32_t blockBaseUs_   = 0;
    uint32_t blockPeriodUs_ = 0;
    uint32_t jitterRangeUs_ = 0;
    size_t   chunkIndex_    = 0;
    // Fixed nonzero seed: xorshift32 must never be seeded with 0, and this
    // only needs to decorrelate timing, not provide unpredictability.
    uint32_t xorshiftState_ = 0x9E3779B9u;
};
