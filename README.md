# Super Boom

Master bus destructor & harmonic sculptor for [Ableton Move](https://www.ableton.com/move/), built for the [Move Everything](https://github.com/charlesvestal/move-everything) framework.

Analog warming unit-inspired master bus destructor for Ableton Move with 10 preamp models, 8-band filterbank with vocoder mode and frequency shifting, creative compressor, EQ and tape output stage.

## Signal Flow

```
Input Gain → Compressor → 8-Band Filterbank → Distortion → DriveMix
→ Preamp (2x oversampled) → Tape Stage → Lo/Hi Cut → Limiter → Mix → Output
```

## Parameters

### Boom
| Knob | Parameter | Range |
|------|-----------|-------|
| 1 | Input Gain | 0.5 - 4x |
| 2 | Compressor | 0 - 100% |
| 3 | Drive | 1 - 20 |
| 4 | Drive Mix | 0 - 100% |
| 5 | Mode | Boost / Tube / Fuzz / Square |
| 6 | Shift | -2 to +2 |
| 7 | Mix | 0 - 100% |
| 8 | Level | 0 - 200% |

### Skulpt (8-Band Filterbank)
| Knob | Parameter | Range |
|------|-----------|-------|
| 1 | Low | 0 - 200% |
| 2 | B2 | 0 - 200% |
| 3 | B3 | 0 - 200% |
| 4 | Mid | 0 - 200% |
| 5 | B5 | 0 - 200% |
| 6 | B6 | 0 - 200% |
| 7 | B7 | 0 - 200% |
| 8 | High | 0 - 200% |

Additional (jog): Mod Shift, Flavor (8 presets), Vocoder Mode, Vocoder Gain

### Pre & Comp
| Knob | Parameter | Range |
|------|-----------|-------|
| 1 | Attack | 50us - 33ms |
| 2 | Release | 50ms - 1s |
| 3 | Threshold | 0 - 100% |
| 4 | Mod Drive | 0 - 100% |
| 5 | Preamp | Cass1 / Cass2 / Mast / Slam / Thick / 12bit / 8bit / Brit / USA / Cln |
| 6 | Grit | 0 - 100% |
| 7 | Gate | Off / -50dB to 0dB |
| 8 | Link | 0 - 100% |

### Seal
| Knob | Parameter | Range |
|------|-----------|-------|
| 1 | Lo Cut | Off / 75 / 150 / 300 Hz |
| 2 | Hi Cut | 20 - 20000 Hz |
| 3 | Saturation | 0 - 100% |
| 4 | Age | 0 - 100% |
| 5 | Flutter | 0 - 100% |
| 6 | Bump | 0 - 100% |
| 7 | Limiter | Off / On |
| 8 | Bypass | On / Bypass |

## Vocoder Mode

Enable Vocoder on the Skulpt page to use mic/line-in as a modulator. The mic signal controls the 8-band filterbank envelope while the main audio (track or master bus) is the carrier being processed through it.

## Build & Deploy

```bash
./scripts/build.sh      # Cross-compile via Docker
./scripts/install.sh     # Deploy to Move via SSH
```

Requires Docker or `aarch64-linux-gnu-gcc` cross-compiler.

## Release

1. Update version in `src/module.json`
2. Commit and push
3. Tag: `git tag v<version> && git push --tags`
4. GitHub Actions builds, creates release, updates `release.json`

## License

GPL-3.0
