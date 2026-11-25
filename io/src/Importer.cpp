// Importer.cpp: WAV/FLAC/MP3 -> 48 kHz stereo 24-bit FLAC, SHA-256, target names.
#include "bigtop/io/Importer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include <CDSPResampler.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

namespace bigtop::io {

namespace {

constexpr double kShowRate = 48000.0;
constexpr int kShowChannels = 2;
constexpr int kShowBits = 24;
// Header values outside this range are junk, and a 1 s chunk of them could
// be a huge allocation.
constexpr double kMinRate = 8000.0, kMaxRate = 768000.0;
// Random bytes can pass as a few MP3 frames (measured: 5 frames, 0.5 s, of a
// 10.9 s estimate); an MP3 ending this early and this short is refused as junk.
constexpr double kMp3MinFractionOfEstimate = 0.25;
constexpr double kMp3MinSecondsIfEarly = 2.0;
// A decoder (minimp3-class and LAME's own) adds this many frames of delay on
// top of the encoder delay stored in the tag.
constexpr int kMp3DecoderDelay = 529;

/// Frames to cut from an MP3's decoded audio, from its LAME-style tag.
struct Mp3Trim {
    int skipStart = 0;  ///< Decoded frames to skip at the start (encoder delay + 529).
    int dropEnd = 0;    ///< Decoded frames to drop at the end (padding - 529, not below 0).
};

/// Reads the encoder delay and padding from the Xing/Info header of the first
/// MP3 frame. Layout (LAME tag spec): the first frame holds, after its 4-byte
/// header and the side info (32 bytes MPEG-1 stereo, 17 MPEG-1 mono or MPEG-2
/// stereo, 9 MPEG-2 mono; +2 if CRC protected), "Xing" or "Info", a 32-bit
/// flags field, then the optional frames (4), bytes (4), TOC (100) and quality
/// (4) fields the flags announce, then the extension: a 9-byte encoder name
/// ("LAME3.99r", ffmpeg writes "Lavc63.1."), 12 bytes of other data, and at
/// byte 21 three bytes holding delay (high 12 bits) and padding (low 12 bits).
/// Returns zeros when there is no such tag (nothing is trimmed then).
Mp3Trim readMp3Trim(const juce::File& file) {
    juce::FileInputStream in(file);
    if (!in.openedOk()) return {};
    // An ID3v2 tag (often with cover art, tens of KB) comes first: read its
    // size, then look at the window starting at the first frame, not the file.
    unsigned char id3[10] = {};
    if (in.read(id3, 10) != 10) return {};
    juce::int64 start = 0;
    if (id3[0] == 'I' && id3[1] == 'D' && id3[2] == '3')
        // syncsafe size + 10-byte header (+10 more if the footer flag is set)
        start = 10 + ((juce::int64{id3[6]} & 0x7f) << 21 | (juce::int64{id3[7]} & 0x7f) << 14 | (juce::int64{id3[8]} & 0x7f) << 7 | (id3[9] & 0x7f))
                + ((id3[5] & 0x10) ? 10 : 0);
    if (!in.setPosition(start)) return {};
    juce::MemoryBlock block(4096);
    block.setSize(static_cast<size_t>(in.read(block.getData(), 4096)));
    const auto* b = static_cast<const unsigned char*>(block.getData());
    const size_t size = block.getSize();
    size_t f = 0;  // start of the first frame; tags may be followed by zero padding
    while (f < size && b[f] == 0) ++f;
    if (f + 4 > size || b[f] != 0xff || (b[f + 1] & 0xe0) != 0xe0) return {};
    if (((b[f + 1] >> 1) & 3) != 1) return {};  // layer bits 01 = Layer III; the tag layout is MP3's
    const bool mpeg1 = ((b[f + 1] >> 3) & 3) == 3, mono = ((b[f + 3] >> 6) & 3) == 3, crc = (b[f + 1] & 1) == 0;
    const size_t tag = f + 4 + (mpeg1 ? (mono ? 17u : 32u) : (mono ? 9u : 17u)) + (crc ? 2u : 0u);
    if (tag + 8 > size || (std::memcmp(b + tag, "Xing", 4) != 0 && std::memcmp(b + tag, "Info", 4) != 0)) return {};
    const unsigned flags = (unsigned{b[tag + 4]} << 24) | (unsigned{b[tag + 5]} << 16) | (unsigned{b[tag + 6]} << 8) | b[tag + 7];
    const size_t ext = tag + 8 + ((flags & 1) ? 4 : 0) + ((flags & 2) ? 4 : 0) + ((flags & 4) ? 100 : 0) + ((flags & 8) ? 4 : 0);
    // Xing without the LAME extension has no encoder name here; accept only
    // the two writers known to use this layout.
    if (ext + 24 > size || (std::memcmp(b + ext, "LAME", 4) != 0 && std::memcmp(b + ext, "Lavc", 4) != 0)) return {};
    const int delay = (b[ext + 21] << 4) | (b[ext + 22] >> 4);
    const int padding = ((b[ext + 22] & 0x0f) << 8) | b[ext + 23];
    return {delay + kMp3DecoderDelay, std::max(0, padding - kMp3DecoderDelay)};
}

}  // namespace

bool isImportable(const juce::File& file) { return file.hasFileExtension("wav;flac;mp3"); }

std::string hashFile(const juce::File& file) {
    juce::FileInputStream in(file);
    if (!file.existsAsFile() || !in.openedOk()) return {};
    const juce::SHA256 sha(in);
    // SHA256 stops quietly at a read error; a hash of part of the file would
    // wrongly match or miss other files.
    if (in.getStatus().failed() || in.getPosition() != file.getSize()) return {};
    return sha.toHexString().toStdString();
}

juce::File chooseAudioTarget(const juce::File& audioDir, const juce::String& stem, const Show& show) {
    const auto showDir = audioDir.getParentDirectory();
    const auto taken = [&](const juce::File& f) {
        if (f.exists()) return true;
        return std::any_of(show.files.begin(), show.files.end(), [&](const AudioFile& a) {
            return showDir.getChildFile(juce::String::fromUTF8(a.path.c_str())).getFullPathName().equalsIgnoreCase(f.getFullPathName());
        });
    };
    for (int n = 1;; ++n) {
        const auto f = audioDir.getChildFile(n == 1 ? stem + ".flac" : stem + " (" + juce::String(n) + ").flac");
        if (!taken(f)) return f;
    }
}

ImportResult convertToShowAudio(const juce::File& source, const juce::File& target) {
    const auto tmp = target.withFileExtension(".flac.tmp");
    std::unique_ptr<juce::OutputStream> out;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    // Every failure path goes through here so no partial file survives; the
    // file is closed first because Windows cannot delete an open file.
    const auto fail = [&](std::string why) {
        writer.reset();
        out.reset();
        tmp.deleteFile();
        return ImportResult{false, std::move(why)};
    };

    // moveFileTo would silently replace it; chooseAudioTarget never picks one.
    if (target.exists()) return ImportResult{false, "The target already exists: " + target.getFullPathName().toStdString()};

    juce::AudioFormatManager formats;  // the only formats the owner may drop
    formats.registerFormat(new juce::WavAudioFormat, true);
    formats.registerFormat(new juce::FlacAudioFormat, false);
    formats.registerFormat(new juce::MP3AudioFormat, false);
    const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(source));
    if (reader == nullptr) return fail("Not a readable WAV, FLAC or MP3 file: " + source.getFileName().toStdString());
    const double rate = reader->sampleRate;
    // Shrinks when an MP3 ends before JUCE's length estimate (see readMp3 below).
    juce::int64 inLen = reader->lengthInSamples;
    const bool isMp3 = reader->getFormatName() == juce::MP3AudioFormat().getFormatName();
    // Gapless: reads start `startOffset` frames into the decoded audio and the
    // padding at the end is left unread. If the trim would eat everything the
    // tag is not believable, so the file is imported untrimmed.
    juce::int64 startOffset = 0;
    if (isMp3) {
        const auto trim = readMp3Trim(source);
        if (inLen > static_cast<juce::int64>(trim.skipStart) + trim.dropEnd) {
            startOffset = trim.skipStart;
            inLen -= startOffset + trim.dropEnd;
        }
    }
    const juce::int64 estimatedLen = inLen;
    if (inLen <= 0 || reader->numChannels == 0 || !(rate >= kMinRate && rate <= kMaxRate))
        return fail("The file contains no usable audio: " + source.getFileName().toStdString());

