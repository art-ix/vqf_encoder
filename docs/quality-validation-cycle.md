# Quality optimization validation cycle

User-requested cadence: a few targeted checks per stage; the complete test
suite after every three completed stages. Start this cycle after `72a5a15`.

| Stage | Work | Full regression |
| --- | --- | --- |
| 1 | Independent reverse-codebook VQ seeds | Included in stage-3 validation |
| 2 | Third bounded gain/VQ iteration with candidate retention | Included in stage-3 validation |
| 3 | Eight reverse VQ seeds with four-seed winner retention | Full validation checkpoint |

Do not count an abandoned experiment as a completed stage. Record subsequent
stages here so the deferred validation is not forgotten across sessions.
Intermediate commits use `[skip ci]` to defer the push-triggered full Windows
workflow as well. The stage-3 commit must omit that marker and run the full
local suite (`make test` and `python tools/test_vq_options.py bin/vqf_encode`),
plus the normal Windows CI workflow. This does not waive required checks for
merging or releasing. Keep private audio and measurements outside Git.


Stage-3 local validation: `make test` and
`python tools/test_vq_options.py bin/vqf_encode` passed. This includes all 18
mode/channel combinations in each of the five codec suites, resampling,
MDCT/window/masking checks, chunking/flush, CLI options and roundtrip alignment.
The Windows workflow must also succeed on the stage-3 code commit. Its status
is recorded by GitHub Actions on that commit; once successful, this cycle is
complete and the next cycle starts at stage 1/3. No recording-derived results
are stored in this record.
