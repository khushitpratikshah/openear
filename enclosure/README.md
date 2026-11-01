# OpenEar enclosures

## OpenEar pod (pocket clip, display case) - `openear_pod.py`

A two-part pod for the MAX32655FTHR with a raised **OpenEar** wordmark and a
pocket clip. No battery bay: it is made for showing the board, and the LiPo
lead exits through a slot next to the battery connector.

| | |
|---|---|
| Size | 74.6 x 28.9 x 18.7 mm (incl. 0.8 mm wordmark) |
| Board geometry | from a calibrated photo scan of the real board (holes, buttons, mic, jacks, USB; about +-0.2 mm). Part heights are estimates. |
| Openings | headphone + line-in jacks (jack end), micro-USB (other end), 6 buttons, hex mic grille over the MEMS mic, battery lead slot |
| Fixing | the board sits on 4 standoffs with locating pins through its mounting holes; the lid's rabbet fits over a lip on the base (friction fit) |
| Clip | pen-style, 38 mm arm, 1.8 mm gap; printed separately and fixed with two dowels (4.5 mm pieces of 1.75 mm filament) plus a drop of superglue. Jack end up in the pocket, so the earphone cable runs straight up |

Print (PLA or PETG, 0.2 mm layers, 3 walls). **No supports needed for any part.**
`python3 check_overhangs.py` checks this: it finds every down-facing surface
steeper than 45 degrees that is not on the bed, in each part's print
orientation, and reports 0 mm2 for all three. What is left are short bridges with both ends
anchored (the lid ceiling, 25 mm; the USB, battery-slot and jack-hole tops).
How: a 45 degree chamfer on the bottom edge (not a fillet), flat-topped teardrop
jack holes, chamfered USB corners, a steep 40 degree roof on the lid's rabbet,
and a clip with chamfered corners and teardrop dowel holes.


| File | Orientation |
|---|---|
| `stl/openear_pod_base.stl` | as exported, open side up |
| `stl/openear_pod_lid.stl` | as exported, rim on the bed (the 25 mm ceiling bridges). For a white wordmark, pause and swap filament at the first layer above the lid top (z = 6.2 mm in the lid file) |
| `stl/openear_pod_clip.stl` | as exported, lying on its side (layers run along the arm, so it springs instead of snapping) |

Regenerate: `pip install manifold3d trimesh numpy matplotlib pillow`, then
`python3 openear_pod.py` (STLs) and `python3 render_pod.py` (the renders in `stl/`).
If the board is a loose fit or the pins miss the holes, change `PCB_L`, `PCB_W`,
`HOLES` or `FIT` at the top of the script; snipping the pins off also works.

---

## Original body-worn case with battery bay - `openear_case.py`


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