    auto stream = std::make_unique<juce::FileOutputStream>(tmp);
    if (stream->failedToOpen()) return fail("Cannot write " + tmp.getFullPathName().toStdString());
    stream->setPosition(0);
    stream->truncate();  // a leftover tmp of a crashed import must not leak into this one
    out = std::move(stream);
    writer = juce::FlacAudioFormat().createWriterFor(
        out, juce::AudioFormatWriterOptions{}.withSampleRate(kShowRate).withNumChannels(kShowChannels).withBitsPerSample(kShowBits));
    if (writer == nullptr) return fail("Cannot create the FLAC encoder");

    const bool resample = rate != kShowRate;
    const int chunk = static_cast<int>(rate);  // 1 s of input
    const int inCh = static_cast<int>(std::min<unsigned>(reader->numChannels, kShowChannels));
    // Same duration as the source: r8brain's output is latency-compensated,
    // so after flushing exactly this many frames it is time-aligned.
    const auto outLen = [&] { return resample ? std::llround(static_cast<double>(inLen) * kShowRate / rate) : inLen; };

    std::vector<std::unique_ptr<r8b::CDSPResampler24>> resamplers;
    if (resample)
        for (int ch = 0; ch < inCh; ++ch) resamplers.push_back(std::make_unique<r8b::CDSPResampler24>(rate, kShowRate, chunk));
    const int maxOut = resample ? resamplers[0]->getMaxOutLen(chunk) : chunk;

