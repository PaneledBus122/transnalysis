#include "transportmath.h"

#include <algorithm>
#include <utility>

namespace TransportMath {

bool interpLinear(const QVector<double>& x, const QVector<double>& y, double xq, double& yq)
{
    if (x.size() < 2) return false;
    if (xq < x.front() || xq > x.back()) return false;

    auto it = std::lower_bound(x.begin(), x.end(), xq);
    int j = int(it - x.begin());

    if (j == 0) { yq = y.front(); return true; }
    if (j >= x.size()) { yq = y.back(); return true; }

    const double x1 = x[j - 1], x2 = x[j];
    const double y1 = y[j - 1], y2 = y[j];
    if (x2 == x1) return false;

    const double t = (xq - x1) / (x2 - x1);
    yq = y1 + t * (y2 - y1);
    return true;
}

void sortAndAverage(QVector<double>& x, QVector<double>& y)
{
    if (x.isEmpty() || x.size() != y.size()) return;

    QVector<std::pair<double, double>> pairs;
    for (int i = 0; i < x.size(); ++i) pairs.push_back({x[i], y[i]});
    std::sort(pairs.begin(), pairs.end());

    QVector<double> newX, newY;
    int i = 0;
    while (i < pairs.size()) {
        double currentX = pairs[i].first;
        double sumY = 0;
        int count = 0;

        while (i < pairs.size() && pairs[i].first == currentX) {
            sumY += pairs[i].second;
            count++;
            i++;
        }

        newX.push_back(currentX);
        newY.push_back(sumY / count);
    }

    x = newX;
    y = newY;
}

double interpolate(double x, const QVector<double>& xData, const QVector<double>& yData)
{
    if (xData.size() < 2) return (yData.isEmpty() ? 0.0 : yData[0]);

    auto it = std::lower_bound(xData.begin(), xData.end(), x);
    if (it == xData.begin()) return yData.front();
    if (it == xData.end()) return yData.back();

    int idx1 = int(std::distance(xData.begin(), it)) - 1;
    int idx2 = idx1 + 1;

    double x1 = xData[idx1], x2 = xData[idx2];
    double y1 = yData[idx1], y2 = yData[idx2];

    if (std::abs(x1 - x2) < 1e-12) return y1;
    return y1 + (x - x1) * (y2 - y1) / (x2 - x1);
}

QVector<FieldSweepPoint> symmetrizeFieldSweep(QVector<double> fieldRaw,
                                               QVector<double> valueRaw,
                                               const QVector<double>& xNew)
{
    sortAndAverage(fieldRaw, valueRaw);
    if (fieldRaw.size() < 2)
        return {};

    QVector<FieldSweepPoint> result;
    result.reserve(xNew.size());

    for (double b : xNew) {
        FieldSweepPoint p;
        double vp = 0.0, vn = 0.0;
        if (interpLinear(fieldRaw, valueRaw,  b, vp) &&
            interpLinear(fieldRaw, valueRaw, -b, vn)) {
            p.orig = vp;
            p.sym  = 0.5 * (vp + vn);
            p.asym = 0.5 * (vp - vn);
        }
        result.push_back(p);
    }
    return result;
}

} // namespace TransportMath
