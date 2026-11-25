# io test fixtures

Files the tests cannot make themselves (there is no MP3 encoder in the build).
Tests that need a missing fixture are reported as skipped, not failed.

## sine440_44k1_mono.mp3

A 2 s, 440 Hz sine (peak about 0.5: the `sine` source is 1/8 by default, hence `volume=4`), 44.1 kHz mono, 128 kbit/s MP3. Used by `ImporterTests.cpp`
("MP3 imports", "An MP3 cut in half by bytes imports its first half", "An MP3 with trailing junk imports at full length"). Made in this
folder with:

    ffmpeg -f lavfi -i "sine=frequency=440:duration=2" -af volume=4 -ar 44100 -ac 1 -b:a 128k sine440_44k1_mono.mp3

Tool version: ffmpeg version 9.0.2-full_build-www.gyan.dev (generated 2026-11-17).

## sine440_44k1_stereo_lame.wav / .mp3

The same 2 s, 440 Hz sine (peak about 0.5), 44.1 kHz stereo, as a WAV (the reference) and as a 192 kbit/s MP3 with a LAME-style Info
header (encoder delay 576, padding 1080; ffmpeg names its encoder "Lavc"). Used by `ImporterTests.cpp` ("An MP3 with a LAME tag imports
without its encoder delay"). Made in this folder with:

    ffmpeg -f lavfi -i "sine=frequency=440:duration=2" -af volume=4 -ar 44100 -ac 2 sine440_44k1_stereo_lame.wav
    ffmpeg -i sine440_44k1_stereo_lame.wav -c:a libmp3lame -b:a 192k sine440_44k1_stereo_lame.mp3

Tool version: ffmpeg version 9.0.2-full_build-www.gyan.dev (generated 2026-11-18).

`sine440_44k1_mono.mp3` has the same kind of header; "An MP3 without a LAME tag imports as before" breaks its "Info" marker in a copy.
