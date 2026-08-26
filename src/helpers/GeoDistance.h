#pragma once

#include <stdint.h>
#include <math.h>

// metres per micro-degree of latitude (WGS84 mean, good to ~0.3% anywhere)
#define MICRODEG_TO_M      0.111320f
#define DEG_TO_RAD         0.017453292f

#define LON_FULL_E6        360000000
#define LON_HALF_E6        180000000

/**
 * \brief  "Have we moved at least this far?", and nothing else.
 *
 * Equirectangular, which is ample for a threshold test and costs no sqrt and no per-call
 * trig: the only question asked is whether a point is inside or outside a small circle,
 * and over the few kilometres that matters for the answer is the same one a great-circle
 * formula gives. Shared so that the advert scheduler, the tracking sampler and the
 * history downsampler all mean the same thing by "moved 100 metres" - three copies of
 * this arithmetic would eventually be three different distances.
 */
class GeoDistance {
public:
  /**
   * \brief  The longitude scale at a latitude: east/west degrees shrink towards the poles.
   *         Floored, so east/west cannot collapse to nothing at the pole itself.
   */
  static float cosLatFor(int32_t lat_e6) {
    float c = cosf((float)lat_e6 * 1.0e-6f * DEG_TO_RAD);
    return (c < 0.01f) ? 0.01f : c;
  }

  /**
   * \param  cos_lat  from cosLatFor(), passed in so a caller polling at a fixed point
   *                  can compute it once rather than on every comparison.
   * \returns  true when the two points are at least 'metres' apart. A threshold of 0 is
   *           "any distance at all", which is never true of a point against itself.
   */
  static bool movedAtLeast(int32_t lat1_e6, int32_t lon1_e6,
                           int32_t lat2_e6, int32_t lon2_e6,
                           float cos_lat, uint32_t metres) {
    int32_t dlat = lat2_e6 - lat1_e6;
    int32_t dlon = lon2_e6 - lon1_e6;

    // take the short way round, so a step across the antimeridian is not read as half a
    // planet of travel
    if (dlon > LON_HALF_E6) dlon -= LON_FULL_E6;
    else if (dlon < -LON_HALF_E6) dlon += LON_FULL_E6;

    float dy = (float)dlat * MICRODEG_TO_M;
    float dx = (float)dlon * MICRODEG_TO_M * cos_lat;
    float thresh = (float)metres;

    return (dx * dx + dy * dy) >= (thresh * thresh);
  }

  /** \brief  The same test, working out the longitude scale for itself. */
  static bool movedAtLeast(int32_t lat1_e6, int32_t lon1_e6,
                           int32_t lat2_e6, int32_t lon2_e6, uint32_t metres) {
    return movedAtLeast(lat1_e6, lon1_e6, lat2_e6, lon2_e6, cosLatFor(lat2_e6), metres);
  }
};
