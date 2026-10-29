# Image build provenance

The single-cell setup pins these inspected Linux AMD64 radio artifacts:

- `ghinwa555/oai-gnb-e2-chan@sha256:1bc3c341b498fdc2096721bd0b86f5bdd3ca985a930617163c611db5a9bc5501`
- `ghinwa555/oai-nr-ue-chan@sha256:f00af9714f0e953c79d8e4def6b71d15d9af2c504bca1e0a8db2edbb59d6fb60`

Both inspected executables report revision `70508ebaf5`; their RFsim libraries
match byte for byte. These observations identify the candidate artifacts and
self-reported version, not their complete patch sets or reproducible build recipes.
Original Dockerfiles, source modifications and complete build commands remain
unrecorded. Do not infer the running source solely from the core checkout.

The core setup pins OAI `70508ebaf52f2aae420566d380c6537f2efb9f0c`
and FlexRIC `ef6d722f22191eea74089966983da1f5ec1fedd4` for the host RIC build.
These revisions do not establish the provenance of either radio image.
`Dockerfile.flexric` is a reference for a containerised RIC, not a radio build.

Before replacing images, collect the running container image IDs and inspect
those exact IDs, rather than a mutable tag. Use `bin/collect-cell-evidence.py`.
Missing revision labels remain an unresolved provenance gap. A digest identifies
an artifact; it does not prove its source or RLC buffer configuration.

For future radio builds, archive the Dockerfiles, full source and submodule
revisions, patches, build flags, compiler versions, and resulting registry digests.
Include `org.opencontainers.image.revision` and `org.opencontainers.image.source`
labels. Pin verified image digests only after testing attachment, E2 indications,
and per-UE channel read-back. Never invent a revision or digest for existing images.
