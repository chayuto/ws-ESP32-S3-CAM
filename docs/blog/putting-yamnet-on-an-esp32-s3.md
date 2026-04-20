# Putting YAMNet on a $12 Microcontroller

*Notes from building an always-on audio classifier on an ESP32-S3 — and why the model turned out to be the easy part.*

---

The microcontroller on my desk costs about twelve dollars, has eight megabytes of PSRAM, a dual-core 240 MHz Xtensa CPU with a small vector unit, and a pair of MEMS microphones soldered to a four-channel audio ADC. It is supposed to be a camera board. I ignored the camera.

The question I wanted to answer was narrow: can a pretrained audio classifier — a real one, not a toy — run continuously on this class of hardware and do something useful? Not "can it be flashed." Can it be left plugged in for eight hours, ingest real microphone input, and produce predictions that actually track what a human listener would hear?

This is a writeup of the first month of finding out. It is mostly a story about debugging, instrumentation, and the gap between "the model loads" and "the model works." The model itself was the shortest part of the project.

## The hardware premise

The board is the Waveshare ESP32-S3-CAM-GC2145. I use it as a discreet bedside listener: no screen, no camera, one red LED for local status, an SD card for long-form logging, and a small web UI served over Wi-Fi for live inspection. The only input path that matters is audio — a pair of mics into the ES7210 ADC, over I²S, at 16 kHz.

The ESP32-S3 is the first ESP chip where a model like YAMNet starts to make sense on-device. Three features matter:

- **PSRAM.** Eight megabytes of it, at 80 MHz over octal SPI. The model weights live here.
- **PIE vector extensions.** A narrow SIMD unit that ESP-NN kernels can exploit for INT8 conv/matmul on the LX7 cores. The difference between "usable" and "one inference every two seconds."
- **Dual-core.** Wi-Fi and lwIP get pinned to core 0. The audio + inference loop runs unpinned and never starves.

None of this is remarkable by laptop standards. But for a three-cell-Lithium-battery-sized device that costs less than lunch, it's the first plausible target for real audio ML.

## Why YAMNet

