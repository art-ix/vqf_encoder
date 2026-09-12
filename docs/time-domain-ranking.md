# Experimental time-domain candidate ranking

Enable `--temporal-search` or `Encoder::Config::temporal_search = true`.
Default encoding is unchanged. This ranks existing full-frame LSP/Bark
candidates; it does not change main-VQ candidate generation or bit allocation.
When both LSP and Bark searches are disabled there is only one candidate.

## Objective and state

The encoder retains the original two PCM hops before replacing input overlap.
For each ear, approximately 3 ms slices provide energy estimates. A rise
above eight times the preceding slice's energy marks the two preceding slices
with weight four; other samples retain weight one. A relative numerical floor
avoids division by silence. This is a heuristic, not an audibility threshold.

For each candidate, reconstruct its main-VQ spectrum with the final decoded
gains and synthesis envelopes, including Long-frame PPC. Subtract the original
MDCT, apply the decoder IMDCT and shared window synthesis, and combine with
the committed error overlap from the preceding frame. Reconstruct L/R error
before applying ear-specific weights, preserving stereo cross terms.

The next hop is estimated from the candidate's tail with zero quantization
error in the next frame, whose vectors are not yet known. This approximation
is used only in scoring. No provisional state is committed. Error overlap,
LSP and Bark histories belong only to the selected candidate.

Select the lowest temporal error among candidates whose spectral objective
is at most 1.05 times the best existing frame candidate's objective. With
psychoacoustic weighting enabled, that guard uses the existing weighted
spectral objective. The guard is per-frame with shared prior histories; it
is not a whole-track SNR or subjective-quality guarantee.

The chosen candidate is regenerated once to commit its exact frame state.
This adds transforms and one candidate evaluation per frame. It uses bounded
memory and adds no PCM lookahead beyond the selected block mode. The decoder
and VQF format do not change.

## Validation and limits

`--test-codec-time` exercises adaptive 80/96 kbps with early, central and late
side attacks, transmitted windows, chunking, flush, short/empty input, finite
PCM, pre-attack leakage and attack gain. Existing central-attack and late
leakage gates are retained. Targeted FFmpeg comparisons cover Long and
adaptive encoding. Private audio and its measurements stay outside Git.

A synthetic Long-frame onset showed a modest reduction in pre-attack error
at the expense of error during the attack. Adaptive synthetic output did not
improve in the tested case. These results justify an experimental comparison
option, not a new default. The candidate pool is limited: temporal ranking
cannot find codevectors absent from the current spectral VQ search. Further
work should examine time-aware VQ candidates and independent listening before
broadening the spectral guard or enabling this option by default.


## Rejected gain/VQ intermediate-pool experiment

The temporal pool was extended to include distinct intermediate main-VQ and
gain/sub-gain states already visited during each envelope trial. The final
spectral winner remained available. Candidates were pruned against the
unchanged 5% frame spectral guard, and replay restored the exact saved
quantization state with the chosen envelope's histories.

Targeted adaptive codec checks passed. The extra pool did not produce a
useful overall improvement; in the synthetic Long-frame onset check both
pre-attack and attack error increased slightly. The extension was reverted.
No extra pool option is exposed. Private measurements remain outside Git.

This also illustrates the limit of the current objective: a better local
score with fixed prior history and assumed zero future error is not a
monotonic improvement of the final decoded signal. Reconsider the objective
and candidate diversity before expanding this pool again. A separate next
priority is full PPC search, currently unused for harmonic structure, rather
than further breadth changes to these closely related gain/VQ candidates.

## PPC integration

Optional bounded PPC search is now available as `--ppc-search`. Its final
LSP/Bark/PPC candidates participate in temporal ranking with the same spectral
guard. See [PPC search](ppc-search.md); this does not reinstate the rejected
intermediate gain/VQ candidate pool described above.
