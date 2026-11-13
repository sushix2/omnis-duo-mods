# Hardware Beat FX Channel Quick-Select (`mods/beat-fx`)

C source code for the **OMNIS-DUO (v1.23)** Beat FX target channel selector mod (`libfxswitcher.c`).

## Controls

| Shortcut | Action |
|---|---|
| **`SHIFT` + Headphone `CUE 2`** | Cycle to **NEXT** Beat FX target (`CH1` → `CH2` → `MIC` → `MASTER`) |
| **`SHIFT` + Headphone `CUE 1`** | Cycle to **PREVIOUS** Beat FX target |

* Masks headphone `CUE` bits while `SHIFT` is held so monitor cueing is unaffected.
* Opens the `CH SELECT` drawer for visual feedback and auto-closes after 3 seconds.
* Adds an `On` / `Off` toggle under `UTILITY` → `MOD`.

## Building from Source (ARM64)

```bash
gcc -shared -fPIC -O2 -Wall -Wextra -o libfxswitcher.so libfxswitcher.c -ldl
```