    juce::AudioBuffer<float> in(inCh, chunk), outBuf(kShowChannels, maxOut);
    std::vector<double> dbl(static_cast<size_t>(chunk));
    juce::int64 readPos = 0, written = 0;
    // Bounds the zero-feeding flush; r8brain's latency is far below 10 s.
    int flushChunks = 0;

    // Reads n frames of an MP3 one MP3 frame (1152) at a time and returns how
    // many decoded. JUCE's MP3 length is an estimate, so reading past the
    // real end is normal and must not refuse a playable file; the piece in
    // which the end falls is dropped (at most 26 ms), since JUCE does not
    // say how much of it was real audio and how much its zero fill.
    // shortcut: JUCE estimates MP3 length from the first frame when there is no Xing/Info tag, so a header-less VBR file whose estimate is too short imports with its ending cut, and a tag-less VBR clip under 2 s whose estimate is far too long is refused as junk; upgrade to a decoder with exact VBR length if rehearsal shows it
    const auto readMp3 = [&](int n) {
        int got = 0;
        while (got < n) {
            const int piece = std::min(1152, n - got);
            float* dest[kShowChannels] = {};
            for (int ch = 0; ch < inCh; ++ch) dest[ch] = in.getWritePointer(ch, got);
            if (!reader->read(dest, inCh, startOffset + readPos + got, piece)) break;
            got += piece;
        }
        return got;
    };

    while (written < outLen()) {
        int n = static_cast<int>(std::min<juce::int64>(chunk, inLen - readPos));
        if (n > 0) {
            if (isMp3) {
                const int got = readMp3(n);
                if (got < n) {
                    inLen = readPos + got;  // the real end
                    const bool junk = inLen == 0
                        || (static_cast<double>(inLen) < kMp3MinFractionOfEstimate * static_cast<double>(estimatedLen)
                            && static_cast<double>(inLen) < kMp3MinSecondsIfEarly * rate);
                    if (junk) return fail("Nothing in the file decodes as audio: " + source.getFileName().toStdString());
                }
                n = got;
            } else if (!reader->read(in.getArrayOfWritePointers(), inCh, readPos, n)) {
                // A WAV read error means a damaged file; importing the zeros
                // JUCE fills in would silently replace music with silence.
                return fail("The file is damaged or cut short: " + source.getFileName().toStdString());
            }
            readPos += n;
            if (written >= outLen()) break;  // an MP3 that ended exactly where the last chunk did
        }
        if (n == 0 && (!resample || ++flushChunks > 10)) {
            return fail("Internal error: resampler did not produce the expected length");
        }

        int produced = n;
        if (resample) {
            const int len = n > 0 ? n : chunk;  // past the end: feed silence to flush the filter tail
            for (int ch = 0; ch < inCh; ++ch) {
                if (n > 0) std::copy(in.getReadPointer(ch), in.getReadPointer(ch) + n, dbl.begin());
                else std::fill(dbl.begin(), dbl.end(), 0.0);
                double* op = nullptr;
                // Same input lengths into identical resamplers give the same output length.
                produced = resamplers[static_cast<size_t>(ch)]->process(dbl.data(), len, op);
                std::transform(op, op + produced, outBuf.getWritePointer(ch), [](double v) { return static_cast<float>(v); });
            }
        } else {
            for (int ch = 0; ch < inCh; ++ch) outBuf.copyFrom(ch, 0, in, ch, 0, n);
        }
        if (inCh == 1) outBuf.copyFrom(1, 0, outBuf, 0, 0, produced);  // mono plays on both sides

        const int toWrite = static_cast<int>(std::min<juce::int64>(produced, outLen() - written));
        if (toWrite > 0 && !writer->writeFromAudioSampleBuffer(outBuf, 0, toWrite))
            return fail("Cannot write " + tmp.getFullPathName().toStdString() + " (disk full?)");
        written += toWrite;
    }

    writer.reset();  // finishes the FLAC stream (header with length) and closes the file
    out.reset();
    // JUCE cannot report errors on close (e.g. disk full at the end), so the
    // finished file is read back before it gets the real name.
    {
        const std::unique_ptr<juce::AudioFormatReader> check(juce::FlacAudioFormat().createReaderFor(new juce::FileInputStream(tmp), true));
        if (check == nullptr || check->lengthInSamples != written || check->sampleRate != kShowRate || check->numChannels != kShowChannels)
            return fail("The written file did not read back correctly (disk full?): " + tmp.getFullPathName().toStdString());
    }
    if (!tmp.moveFileTo(target)) return fail("Cannot rename to " + target.getFullPathName().toStdString());
    return {true, {}};
}

}  // namespace bigtop::io
