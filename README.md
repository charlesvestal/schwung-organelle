# Schwung Organelle

Run Critter & Guitari [Organelle](https://www.critterandguitari.com/organelle) Pure Data patches on Ableton Move as a Schwung shadow-slot sound generator.

Embeds [libpd](https://github.com/libpd/libpd) (multi-instance) and translates the Organelle screen/knob/encoder conventions to Move controls.

## Status

In development. v1 ships vanilla Pd objects only — no externals.

## Control mapping

| Organelle | Move |
|---|---|
| Knob 1–4 | Knob 1–4 |
| Encoder turn | Jog turn |
| Encoder click | Jog click |
| Aux button | Knob 7 capacitive touch |
| Foot switch | Knob 8 capacitive touch |
| 25-key keybed | Slot's MIDI in (passthrough; per-slot Octave nudge available) |
| 128×64 OLED | Move's 128×64 display |

## Patches

Patches live in `/data/UserData/schwung/organelle-patches/` on device.

A starter set ships with the module install. Drop more in via SFTP:

```bash
scp -r MyPatch ableton@move.local:/data/UserData/schwung/organelle-patches/
```

Each patch is a folder containing `main.pd`.

## Design

See [docs/plans/2026-05-12-organelle-port-design.md](https://github.com/charlesvestal/schwung/blob/main/docs/plans/2026-05-12-organelle-port-design.md) in the main Schwung repo.

## Build

```bash
./scripts/build.sh           # Docker cross-compile for ARM64
./scripts/install.sh         # Deploy to move.local
```

## License

MIT (this module). libpd and Pure Data are BSD-3-Clause / Pure Data License — see [LICENSE](LICENSE).
