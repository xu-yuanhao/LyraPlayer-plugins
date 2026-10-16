# Transition Stems (experimental)

Independently installable optional audio analysis plugin for official FFmpeg 1.3.0 or newer. Native ABI 3, typed worker interface 24. The player core never links this module.

Uses the actual MIT Spleeter 2-stem vocal U-Net, exported by sherpa-onnx and converted through pinned pnnx 20260526 to ncnn 20260526. Accompaniment is the sample-aligned residual of the original stereo audio minus predicted vocals. This is vocals/accompaniment separation, not a drums/bass four-stem model.

Model STFT is 44100 Hz, 4096 FFT / 1024 hop, 64 time frames. Process input is 68608 stereo frames; the host retains the central 32768 frames with 16384 context. A 4096-frame estimate overlap is blended by the host. The exported upstream metadata says 41000; it is not used as a playback rate.

Run `python3 prepare.py` in an environment with `onnx` and `pnnx==20260526`, then `ANDROID_HOME=... ./build.sh`. Downloads and model binaries are ignored. prepare.py pins source revisions and archive/model hashes. The FP16 converted weights are 19660432 bytes; they are outside the main APK.

Worker calls are synchronous and serialized. A small facade owns a socket-connected native helper; only the helper links ncnn. The helper starts through the Android system linker with the same app UID, constructs its network after exec, uses one low-priority thread and exits when its parent dies. No inference or worker IPC on the audio output path. A failed, killed or unsupported worker returns an explicit failure and is reaped; it cannot fabricate a vocal curve. Private module snapshots reference one immutable cached weight file rather than copying weights on each launch. No hard-link permission is required.

The process launch uses only descriptor operations and exec after fork; no model allocation or JNI in the forked ART heap. On systems that reject helper startup, playback retains the existing non-neural fallback. Full-track preparation costs CPU and model memory once; persistent cache is bounded by the player configuration. Missing models or failed inference return unsupported, never fabricated vocal detection.

Sources: https://github.com/deezer/spleeter ; https://github.com/k2-fsa/sherpa-onnx ; https://github.com/Tencent/ncnn ; https://github.com/pnnx/pnnx . See licenses/.

Install this package separately from FFmpeg and restart Lyra Player. Without it FFmpeg retains decoding, beat/EQ transitions and non-neural fallback; no model is downloaded by the playback plugin. The independent plugin stages worker resources privately because Android cannot execute the installed external-files payload directly. The only model is vocals/accompaniment, not separate drum/bass stems.
