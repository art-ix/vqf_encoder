# Quality optimization validation cycle

User-requested cadence: a few targeted checks per stage; the complete test
suite after every three completed stages. Start this cycle after `72a5a15`.

| Stage | Work | Full regression |
| --- | --- | --- |
| 1 | Independent reverse-codebook VQ seeds | Deferred |
| 2 | Pending | Deferred |
| 3 | Pending | Required before completing stage 3 |

Do not count an abandoned experiment as a completed stage. Record subsequent
stages here so the deferred validation is not forgotten across sessions.
Intermediate commits use `[skip ci]` to defer the push-triggered full Windows
workflow as well. The stage-3 commit must omit that marker and run the full
local suite (`make test` and `python tools/test_vq_options.py bin/vqf_encode`),
plus the normal Windows CI workflow. This does not waive required checks for
merging or releasing. Keep private audio and measurements outside Git.
