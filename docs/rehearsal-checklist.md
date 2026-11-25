# Rehearsal checklist

Do this on the show laptop with the real sound gear. Tick each box, and write down anything odd next to it. For every item: **Do** is what you do, **See** is what must happen. If it does not, note it and carry on with the next item.

Items marked *Machine-checked* are run by the app tests (`app/tests/`, core tests in `core/tests/`) on every build; on the laptop only the part named "by hand" is left: real touch and fingers, the ear, the devices. Always by hand: real touch and capture loss, drag and drop from Explorer, the mode-button tap, Narrator itself and its notifications, audio by ear, audio devices, Art-Net on the NX1.

Before you start: have ten or so audio files ready (WAV, MP3 and one FLAC, at least one at 44.1 kHz and one at 48 kHz, one of them a loud, hot track), a text file, and a "junk" file (any non-audio file renamed to `.mp3`).

## 1. Start-up and recovery

- [ ] **First start.** Do: delete `%APPDATA%\Bigtop Player\settings.xml` (or rename it), start the app. See: the start panel with New / Open.
- [ ] **New show.** Do: New, pick an empty folder. See: the main screen with an empty step list. After your first edit, the status bar says "Saved hh:mm:ss".
- [ ] **Reopen.** Do: quit, start again. See: the same show opens on its own, the same step is NEXT, and nothing plays.
- [ ] **Moved folder.** Do: quit, rename the show folder, start. See: the start panel (not a crash).
- [ ] **Broken show file.** Do: quit, put the folder back, open `show.json` in Notepad, delete half of it, save, start. See: a "Restore backup from <time>" offer with the error; pressing it opens the show with your steps.
- [ ] **Switch shows.** Do: in Edit mode press "Shows…" in the status bar, open another show folder (or New). See: the other show opens. Press "Shows…" again and "Back to <show>": the GO screen returns unchanged. Then press GO and, while the sound plays, press "Shows…": a notice says sounds are still playing and nothing changes. Outside Edit mode (Rehearsal or Show) the button is greyed out.
- [ ] **Pause, switch show, GO.** Do: in Edit mode with nothing playing press Pause (P), then open another show ("Shows…", Open or New). See: the new show is not paused (no PAUSED strip, the button says Pause). Press GO: the step is heard at once. Switching to Show mode works.
- [ ] **Kill during playback.** Do: with a show of several steps, press GO twice, then end the app in Task Manager while a sound plays. Start it again. See: the same show, the same NEXT step, and nothing plays by itself.
- [ ] **Kill during an import.** Do: drop several long WAVs and end the app in Task Manager before they finish. Start again. See: the unfinished step is red, the others are fine. Drop the same file again: a new step is added for it, AND the old red step becomes playable too (both use the same file entry, which is imported again).

## 2. Building a show

