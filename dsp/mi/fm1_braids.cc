/* SPDX-License-Identifier: MIT */
#include "fm1_braids.h"

namespace fm1 {

void BraidsVoice::Init()
{
    osc_.Init();
    for (size_t i = 0; i < kBraidsChunkMax; i++) {
        sync_[i] = 0;
        buf_[i]  = 0;
    }
    avail_ = 0;
    pos_   = 0;
    next_  = kBraidsChunkA;
}

void BraidsVoice::Render(int16_t *out, size_t n)
{
    size_t done = 0;
    while (done < n) {
        if (avail_ == 0) {
            /* An EVEN size, never more than temp_buffer_'s 24: RenderStruckBell
             * writes two samples an iteration and underflows on an odd size.
             * Alternating 12 and 10 averages 11, which keeps Braids'
             * per-block processing at its native wall-clock rate. See the
             * note on kBraidsChunkA in fm1_braids.h. */
            osc_.Render(sync_, buf_, next_);
            avail_ = next_;
            pos_   = 0;
            next_  = (next_ == kBraidsChunkA) ? kBraidsChunkB : kBraidsChunkA;
        }
        size_t take = n - done;
        if (take > avail_) take = avail_;
        for (size_t i = 0; i < take; i++) out[done + i] = buf_[pos_ + i];
        pos_   += take;
        avail_ -= take;
        done   += take;
    }
}

}  // namespace fm1
