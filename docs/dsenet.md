# DSENet training workspace

DSENet is an experimental directional extraction path for the Sonitude 6-microphone array. This tree currently ships the **Python training/export workspace** used to produce float32 TensorFlow Lite weights. Live C++ runtime integration is documented separately when it is on the branch you are using.

## Colab training

- End-user instructions: [dsenet_colab_training_guide.md](dsenet_colab_training_guide.md)
- Notebook: `python/dsenet/notebooks/dsenet_colab_training.ipynb`
- Package: `python/dsenet/`

The Colab workflow:

1. Generates synthetic 6-mic mixtures from LibriSpeech (paper-style priors).
2. Optionally fits Sound Bubble `metadata.json` room/source distributions and generates a second synthetic subset (Sound Bubble packaged `mixture.wav` files are **not** used as supervised targets).
3. Trains with SI-SDR, estimates scale `eta`, exports `dsenet_streaming_step.tflite` plus `dsenet_streaming_step.dsenet.json`.
4. Runs a Keras-vs-TFLite streaming parity check with a mid-stream GRU state reset.

This is a Sonitude 6-mic, 44.1 kHz adaptation (`L = Lp = Lf = 88`), not a paper-faithful 3-mic 16 kHz reproduction.
