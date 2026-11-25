/// @file SoundSource.h
/// @brief The interface the engine reads audio from.
///
/// A SoundSource is a decoded (or streamed) sound at the engine sample rate.
/// The engine and the controller never own sources: the SourceProvider
/// (ShowController.h) does. A voice only holds a raw pointer.
///
/// Lifetime rule: a source must outlive every voice reading it, including
/// voices that lost their key and are fading out for kDeclickSeconds after a
/// restart, seek or second fade-out-all; those do not appear in Engine::snapshot().
///
/// A source's read count is the number of read() calls made on that source
/// object since it was created. A provider may free or replace a source only
/// when all of these hold:
///  - the source's read count has not changed for at least 1 s of engine
///    audio, measured as 1 s worth of frames (e.g. 48000) of
///    Snapshot::framesProcessed advancing, never wall-clock time;
///  - no pending voice in Engine::snapshot() holds the key it was handed out for
///    (a voice still in its start delay has not read yet, so silence proves nothing);
///  - if its read count is 0, no voice at all in Engine::snapshot() holds that
///    key: a voice whose delay ends exactly at the end of a block shows as not
///    pending one block before its first read();
///  - framesProcessed is advancing: while no audio runs (e.g. the device is
///    unplugged) nothing may be freed, because the count of engine frames does
///    not grow.
/// The 1 s margin covers voices that lost their key (they read for up to
/// kDeclickSeconds) and commands still queued (applied within a few blocks).
/// The engine has no "voice ended" callback; the snapshot is the only signal.
#pragma once

#include <cstdint>

namespace bigtop {

/// @brief A stereo sound the engine can play, addressed by frame.
///
/// Frames are at the engine sample rate. Mono sources fill both channels with
/// the same samples.
/// @note read() is called on the audio thread; lengthFrames() on the audio and
///       control threads. Both must be realtime-safe.
class SoundSource {
public:
    /// @brief Virtual destructor so sources can be deleted through this interface.
    virtual ~SoundSource() = default;

    /// @brief Length of the sound.
    /// @return Number of frames, >= 0. Must not change while a voice plays it.
    virtual int64_t lengthFrames() const noexcept = 0;

    /// @brief Copies frames [startFrame, startFrame + frames) into the buffers.
    /// @param startFrame [in] First frame to read; the engine guarantees
    ///        0 <= startFrame and startFrame + frames <= lengthFrames().
    /// @param left [out] Receives @p frames samples of the left channel.
    /// @param right [out] Receives @p frames samples of the right channel.
    /// @param frames [in] Number of frames to read, 1..512.
    /// @warning Called on the audio thread: must not block, lock, allocate or
    ///          throw. A streaming source that is not ready must write silence.
    virtual void read(int64_t startFrame, float* left, float* right, int frames) noexcept = 0;
};

}  // namespace bigtop
