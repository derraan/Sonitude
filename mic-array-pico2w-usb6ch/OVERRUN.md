# OVERRUN LOG

## Phase 3 — Layer 1 SDK Upgrade

**Reason:** SDK v2.0.0 does not contain the `pico2_w` board definition (added in SDK v2.1.0).
The done condition requires `PICO_BOARD=pico2_w`. A shallow clone of SDK v2.1.0 was required.

**Action taken:** Shallow clone of pico-sdk v2.1.0 with required submodules to
`C:\Users\darre\pico\pico-sdk-2.1.0`. PICO_SDK_PATH updated to point to v2.1.0.

**Time impact:** ~5-10 minutes for SDK clone.

**Decision:** Conservative interpretation — use `pico2_w` per plan, not `pico2`.
The application does not use CYW43 wireless; board selection affects only SDK board
definitions and CMake toolchain selection, not application behaviour.
