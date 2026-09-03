# External Release Artifacts

No attestation bundle is generated or consumed by the current build. This
directory intentionally contains documentation only.

The runtime's attestation API is local: it computes a SHA-256 digest of the
currently selected immutable trampoline slot and lets an embedding application
compare that digest with an expected value. It does not load a measurement
manifest, verify a signature, contact an attestation service or manage nonces.
See [`../../include/patcher/attestation.h`](../../include/patcher/attestation.h).

Deployments may keep signed manifests, SBOMs, image signatures or provenance in
their own release system, but those are external controls and should not be
described as enforced by this repository unless integration code is added and
tested.
