# OpenEar enclosure

A body-worn case for the MAX32655FTHR plus a small LiPo. It measures about
70 × 27 × 24 mm and has a loop underneath for a badge clip or lanyard.

ADI doesn't publish connector coordinates for this board, so **every opening is
a number you set after measuring.** The STLs in `stl/` use best guesses. Don't
print the full case from them unchanged.

## Battery

Use a **602030 or 702030 LiPo** (20 × 30 mm, 6–7 mm thick, about 300–400 mAh) with a
JST-PH 2-pin plug. It sits under the board, inside the PCB footprint.

> ⚠️ **Check polarity before plugging in.** Cheap LiPo leads are often wired the opposite
> way to what the board expects. Compare red/black against the + and − marks next to the
> board's battery connector. A reversed battery can destroy the PMIC.

## Workflow

1. **Measure** the real board with calipers (checklist below).
2. Edit `CONFIG` at the top of `openear_case.py`.
3. `pip install manifold3d trimesh numpy matplotlib`, then run `python3 openear_case.py`.
4. **Print `openear_fitcheck.stl` first.** It's a 1.2 mm plate that takes about 10 minutes
   to print. Lay it on the board inside its raised outline and check that every hole sits
   over its button, LED and mic.
5. Print `openear_tray.stl` (open side up) and `openear_lid.stl` (flat face down). Use PLA
   or PETG, 0.2 mm layers, 3 walls. No supports are needed.

## Caliper checklist

Measure everything from the board's **USB-end, left corner** with the components facing up.
x runs along the board and y runs across it.

| # | Measure | CONFIG key |
|---|---|---|
| 1 | Which end or edge the USB connector is on, and its offset from the centre line | `usb` |
| 2 | Which face each 3.5 mm jack opens onto, and its position along that face | `jacks[].face`, `along` |
| 3 | Height of each jack's centre above the PCB top (headset) or below it (line-in) | `jacks[].z` |
| 4 | Tallest part on top of the PCB | `over_clear` |
| 5 | Tallest part under the PCB | `under_clear` |
| 6 | x, y of the mic (the small metal can with a hole, MK1) | `mic` |
| 7 | x, y of SW1 (power), SW2, SW3, SW5 (reset) | `buttons` |
| 8 | x, y of the user RGB LED | `leds` |
| 9 | Your battery's thickness | `battery_h` |
| 10 | microSD slot position, if you want card access without opening the case | `sd` |

## Design notes

* The PCB rests on two 1 mm ledges along its long edges, and two ribs on the lid clamp
  it down. No screws, and no need to know where the mounting holes are.
* The mic port is a cluster of 1 mm holes, which keeps fingers and dust out while still
  letting sound in.
* The lid is a friction fit. A drop of glue or a strip of tape keeps it shut once the
  design is final.
* Keep the earphone cable away from the mic port. Earphone sound leaking back into the
  mic is what causes howling.
