/// @file StreamingSource.h
/// @brief The app's SourceProvider: plays the show's FLAC files from disk
///        without loading whole shows into RAM, with an instant GO.
///
/// Every sound reads from two places. The head, the first kHeadSeconds from
/// the sound's start point, is decoded into RAM (16-bit) when the show is
/// set, and is shared read-only by every voice that starts there: it makes
/// GO instant and loops gapless (a loop wraps back into the head). The tail,
/// everything after the head, streams through a juce::BufferingAudioReader
/// per voice start on one shared background thread; data that is late reads
/// as silence and counts as a dropout. Real data that follows such silence
/// (a dropout recovery or a seek into the tail) rises from 0 to full over
/// kDeclickSeconds so it cannot click; head reads at a fresh start never fade.
///
/// Lifetime: every sourceFor() hands out a new source, which the provider
/// owns and frees in collect() only under the rule in SoundSource.h (1 s of
/// engine audio without a read, no pending voice with its key).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "bigtop/Engine.h"
#include "bigtop/Show.h"
#include "bigtop/ShowController.h"

namespace bigtop::io {

/// @brief Length of each sound's head in RAM, in seconds from its start point
///        (about 1 MB per head at 48 kHz, 16-bit stereo).
inline constexpr double kHeadSeconds = 5.0;
/// @brief How far each tail reader buffers ahead of the playing position, in seconds.
inline constexpr double kTailReadAheadSeconds = 5.0;
/// @brief Engine audio, in seconds, a handed-out source must go unread before it may be freed.
inline constexpr double kIdleFreeSeconds = 1.0;

/// @brief The rise that keeps late tail data from clicking: real data that
///        follows silence (filled in for data the reader did not have yet)
///        rises linearly from 0 to full over a fixed number of frames.
///
/// The count is per frame, so one rise may span several reads. A fresh
/// object is not rising: a fresh start plays at full value at once.
/// @note Audio thread after construction; no allocation, no locks.
class DataRise {
public:
    /// @brief Creates a rise that is not running.
    /// @param riseFrames Frames from 0 to full; at least 1.
    explicit DataRise(int riseFrames) noexcept : riseFrames_(riseFrames), done_(riseFrames) {}

    /// @brief Real data, all of it delivered: scaled while a rise runs, else unchanged.
    /// @param left [in,out] Left samples.
    /// @param right [in,out] Right samples.
    /// @param n Frame count.
    void data(float* left, float* right, int n) noexcept;

    /// @brief A chunk the reader could not fully deliver: the missing blocks
    ///        are zeros, so it is some mix of real data and silence.
    ///
    /// Leading zeros stay zeros; real data after them rises from 0 (so a seek
    /// whose first chunk is only partly loaded still enters from 0). A chunk
    /// that starts with data continues the rise or the full data before it,
    /// never jumps. A chunk that ends in silence makes the next data rise again.
    /// @param left [in,out] Left samples.
    /// @param right [in,out] Right samples.
    /// @param n Frame count; at least 1.
    void late(float* left, float* right, int n) noexcept;

private:
    int riseFrames_;
    int done_;  // frames of the rise output since the last silence; riseFrames_ = not rising
};

/// @brief Streams the show's audio files: shared heads in RAM, a tail reader per voice start.
/// @note Control thread for every member except dropouts(). Not copyable.
class StreamingSourceProvider : public SourceProvider {
public:
    /// @brief Creates a provider with no show.
    /// @param showDir [in] The show folder; AudioFile::path is relative to it.
    /// @param readThread [in] Background thread the tail readers load on; must
    ///        outlive the provider. It need not be running yet (tails then stay
    ///        silent, heads still play).
    /// @note Control thread.
    StreamingSourceProvider(juce::File showDir, juce::TimeSliceThread& readThread);

    /// @brief Frees every source and head.
    /// @warning Only once the engine no longer plays any of them (closing a
    ///          show waits for silence first).
    ~StreamingSourceProvider() override;

