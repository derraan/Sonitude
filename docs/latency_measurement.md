# Latency Measurement Guide

Milestone 0 introduces the measurement method and reporting format; instrumentation tooling arrives in Milestone 8.

## Why software period math is not enough

Buffer/period arithmetic estimates only host-side buffering. True one-way latency also includes:

- USB transfer scheduling
- device internal buffering
- DAC/analogue stage delay
- optional vendor DSP (e.g., Super X-Fi mode)

Therefore, period-derived estimates are advisory only.

## Required measurement method (target)

1. Generate a repeatable impulse marker at microphone input.
2. Record analogue DAC output with independent measurement hardware.
3. Compute input-to-output delay distribution:
   - median
   - p95
   - p99
4. Repeat for each tested DAC mode:
   - plain DAC / bypass (if available)
   - Super X-Fi enabled (if available)

## Reporting requirements

Every latency report must include:

- capture/playback negotiated rates/formats/period/buffer
- ASRC status and observed ratio behavior
- test hardware and wiring
- sample count and percentile table
- statement separating estimated and measured latency

## Acceptance baseline target

Engineering target is 6-10 ms one-way subject to hardware behavior. Compliance is only valid after analogue-output measurements are recorded.
