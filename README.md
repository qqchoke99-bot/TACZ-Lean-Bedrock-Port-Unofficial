# TaczLean (Levi)

Port of **taczleaning 1.0** (Nwkey) — **lean system only**.

## Features
- Hold **Q** (left) / **E** (right) on-screen buttons → camera rolls left/right
- Values from Java: max lean **17°**, smooth **0.25**, FP roll ≈ 1:1
- Same `CameraBlendSystemTick` hook family as CameraOverhaul (1.26.50+)

## Install
1. Build → import `TaczLean.levipack` in Levi
2. Copy button PNGs to:
```text
/sdcard/games/TaczLean/buttons/
  button_iconL.png
  button_icon_pressedL.png
  button_iconR.png
  button_icon_pressedR.png
  button_bg.png
  button_bg_pressed.png
```
(If missing, buttons still appear as text **Q** / **E**)

## Java mapping
| Java | This mod |
|------|----------|
| Key hold Lean Left/Right | ButtonBehavior::Hold L/R |
| currentAngleDeg smooth | smoothFactor lerp |
| ComputeCameraAngles setRoll | quaternion roll bias on camera |

Third-person body shift is **not** ported (Bedrock FP focus).