- [ ] **Drop many files.** Do: in Edit mode, drop 10 files at once (WAV 44.1, WAV 48, MP3, the text file, the junk `.mp3`) on the step list. See: 8 steps appear at the drop position, named after the files. The text file is refused with a notice. The junk file is red "import failed"; hover it to see the reason.
- [ ] **Same file twice.** Do: drop the same WAV twice in one drop, and again in a second drop. See: the `audio` folder inside the show folder gains only one file.
- [ ] **Reorder.** Do: in Edit mode, with 30 or more steps, drag a row by its grab handle (the three bars at its left edge, 48 px wide) up and down, with mouse and with a finger, also near the top and bottom edge. See: a line shows where the row will land, the list scrolls near the edges, the order changes and stays after restart, NEXT stays on its step. Dragging elsewhere in a row scrolls (finger) or does nothing (mouse); there are no handles in Rehearsal and Show mode. Also drag audio files over the list: the same line shows. Add fade step: a step named Fade appears after NEXT and its row says "fade". *Machine-checked: StepListTests.cpp; by hand: touch only (finger drag and finger scroll, files dragged from Explorer).*
- [ ] **Missed touch release on a drag handle.** Do: start dragging a row by its handle with a finger, then slide the finger off the window (or off the screen edge) before lifting it. See: no row is left stuck to the finger; the next tap works normally and the order is either unchanged or the row sits where the line showed. *Machine-checked: StepListTests.cpp; by hand: touch only (a real finger sliding off the screen).*
- [ ] **Details lines.** Do: tick Details in the top mode bar in each mode (also while a hold-to-leave runs in Show mode: it must not cancel or trigger the hold); then repeat the Reorder check (handle drag, edge scroll, file drop line, finger scroll) and tap rows and their Edit buttons with Details on, and restart the app. See: each row shows its file, fades and changes in grey under its name (a missing file in red); taps, drags and the line land on the right row although rows now differ in height; NEXT stays in view after Up/Down; the tick is still set after the restart. Untick: the list looks as before. *Machine-checked: StepListTests.cpp, LayoutTests.cpp; by hand: touch only.*
- [ ] **Live NEXT line.** Do: tick Details, GO a song, then make the step after it (one that fades everything else) NEXT and let the song end by itself. See: the NEXT row's "everything else fades out ..." line goes away (its room stays blank) and no row moves. Open a fresh show with nothing playing: the NEXT row shows no such line. *Machine-checked: StepListTests.cpp; by hand: a glance only.*
- [ ] **Narrator reads the step list.** Do: start Windows Narrator (Ctrl+Win+Enter), move through the step list with Caps Lock+arrows, with Details off and on, and activate a row (Caps Lock+Enter). See: each row is read as "number, name", plus "next" on the NEXT row, "missing file / import failed" on a red one and the Details lines when Details is on; activating a row makes it NEXT and Narrator reads the change.
- [ ] **Delete.** Do: press the Delete key on the NEXT (selected) row, and use "Delete step" in the step editor. See: the step goes, its audio file stays in `audio`. There is no Delete button on the row any more.
- [ ] **Delete a playing step.** Do: GO, then delete the step that is playing. See: the sound plays on.
- [ ] **Show and Rehearsal mode are locked.** Do: tap SHOW in the mode strip at the top (green), then try to drop a file, drag a row, press Delete; "Shows…", Show settings and Settings are greyed out. Then tap REHEARSAL (blue) and try the same. See: nothing structural changes, and a notice names the mode. Each mode shows its own colour and name; the GO button's border has the same colour. *Machine-checked: ModeTests.cpp; by hand: the mode-button tap and the colours by eye.*
- [ ] **Leaving Show mode takes a hold.** Do: in Show mode press EDIT and hold: a bar fills over 2 s, then the mode changes. Try again but let go early, drag off the button, or touch a second finger: nothing changes. See: no dialog at any point. *Machine-checked: ModeTests.cpp; by hand: a real finger on a mode button and the tap.*
- [ ] **Drop while switching.** Do: drop many files, switch to Rehearsal or Show mode before they all appear. See: nothing changes and a notice says N files wait for Edit mode. Switch back to Edit mode: they are added in order. *Machine-checked: StepListTests.cpp; by hand: drag and drop from Explorer.*

## 3. Playing

- [ ] **Walk the list.** Do: press GO (Space) through the show. See: each step starts; the old sound fades out over about 2 s while the new one comes in; the highlight moves to the next row. Listen: no clicks, no gaps.
- [ ] **Double tap.** Do: tap GO twice within a fraction of a second, with the mouse or touch, and with Space. See: only one step runs.
- [ ] **Held key.** Do: hold Space for 2 s. See: only one step runs.
- [ ] **Select a row.** Do: tap a row, or use Up/Down. See: it becomes NEXT and stays so after restart.
- [ ] **Up/Down held.** Do: hold Down for a few seconds to run through the list, then quit and start. See: the list moves smoothly, the app stays responsive, and the NEXT step after restart is where you stopped (state is saved at most every 0.5 s and on release).
- [ ] **Fade out.** Do: press Esc (or Fade out) while sounds play. See: they fade. Press Esc a second time during the fade: everything stops at once.
- [ ] **Faders.** Do: drag a running sound's fader by touch, to the far left, and double-click it. See: the level follows your finger, far left is silent (-inf), double-click goes back to 0 dB, and the fader resets when the sound ends.
- [ ] **Missing file.** Do: in Windows Explorer move one audio file out of `audio`, then reach that step. See: the row is red "missing file", GO is refused with a notice, no dialog, and the highlight stays. Put the file back: the row turns normal again after a restart.

## 4. Audio devices

For each device, open Settings (status bar), pick it, set the rate, close Settings, and play three steps.

