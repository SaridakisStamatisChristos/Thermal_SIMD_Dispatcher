# Runbook: Trampoline Measurement Mismatch

## Scope

This runbook applies when an embedding application calls
`tsd_attestation_expect_active_hash` and receives a mismatch. The repository
does not ship an attestation daemon, alert rule, signed measurement bundle or
remote nonce service.

The measured value is the SHA-256 digest of the currently selected immutable
trampoline slot. A mismatch can mean the expected value is for another width or
release, the caller compared at the wrong point in the selection lifecycle, or
process memory/integration state is corrupted.

## Immediate actions

1. Stop sending work through the affected dispatcher instance and retain its
   logs, executable digest, exact commit/version and the expected/actual slot
   digests.
2. Determine the active SIMD width at the same synchronized point used for the
   comparison. Expected hashes are width- and build-specific.
3. Run the standalone startup diagnostic from the same installed artifact:

   ```bash
   /usr/local/bin/thermal_simd --sandbox-only --metrics-port=0
   ```

4. Do not treat a restart as proof of integrity. If provenance is uncertain,
   replace the executable from a trusted release source first.

## Investigation

- Compare `sha256sum` of the installed executable and controller coefficient
  file against externally retained release provenance.
- Confirm the embedding application uses the public attestation API while the
  dispatcher object/runtime lifetime remains valid.
- Confirm it does not reuse an expected digest from a different compiler,
  release or SIMD width.
- Review logs for `Trampoline hash=...`, failed immutable mapping validation or
  rejected width-selection messages.
- Reproduce the local mismatch test:

  ```bash
  cmake -S . -B build -DBUILD_TESTING=ON
  cmake --build build --target test_trampoline_security
  ctest --test-dir build --output-on-failure -R '^trampoline_security$'
  ```

## Recovery

Redeploy a verified artifact and rerun the startup diagnostic plus the
embedding application's expected-hash check. Escalate as a potential process
integrity incident if a build- and width-correct digest still differs. Any
signature verification, remote attestation or image admission policy belongs
to the deployment platform and should be investigated there separately.
