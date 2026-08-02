#include "GeoCoordinates.hpp"

#include <cmath>
#include <stdexcept>

namespace afsim_ns3
{
namespace
{

constexpr double Pi = 3.14159265358979323846;
constexpr double Wgs84SemiMajorM = 6378137.0;
constexpr double Wgs84EccentricitySquared =
    6.6943799901413165e-3;

double
Radians(double degrees)
{
    return degrees * Pi / 180.0;
}

void
Validate(const GeoPoint& point)
{
    if (!std::isfinite(point.latitudeDeg) ||
        !std::isfinite(point.longitudeDeg) ||
        !std::isfinite(point.altitudeM) ||
        point.latitudeDeg < -90.0 || point.latitudeDeg > 90.0 ||
        point.longitudeDeg < -180.0 || point.longitudeDeg > 180.0)
    {
        throw std::invalid_argument("invalid geodetic coordinate");
    }
}

void
ToEcef(const GeoPoint& point, double result[3])
{
    Validate(point);
    const double latitude = Radians(point.latitudeDeg);
    const double longitude = Radians(point.longitudeDeg);
    const double sinLatitude = std::sin(latitude);
    const double cosLatitude = std::cos(latitude);
    const double radius = Wgs84SemiMajorM /
        std::sqrt(
            1.0 - Wgs84EccentricitySquared *
                      sinLatitude * sinLatitude);
    result[0] =
        (radius + point.altitudeM) * cosLatitude * std::cos(longitude);
    result[1] =
        (radius + point.altitudeM) * cosLatitude * std::sin(longitude);
    result[2] =
        (radius * (1.0 - Wgs84EccentricitySquared) + point.altitudeM) *
        sinLatitude;
}

} // namespace

LocalEnuFrame::LocalEnuFrame(const GeoPoint& origin)
    : mOrigin(origin)
{
    ToEcef(origin, mOriginEcef);
    const double latitude = Radians(origin.latitudeDeg);
    const double longitude = Radians(origin.longitudeDeg);
    mSinLatitude = std::sin(latitude);
    mCosLatitude = std::cos(latitude);
    mSinLongitude = std::sin(longitude);
    mCosLongitude = std::cos(longitude);
}

Position
LocalEnuFrame::ToEnu(const GeoPoint& point) const
{
    double ecef[3]{};
    ToEcef(point, ecef);
    const double dx = ecef[0] - mOriginEcef[0];
    const double dy = ecef[1] - mOriginEcef[1];
    const double dz = ecef[2] - mOriginEcef[2];
    return Position{
        -mSinLongitude * dx + mCosLongitude * dy,
        -mSinLatitude * mCosLongitude * dx -
            mSinLatitude * mSinLongitude * dy + mCosLatitude * dz,
        mCosLatitude * mCosLongitude * dx +
            mCosLatitude * mSinLongitude * dy + mSinLatitude * dz,
    };
}

const GeoPoint&
LocalEnuFrame::Origin() const
{
    return mOrigin;
}

Velocity
VelocityFromSpeed(
    double speedMps,
    double headingDeg,
    double pitchDeg)
{
    if (!std::isfinite(speedMps) || speedMps < 0.0 ||
        !std::isfinite(headingDeg) || !std::isfinite(pitchDeg))
    {
        throw std::invalid_argument("invalid speed or orientation");
    }
    const double heading = Radians(headingDeg);
    const double pitch = Radians(pitchDeg);
    const double horizontal = speedMps * std::cos(pitch);
    return Velocity{
        horizontal * std::sin(heading),
        horizontal * std::cos(heading),
        speedMps * std::sin(pitch),
    };
}

} // namespace afsim_ns3
