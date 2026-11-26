# Troubleshooting

| Problem | What to do |
|---|---|
| A row is red "missing file" | The audio file is gone from the show's `audio` folder. Put it back, or drop the file again. |
| A row says "import failed" | The file could not be read. Hover it for the reason; convert it to WAV and drop it again. |
| GO does nothing and a notice says paused | Press Resume first. |
| SHOW does nothing and a notice says "Resume or press Esc first" | Show mode has no Pause, so it is refused while sounds are paused. Press Resume, or Esc to stop them, then SHOW. |
| An MP3 ends too early | Some variable-bit-rate MP3s without a proper header are cut short. Listen to each MP3 ending once; convert to WAV if cut. |
| A short gap after seeking in a long sound | Accepted: a seek into the streamed part can leave tens of milliseconds of silence, no click. |
| GO is late with a show on an external drive | Every GO opens the file; a spun-down drive is slow. Keep shows on the internal disk. |
| Dropped files are missing | Files can only be dropped in Edit mode. If you leave Edit before an import has started, the files are held ("N dropped files wait for Edit mode") and lost if the app quits first; a file dropped on a pad is discarded with a notice. Do not leave Edit mode right after dropping files: wait until "importing..." has started, ideally finished. |
| A failed import shows "missing file" after restart | Failures are remembered only until the app closes. Drop the file again. |
| Quitting takes a while | One conversion at a time; the app waits for the running one. |
| Art-Net reaches nothing | In Show settings pick the right network adapter instead of Default. |
| "device lost" banner | The sound card was unplugged. Plug it back in; sound resumes within seconds. |

More checks: the [rehearsal checklist](../rehearsal-checklist.md).
