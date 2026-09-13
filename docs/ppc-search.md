# Long-frame PPC search

PPC search is enabled by default on `encoder-quality`.
Use `--no-ppc-search` or `Encoder::Config::ppc_search = false` to restore
the fixed-index behavior. `--ppc-search` explicitly enables the search.
This implementation uses
only the period, gain and shape fields already reserved in Long frames;
it adds no bitstream fields, bitrate or lookahead. Short and Medium frames
are unaffected by this search.

## Candidate construction

For each mode, cache the spectral positions produced by every representable
PPC period using the decoder's exact integer mapping, including its special
22 kHz / 32 kbps per channel width rule. Rank periods by weighted original
spectral energy at those positions and propose the highest-energy period
independently for each encoded channel (mid/side for stereo).

Normalize the shape target by a representable initial PPC gain. Reuse the
weighted forward/reverse main-VQ search with the PPC codebooks and permutation.
Quantize both channels jointly because PPC permutations can cross channels.
Fit each channel's gain to the decoded shape using the actual transmitted
gain values, then repeat shape/gain fitting once and retain the better fit.
If the fitted gain indices are unchanged in every channel, skip the repeated
pass: its joint shape-VQ inputs and deterministic result would be identical.
The check must include all channels because the PPC permutation can mix them.

Subtract the decoded PPC contribution before Bark, gain and main VQ fitting.
Compare the resulting complete spectral reconstruction with a fixed-PPC trial
for every LSP/Bark strategy. Save the winning PPC fields together with the
other frame fields and histories. Temporal ranking, when enabled, evaluates
these final candidates and regenerates the selected PPC state before writing.

After Bark/VQ keep-best, Long frames refit PPC to the leftover
`original/env - bark * vq`. The three highest leftover-energy periods per
channel (including the current period) are shape-quantized with locked
periods. The previous PPC triple is restored unless weighted MDCT error falls.
If the triple does change, one extra main-VQ pass and gain fit run on the
new residual; the previous vectors are restored unless that error falls too.

## Limits

This is a bounded search: every period is ranked, but only one period proposal
per channel is shape-quantized for each envelope. It is not an exhaustive joint
period/shape/main-VQ optimum. The existing fixed-PPC candidate remains eligible
from the same incoming history; this per-frame guard does not guarantee better
whole-track error after histories diverge. Temporal ranking retains its existing
5% spectral guard. Listening validation is still needed across a broader range of material.

## Regression coverage

`--test-codec-ppc` exercises all supported sample-rate/bitrate modes in mono and
stereo, irregular feeding, flush, decoded duration and finite samples, existing
quality/gain gates, silence and actual nonzero transmitted PPC parameters.
The regular suite also checks that fixed-PPC trials transmit zero period/gain
indices when the option is off. The decoder implementation is unchanged.

`--test-codec-ppc-time` combines PPC, adaptive blocks and temporal ranking on
synthetic attacks at three positions, checking chunking, transitions, flush
and the existing pre-echo/gain gates. Limited external-decoder comparisons
also check unchanged output bytes when PPC is disabled and unchanged file
sizes when enabled. No listening validation has been performed.

## Follow-up experiments

Two more expensive proposals were evaluated and excluded from the retained
encoder: refitting the period against a decoded shape, and exhaustive signed
PPC codebook-pair search alongside the existing beam candidate. Their limited
additional reconstruction gains and mixed spectral results did not justify
the extra work. Neither is enabled by `--ppc-search`.

The retained implementation instead avoids redundant work without narrowing
its candidate set. See [encoder search performance](encoder-search-performance.md).

Two additional performance directions were inspected. Precomputing per-bin
weighted energy before ranking periods preserved output in limited checks but
showed no consistent end-to-end benefit, so it was not retained. Exact duplicate
period maps were counted; their small share in the 44.1 kHz modes did not justify
adding a second map index. The retained convergence check removes a whole
redundant shape/gain pass without changing candidate selection.
