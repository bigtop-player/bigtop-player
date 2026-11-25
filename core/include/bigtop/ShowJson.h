/// @file ShowJson.h
/// @brief Reading and writing a Show as JSON text (the show file format).
///
/// Sits between the file on disk and the plain structs in Show.h; nlohmann/json
/// is an implementation detail and deliberately not exposed here.
///
/// Versioning: files carry "formatVersion". This build reads exactly version 1.
/// A missing or different version is rejected rather than guessed at, because
/// silently misreading a newer file could drop data and then overwrite it on
/// save.
///
/// @note Control thread only: parsing allocates and throws, never call from
///       the audio thread.
#pragma once
#include <stdexcept>
#include <string>
#include <string_view>

#include "bigtop/Show.h"

namespace bigtop {

/// @brief Thrown by fromJson() for any file that is not a valid show.
///
/// The message names the offending field. Callers can show it to the user and
/// keep their old show.
struct ShowFormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @brief Serialises a show to pretty-printed JSON text (formatVersion included).
/// @param show The show to write.
/// @return The JSON document as text.
/// @note Control thread only.
std::string toJson(const Show& show);

/// @brief Parses show text.
///
/// Missing optional fields take their defaults and unknown fields are ignored
/// (so newer minor additions don't break older readers). Never crashes on bad
/// input.
/// @param text The JSON document.
/// @return The parsed show.
/// @throws ShowFormatError for invalid JSON, formatVersion missing or not 1,
///         wrong field types, a null where a number is required, non-finite
///         numbers, timecodeFps other than 24/25/30, step id 0, step ids or
///         change targets above kMaxStepId, duplicate step ids or file ids,
///         an Others kind of "level", more than kMaxPads (24) pads, a pad grid outside 1..6 x 1..6 or above 24 cells, and a
///         malformed timecode.
/// @note Control thread only: allocates and throws.
Show fromJson(std::string_view text);

}  // namespace bigtop
