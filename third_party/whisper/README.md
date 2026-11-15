# Bundled offline voice input

The source archive is whisper.cpp v1.7.6, commit `a8d002cfd879315632a579e73f0148d06959de36`. The multilingual model was produced with that release's `quantize ggml-tiny.bin ggml-tiny-q4_0.bin q4_0` from the [upstream GGML tiny weights](https://huggingface.co/ggerganov/whisper.cpp/blob/main/ggml-tiny.bin). The source model SHA-256 is `be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21`. The build inputs and licenses are pinned in the parent manifest.

SerialCtl statically links the CPU engine, embeds the quantized model in the executable and launches its own transcriber as a cancellable child process. The executable fits the existing 32 MiB per-file updater limit. The portable package uses the existing third-party notices file for both MIT licenses.
