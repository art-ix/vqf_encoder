# Optional Long-frame PPC search

Enable `--ppc-search` or `Encoder::Config::ppc_search = true`.
The default remains the existing fixed PPC indices. This implementation uses
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

Subtract the decoded PPC contribution before Bark, gain and main VQ fitting.
Compare the resulting complete spectral reconstruction with a fixed-PPC trial
for every LSP/Bark strategy. Save the winning PPC fields together with the
other frame fields and histories. Temporal ranking, when enabled, evaluates
these final candidates and regenerates the selected PPC state before writing.

## Limits

This is a bounded search: every period is ranked, but only one period proposal
per channel is shape-quantized for each envelope. It is not an exhaustive joint
period/shape/main-VQ optimum. The existing fixed-PPC candidate remains eligible
from the same incoming history; this per-frame guard does not guarantee better
whole-track error after histories diverge. Temporal ranking retains its existing
5% spectral guard. Listening is needed before promoting the option to default.

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
