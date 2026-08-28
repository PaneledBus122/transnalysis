#ifndef PLOTWINDOW_H
#define PLOTWINDOW_H

#include <QWidget>
#include <QAbstractItemModel>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#include <QTimer>
#include <QGraphicsEllipseItem>
#include <QGraphicsSimpleTextItem>
#include <QtGlobal>
#include <QListWidgetItem>
#include <QPoint>
#include <QVector>
#include <QString>
#include <QStringList>
#include <QLabel>
#include <QtCharts/QValueAxis>

namespace Ui { class PlotWindow; }

class PlotWindow : public QWidget
{
    Q_OBJECT
protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

public:
    PlotWindow(QAbstractItemModel* model,
               int xCol,
               const QVector<int>& yCols,
               const QString& xLabel,
               const QStringList& yLabels,
               int startRow,
               int maxRows,
               QWidget* parent = nullptr);
    ~PlotWindow();

private slots:
    void updatePlot();

    void onSeriesItemChanged(QListWidgetItem* item);
    void onAutoZoomClicked();
    void onCopyChartClicked();
    void onDeselectClicked();
    void onAxisRangeChanged(qreal /*min*/, qreal /*max*/);
    void on_btnGenerateReport_clicked();
    void onPlotScaleFontChanged(int value);
    void onPlotLineWidthChanged(int value);
    void onListRowsMoved(const QModelIndex &parent, int start, int end,
                         const QModelIndex &destination, int row);

private:
    Ui::PlotWindow *ui = nullptr;

    QAbstractItemModel* model_ = nullptr;
    int xCol_ = -1;
    QString xLabel_;

    QVector<int> yCols_;
    QStringList  yLabels_;

    int startRow_ = 0;
    int maxRowsPerPlot_ = 10000;

    QChart* chart_ = nullptr;
    QChartView* chartView_ = nullptr;

    QVector<QLineSeries*> seriesList_;
    QVector<int> seriesYCols_;

    QValueAxis *axisX_ = nullptr;
    QValueAxis *axisY_ = nullptr;
    bool axesInitialized_ = false;

    // Cursor UI
    QGraphicsEllipseItem *cursorPoint_ = nullptr;
    QGraphicsSimpleTextItem *cursorInfo_ = nullptr;
    QGraphicsSimpleTextItem *hintItem_ = nullptr;

    QTimer *updateTimer_ = nullptr;

    // Last cursor info (used for copy-to-clipboard, tooltip, etc.)
    int lastCursorRow_ = -1;
    double lastCursorX_ = qQNaN();
    double lastCursorY_ = qQNaN();
    QString lastCursorSeries_;

    // --- Zoom/Pan state (moved from ZoomChartView) ---
    bool panning_ = false;
    QPoint lastMousePos_;

    bool baseValid_ = false;
    double baseXMin_ = 0.0, baseXMax_ = 0.0;
    double baseYMin_ = 0.0, baseYMax_ = 0.0;

    // Helpers
    void initPlot();
    void rebuildSeries(int rowStart, int rowEnd, bool resetAxes);

    void rebuildSeriesListUI();
    void applySeriesVisibility();
    void autoZoomVisibleSeries();

    // Zoom helpers (moved from ZoomChartView)
    void setBaseAxisRanges(double xMin, double xMax, double yMin, double yMax);
    void restoreBaseAxes();

    // Cursor update (direct call from eventFilter)
    void updateCursorFromMouse(const QPoint& pos);

    // Zoom actions used by eventFilter
    void zoomByWheel(QWheelEvent* we);
    void beginPan(QMouseEvent* me);
    void updatePan(QMouseEvent* me);
    void endPan(QMouseEvent* me);

    QLabel* lblXRange_ = nullptr;
    QLabel* lblYRange_ = nullptr;

    void setupRangeLabels();
    void updateRangeLabels();

    bool rangeLabelsConnected_ = false;

    QGraphicsSimpleTextItem* fixedInfo_ = nullptr;

    void generateReport();
    QImage renderCurrentChart(int width, int height);

};

#endif // PLOTWINDOW_H