[YAMNet](https://www.tensorflow.org/hub/tutorials/yamnet) is Google's pretrained audio event classifier. It ingests 0.96-second log-mel patches at 16 kHz and emits probabilities over 521 AudioSet ontology classes — things like *speech*, *laughter*, *baby cry*, *siren*, *vacuum cleaner*. The full-precision model is a MobileNet-style CNN around 4 MB; after INT8 post-training quantisation it stays around 4 MB and runs comfortably from SPIFFS on the S3's 16 MB flash.

The attraction is leverage. Training a custom audio classifier from scratch means collecting data, labelling, training, quantising, validating — weeks of work before you know whether the hardware can keep up. A pretrained model lets you invert the question: assume the hardware is fine, find out what's actually hard about the end-to-end system.

A lot turned out to be hard about the end-to-end system.

## The pipeline

The inference loop is unexciting and that is the point:

```
mic → I²S DMA → 16 kHz PCM ring buffer
     → log-mel spectrogram (96 × 64, 0.48 s hop)
     → YAMNet INT8 (521 class logits)
     → sigmoid → per-class confidence
     → detector state machine → LED + SD log + web push
```

End-to-end latency is about 680 ms per patch, which works out to 1.46 inferences per second. Each inference touches 62 KB of activation RAM in PSRAM and hits the vector kernels on core 1. The detector state machine is a handful of lines: take the confidence for class 20 (*Baby cry, infant cry*), increment a streak counter if it's above threshold, fire an alert when the streak exceeds `CONSEC_FRAMES`.

That's the entire ML. The other forty files in the repo are everything around it: the ring buffer, the I²S driver shim, the web UI with server-sent events, the SD log rotator, the file API, the metrics exporter, the boot self-test, the NTP sync, the noise-floor tracker. An audio ML system on a microcontroller is roughly 5% model and 95% plumbing.

## The first overnight deployment

First real deployment ran for eight hours with the device in a quiet bedroom. The recording pipeline captured fourteen elevated-RMS audio events during the night. The detector fired zero times.

This was not a bug in the audio pipeline — the events were there in the logs, with RMS values well above ambient. It was a problem upstream, in the model itself. Specifically, in the INT8 calibration.

YAMNet's float weights had been quantised using a calibration dataset of synthetic signals — sine sweeps and noise tones — not real audio. The resulting INT8 activations had enough dynamic range to separate classes *within* the synthetic calibration distribution, but real bedroom audio landed in a tiny region of that distribution. All 521 class outputs compressed into a narrow band between 0.56 and 0.64. Speech, silence, baby cry, vacuum cleaner — every class looked roughly the same.

Pretrained, in the phrase "pretrained model," is doing less work than it appears. The weights are pretrained. The quantisation is yours. If your calibration set does not resemble your deployment distribution, you have not deployed YAMNet — you have deployed a 4 MB lookup table that reports 0.6 for everything.

Fixing this meant recalibrating with real captured audio from the target device, microphone gain, and room acoustics. That meant the device first had to record audio it could not yet classify. Edge ML is circular that way.

## Debug tooling before debugging

The second thing that becomes clear on this kind of project is that iteration cost is the real constraint, not CPU or memory.

A full flash-and-boot cycle on this board is about 20 seconds. Opening the serial monitor to read logs triggers a DTR reset, which costs another 5 seconds and loses whatever state the device was in. The SD card holds the interesting logs but physically removing it means powering the device down. None of these are individually expensive. All of them together mean you iterate five to ten times an hour if you're doing it by hand, and every iteration loses the context of the previous one.

Almost everything worth shipping on this project was a response to that iteration cost rather than a feature spec:

- A `/files/` HTTP API that exposes the SD filesystem over Wi-Fi, with `ls`, `get`, `tail`, and `stat` endpoints and path-traversal protection. You pull the day's log with `curl` instead of walking to the device.
- A `/metrics` endpoint that streams current inference confidence, noise floor, heap, overrun counters, and build ID. Anomalies surface in the poller instead of in tomorrow's log file.
- Per-frame logging of all 20 watched class confidences to JSONL — not just the one class driving the alert. This is the single most useful thing the system does, and none of the original design documents mention it.

The instrumentation was not the project. But the project does not exist without the instrumentation, because without it you cannot tell the difference between "the model is wrong" and "the preprocessing is wrong" and "the audio is just quiet."

## A bug that looked like it wasn't one

Partway through the project, the detector appeared to work — confidences moved, alerts fired, the web UI looked alive — but the numbers never quite landed where they should. Speech samples scored near baby-cry. Silence scored at 0.50. Everything was plausible and nothing was right.

The actual bug was in one of the managed components: a silent skip in the YAMNet dense-layer invocation meant the classifier head was effectively not being run. The system was emitting logits from a partial graph, sigmoid-squashed into a range that looked like predictions. It passed every "does it boot, does it respond, does it produce numbers" check I had.

The only way this surfaced was comparing on-device inference against the same TFLite model run offline on the same captured audio. When the offline model said 0.88 and the device said 0.52 on the same input, the device lost. The fix was small. The lesson is that on embedded ML, "it loads and produces numbers" is not close to "it works." You need an offline replay path that bit-exactly matches what the device is doing, and you need to diff them.

## The first real detection

The second overnight deployment captured one unambiguous event at 06:30 in the morning. The audio was clearly a cry. The device logged the event, saved the WAV, and updated the metrics — but did not fire an alert.

The peak confidence across the patch was 0.622. The alert threshold was 0.70. Below the line, by seven and a half hundredths.

Three observations from the per-frame log made this interesting rather than demoralising:

1. **The miss was not close to random.** Three separate cry-family classes (*baby cry*, *adult cry*, *whimper*) all hit 0.622 on the same frame. That specific value is a dequantisation bucket — an exact output the INT8 head can produce. The classifier was confident; the threshold was above the bucket.
2. **A fusion rule would have fired.** A simple `max(cry_baby, cry_adult, whimper, wail_moan)` over the same frame would have been 0.622, but the shape of the distribution — multiple cry-family classes peaking together versus one hot class and chatter — is distinguishable from false-positive noise.
3. **The 20-class log made the analysis possible.** Without it, the diagnosis would have been "model scored 0.62, threshold is 0.70, lower the threshold." That would have been wrong, because the frame immediately before also scored 0.6 on non-cry classes due to preprocessing noise. Dropping the threshold catches that too.

The tempting fix is to drop the threshold from 0.70 to 0.60 and ship. The correct response is that n=1 is not a tuning set. One detected cry, one missed detection, is not data — it is an anecdote. A threshold change justified by one sample will fail the next time ambient noise looks like a cry, and I will not know why, because the threshold was not principled.

## What the project is actually teaching me

A few things I did not know at the start and am reasonably sure of now:

- **Pretrained models on edge hardware are a calibration problem, not a deployment problem.** The weights are free. The quantisation is yours. The gap between a model that loads and a model that discriminates is an hour of setup and a week of real audio.
- **Instrument before you need to.** The per-frame 20-class log cost almost nothing to build and cost everything to not have. If the system emits a single number per frame, you will end up guessing. If it emits the full classifier output, you can reconstruct the reasoning afterwards.
- **The hardest part of edge ML is the feedback loop.** Offline replay that matches on-device numerics, logs that survive reboots, metrics that update without touching the device, a way to pull audio without opening the case — none of these are features, all of them are prerequisites to learning anything.
- **n=1 is not a dataset.** This sounds obvious until you have stayed up until 2 AM chasing a threshold that seemed right for one sample.

## What comes next

Not a threshold change. A dataset.

The device has been logging watched-class confidences, RMS traces, and raw audio snippets for the elevated-RMS events it does capture. Each real-world cry is a few seconds of labelled audio. The target is around 500 positive examples and a similar count of hard negatives (speech, cough, TV, appliances) before attempting a binary head retrain on top of the frozen YAMNet embedding. That's the Tier 3.2 approach in the internal plan — keep the pretrained feature extractor, train only the last layer on real data from the actual deployment environment.

At the time of writing there are about 295 labelled positives from three weeks of overnight runs. The arithmetic is unromantic. Another three to four weeks of deployment, assuming collection rate holds, and the head retrain becomes viable.

The detector will stay at its current 0.70 threshold in the meantime. The missed cry will be missed. The interesting work is not in tuning — it is in the data accretion, the instrumentation that makes retrospective analysis cheap, and the discipline to not ship a threshold change justified by a single sample.

Edge ML is a loop: deploy, log everything, fail honestly, collect, retrain, redeploy. The model is the easiest part. The loop is the project.

---

*Source, research notes, and build instructions: [github.com/chayuto/ws-ESP32-S3-CAM](https://github.com/chayuto/ws-ESP32-S3-CAM).*
