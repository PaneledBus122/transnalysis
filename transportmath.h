#ifndef TRANSPORTMATH_H
#define TRANSPORTMATH_H

// TransportMath: pure numerical routines used by the transport-analysis
// pipeline (interpolation, averaging, field-sweep symmetrization).
//
// These functions take and return plain Qt containers (QVector<double>) and
// have no dependency on QWidget/QMainWindow or any UI state, so they can be
// exercised directly by unit tests without constructing a GUI.

#include <QVector>
#include <QtGlobal>

namespace TransportMath {

// Linear interpolation of the sample set (x, y) at query point xq.
// Requires x to be sorted ascending (use sortAndAverage() first if needed).
// Returns false -- leaving yq unchanged -- if there are fewer than two
// samples or xq falls outside [x.front(), x.back()] (no extrapolation).
bool interpLinear(const QVector<double>& x, const QVector<double>& y, double xq, double& yq);

// Sorts the (x[i], y[i]) pairs by x ascending, and averages y over any
// samples that share the same x value. Used to collapse repeated/noisy
// field or temperature readings (e.g. from a slow sweep where many rows
// share the same nominal setpoint) into one point per setpoint before
// interpolation.
void sortAndAverage(QVector<double>& x, QVector<double>& y);

// Interpolates y(x) from the sample set (xData, yData) at point x.
// Unlike interpLinear(), this version does not fail outside the data
// range: it clamps to the nearest endpoint value instead. Used where a
// best-effort estimate is preferable to a missing point (e.g. aligning
// several field sweeps onto a common temperature/time grid).
double interpolate(double x, const QVector<double>& xData, const QVector<double>& yData);

// One point of a field-sweep symmetrization result at a given field value.
// All fields default to NaN, meaning "no valid data at this field".
struct FieldSweepPoint {
    double orig = qQNaN(); // interpolated raw value at +field
    double sym  = qQNaN(); // symmetric (even-in-field) part:  (V(+B) + V(-B)) / 2
    double asym = qQNaN(); // antisymmetric (odd-in-field) part: (V(+B) - V(-B)) / 2
};

// Field-sweep symmetrization / antisymmetrization.
//
// Physical motivation: in a transport measurement swept through a magnetic
// field B, a measured voltage generally contains both a magnetoresistance-
// like contribution that is even in B (unchanged under B -> -B) and a
// Hall-like contribution that is odd in B (flips sign under B -> -B).
// Averaging the signal measured at +B and -B cancels the odd part and
// isolates the even part (`sym`); taking half the difference cancels the
// even part and isolates the odd part (`asym`). This routine performs that
// decomposition on a caller-supplied symmetric field grid `xNew` (expected
// to contain both +b and -b for every field of interest).
//
// `fieldRaw`/`valueRaw` are the raw (field, value) samples for one sweep,
// in any order; they are sorted and duplicate-averaged internally via
// sortAndAverage() and are therefore taken by value.
//
// Returns one FieldSweepPoint per entry of `xNew`, or an empty vector if
// fewer than two distinct field values remain in the raw data after
// sorting/averaging (not enough data to interpolate). A grid point whose
// +b or -b falls outside the raw data range yields an all-NaN
// FieldSweepPoint rather than shrinking the output.
QVector<FieldSweepPoint> symmetrizeFieldSweep(QVector<double> fieldRaw,
                                               QVector<double> valueRaw,
                                               const QVector<double>& xNew);

} // namespace TransportMath

#endif // TRANSPORTMATH_H
