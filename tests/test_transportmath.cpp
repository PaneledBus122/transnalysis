#include <QtTest>

#include "transportmath.h"

// Unit tests for TransportMath (see ../transportmath.h). These exercise the
// pure numerical routines shared by the Resitivity, H_Swp_Sym, Swp@H_Sym,
// MultiSwp@H, HSwp_Hall and TSwp_Hall tabs, without needing to construct any
// GUI (MainWindow/QApplication).
class TestTransportMath : public QObject
{
    Q_OBJECT

private slots:
    void interpLinear_basic();
    void interpLinear_outOfRangeFails();
    void sortAndAverage_sortsAndAveragesDuplicates();
    void interpolate_clampsAtEnds();
    void symmetrizeFieldSweep_separatesEvenAndOddParts();
    void symmetrizeFieldSweep_insufficientDataIsEmpty();
    void symmetrizeFieldSweep_outOfRangeGridPointIsNaN();
};

void TestTransportMath::interpLinear_basic()
{
    const QVector<double> x = {0.0, 1.0, 2.0};
    const QVector<double> y = {0.0, 10.0, 20.0};

    double yq = 0.0;
    QVERIFY(TransportMath::interpLinear(x, y, 0.5, yq));
    QCOMPARE(yq, 5.0);

    QVERIFY(TransportMath::interpLinear(x, y, 1.0, yq));
    QCOMPARE(yq, 10.0);
}

void TestTransportMath::interpLinear_outOfRangeFails()
{
    const QVector<double> x = {0.0, 1.0, 2.0};
    const QVector<double> y = {0.0, 10.0, 20.0};
    double yq = 0.0;

    QVERIFY(!TransportMath::interpLinear(x, y, 5.0, yq));
    QVERIFY(!TransportMath::interpLinear(x, y, -5.0, yq));

    const QVector<double> tooShortX = {0.0};
    const QVector<double> tooShortY = {0.0};
    QVERIFY(!TransportMath::interpLinear(tooShortX, tooShortY, 0.0, yq));
}

void TestTransportMath::sortAndAverage_sortsAndAveragesDuplicates()
{
    // Two samples at x=0: (2, 4) should average to (0, 3).
    QVector<double> x = {2.0, 0.0, 1.0, 0.0};
    QVector<double> y = {20.0, 2.0, 10.0, 4.0};

    TransportMath::sortAndAverage(x, y);

    const QVector<double> expectedX = {0.0, 1.0, 2.0};
    const QVector<double> expectedY = {3.0, 10.0, 20.0};
    QCOMPARE(x, expectedX);
    QCOMPARE(y, expectedY);
}

void TestTransportMath::interpolate_clampsAtEnds()
{
    const QVector<double> x = {0.0, 1.0, 2.0};
    const QVector<double> y = {0.0, 10.0, 20.0};

    QCOMPARE(TransportMath::interpolate(0.5, x, y), 5.0);
    // Unlike interpLinear(), out-of-range queries clamp to the nearest
    // endpoint instead of failing.
    QCOMPARE(TransportMath::interpolate(-10.0, x, y), 0.0);
    QCOMPARE(TransportMath::interpolate(10.0, x, y), 20.0);
}

void TestTransportMath::symmetrizeFieldSweep_separatesEvenAndOddParts()
{
    // V(B) = 3*B + 7 is the sum of an odd part (3*B, e.g. a Hall-like
    // response) and an even part (the constant 7, e.g. a magnetoresistance
    // offset). Symmetrization should recover exactly 7 (sym) and 3*B (asym)
    // at every grid point, and orig should reproduce V(B) itself.
    const QVector<double> field = {-2.0, -1.0, 0.0, 1.0, 2.0};
    QVector<double> value;
    for (double b : field) value.push_back(3.0 * b + 7.0);

    const auto points = TransportMath::symmetrizeFieldSweep(field, value, field);
    QCOMPARE(points.size(), field.size());

    for (int i = 0; i < field.size(); ++i) {
        const double b = field[i];
        QCOMPARE(points[i].orig, 3.0 * b + 7.0);
        QCOMPARE(points[i].sym, 7.0);
        QCOMPARE(points[i].asym, 3.0 * b);
    }
}

void TestTransportMath::symmetrizeFieldSweep_insufficientDataIsEmpty()
{
    const QVector<double> field = {0.0};
    const QVector<double> value = {1.0};
    const QVector<double> grid  = {-1.0, 0.0, 1.0};

    const auto points = TransportMath::symmetrizeFieldSweep(field, value, grid);
    QVERIFY(points.isEmpty());
}

void TestTransportMath::symmetrizeFieldSweep_outOfRangeGridPointIsNaN()
{
    const QVector<double> field = {-1.0, 0.0, 1.0};
    const QVector<double> value = {-1.0, 0.0, 1.0};
    const QVector<double> grid  = {-2.0, 0.0, 2.0}; // +-2 falls outside [-1, 1]

    const auto points = TransportMath::symmetrizeFieldSweep(field, value, grid);
    QCOMPARE(points.size(), grid.size());

    QVERIFY(qIsNaN(points[0].orig));
    QCOMPARE(points[1].orig, 0.0);
    QVERIFY(qIsNaN(points[2].orig));
}

QTEST_APPLESS_MAIN(TestTransportMath)
#include "test_transportmath.moc"
