#include "bigtop/Gain.h"

#include <cmath>

namespace bigtop {

double dbToGain(double db) {
    // Special-case silence: pow(10, -5) is tiny but not zero, and a sound that
    // is "off" should contribute exactly nothing to the mix.
    return db <= kSilenceDb ? 0.0 : std::pow(10.0, db / 20.0);
}

double gainToDb(double gain) {
    // log10(0) is -infinity; map it (and negatives) to the silence floor.
    if (gain <= 0.0) return kSilenceDb;
    const double db = 20.0 * std::log10(gain);
    // Clamp so tiny gains agree with dbToGain's floor.
    return db < kSilenceDb ? kSilenceDb : db;
}

}  // namespace bigtop