- [ ] **Built-in audio at 48 kHz.** See: status bar "audio OK", sound is clean.
- [ ] **SQ-5 over USB at 96 kHz.** See: "audio OK", sound clean, at the right pitch and speed (a 96 kHz card must not play fast or slow).
- [ ] **NI Komplete Audio 6 at 44.1 kHz.** See: same, correct pitch and speed.
- [ ] **Restart remembers it.** Do: quit and start. See: the same device is used without opening Settings.
- [ ] **Unplug during playback.** Do: with sounds running on the SQ-5, pull the USB cable. See: a "device lost" banner, silence, no sound from the laptop speakers. Replug: the banner clears within a few seconds and the sound carries on from where it stopped (positions freeze while the device is gone; nothing restarts). Press GO afterwards and a new step plays.
- [ ] **Unplug while Settings is open.** Do: open Settings, then pull the cable. See: no crash. Close Settings without changing anything: the banner stays and the app keeps waiting for the SQ-5 (it must not switch to the speakers). Replug: audio comes back.
- [ ] **Switch device while playing.** Do: pick another device in Settings during playback. See: the sound moves to the new device after a short break; no crash.
- [ ] **A pick that fails.** Do: choose a rate or device the card refuses (for example a rate it does not support, or a device in use by another program). See: an error or no change, no crash, and the previous good device is still the one in use after you close Settings and restart.
- [ ] **Missing at launch.** Do: quit, unplug the SQ-5, start. See: "device lost" (not the speakers). Plug in: audio starts within a few seconds.

## 5. Failure cases

- [ ] **Junk file.** Covered above (red "import failed"); check the app stays fine and other steps still play.
- [ ] **Playing from a slow disk.** Do: put a show on an external USB drive and play it. See: sounds start on time. Known: every GO opens the file, so a spun-down drive may delay GO; if you see that, note by how long.

## 6. Performance and levels

- [ ] **Big show opens fast.** Do: build or copy a show with 40 tracks, quit, start with a stopwatch. See: the show is on screen in under 5 s. Write the time down.
- [ ] **10-minute run.** Do: for 10 minutes press GO every 30 s with real music. See: the status bar's dropout counter stays at 0 and you hear no glitches. Write the number down.
- [ ] **CLIP light.** Do: on a loud track, push the live fader to +12 dB. See: CLIP lights in the status bar (it holds for 2 s). Listen: no harsh distortion; the limiter holds the peaks.
- [ ] **MP3 endings.** Do: import each MP3 you plan to use, play it to the end once, and listen to the last second. See: the ending is complete. Known: an MP3 without a proper header (some variable-bit-rate files) may be imported with its ending cut. If so, note which file and convert it to WAV first.

## 7. Modes

Three buttons in the strip at the top: EDIT, REHEARSAL (blue), SHOW (green).

- [ ] **Every table row.** Do: in each mode try: drop a file on the list, drop a file on a pad, drag a row, Delete key, Add fade step, a step's Edit button, Edit pads, Show settings, Settings, "Shows…", Pause/Resume, Keep, the faders, GO, Esc. See: Edit allows all of it; Rehearsal allows playing, faders, Keep, Pause, and in the editor only level and time fields (the rest is greyed); Show allows only GO, Esc, faders and pads (no editor panel, no Pause button). A refused action gives a notice naming the mode. *Machine-checked: ModeTests.cpp (Settings: its enabled state only; Edit pads: PadTests.cpp); by hand: the Settings dialog itself and touch.*
- [ ] **Hold to leave Show.** Do: in Show mode hold EDIT for 2 s, and again hold REHEARSAL. See: a bar fills, then the mode changes. Let go early, drag off the button, or add a second finger: nothing changes. Do it with a finger on the touch screen. *Machine-checked: ModeTests.cpp; by hand: a real finger on a mode button and the tap.*
- [ ] **Show mode while the editor is open.** Do: open a step's editor, switch to Show. See: the panel closes.
- [ ] **Mode survives a restart.** Do: quit in Show mode, start. See: Show mode again.

## 8. Step editor