    /// @brief Makes the heads match a show.
    /// @param show [in] The show; its step sounds (at their start points) and
    ///        pads (from frame 0) say which heads are needed.
    /// @note Loads (blocking) the head of every distinct (file, start frame)
    ///       that is not loaded yet; drops heads no longer used, whose memory
    ///       goes once no handed-out source uses them. A file that cannot be
    ///       opened, or is not at 48 kHz, has no head. Call again after an edit
    ///       or a finished import. Control thread.
    void setShow(const Show& show);

    /// @brief A new source for one voice start (see SourceProvider::sourceFor).
    /// @param key [in] The voice the source is for; collect() keeps the source
    ///        while a pending voice has this key.
    /// @param file [in] AudioFile::id.
    /// @param startFrame [in] The sound's start point; must be one setShow() saw.
    /// @return A source sharing the head and with a fresh tail reader (timeout
    ///         0, kTailReadAheadSeconds) pointed at the end of the head; nullptr
    ///         when there is no head for (file, startFrame), the file cannot be
    ///         reopened or changed length, or on any exception.
    /// @note Opens the file (blocking, but no decoding). Control thread.
    SoundSource* sourceFor(VoiceKey key, FileId file, int64_t startFrame) noexcept override;

    /// @brief Frees handed-out sources nothing can read any more.
    /// @param snap [in] Engine::snapshot() taken for this call:
    ///        `collect(engine.snapshot())`. Never a snapshot taken before a
    ///        sourceFor(): its older framesProcessed would start the new
    ///        source's idle clock too early.
    /// @note Frees a source once its read count has not changed for at least
    ///       kIdleFreeSeconds * 48000 frames of snap.framesProcessed, no
    ///       pending voice in @p snap has its key and, if it was never read,
    ///       no voice at all has its key (see SoundSource.h). A source's clock starts at
    ///       the first collect() after it was handed out. While no audio runs,
    ///       framesProcessed stands still and nothing is freed. While
    ///       snap.paused nothing is freed either (paused voices read nothing
    ///       but still hold their sources) and every idle clock restarts, so
    ///       idle time counts from the pause's end. Call several
    ///       times per second. Control thread.
    void collect(const Snapshot& snap);

    /// @brief Reads that returned silence because tail data was late (or unreadable).
    /// @return Count since construction; the 1-frame reads that re-point a
    ///         tail are not counted.
    /// @note Any thread.
    uint32_t dropouts() const noexcept;

    /// @brief Sources handed out and not yet freed (tests, status).
    /// @return The count.
    /// @note Control thread.
    size_t liveSources() const noexcept;

    /// @brief Whether a file can play: it has at least one head.
    /// @param file [in] AudioFile::id.
    /// @return False for a file missing, unreadable, or used by no step or pad
    ///         (the UI shows its steps red).
    /// @note Control thread.
    bool fileUsable(FileId file) const;

    /// @brief Length of a usable file, for the running-sounds display.
    /// @param file [in] AudioFile::id.
    /// @return Frames at 48 kHz in the whole file; 0 when !fileUsable(file).
    /// @note Read from the loaded head, so no disk access. Control thread.
    int64_t fileLengthFrames(FileId file) const;

private:
    struct Head;
    class StreamingSource;
    // A handed-out source and what collect() knows about its reads.
    struct Live {
        std::unique_ptr<StreamingSource> source;
        VoiceKey key = 0;
        bool seen = false;         // collect() has recorded a read count yet
        uint32_t reads = 0;        // read count at the last change collect() saw
        int64_t sinceFrame = 0;    // snap.framesProcessed when that change was seen
    };

    std::shared_ptr<const Head> loadHead(const juce::File& file, int64_t startFrame);

    juce::File showDir_;
    juce::TimeSliceThread& readThread_;
    juce::AudioFormatManager formats_;
    std::map<std::pair<FileId, int64_t>, std::shared_ptr<const Head>> heads_;
    // Declared before live_: sources count into it until they are destroyed.
    std::atomic<uint32_t> dropouts_{0};
    std::vector<Live> live_;
};

}  // namespace bigtop::io
