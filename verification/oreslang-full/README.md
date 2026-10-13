# Full PR #244 source-tree validation

This nested `source/` is the **exact Git tree**
`83495708b11c6b54a88ca50596be9b3dd6a97de1` of upstream LLVM PR #244 head
`442fc9d99376dbfe58b970e5f41c959715285f9d`.

The Git tree hash (not a file list or approximate export) is verified by the
funded Debug/Release full-tree CI workflow before any build or test step.
The 366 upstream blobs are all present byte-for-byte. Tests are not filtered.

This is a validation copy only; the original PR remains draft.
