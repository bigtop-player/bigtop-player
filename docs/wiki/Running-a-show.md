# Running a show

![Three sounds running](../images/running-sounds.png)

- **GO** (button or Space) runs the highlighted step. Two GOs within a fraction of a second count as one.
- **Details** in the top bar shows what each step will do (fades, level changes, everything else) before you press GO; see [Building a show](Building-a-show.md#details). The NEXT row's "everything else" line tells what will really be faded: the sounds playing now (waiting start delays included, pads never), so a song that ended by itself drops out of it (the line goes blank; the rows do not move).
- **Fade out** (button or Esc) fades everything; a second press stops it at once.
- **Faders** on each running sound change its level live. Double-click returns to 0 dB.
- **Keep** (Edit and Rehearsal) is greyed out while the fader is at 0 dB and lights up when you move it. It stores what you hear where that sound's level was last set: in the step that last changed its level, or else in the step that started it.
- **Fade** on a running sound fades just that sound; a second tap while it fades stops it.
- The **position bar** shows the sound's progress; drag it to seek. Behind it you see the sound's waveform from its start to its end point (the loop range for a loop): the played part in blue, what is coming in grey, so drops and endings show before you hear them. The waveform appears a moment after a show opens or a file finishes importing (it is read in the background); until then the plain bar is shown.
- **Screen readers** (Windows Narrator) read the step list as a list: each row's number, name, "next", a missing file / import failed and, with Details on, its Details lines. Activating a row makes it NEXT, as a tap does.
- **Pause** (button or P; hidden in Show mode) fades over 0.1 s and freezes all step sounds, including waiting start delays. Resume carries on from the same place. GO is refused while paused; pads keep playing. Esc while paused stops everything at once and ends the pause.

![Paused](../images/paused.png)

## Pads

The pad grid on the right plays a sound with one tap, whatever the steps do. A pad fires when your finger lifts, not when it touches. Tap again to fade it out. A playing pad is green with its remaining time.

![The pad grid with a pad playing](../images/pads.png)