- [ ] **Open it.** Do: tap Edit on a row (Edit and Rehearsal mode). See: a side panel opens with that step. Add fade step opens its new step.
- [ ] **Every field.** Do: change each in turn: name, note, level, fade in, start delay, loop, start, end, timecode start, file trim, everything else. See: each change shows in the row or the running sound after Enter, focus loss or a tap away, and stays after restart.
- [ ] **Junk input.** Do: in every number field type letters, nothing, a comma (1,5), a huge number, "nan", a negative time, and a start at or after the end. See: junk puts the old value back, "1,5" counts as 1.5, a start not below the end is refused with a message, no crash. The - and + buttons step by 0.5. *Machine-checked: EditorTests.cpp; by hand: real focus moves (tap away, Tab).*
- [ ] **Timecode start.** Do: type 00:00:10:30 at 25 fps. See: refused (frame out of range); 00:00:10:12 is accepted. *Machine-checked: EditorTests.cpp; by hand: real focus moves.*
- [ ] **Sounds expected to be running.** Do: add changes for running sounds (keep, to level, fade out, default). See: "default" stores nothing and names what everything else will do; a change for a sound that is gone is greyed with Remove; the counter shows "n of 16 changes" and the 17th is refused.
- [ ] **The four-step example.** Do: build the README's four-step example in the editor (Atmo loop, fade in 3 s; no sound: Atmo to -12 dB in 2 s; Music B fade in 5 s, Atmo to -15 dB in 3 s; no sound: Atmo fade out 8 s). Play it with GO. See: the mix does what the table says; listen for clicks at every change.
- [ ] **Touch size.** Do: use the whole editor with a finger, also in a narrow window. See: every button and field is easy to hit and nothing is cut off.

## 9. Running rows: seek, pause, Fade, Keep

- [ ] **Seek by the bar.** Do: while a long sound plays, drag the position bar's thumb into the first 5 s, the middle and the end part, and release; also several times quickly. See: one jump on release, remaining time ("-m:ss") follows. Listen for clicks: a short gap in the tail is accepted, a click is not. A loop says "loop", a delayed start says "delay". *Machine-checked: RunningTests.cpp; clicks also core/tests/EngineClickTests.cpp; by hand: ear and touch.*
- [ ] **Pause and Resume.** Do: with three sounds running and one still in its start delay, press Pause (or P). See: the orange PAUSED strip, sounds fade in 0.1 s and freeze, the delay freezes, GO is refused with "Paused – press Resume". Resume: all carry on from where they stopped, no click. Pads play on while paused. Esc while paused stops everything and ends the pause. No Pause button in Show mode. *Machine-checked: RunningTests.cpp; clicks also core/tests/EngineClickTests.cpp; by hand: ear and touch.*
- [ ] **Per-sound Fade.** Do: tap Fade on one running row; tap it again during its fade; tap it twice quickly. See: the first fades that sound out, the second ("Stop") ends it within a moment without a click, the other sounds play on. *Machine-checked: RunningTests.cpp; clicks also core/tests/EngineClickTests.cpp; by hand: ear and touch.*
- [ ] **Keep.** Do: move a running sound's fader to -10 dB and press Keep (while the fader is still moving, or while a finger still holds it, it is refused). See: no audible change, the fader goes back to 0 dB, and the step's level in the editor is now lower. Keep is greyed out at 0 dB and hidden in Show mode. *Machine-checked: RunningTests.cpp; by hand: ear and touch.*
- [ ] **Waveform in the running rows.** Do: play songs you know well and watch the bar. See: the waveform matches the song by ear; drops and endings show where you hear them. *Machine-checked: WaveformTests.cpp (io), RunningTests.cpp; by hand: ear and eye.*
- [ ] **Narrow window.** Do: open the editor so the running rows narrow. See: the position bar stays usable; TC and elapsed disappear before it shrinks.

## 10. Pad grid

- [ ] **Grid size.** Do: in Show settings change rows and columns (for example 4x2, then 3x5, then back to 4x2). See: the grid follows; pads that were hidden come back with their sound; at most 24 cells; buttons never below 48 px (the grid scrolls instead).
- [ ] **Play and fade.** Do: tap a pad, tap it again. See: a pad fires when the finger lifts, not when it touches. It turns green with "-m:ss" and a progress line while it plays; the second tap fades it out. Two pads over a running step play together. *Machine-checked: PadTests.cpp; by hand: ear and touch.*
- [ ] **Pads and the transport.** Do: with a pad playing press GO, Esc and Pause. See: none of them touches the pad.
- [ ] **Drop on a pad.** Do: in Edit mode drop a file on a pad. See: "importing..." and then the pad is named after the file. A pad with a missing file says "missing file", a junk file "import failed". *Machine-checked: PadTests.cpp (the pad is named after the dropped file); by hand: drag and drop from Explorer, the "missing file" and "import failed" labels.*
- [ ] **Edit pads.** Do: turn "Edit pads" on and tap a pad. See: the pad editor opens (name and Clear pad in Edit mode; level and trim in Edit and Rehearsal) and nothing plays. While that pad plays: a new name changes the label and the sound plays on; a new level or trim is heard at once without a click; Clear pad fades the sound out like a second tap and the pad is empty (a tap on it then plays nothing). *Machine-checked: PadTests.cpp; by hand: touch, and the level ramp by ear.*
- [ ] **Touch size.** Do: use the grid with a finger. See: every pad is easy to hit.

