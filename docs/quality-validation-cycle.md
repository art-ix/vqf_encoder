# Quality validation policy

The former three-stage validation cadence has ended at the user's request.
Do not start new cycles or defer CI with `[skip ci]` for that reason.

Use a few targeted checks appropriate to each change. Broaden validation only
for a concrete risk or a required gate. Normal branch CI remains enabled.
Keep private audio, excerpts and recording-derived measurements outside Git.

The previous VQ refinement work completed local regression and Windows CI
at commit `88f6e83`.
