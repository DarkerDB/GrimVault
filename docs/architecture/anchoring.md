# Anchoring

How GrimVault tracks the in-game tooltip smoothly and exactly.

The game positions its tooltip as a pure function of the cursor:

    tooltip_topleft = clamp (cursor + offset, game_bounds)

`offset` is fixed for the lifetime of a hover. The bottom/right clamp is
why a tooltip pins to the screen edge and stops following the mouse.

Anchoring exploits this: vision establishes the *anchor* (the offset, the
exact box size, and a visual fingerprint) once per hover; afterwards the
box is pure cursor math at timer rate. Detection accuracy stops mattering
for smoothness, and capture latency leaves the drawing loop entirely.

## 1. Definitions

    anchor      { offset, size, fingerprint, content_hash, per-axis pin }
    offset      tooltip top-left minus cursor position, both at the same
                capture instant, per axis
    fingerprint small full-res border patch (e.g. 48x16 from the tooltip's
                top frame) grabbed at acquisition, used for verification
    locked      an axis whose offset is confirmed (measured while the
                tooltip was NOT pinned at a clamp edge on that axis)

## 2. State machine

    IDLE ──detector hit──> SETTLING ──2 agreeing refines──> ANCHORED
     ^                        │                                │
     │                        └──timeout / hit lost──> IDLE    │
     └──────────presence loss / identity change / game hidden──┘

    IDLE      no tooltip. Detector (tooltip.onnx) runs per captured frame.
    SETTLING  a hit exists but the tooltip is still fading/scaling in.
              Refine every frame; require two consecutive refined boxes
              agreeing within 2 px (position, cursor-relative, and size).
              Nothing is drawn yet: reveals land settled, never wobbling.
    ANCHORED  anchor established. Detector stops. Presenter draws from
              cursor math. Verifier checks each captured frame. OCR (full
              pipeline mode) fires exactly once, on entry.

Transitions log at debug level with the anchor values.

## 3. Components

### Acquirer  (existing tooltip.onnx, vision thread)

Coarse box only. Runs in IDLE and SETTLING; never in ANCHORED. Its
~5-10 px error is irrelevant because it only seeds the refiner. Future
options (DirectML EP, retraining on settled tooltips) may speed up acquisition
but change nothing downstream.

### Refiner  (classical CV, full resolution, ~1-3 ms)

Input: coarse box + frame. Expand the box by a 20 px margin, work on the
full-res crop:

1. Sobel row/column projections. The tooltip frame art produces hard
   gradient peaks; snap each box edge to the nearest strong peak.
2. Fallback when a projection is ambiguous: template-match the fixed
   corner ornaments (small patches, cv::matchTemplate, TM_CCOEFF_NORMED).

Output: box exact to ~1 px, or failure (stay in SETTLING).

### Anchor estimator

    offset = refined_topleft - cursor_at (frame.timestamp)

Cursor history is the existing 60 Hz sample ring; keep it running whenever
the game window is visible. Per-axis lock rule: an axis is `locked` only
if the refined box does not touch that axis's clamp boundary (within
2 px). A tooltip acquired while pinned bottom-right has a meaningless
offset on the pinned axes; the presenter still draws it correctly (the
clamp pins it regardless), and the estimator keeps re-measuring each
refined frame until the axis unpins and locks. Once both axes are locked
the estimator stops updating (the offset is constant; later measurements
are only noise).

Size: latest refined size. Fingerprint: grabbed at ANCHORED entry.

### Presence and identity verifier  (per captured frame in ANCHORED)

Presentation and observation are deliberately separate. The presenter uses
only cursor math (or the immutable coordinate of a pinned axis). Vision
NCC-locates the border near that prediction, with a narrow search on pinned
axes and a wider search on free axes. The observed position becomes the
center of the next vision search but never moves the Augment.

At the observed box, a fixed-cost 64-bit grid hash samples the tooltip
interior. Sampling cost is independent of tooltip area. A stable hash means
the same card: mouse positioning remains authoritative. Replacement demands
hard confirmation: the hash delta (`identity_bits`) AND the changed
detail-pixel count (`identity_detail_px`) must both cross threshold, and must
do so on `identity_frames` consecutive frames. Either signal alone is capture
noise — dxgi in particular moves tens of detail pixels on a static card,
while a real swap or a failed locate saturates both (64 bits, 1024 px) and
still confirms within two frames. A confirmed replacement hard-hides the old
Augment immediately, then passes through the normal two-frame detector and
refiner settle gate before the new Augment animates in. This prevents a single
fade-out frame from resurrecting a disappearing tooltip.

### Reset on cursor jump  (ANCHORED, per frame)

Before verifying, compare the cursor to the previous frame's. Travel past
`cursor_reset_px` (~140 px) in one frame is a flick, not slow drift within a
large item: the hover is leaving. Drop the anchor immediately and hard-hide
the Augment (no grace), then re-acquire fresh. The baseline slides per
frame so a slow drag across a big item never trips it; the translation-tolerant
presence check handles gentler movement.

### Presenter  (UI thread, 120 Hz timer)

    draw_topleft = clamp (cursor_now + offset, game_bounds - size)

Runs only in ANCHORED. Pure math per tick, no capture involvement: the
box moves with the mouse at timer rate and pins at edges exactly like the
game's own layout. The debug overlay draws this box; the Augment anchors
beside it; OCR crops from it.

## 4. Threads and data flow

    capture thread   frames ──────────────┐
                                          v
    vision thread    IDLE/SETTLING: detect -> refine -> estimator
                     ANCHORED:      verify
                                          │  anchor / lost events
                                          v
    UI thread        presenter timer: clamp (cursor + offset)
                     cursor sample ring (60 Hz, game visible)

Events cross threads as queued Qt invocations carrying plain values
(anchor struct, frame timestamps). No shared mutable state beyond the
pipeline's existing atomics.

## 5. Budget

    stage                     cost              rate
    detect  (acquire)         30-60 ms CPU      only IDLE/SETTLING
    refine                    1-3 ms            SETTLING frames
    locate + grid hash        fixed small ROI   ANCHORED frames
    present                   ~0 (arithmetic)   120 Hz
    reveal latency            ~2 frames after fade-in settles
    hide latency              <= 1 capture interval (8-17 ms during motion)
    steady-state CPU          locator + identity signatures; detector idle

## 6. Edge cases

- Acquired while pinned: axis stays unlocked, presenter clamps anyway;
  locks when the cursor moves inward. (§3 estimator.)
- Item-compare side tooltips: multiple detector hits. Anchor the box
  nearest the cursor; others are ignored until the primary is lost.
- Game window moves/resizes: bounds change re-clamps automatically;
  a monitor/DPI change invalidates the anchor (offset is physical px)
  and returns to IDLE.
- Tooltip content swap in place (hover moves to the adjacent slot and a
  new tooltip appears at nearly the same spot): fingerprint mismatch,
  IDLE, re-acquire. The next anchor may seed SETTLING with the previous
  offset as a prior to converge faster.
- fcr: `--fcr` keeps its meaning (capture rate). Acquisition latency
  scales with it; presenting does not, because presenting no longer
  consumes frames.

## 7. Augment lifetime

The debug box renders raw anchor state; the Augment must not. Its policy:

- ANCHORED (full mode) fires OCR -> lookup once; the Augment presents
  keyed to the resolved item.
- Confirmed presence loss and cursor-jump resets hide the Augment immediately.
- A short ~40 ms grace remains available only for future soft-loss signals;
  current loss and replacement signals are hard confirmations.

While anchored, capture runs at 60 fps and bursts to 120 fps for 150 ms after
cursor motion. Acquisition and its expensive detector remain at the configured
active rate (15 fps). The burst lowers the common motion/removal bound to about
8 ms without permanently doubling steady-state capture work.

## 8. Superseded tracking behavior

Earlier drift following, capture-time anchor lookup at draw time, jitter
blending, and leave-threshold hiding are superseded by `clamp (cursor +
offset)` plus presence/identity events. The overlay only paints and tracks the
capture-region border.

## 9. Module plan

    src/vision/tooltip_tracker.{h,cpp}   refiner + presence/identity checks,
                                         pure OpenCV, no Qt: unit-testable
                                         against PNG fixtures
    src/ocr/pipeline.cpp                 state machine; detector gating;
                                         emits anchor/lost events
    src/ui/debug_overlay.cpp             presenter timer + painting
    src/app/controller.cpp               event routing (unchanged shape)

## 10. Stages, each verifiable

    a. Refiner on fixtures: hand-label tooltip edges on captured PNGs,
       assert refined boxes within ±2 px. ctest label `unit`.
    b. Estimator + presenter behind --debug: box glued at 120 Hz, edge
       pinning for free. Manual: hover near edges, sweep the cursor.
    c. Verifier: hide within one capture interval of the tooltip
       vanishing; no travel heuristic. Manual + fixture test with a
       tooltip-free frame.
    d. Settle gate: reveals land at the final position; no early wobble.
       Manual: rapid hover across inventory rows.

Fixtures come from the debug dumps (`GRIMVAULT_OCR_DEBUG=1` capture_raw
frames) so they match real game rendering exactly.

## 11. Diagnostics and tuning

Normal runs emit structured `[vision]` events for anchor acquisition/loss,
replacement candidates and rejection, and periodic `anchor_metrics` summaries
(frame age, locator cost, and maximum hash distance). Threshold changes should
be justified from these measurements rather than visual guesswork.

Debug runs retain the newest 40 replacement-candidate/settled frames in
`%LOCALAPPDATA%\GrimVault\<env>\logs\anchoring`. Production omits the
`<env>` segment. These become real-game regression
fixtures after review and sanitization. `tools/anchor-log-summary.{sh,ps1}`
summarizes the latest rotating log.