## 11. Import: FLAC and MP3

- [ ] **FLAC import.** Do: drop a FLAC file (also a 44.1 kHz one). See: a step appears, imports without error, plays at the right pitch and speed.
- [ ] **MP3 starts on time.** Do: import an MP3 made by LAME or ffmpeg and one from another encoder; play each next to a WAV of the same piece started on the same GO. See: the LAME/ffmpeg MP3 starts with the WAV (no silence at the front) and its ending is whole. The other one may start late; if so, note which file.
- [ ] **Re-drop an old MP3.** Do: drop an MP3 that was in the show before gapless MP3 import. See: it is converted again and now starts on time.

## 12. Art-Net on the Onyx NX1

Do this with the laptop and the NX1 on the same network.

- [ ] **Default adapter, Wi-Fi and Ethernet both on.** Do: in Show settings set the NX1's IP address as Art-Net target, adapter Default, the fps the show uses. Play a step with a timecode start. See: the status bar's Art-Net light shows sending; Onyx shows the incoming timecode and follows the sound without drifting over a few minutes.
- [ ] **Lighting adapter, Wi-Fi and Ethernet both on.** Do: pick the Ethernet adapter that faces the lighting network from the adapter list and repeat. See: same result. If one works and the other does not, note which.
- [ ] **Missing adapter.** Do: in Show settings pick an adapter, quit, disable that adapter in Windows (or unplug its USB network dongle), start. See: the app falls back to Default with a status text, no crash.
- [ ] **Cable plugged in after start.** Do: quit, unplug the lighting network cable, start the app (the status says the adapter is missing, using default), then plug the cable in. See: within a few seconds the status says sending and Onyx receives timecode from the chosen adapter, without touching Show settings.
- [ ] **Pull and re-plug during timecode.** Do: while a timecode sound plays, pull the lighting cable for 10 s and plug it back in. See: the light shows an error while it is out (audio unaffected), then sending again; Onyx follows the timecode again.
- [ ] **Bad target.** Do: type a target that is not an IPv4 address. See: refused, the old target stays.
- [ ] **Pause and idle.** Do: pause a timecode sound, then let it end. See: while paused the frame stays frozen on Onyx; with no timecode sound the light says idle and nothing is sent.

## 13. Getting the app onto the laptop

- [ ] **Download.** Do: download the Windows zip of the latest release from https://github.com/bigtop-player/bigtop-player/releases, unzip it into a normal folder, start "Bigtop Player.exe" on a laptop with nothing installed. See: the app starts with no message about a missing DLL.

## 14. Export and import a show

- [ ] **Export.** Do: in Edit mode press Shows..., then Export show..., pick a folder. See: "Exported: ...<ShowName>-YYYYMMDD-HHMM.bigtop.zip"; the zip holds show.json, state.json and the audio (originals, or FLAC where there is none), no backups; the buttons are greyed while it writes.
- [ ] **Import on another folder.** Do: Shows..., Import show..., pick the zip and an empty folder. See: the show opens; rows whose FLAC was left out say "importing..." and GO is refused until they are done; then every step plays.
- [ ] **Play it.** Do: play two songs from the imported show next to the same two in the original show. See: same sound, same length, same levels.
- [ ] **Refusals.** Do: import a .txt file renamed to .zip, and import into a folder that has files in it. See: a red message, nothing unpacked (the folder as it was).
- [ ] **Old show.** Do: export a show made before this build. See: it works; those files travel as FLAC.
- [ ] **On the show laptop.** Do: export one real show on the show laptop and import it there into a new empty folder. See: it opens in Edit mode and plays like the original.
- [ ] **Edit mode only.** Do: switch to Rehearsal or Show. See: the start panel cannot be opened, so no export or import chooser can appear during a show.

## Result

Date, laptop, devices tried, and anything that failed:

