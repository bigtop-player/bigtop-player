// StreamingSource.cpp: heads in RAM, tails through BufferingAudioReader,
// and the freeing rule of SoundSource.h.
#include "bigtop/io/StreamingSource.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "bigtop/io/AudioOutput.h"

namespace bigtop::io {

namespace {

constexpr float kFromInt16 = 1.0f / 32768.0f;
/// Frames over which data rises after a stretch of silence (kDeclickSeconds).
constexpr int kRiseFrames = static_cast<int>(kDeclickSeconds * kEngineSampleRate);

}  // namespace

void DataRise::data(float* left, float* right, int n) noexcept {
    for (int i = 0; i < n && done_ < riseFrames_; ++i, ++done_) {
        const float g = static_cast<float>(done_) / static_cast<float>(riseFrames_);
        left[i] *= g;
        right[i] *= g;
    }
}

void DataRise::late(float* left, float* right, int n) noexcept {
    int z = 0;
    while (z < n && left[z] == 0.0f && right[z] == 0.0f) ++z;
    // Only silence restarts the rise; data straight after data carries on
    // (a running rise continues, full data stays full).
    if (z > 0) done_ = 0;
    data(left + z, right + z, n - z);
    if (left[n - 1] == 0.0f && right[n - 1] == 0.0f) done_ = 0;
}

/// @brief The first kHeadSeconds from one start point of one file, 16-bit.
/// @note Immutable once built, so any number of voices read it on the audio
///       thread without locks.
struct StreamingSourceProvider::Head {
    juce::File file;                ///< @brief The FLAC it came from (tail readers reopen it).
    int64_t length = 0;             ///< @brief Frames in the whole file.
    int64_t start = 0;              ///< @brief File frame of left[0] and right[0].
    std::vector<int16_t> left;      ///< @brief Left channel samples.
    std::vector<int16_t> right;     ///< @brief Right channel samples.
    /// @brief File frame after the last head frame.
    /// @return start + number of head frames.
    int64_t end() const noexcept { return start + static_cast<int64_t>(left.size()); }
};

/// @brief What the engine plays: frames in [head start, head end) from the
///        head, every other frame from the tail reader.
class StreamingSourceProvider::StreamingSource final : public SoundSource {
public:
    /// @brief Combines a head and a tail reader; points the tail at the head's end.
    /// @param head [in] The shared head of the sound's start point.
    /// @param tail [in] Reader for the rest of the file; null when the head
    ///        reaches the end of the file.
    /// @param dropouts [in] The provider's counter; must outlive the source.
    /// @note Control thread.
    StreamingSource(std::shared_ptr<const Head> head, std::unique_ptr<juce::BufferingAudioReader> tail,
                    std::atomic<uint32_t>& dropouts)
        : head_(std::move(head)), tail_(std::move(tail)), dropouts_(dropouts) {
        // Points the read-ahead at the end of the head now, on the control
        // thread, so it has the head's whole playing time to fill. The result
        // is irrelevant (and silence if the thread has not loaded it yet).
        if (tail_) readTail(head_->end(), hint_, hint_ + 1, 1);
    }

    int64_t lengthFrames() const noexcept override { return head_->length; }

    void read(int64_t startFrame, float* left, float* right, int frames) noexcept override {
        reads_.fetch_add(1);
        const int64_t hs = head_->start, he = head_->end();
        bool fromHead = false;
        while (frames > 0) {
            int n;
            if (startFrame >= hs && startFrame < he) {
                n = static_cast<int>(std::min<int64_t>(frames, he - startFrame));
                const auto o = static_cast<size_t>(startFrame - hs);
                for (int i = 0; i < n; ++i) {
                    left[i] = head_->left[o + static_cast<size_t>(i)] * kFromInt16;
                    right[i] = head_->right[o + static_cast<size_t>(i)] * kFromInt16;
                }
                fromHead = true;
                rise_.data(left, right, n);
            } else {
                // Before the head (not reached by the engine, which clamps to
                // the start point) or after it: the tail. Past the file's end
                // the reader writes zeros, so n never needs to stop there.
                n = startFrame < hs ? static_cast<int>(std::min<int64_t>(frames, hs - startFrame)) : frames;
                if (!readTail(startFrame, left, right, n)) {
                    dropouts_.fetch_add(1);
                    rise_.late(left, right, n);
                } else {
                    rise_.data(left, right, n);
                }
            }
            startFrame += n;
            left += n;
            right += n;
            frames -= n;
        }
        // After a loop wrap or a seek back the read-ahead still sits where the
        // tail was last read; one frame at the head's end moves it back, so the
        // tail is ready again when playback leaves the head. Not a dropout.
        if (fromHead && tail_ && lastTailFrame_ != he) readTail(he, hint_, hint_ + 1, 1);
    }

    /// @brief Times read() was called; collect() frees the source once it stops changing.
    /// @return The count since construction.
    /// @note Any thread.
    uint32_t reads() const noexcept { return reads_.load(); }

private:
    // False when the reader had no data for some of the frames (they are zeros).
    bool readTail(int64_t startFrame, float* left, float* right, int frames) noexcept {
        if (!tail_) {
            std::fill(left, left + frames, 0.0f);
            std::fill(right, right + frames, 0.0f);
            return true;
        }
        // BufferingAudioReader keeps its read-ahead at the start of the last read.
        lastTailFrame_ = startFrame;
        // The reader produces floats; JUCE's int* read path carries them
        // unconverted and copies a mono file's channel to the right side.
        int* const channels[2] = {reinterpret_cast<int*>(left), reinterpret_cast<int*>(right)};
        // shortcut: BufferingAudioReader takes a CriticalSection per read on the audio thread; replace with a lock-free block cache if dropouts or seek clicks show up in rehearsal
        return tail_->read(channels, 2, startFrame, frames, true);
    }

    std::shared_ptr<const Head> head_;
    std::unique_ptr<juce::BufferingAudioReader> tail_;
    std::atomic<uint32_t>& dropouts_;
    std::atomic<uint32_t> reads_{0};
    // Audio thread only after construction (the engine queue hands the
    // pointer over with a release/acquire, so the constructor's write is seen).
    int64_t lastTailFrame_ = 0;
    DataRise rise_{kRiseFrames};  // fades real data in after late-data silence
    float hint_[2] = {};  // scratch for the discarded 1-frame reads (left, right)
};

StreamingSourceProvider::StreamingSourceProvider(juce::File showDir, juce::TimeSliceThread& readThread)
    : showDir_(std::move(showDir)), readThread_(readThread) {
    formats_.registerFormat(new juce::FlacAudioFormat, true);
}

StreamingSourceProvider::~StreamingSourceProvider() = default;

std::shared_ptr<const StreamingSourceProvider::Head> StreamingSourceProvider::loadHead(const juce::File& file,
                                                                                      int64_t startFrame) {
    std::unique_ptr<juce::AudioFormatReader> r(formats_.createReaderFor(file));
    // A file at another rate would play at the wrong speed: treat it as unusable.
    if (r == nullptr || r->sampleRate != kEngineSampleRate || r->lengthInSamples < 0 || r->numChannels < 1)
        return nullptr;
    auto head = std::make_shared<Head>();
    head->file = file;
    head->length = r->lengthInSamples;
    head->start = std::min(startFrame, head->length);  // a start past the end gets an empty head
    const int n = static_cast<int>(
        std::min<int64_t>(std::llround(kHeadSeconds * kEngineSampleRate), head->length - head->start));
    juce::AudioBuffer<float> buf(2, n);
    // useRightChan true: a mono file fills both channels.
    if (!r->read(&buf, 0, n, head->start, true, true)) return nullptr;
    head->left.resize(static_cast<size_t>(n));
    head->right.resize(static_cast<size_t>(n));
    const auto toInt16 = [](float x) {
        return static_cast<int16_t>(std::clamp(static_cast<int>(std::lrint(x * 32768.0f)), -32768, 32767));
    };
    for (int i = 0; i < n; ++i) {
        head->left[static_cast<size_t>(i)] = toInt16(buf.getSample(0, i));
        head->right[static_cast<size_t>(i)] = toInt16(buf.getSample(1, i));
    }
    return head;
}

void StreamingSourceProvider::setShow(const Show& show) {
    std::set<std::pair<FileId, int64_t>> wanted;
    for (const Step& step : show.steps)
        // The controller's own conversion, so heads have exactly the start
        // frames sourceFor() is asked for.
        if (step.sound) wanted.insert({step.sound->file, secondsToFrames(step.sound->startSeconds, kEngineSampleRate)});
    for (const Pad& pad : show.pads)
        if (pad.file) wanted.insert({*pad.file, 0});

    // Sources hold their head through a shared_ptr, so dropping it here frees
    // the memory only once the last source using it is freed.
    std::erase_if(heads_, [&](const auto& kv) { return !wanted.contains(kv.first); });
    for (const auto& key : wanted) {
        if (heads_.contains(key)) continue;
        const auto f = std::find_if(show.files.begin(), show.files.end(),
                                    [&](const AudioFile& a) { return a.id == key.first; });
        if (f == show.files.end()) continue;
        if (auto head = loadHead(showDir_.getChildFile(juce::String(f->path)), key.second)) heads_.emplace(key, std::move(head));
    }
}

SoundSource* StreamingSourceProvider::sourceFor(VoiceKey key, FileId file, int64_t startFrame) noexcept {
    try {
        const auto it = heads_.find({file, startFrame});
        if (it == heads_.end()) return nullptr;
        const std::shared_ptr<const Head>& head = it->second;
        std::unique_ptr<juce::BufferingAudioReader> tail;
        if (head->end() < head->length) {
            std::unique_ptr<juce::AudioFormatReader> r(formats_.createReaderFor(head->file));
            // Gone or replaced since the head was loaded: the tail would not fit the head.
            if (r == nullptr || r->lengthInSamples != head->length) return nullptr;
            tail = std::make_unique<juce::BufferingAudioReader>(
                r.release(), readThread_, static_cast<int>(kTailReadAheadSeconds * kEngineSampleRate));
            tail->setReadTimeout(0);  // never wait on the audio thread: late data reads as silence
        }
        live_.push_back({std::make_unique<StreamingSource>(head, std::move(tail), dropouts_), key});
        return live_.back().source.get();
    } catch (...) {
        return nullptr;
    }
}

void StreamingSourceProvider::collect(const Snapshot& snap) {
    const auto idleFrames = std::llround(kIdleFreeSeconds * kEngineSampleRate);
    std::erase_if(live_, [&](Live& e) {
        const uint32_t reads = e.source->reads();
        // Paused voices hold their sources without reading them. Restart the
        // idle clock on every look so the first block after the resume (which
        // reads them) comes before any freeing; nothing is freed while paused.
        if (snap.paused || !e.seen || reads != e.reads) {
            // Read since the last look (or never looked at): the idle clock restarts.
            e.seen = true;
            e.reads = reads;
            e.sinceFrame = snap.framesProcessed;
            return false;
        }
        if (snap.framesProcessed - e.sinceFrame < idleFrames) return false;
        // A voice in its start delay holds the source without reading it, and
        // one whose delay ended exactly at a block end shows as not pending
        // before its first read: keep a never-read source while its key exists.
        const int n = std::clamp(snap.numVoices, 0, kMaxVoices);
        return std::none_of(snap.voices, snap.voices + n, [&](const VoiceState& v) {
            return v.key == e.key && (v.pending || reads == 0);
        });
    });
}

uint32_t StreamingSourceProvider::dropouts() const noexcept { return dropouts_.load(); }

size_t StreamingSourceProvider::liveSources() const noexcept { return live_.size(); }

bool StreamingSourceProvider::fileUsable(FileId file) const {
    const auto it = heads_.lower_bound({file, INT64_MIN});
    return it != heads_.end() && it->first.first == file;
}

int64_t StreamingSourceProvider::fileLengthFrames(FileId file) const {
    const auto it = heads_.lower_bound({file, INT64_MIN});
    return it != heads_.end() && it->first.first == file ? it->second->length : 0;
}

}  // namespace bigtop::io
