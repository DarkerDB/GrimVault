#!/bin/bash
#
# Fetch a PaddleOCR PP-OCRv5 recognizer for one language family into
# models/paddle/<family>/{rec.onnx,dict.txt}, ready for the cv::dnn
# pipeline. Stock exports use dynamic shapes that OpenCV's ONNX importer
# rejects, so the model is simplified to the fixed 1x3x48x320 input the
# recognizer feeds (requires `uv` for a throwaway onnxsim venv).
#
#    tools/fetch-ocr-model.sh en latin
#    tools/fetch-ocr-model.sh eslav korean ch
#
# Family -> upstream model name (HuggingFace, PaddlePaddle org). PP-OCRv5
# has no japan/chinese_cht models; the base "ch" model covers Simplified +
# Traditional Chinese and Japanese, and eslav covers Cyrillic (ru/be/uk):
#    en       en_PP-OCRv5_mobile_rec_onnx
#    latin    latin_PP-OCRv5_mobile_rec_onnx
#    eslav    eslav_PP-OCRv5_mobile_rec_onnx
#    korean   korean_PP-OCRv5_mobile_rec_onnx
#    ch       PP-OCRv5_mobile_rec_onnx

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"

repo_for () {
   case "$1" in
      en)     echo "PaddlePaddle/en_PP-OCRv5_mobile_rec_onnx" ;;
      latin)  echo "PaddlePaddle/latin_PP-OCRv5_mobile_rec_onnx" ;;
      eslav)  echo "PaddlePaddle/eslav_PP-OCRv5_mobile_rec_onnx" ;;
      korean) echo "PaddlePaddle/korean_PP-OCRv5_mobile_rec_onnx" ;;
      ch)     echo "PaddlePaddle/PP-OCRv5_mobile_rec_onnx" ;;
      *)      echo "unknown family: $1" >&2; exit 2 ;;
   esac
}

venv=/tmp/gv-ocrsim
if [ ! -x "$venv/bin/python" ]; then
   uv venv "$venv" -q
   uv pip install --python "$venv/bin/python" -q onnx onnxruntime onnxsim pyyaml
fi

for family in "$@"; do
   repo="$(repo_for "$family")"
   dest="$root/models/paddle/$family"
   mkdir -p "$dest"

   echo "==> $family  ($repo)"
   curl -sfL "https://huggingface.co/$repo/resolve/main/inference.onnx" -o "/tmp/gv-$family.onnx"
   curl -sfL "https://huggingface.co/$repo/resolve/main/inference.yml"  -o "/tmp/gv-$family.yml"

   "$venv/bin/python" - "$family" "$dest" <<'PY'
import sys, yaml, onnx
from onnxsim import simplify

family, dest = sys.argv[1], sys.argv[2]
width = 960 if family == 'en' else 320

cfg = yaml.safe_load(open(f'/tmp/gv-{family}.yml'))
chars = cfg['PostProcess']['character_dict']
open(f'{dest}/dict.txt', 'w', encoding='utf-8').write('\n'.join(chars) + '\n')

model = onnx.load(f'/tmp/gv-{family}.onnx')
simplified, ok = simplify(model, overwrite_input_shapes={'x': [1, 3, 48, width]})
assert ok, 'onnxsim check failed'
onnx.save(simplified, f'{dest}/rec.onnx')

print(f'    dict: {len(chars)} chars; model simplified to 1x3x48x{width}')
PY
done
