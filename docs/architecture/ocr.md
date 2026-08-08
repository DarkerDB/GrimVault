# Cross-language OCR

Contract and implementation plan for reliable, low-latency tooltip text
recognition. Anchoring owns tooltip lifetime and exact crops; OCR must never
control overlay position or keep a stale Augment alive.

## Current boundary

Input is one settled full-resolution BGRA tooltip crop, its capture timestamp,
an anchor generation, exact geometry, and the selected game locale. Output is
UTF-8 text with per-line confidence and the same generation. Results from an
older generation are discarded.

The current implementation segments bright rows, trims horizontal whitespace,
splits wide lines at whitespace valleys, and runs PP-OCRv5 mobile recognition
through OpenCV DNN. Locale routing is explicit:

| Family | Locales | Model directory |
| --- | --- | --- |
| Latin | de, en, es, fr, pt-BR | `latin` |
| East Slavic | ru | `eslav` |
| Korean | ko | `korean` |
| CJK | ja, zh-Hans, zh-Hant | `ch` |

## Reliability requirements

- Preprocessing is deterministic and fixture-tested for every family.
- Unicode is normalized for lookup while raw OCR remains available.
- Confidence is retained per line/token rather than only globally averaged.
- OCR failure cannot reuse text from a previous anchor generation.
- Segmentation tolerates rarity colors, dim metadata, outlines, DPI, and scale.
- Name recognition may finish before secondary stats, but stale work never
  publishes into a newer tooltip.

## Hot-path latency budget

| Stage | p50 | p95 |
| --- | ---: | ---: |
| Crop + line segmentation | 1 ms | 2 ms |
| Primary/name recognition | 4 ms | 8 ms |
| Lookup dispatch | 1 ms | 2 ms |
| Remaining visible lines | 8 ms | 20 ms |
| OCR before lookup | 6 ms | 12 ms |

Model loading is excluded. The active locale family must be prewarmed when the
game window becomes visible.

## Execution model

1. Anchoring emits a monotonically increasing generation.
2. A primary pass recognizes the item-name region first.
3. Lookup begins once a sufficiently confident normalized name exists.
4. Remaining lines run concurrently or as a dynamic batch.
5. Every stage checks generation/cancellation before publishing.

`LanguageRegistry` currently returns a raw pointer from an LRU pool. That is
safe only with the single OCR consumer used today. Parallel OCR must first use
shared recognizer leases or pinned session handles so eviction cannot
invalidate in-flight inference.

## Recognition strategy

- Prewarm the selected family and retain the most recent fallback.
- Prefer batched lines when the model supports dynamic batch; otherwise use a
  small per-family session pool.
- Preserve natural glyph aspect ratio and split only at measured whitespace.
- Replace the global luma threshold with color-aware foreground extraction.
- Normalize Unicode width, compatibility characters, whitespace, punctuation,
  and locale-specific decimal separators for lookup.
- Apply DDB lexicon matching after OCR, retaining raw output and edit distance.
- Retry only uncertain name lines with an alternate scale/threshold.

## Measurements and fixtures

Structured events record generation, locale/family, warm/cold state,
segmentation and inference times, confidence, retry reason, cancellation,
normalized lookup key, and capture-to-lookup latency.

Golden fixtures cover every locale, rarity color, short/long names, mixed
digits and symbols, DPI scales, and ambiguous glyphs. Each fixture carries
expected lines and the normalized lookup key. Accuracy and latency regressions
fail independently.

## Implementation checkpoints

1. Generation-based cancellation and stale-result tests.
2. Safe recognizer leases instead of raw registry pointers.
3. Pure fixture-tested preprocessing and segmentation module.
4. Active-family prewarming with cold/warm timing events.
5. Name-first recognition and early lookup dispatch.
6. Per-family golden fixtures and Unicode normalization.
7. Benchmark dynamic batching versus a small session pool.
8. Tune retry and lexicon correction from captured failures.

