# LocalVQE — PyTorch Reference

This directory contains the PyTorch definition of LocalVQE. It is provided
for research, verification, and downstream fine-tuning. For production /
end-user inference, use the GGML build in [`../ggml/`](../ggml/).

## Install

```bash
pip install -r requirements.txt
```

(Or use `uv`, `poetry`, etc. — only `torch` and `pyyaml` are required.)

## Use

```python
import yaml, torch
from localvqe.model import LocalVQE

cfg = yaml.safe_load(open("configs/default.yaml"))
model = LocalVQE(
    **cfg["model"],
    n_freqs=cfg["audio"]["n_freqs"],
)
model.eval()

# Load published checkpoint (torch format, from Hugging Face)
state = torch.load("best.pt", map_location="cpu")
model.load_state_dict(state["model"])

# Bake the trained AlignBlock softmax temperature (carried in the
# checkpoint as a buffer) into the smoothing-conv weights. The GGML
# graph has no temperature parameter, so PyTorch inference must call
# this to match its behavior; without it the model runs at the default
# 1.0 and loses several dB of FE-ST ERLE on real recordings.
model.align.fold_temperature()

# Process a (mic, far) pair — shape (B, N_samples)
with torch.no_grad():
    enhanced = model(mic_pcm, far_pcm)
```

## Files

```
localvqe/
  model.py    LocalVQE + DCTEncoder + DCTDecoder
  blocks.py   FE, EncoderBlock, DecoderBlock, S4DBottleneck
  align.py    AlignBlock (soft delay cross-attention)
  ccm.py      Complex convolving mask (real-valued arithmetic)
configs/
  default.yaml   Architecture + audio settings for the published model
```

Only the model definition is included here; no training, loss, or data
pipeline code ships in this repository.

## Architecture versions

`configs/default.yaml` carries an `arch_version` field that mirrors the
GGUF `localvqe.version` metadata key:

| `arch_version` | Norm + activation | Matches |
|---|---|---|
| `3` (default) | pre-norm `CausalGroupNorm` + `SiLU`, decoder `skip_norm`, `SubpixelConv2d.norm`, `dmax=64` | `localvqe-v1.3-4.8M.pt` (current), `localvqe-v1.2-1.3M.pt` |
| `2` | pre-norm `CausalGroupNorm` + `ReLU6`, decoder `skip_norm`, `SubpixelConv2d.norm`, `dmax=32` | `localvqe-v1.1-1.3M.pt` |
| `1` | post-conv `BatchNorm2d` + `ELU` (legacy) | pre-v1.1 PyTorch checkpoints / `localvqe-v1-1.3M-f32.gguf` |

v1.2 and v1.3 share `arch_version=3`; they differ only in width
(`mic_channels`, `far_channels`, `bottleneck_hidden` in the YAML),
which the loader picks up from the checkpoint's saved `model_config`
when present. Pass `arch_version=2` (and set `dmax: 32`) in the
config to load a v1.1 checkpoint instead.

## Tests

```bash
pip install -r requirements-dev.txt
pytest tests/
```

`tests/test_architecture.py` runs without any checkpoint files and
verifies the state_dict layout for both `arch_version` values plus a
random-weight forward pass.

`tests/test_checkpoints.py` regression-tests every published checkpoint
listed in `tests/manifest.yaml` against a committed reference output.
The test skips cleanly when a checkpoint is absent, so the suite still
passes on machines without network access. To run against the actual
published weights, fetch them from Hugging Face first
(SHA256-verified, mirror of the GGML side's `regression-assets`):

```bash
python tests/fetch_checkpoints.py    # → pytorch/checkpoints/*.pt
pytest tests/
```

Override the destination with `LOCALVQE_CKPT_DIR=/path/to/dir` or
`--dest`. Existing files are kept as-is when their SHA matches the
manifest.

`tests/test_arch_snapshots.py` is a checkpoint-free numerical-drift
guard: it builds a model with `torch.manual_seed(1234)` for each
`arch_version`, runs forward on the deterministic input shared with
the GGML side, and compares against a committed snapshot. The only
forward-output coverage `arch_version=1` has, since no v1 PyTorch
checkpoint was ever published.

To add a new checkpoint to the regression suite: append an entry to
`manifest.yaml` (with `url` and `sha256`) and run
`python tests/regenerate_fixtures.py` to produce the reference output.
