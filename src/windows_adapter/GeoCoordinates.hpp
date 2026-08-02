#pragma once

#include "AfsimNs3Protocol.hpp"

namespace afsim_ns3
{

struct GeoPoint
{
    double latitudeDeg{};
    double longitudeDeg{};
    double altitudeM{};
};

class LocalEnuFrame
{
  public:
    explicit LocalEnuFrame(const GeoPoint& origin);

    Position ToEnu(const GeoPoint& point) const;
    const GeoPoint& Origin() const;

  private:
    GeoPoint mOrigin;
    double mOriginEcef[3]{};
    double mSinLatitude{};
    double mCosLatitude{};
    double mSinLongitude{};
    double mCosLongitude{};
};

// AFSIM heading is clockwise from north. Positive pitch produces positive up.
Velocity VelocityFromSpeed(
    double speedMps,
    double headingDeg,
    double pitchDeg);

} // namespace afsim_ns3
