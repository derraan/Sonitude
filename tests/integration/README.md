# Integration Tests

C++ CTest remains `ctest --test-dir build --output-on-failure`.

The PySide6 algorithm test bench (`testbench/`) has its own pytest suite,
including real `sonitude_wav_replay` / `sonitude_stream_process` integration
when those binaries are built. See `testbench/README.md`. CI fails those
tests if `SONITUDE_REQUIRE_CPP=1` and the tools are missing.

Still planned here (host/hardware gates, not the Python GUI):

- ODAS mock trajectory and state-machine transition checks
- long-duration ASRC drift simulation
- openMHA golden-render evidence (`openmha_m4_validation.md`)
