#include "plotwindow.h"
#include "ui_plotwindow.h"

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#include <QtCharts/QLegendMarker>

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <QDateTime>
#include <QGraphicsLayout>


PlotWindow::PlotWindow(QAbstractItemModel* model,
                       int xCol,
                       const QVector<int>& yCols,
                       const QString& xLabel,
                       const QStringList& yLabels,
                       int startRow,
                       int maxRows,
                       QWidget* parent)
    : QWidget(parent)
    , ui(new Ui::PlotWindow)
    , model_(model)
    , xCol_(xCol)
    , xLabel_(xLabel)
    , yCols_(yCols)
    , yLabels_(yLabels)
    , startRow_(startRow)
    , maxRowsPerPlot_(maxRows)
{
    ui->setupUi(this);

    // Build a readable window title: "X: ... / Y: ... & ... & (+N more)"
    auto sanitize = [](QString s) {
        s = s.simplified();
        s.replace('\n', ' ');
        return s;
    };

    QString xPart = sanitize(xLabel_);
    if (xPart.isEmpty()) xPart = QString("Col %1").arg(xCol_);

    QStringList yClean;
    for (const auto& yl : std::as_const(yLabels_)) {
        const QString t = sanitize(yl);
        if (!t.isEmpty()) yClean << t;
    }

    // fallback if labels list is missing/short
    if (yClean.isEmpty()) {
        for (int i = 0; i < yCols_.size(); ++i)
            yClean << QString("Col %1").arg(yCols_[i]);
    }

    const int maxShow = 8;  // show first 3, then "+N more"
    QString yPart;
    if (yClean.size() <= maxShow) {
        yPart = yClean.join(" || ");
    } else {
        yPart = yClean.mid(0, maxShow).join(" & ")
        + QString("  (+%1 more)").arg(yClean.size() - maxShow);
    }

    setWindowTitle(QString("X: %1 / Y: %2").arg(xPart, yPart));

    initPlot();
    rebuildSeriesListUI();

    updateTimer_ = new QTimer(this);
    connect(updateTimer_, &QTimer::timeout, this, &PlotWindow::updatePlot);
    updateTimer_->start(200);

    updatePlot();
}


PlotWindow::~PlotWindow()
{
    delete ui;
}

void PlotWindow::initPlot()
{
    qDebug() << "initPlot called, chartView_=" << chartView_ << "chart_=" << chart_;

    const bool needCreate = (chart_ == nullptr || chartView_ == nullptr);
    if (needCreate) {

        // ---- 1) Create chart + chart view ----
        if (!chart_) {
            chart_ = new QChart();
            chart_->legend()->setVisible(true);
        }

        QWidget* host = ui->chartContainer;
        Q_ASSERT(host);

        QLayout* lay = host->layout();
        if (!lay) {
            auto *v = new QVBoxLayout(host);
            v->setContentsMargins(0, 0, 0, 0);
            v->setSpacing(0);
            lay = v;
        } else {
            lay->setContentsMargins(0, 0, 0, 0);
            lay->setSpacing(0);
        }

        if (!chartView_) {
            chartView_ = new QChartView(chart_, host);
            chartView_->setRenderHint(QPainter::Antialiasing, false);
            chartView_->setMouseTracking(true);
            chartView_->viewport()->setMouseTracking(true);
            chartView_->setRubberBand(QChartView::RectangleRubberBand);
            chartView_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            chartView_->setMinimumSize(0, 0);
            chartView_->setContextMenuPolicy(Qt::NoContextMenu);
            chartView_->viewport()->setContextMenuPolicy(Qt::NoContextMenu);
            chartView_->viewport()->installEventFilter(this);
            lay->addWidget(chartView_);
        } else {
            chartView_->setChart(chart_);
        }

        // ---- 2) Axes (create once) ----
        if (!axisX_) axisX_ = new QValueAxis();
        if (!axisY_) {
            axisY_ = new QValueAxis();
            // Y축 눈금 지수 표기법 적용
            axisY_->setLabelFormat("%.2E");
        }

        axisX_->setTitleText(xLabel_);

        // 축을 차트에 추가
        if (!chart_->axes(Qt::Horizontal).contains(axisX_)) {
            chart_->addAxis(axisX_, Qt::AlignBottom);
        }
        if (!chart_->axes(Qt::Vertical).contains(axisY_)) {
            chart_->addAxis(axisY_, Qt::AlignLeft);
        }

        // [수정] 스핀박스 초기 폰트 값 적용
        if (axisX_ && axisY_) {
            QFont font = axisX_->labelsFont();
            font.setPointSize(ui->spinPlotScaleFont->value());
            axisX_->setLabelsFont(font);
            axisY_->setLabelsFont(font);
        }

        // [PlotWindow::initPlot() 내부]
        ui->listSeries->setDragEnabled(true);
        ui->listSeries->setAcceptDrops(true);
        ui->listSeries->setDropIndicatorShown(true); // 어디로 들어가는지 시각적으로 표시
        ui->listSeries->setDefaultDropAction(Qt::MoveAction);
        ui->listSeries->setDragDropMode(QAbstractItemView::InternalMove);

        // [중요] UI 순서가 바뀔 때 내부 QVector(seriesList_ 등)도 같이 옮겨주기 위한 연결
        connect(ui->listSeries->model(), &QAbstractItemModel::rowsMoved,
                this, &PlotWindow::onListRowsMoved);

        setupRangeLabels();

        // ---- 3) Cursor / hint graphics (create once) ----
        // (기존 그래픽스 아이템 생성 로직 유지...)
        if (!cursorPoint_) {
            cursorPoint_ = new QGraphicsEllipseItem(chart_);
            cursorPoint_->setRect(-4, -4, 8, 8);
            cursorPoint_->setZValue(10000);
            cursorPoint_->setBrush(Qt::red);
            cursorPoint_->setVisible(false);
        }
        if (!cursorInfo_) {
            cursorInfo_ = new QGraphicsSimpleTextItem(chart_);
            cursorInfo_->setZValue(10001);
            cursorInfo_->setVisible(false);
        }
        if (!hintItem_) {
            hintItem_ = new QGraphicsSimpleTextItem();
            hintItem_->setVisible(false);
            chart_->scene()->addItem(hintItem_);
        }
        if (!fixedInfo_) {
            fixedInfo_ = new QGraphicsSimpleTextItem(chart_);
            fixedInfo_->setZValue(3000);
            QFont font = fixedInfo_->font();
            font.setBold(true);
            font.setPointSize(10);
            fixedInfo_->setFont(font);
            fixedInfo_->setVisible(false);
        }

        // ---- 4) UI connections (connect once) ----
        connect(ui->btnAutoZoom, &QPushButton::clicked,
                this, &PlotWindow::onAutoZoomClicked, Qt::UniqueConnection);
        connect(ui->listSeries, &QListWidget::itemChanged,
                this, &PlotWindow::onSeriesItemChanged, Qt::UniqueConnection);
        connect(ui->btnCopyChart, &QPushButton::clicked,
                this, &PlotWindow::onCopyChartClicked, Qt::UniqueConnection);
        connect(ui->btnDeselect, &QPushButton::clicked,
                this, &PlotWindow::onDeselectClicked);

        // [신규] 스케일 폰트 및 선 두께 연동 연결
        connect(ui->spinPlotScaleFont, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &PlotWindow::onPlotScaleFontChanged, Qt::UniqueConnection);
        connect(ui->spinPlotLineWidth, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &PlotWindow::onPlotLineWidthChanged, Qt::UniqueConnection);

        // ---- 5) First draw + base ranges ----
        updatePlot();
        autoZoomVisibleSeries();

        if (axisX_ && axisY_) {
            setBaseAxisRanges(axisX_->min(), axisX_->max(),
                              axisY_->min(), axisY_->max());
        }
    }

    // ---- Shortcut registration (기존 로직 유지) ----
    static bool shortcutInstalled = false;
    if (!shortcutInstalled) {
        shortcutInstalled = true;
        auto* actCopyRow = new QAction(this);
        actCopyRow->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
        actCopyRow->setShortcutContext(Qt::ApplicationShortcut);
        addAction(actCopyRow);

        connect(actCopyRow, &QAction::triggered, this, [this]() {
            if (lastCursorRow_ < 0) return;
            QGuiApplication::clipboard()->setText(QString::number(lastCursorRow_));
            if (hintItem_ && chart_) {
                hintItem_->setText(QString("Row %1 copied").arg(lastCursorRow_));
                const QRectF pa = chart_->plotArea();
                hintItem_->setPos(pa.left() + 10, pa.top() - 20);
                hintItem_->setVisible(true);
                QTimer::singleShot(1000, this, [this]() {
                    if (hintItem_) hintItem_->setVisible(false);
                });
            }
        });
    }
}

void PlotWindow::updatePlot()
{
    qDebug() << "updatePlot startRow_=" << startRow_
             << "maxRowsPerPlot_=" << maxRowsPerPlot_
             << "rows=" << model_->rowCount();

    if (!model_ || model_->rowCount() == 0 || model_->columnCount() == 0) return;
    if (xCol_ < 0 || xCol_ >= model_->columnCount()) return;

    int rowStart = std::max(0, startRow_);
    int rowEnd   = model_->rowCount();
    if (maxRowsPerPlot_ > 0) rowEnd = std::min(rowEnd, rowStart + maxRowsPerPlot_);
    if (rowStart >= rowEnd) return;

    rebuildSeries(rowStart, rowEnd, /*resetAxes=*/false);
}

void PlotWindow::rebuildSeries(int rowStart, int rowEnd, bool resetAxes)
{
    if (!chart_ || !model_ || !axisX_ || !axisY_) return;

    const int rc = model_->rowCount();
    if (rc <= 0) return;

    rowStart = qBound(0, rowStart, rc - 1);
    rowEnd   = qBound(0, rowEnd,   rc - 1);
    if (rowEnd < rowStart) return;

    // 0) Ensure series objects exist (create once)
    auto safeHeader = [&](int col) -> QString {
        QString h = model_->headerData(col, Qt::Horizontal, Qt::DisplayRole).toString().trimmed();
        if (h.isEmpty()) h = QString("Col %1").arg(col);
        return h;
    };

    if (seriesList_.isEmpty()) {
        seriesList_.clear();
        seriesYCols_.clear();

        for (int k = 0; k < yCols_.size(); ++k) {
            const int yCol = yCols_[k];
            if (yCol < 0 || yCol >= model_->columnCount()) continue;
            if (yCol == xCol_) continue;

            auto* s = new QLineSeries(chart_);

            //s->setUseOpenGL(true);

            // Prefer label list if it matches, otherwise use model header
            QString name;
            if (k < yLabels_.size())
                name = yLabels_[k].trimmed();

            if (name.isEmpty())
                name = safeHeader(yCol);

            s->setName(name);                 // <-- must be non-empty

            chart_->addSeries(s);
            s->attachAxis(axisX_);
            s->attachAxis(axisY_);

            seriesList_.push_back(s);
            seriesYCols_.push_back(yCol);
        }

        rebuildSeriesListUI();
        applySeriesVisibility();
    }

    // 1) Read current visibility from UI (do not rebuild UI here)
    const int nVis = std::min(ui->listSeries->count(),
                              static_cast<int>(seriesList_.size()));
    QVector<bool> vis(nVis, true);
    for (int i = 0; i < nVis; ++i)
        vis[i] = (ui->listSeries->item(i)->checkState() == Qt::Checked);

    // 2) Update data with replace()
    double xmin=+std::numeric_limits<double>::infinity();
    double xmax=-std::numeric_limits<double>::infinity();
    double ymin=+std::numeric_limits<double>::infinity();
    double ymax=-std::numeric_limits<double>::infinity();

    int si = 0;
    for (int k = 0; k < yCols_.size() && si < seriesList_.size(); ++k) {
        const int yCol = yCols_[k];
        if (yCol < 0 || yCol >= model_->columnCount()) continue;
        if (yCol == xCol_) continue;

        QVector<QPointF> pts;
        pts.reserve(rowEnd - rowStart + 1);

        for (int r = rowStart; r <= rowEnd; ++r) {
            bool okx=false, oky=false;
            const double x = model_->index(r, xCol_).data(Qt::DisplayRole).toDouble(&okx);
            const double y = model_->index(r, yCol).data(Qt::DisplayRole).toDouble(&oky);
            if (!okx || !oky) continue;

            pts.push_back(QPointF(x, y));
            xmin = std::min(xmin, x); xmax = std::max(xmax, x);
            ymin = std::min(ymin, y); ymax = std::max(ymax, y);
        }

        seriesList_[si]->replace(pts);

        // restore visibility
        if (si < vis.size())
            seriesList_[si]->setVisible(vis[si]);

        ++si;
    }

    // 3) Axes only when requested
    if (resetAxes && si > 0 && std::isfinite(xmin) && std::isfinite(xmax) &&
        std::isfinite(ymin) && std::isfinite(ymax))
    {
        if (xmin == xmax) { xmin -= 0.5; xmax += 0.5; }
        if (ymin == ymax) { ymin -= 0.5; ymax += 0.5; }
        axisX_->setRange(xmin, xmax);
        axisY_->setRange(ymin, ymax);
        axesInitialized_ = true;
        setBaseAxisRanges(xmin, xmax, ymin, ymax);
    }
}

bool PlotWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (!chartView_ || obj != chartView_->viewport())
        return QWidget::eventFilter(obj, event);

    switch (event->type()) {

    case QEvent::MouseMove: {
        auto *me = static_cast<QMouseEvent*>(event);

        // Update cursor only when inside plot area
        updateCursorFromMouse(me->pos());

        if (panning_) {
            updatePan(me);
            return true;
        }
        return false;
    }

    case QEvent::Wheel: {
        zoomByWheel(static_cast<QWheelEvent*>(event));
        return true;
    }

    case QEvent::MouseButtonPress: {
        auto *me = static_cast<QMouseEvent*>(event);

        // Middle click: copy current cursor row
        if (me->button() == Qt::MiddleButton) {
            if (lastCursorRow_ >= 0) {
                QGuiApplication::clipboard()->setText(QString::number(lastCursorRow_));
                if (hintItem_ && chart_) {
                    hintItem_->setText(QString("Row %1 copied").arg(lastCursorRow_));
                    const QRectF pa = chart_->plotArea();
                    hintItem_->setPos(pa.left() + 10, pa.top() - 20);
                    hintItem_->setVisible(true);
                    QTimer::singleShot(900, this, [this]() {
                        if (hintItem_) hintItem_->setVisible(false);
                    });
                }
            }
            me->accept();
            return true; // consume only middle click
        }

        // Shift + Left: pan
        beginPan(me);
        return panning_;
    }

    case QEvent::MouseButtonRelease: {
        endPan(static_cast<QMouseEvent*>(event));
        return false;
    }

    default:
        break;
    }

    if (obj == chartView_->viewport() && event->type() == QEvent::Paint) {
        // 1. 먼저 차트가 자기 자신을 그리게 둡니다.
        // (기본 동작이 수행된 후 아래 코드가 실행됩니다.)

        // 2. 그 위에 수동으로 커서 정보를 그립니다.
        if (cursorPoint_->isVisible()) {
            QPainter painter(chartView_->viewport());
            painter.setRenderHint(QPainter::Antialiasing);

            // 마우스 위치에 따른 씬 좌표를 뷰포트 좌표로 변환
            QPointF viewPos = chartView_->mapFromScene(cursorPoint_->scenePos());

            // 빨간 점 그리기 (시리즈보다 위)
            painter.setBrush(Qt::red);
            painter.setPen(Qt::white);
            painter.drawEllipse(viewPos, 4, 4);

            // 텍스트 박스 그리기
            QString txt = cursorInfo_->text();
            QRectF textRect = painter.fontMetrics().boundingRect(QRect(0,0,0,0), Qt::TextWordWrap, txt);
            textRect.adjust(-5, -5, 15, 15); // 여백 추가
            textRect.moveTopLeft(viewPos + QPointF(10, -10));

            // 배경 박스 (노이즈 대비 가독성 확보)
            painter.setBrush(QColor(255, 255, 255, 220));
            painter.setPen(Qt::gray);
            painter.drawRoundedRect(textRect, 5, 5);

            // 글자 쓰기
            painter.setPen(Qt::black);
            painter.drawText(textRect.adjusted(5, 5, -5, -5), txt);

            return false; // Paint 이벤트는 가로채기만 하고 통과시킴
        }
    }

    return QWidget::eventFilter(obj, event);
}

void PlotWindow::updateCursorFromMouse(const QPoint& pos)
{
    if (!chartView_ || !chart_ || !model_ || !axisX_ || !axisY_)
        return;
    if (seriesList_.isEmpty() || seriesYCols_.size() != seriesList_.size())
        return;
    // fixedInfo_가 생성되지 않았을 경우를 대비해 체크 추가 가능
    if (!cursorPoint_ || !cursorInfo_)
        return;

    // viewport -> scene
    const QPointF scenePos = chartView_->mapToScene(pos);

    // Only when inside plot area
    const QRectF pa = chart_->plotArea();
    if (!pa.contains(scenePos)) {
        cursorPoint_->setVisible(false);
        cursorInfo_->setVisible(false);
        // [추가] 고정 정보창도 숨김
        if (fixedInfo_) fixedInfo_->setVisible(false);
        return;
    }

    // Use any existing series for x mapping (first is fine)
    QLineSeries* refSeries = seriesList_.first();
    if (!refSeries) return;

    // scene -> value: trust x only
    const double xMouse = chart_->mapToValue(scenePos, refSeries).x();

    // 1) Find nearest row by X in the plotted window
    const int rc = model_->rowCount();
    const int r0 = std::max(0, startRow_);
    const int r1 = std::min(rc - 1, startRow_ + maxRowsPerPlot_ - 1);

    int bestRow = -1;
    double bestDx = std::numeric_limits<double>::infinity();

    for (int r = r0; r <= r1; ++r) {
        bool ok=false;
        const double xr = model_->index(r, xCol_).data(Qt::DisplayRole).toDouble(&ok);
        if (!ok) continue;

        const double dx = std::abs(xr - xMouse);
        if (dx < bestDx) { bestDx = dx; bestRow = r; }
    }

    if (bestRow < 0) {
        cursorPoint_->setVisible(false);
        cursorInfo_->setVisible(false);
        if (fixedInfo_) fixedInfo_->setVisible(false);
        return;
    }

    // Read X at the snapped row (actual data)
    bool okx=false;
    const double xRow = model_->index(bestRow, xCol_).data(Qt::DisplayRole).toDouble(&okx);
    if (!okx) {
        cursorPoint_->setVisible(false);
        cursorInfo_->setVisible(false);
        if (fixedInfo_) fixedInfo_->setVisible(false);
        return;
    }

    // 2) Among visible series, choose the nearest point in screen space
    int bestSeries = -1;
    double bestDist2 = std::numeric_limits<double>::infinity();
    double bestY = qQNaN();
    QPointF bestPointScene;

    for (int i = 0; i < seriesList_.size(); ++i) {
        QLineSeries* s = seriesList_[i];
        if (!s || !s->isVisible()) continue;

        const int yCol = seriesYCols_[i];
        bool oky=false;
        const double yRow = model_->index(bestRow, yCol).data(Qt::DisplayRole).toDouble(&oky);
        if (!oky) continue;

        // Convert (xRow,yRow) -> scene position for this series
        const QPointF pScene = chart_->mapToPosition(QPointF(xRow, yRow), s);

        const double dx = pScene.x() - scenePos.x();
        const double dy = pScene.y() - scenePos.y();
        const double d2 = dx*dx + dy*dy;

        if (d2 < bestDist2) {
            bestDist2 = d2;
            bestSeries = i;
            bestY = yRow;
            bestPointScene = pScene;
        }
    }

    if (bestSeries < 0 || !std::isfinite(bestY)) {
        cursorPoint_->setVisible(false);
        cursorInfo_->setVisible(false);
        if (fixedInfo_) fixedInfo_->setVisible(false);
        return;
    }

    // 3) Update cursor state (for Ctrl+R / middle-click copy)
    lastCursorRow_ = bestRow;
    lastCursorX_   = xRow;
    lastCursorY_   = bestY;
    lastCursorSeries_ = seriesList_[bestSeries] ? seriesList_[bestSeries]->name() : QString();

    // 4) Snap cursor graphics to that series point
    cursorPoint_->setPos(bestPointScene);
    cursorPoint_->setVisible(true);

    // 텍스트 생성
    const QString txt = QString("%1\nRow %2\nx=%3\ny=%4")
                            .arg(lastCursorSeries_)
                            .arg(bestRow)
                            .arg(xRow, 0, 'g', 6)
                            .arg(bestY, 0, 'g', 6);

    // 5) 기존 툴팁 (마우스를 따라다님)
    cursorInfo_->setText(txt);
    QPointF labelPos = bestPointScene + QPointF(10, -10);

    // 툴팁 위치 클램핑
    QRectF br = cursorInfo_->boundingRect();
    if (labelPos.x() + br.width() > pa.right())  labelPos.setX(pa.right() - br.width());
    if (labelPos.y() < pa.top())                 labelPos.setY(pa.top());
    if (labelPos.y() + br.height() > pa.bottom()) labelPos.setY(pa.bottom() - br.height());
    if (labelPos.x() < pa.left())                labelPos.setX(pa.left());

    cursorInfo_->setPos(labelPos);
    cursorInfo_->setVisible(true);

    // 6) [수정] 차트 영역(plotArea) 바깥 축 근처에 고정 정보창 업데이트
    if (fixedInfo_) {
        fixedInfo_->setText(txt);

        // plotArea 상단 경계선보다 약간 위(여백 공간)로 배치하거나,
        // plotArea 내부가 아닌 차트 전체의 우측 상단 구석으로 배치합니다.

        const QRectF pa = chart_->plotArea();

        // x 위치: plotArea의 오른쪽 끝에서 텍스트 너비만큼 왼쪽으로 (우측 정렬 느낌)
        qreal fx = pa.right() - fixedInfo_->boundingRect().width();

        // y 위치: plotArea의 상단선 바로 위 (축 숫자나 제목이 없는 구석 공간)
        // -20 ~ -30 정도를 주면 그래프 영역 밖인 상단 여백에 표시됩니다.
        qreal fy = pa.top() - fixedInfo_->boundingRect().height() - 20;

        // 만약 상단에 제목이 있어서 겹친다면, 아래처럼 plotArea 내부의 절대적인 구석을 사용하되
        // zValue를 다시 한번 보장합니다.
        fixedInfo_->setPos(fx, fy);
        fixedInfo_->setVisible(true);

        // OpenGL 레이어보다 항상 위에 있도록 보장
        fixedInfo_->setZValue(5000);
    }
}

void PlotWindow::rebuildSeriesListUI()
{
    ui->listSeries->blockSignals(true);
    ui->listSeries->clear();

    for (int i = 0; i < seriesList_.size(); ++i) {
        auto* it = new QListWidgetItem(seriesList_[i]->name(), ui->listSeries);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(seriesList_[i]->isVisible() ? Qt::Checked
                                                      : Qt::Unchecked);
    }

    ui->listSeries->blockSignals(false);
}

void PlotWindow::applySeriesVisibility()
{
    const int n = std::min(ui->listSeries->count(),
                           static_cast<int>(seriesList_.size()));


    for (int i = 0; i < n; ++i) {
        const bool vis = (ui->listSeries->item(i)->checkState() == Qt::Checked);
        seriesList_[i]->setVisible(vis);

        // Optional: hide legend marker too (otherwise legend may still show)
        const auto markers = chart_->legend()->markers(seriesList_[i]);
        for (auto* m : markers) m->setVisible(vis);
    }
}

void PlotWindow::onSeriesItemChanged(QListWidgetItem* /*item*/)
{
    applySeriesVisibility();
}

void PlotWindow::onAutoZoomClicked()
{
    autoZoomVisibleSeries();
}

void PlotWindow::onDeselectClicked()
{
    if (!ui->listSeries) return;

    const int n = ui->listSeries->count();
    if (n == 0) return;

    // Check if there is at least one enabled (checked) series
    bool anyChecked = false;
    for (int i = 0; i < n; ++i) {
        if (ui->listSeries->item(i)->checkState() == Qt::Checked) {
            anyChecked = true;
            break;
        }
    }

    // Block signals to avoid repeated updates
    ui->listSeries->blockSignals(true);

    const Qt::CheckState targetState =
        anyChecked ? Qt::Unchecked : Qt::Checked;

    for (int i = 0; i < n; ++i) {
        ui->listSeries->item(i)->setCheckState(targetState);
    }

    ui->listSeries->blockSignals(false);

    // Apply visibility + rescale
    applySeriesVisibility();
    autoZoomVisibleSeries();
}

void PlotWindow::autoZoomVisibleSeries()
{
    if (!axisX_ || !axisY_) return;

    double xmin =  std::numeric_limits<double>::infinity();
    double xmax = -std::numeric_limits<double>::infinity();
    double ymin =  std::numeric_limits<double>::infinity();
    double ymax = -std::numeric_limits<double>::infinity();
    bool any = false;

    for (auto* s : std::as_const(seriesList_)) {
        if (!s || !s->isVisible()) continue;  // 핵심: visible만

        const auto pts = s->points();
        for (const auto& p : std::as_const(pts)) {
            xmin = std::min(xmin, p.x());  xmax = std::max(xmax, p.x());
            ymin = std::min(ymin, p.y());  ymax = std::max(ymax, p.y());
            any = true;
        }
    }

    if (!any) return;

    if (xmin == xmax) { xmin -= 0.5; xmax += 0.5; }
    if (ymin == ymax) { ymin -= 0.5; ymax += 0.5; }

    axisX_->setRange(xmin, xmax);
    axisY_->setRange(ymin, ymax);
}

void PlotWindow::onCopyChartClicked()
{
    if (!chartView_) return;

    // Grab the chart view as it is currently shown
    const QPixmap pm = chartView_->grab();

    if (pm.isNull())
        return;

    QClipboard *cb = QGuiApplication::clipboard();
    cb->setPixmap(pm);
}

void PlotWindow::setBaseAxisRanges(double xMin, double xMax, double yMin, double yMax)
{
    baseValid_ = true;
    baseXMin_ = xMin; baseXMax_ = xMax;
    baseYMin_ = yMin; baseYMax_ = yMax;
}

void PlotWindow::restoreBaseAxes()
{
    if (!baseValid_ || !axisX_ || !axisY_)
        return;

    axisX_->setRange(baseXMin_, baseXMax_);
    axisY_->setRange(baseYMin_, baseYMax_);
}

void PlotWindow::zoomByWheel(QWheelEvent* we)
{
    if (!we || !axisX_ || !axisY_)
        return;

    const int dy = we->angleDelta().y();
    if (dy == 0)
        return;

    // Zoom factor: <1 zoom in, >1 zoom out
    const double factor = (dy > 0) ? 0.9 : (1.0 / 0.9);

    const Qt::KeyboardModifiers mods = we->modifiers();

    // Ctrl: X only
    const bool onlyX = mods.testFlag(Qt::ControlModifier)
                       && !mods.testFlag(Qt::AltModifier)
                       && !mods.testFlag(Qt::ShiftModifier);

    // Alt OR Shift: Y only
    const bool onlyY = (mods.testFlag(Qt::AltModifier) || mods.testFlag(Qt::ShiftModifier))
                       && !mods.testFlag(Qt::ControlModifier);


    auto scaleAxis = [](QValueAxis* ax, double f)
    {
        const double a = ax->min();
        const double b = ax->max();
        const double c = 0.5 * (a + b);

        double half = 0.5 * (b - a) * f;

        // Avoid collapsing to zero span
        const double minSpan = 1e-300;
        if (half < minSpan) half = minSpan;

        ax->setRange(c - half, c + half);
    };

    if (onlyX) {
        scaleAxis(axisX_, factor);
    } else if (onlyY) {
        scaleAxis(axisY_, factor);
    } else {
        scaleAxis(axisX_, factor);
        scaleAxis(axisY_, factor);
    }

    we->accept();
}

void PlotWindow::beginPan(QMouseEvent* me)
{
    if (!me)
        return;

    if (me->button() == Qt::LeftButton && me->modifiers().testFlag(Qt::ShiftModifier)) {
        panning_ = true;
        lastMousePos_ = me->pos();
        me->accept();
    }
}

void PlotWindow::updatePan(QMouseEvent* me)
{
    if (!me || !panning_ || !axisX_ || !axisY_ || !chart_)
        return;

    const QPoint cur = me->pos();
    const QPoint delta = cur - lastMousePos_;
    lastMousePos_ = cur;

    const QRectF pa = chart_->plotArea();
    if (pa.width() <= 1.0 || pa.height() <= 1.0)
        return;

    const double xSpan = axisX_->max() - axisX_->min();
    const double ySpan = axisY_->max() - axisY_->min();

    // Convert pixel delta into axis delta
    const double dx = -delta.x() * (xSpan / pa.width());
    const double dy =  delta.y() * (ySpan / pa.height()); // y-axis inverted in screen coords

    axisX_->setRange(axisX_->min() + dx, axisX_->max() + dx);
    axisY_->setRange(axisY_->min() + dy, axisY_->max() + dy);

    me->accept();
}

void PlotWindow::endPan(QMouseEvent* me)
{
    if (!me)
        return;

    if (me->button() == Qt::LeftButton && panning_) {
        panning_ = false;
        me->accept();
    }
}

void PlotWindow::setupRangeLabels()
{
    if (rangeLabelsConnected_)
        return;

    lblXRange_ = ui->labelXRange;
    lblYRange_ = ui->labelYRange;

    if (!lblXRange_ || !lblYRange_ || !axisX_ || !axisY_)
        return;

    connect(axisX_, &QValueAxis::rangeChanged,
            this, &PlotWindow::onAxisRangeChanged,
            Qt::UniqueConnection);

    connect(axisY_, &QValueAxis::rangeChanged,
            this, &PlotWindow::onAxisRangeChanged,
            Qt::UniqueConnection);

    rangeLabelsConnected_ = true;
    updateRangeLabels();
}




void PlotWindow::updateRangeLabels()
{
    if (!lblXRange_ || !lblYRange_ || !axisX_ || !axisY_)
        return;

    auto fmtMinMax = [](qreal v) {
        return QString::number(v, 'g', 6);
    };

    auto fmtDelta = [](qreal v) {
        QString s = QString::number(v, 'e', 2);
        s.replace('e', 'E');
        return s;
    };

    const qreal xMin = axisX_->min();
    const qreal xMax = axisX_->max();
    const qreal yMin = axisY_->min();
    const qreal yMax = axisY_->max();

    const qreal dx = xMax - xMin;
    const qreal dy = yMax - yMin;

    lblXRange_->setText(
        QString("X: [%1, %2], Δ=%3")
            .arg(fmtMinMax(xMin), fmtMinMax(xMax), fmtDelta(dx))
        );

    lblYRange_->setText(
        QString("Y: [%1, %2], Δ=%3")
            .arg(fmtMinMax(yMin), fmtMinMax(yMax), fmtDelta(dy))
        );
}

void PlotWindow::onAxisRangeChanged(qreal, qreal)
{
    updateRangeLabels();
}


void PlotWindow::on_btnGenerateReport_clicked() {
    generateReport();
}

void PlotWindow::generateReport() {
    if (seriesList_.isEmpty()) return;
    if (updateTimer_) updateTimer_->stop();

    // 1. 선택된 시리즈 수집
    QVector<int> checkedIndices;
    for (int i = 0; i < seriesList_.size(); ++i) {
        if (ui->listSeries->item(i)->checkState() == Qt::Checked) {
            checkedIndices.push_back(i);
        }
    }
    if (checkedIndices.isEmpty()) return;

    // 2. 레이아웃 설정
    int n = checkedIndices.size();
    int cols = qMax(1, ui->spinReportColumn->value());
    int rows = std::ceil((double)n / cols);
    int tileW = 800, tileH = 600;

    QImage reportImage(cols * tileW, rows * tileH, QImage::Format_RGB32);
    reportImage.fill(Qt::white);

    QPainter painter(&reportImage);
    painter.setRenderHint(QPainter::Antialiasing);

    // 3. 상태 백업 (애니메이션 로직 제외)
    double oldXMin = axisX_->min(), oldXMax = axisX_->max();
    double oldYMin = axisY_->min(), oldYMax = axisY_->max();
    QVector<bool> visibilityBackup;
    QVector<QPen> penBackup;
    for (auto* s : seriesList_) {
        visibilityBackup.push_back(s->isVisible());
        penBackup.push_back(s->pen());
    }
    QFont oldAxisFont = axisX_->labelsFont();
    bool wasLegendVisible = chart_->legend()->isVisible();

    // [설정값 적용]
    int reportLineWidth = ui->spinReportLineWidth->value();
    int reportAxisFontSize = ui->spinReportFontSize->value();
    int reportOverlayFontSize = ui->spinReportFontSize->value();

    QFont axisFont = oldAxisFont;
    axisFont.setPointSize(reportAxisFontSize);
    axisX_->setLabelsFont(axisFont);
    axisY_->setLabelsFont(axisFont);
    chart_->legend()->hide();

    // 4. 리포트 생성 루프
    for (int i = 0; i < n; ++i) {
        int seriesIdx = checkedIndices[i];

        for (auto* s : seriesList_) s->setVisible(false);
        auto* targetSeries = seriesList_[seriesIdx];
        targetSeries->setVisible(true);

        QPen p = targetSeries->pen();
        p.setWidthF(reportLineWidth);
        targetSeries->setPen(p);

        autoZoomVisibleSeries();
        chartView_->repaint();

        // 렌더링 대기
        {
            QElapsedTimer waitTimer;
            waitTimer.start();
            while (waitTimer.elapsed() < 150) { qApp->processEvents(); }
        }

        chartView_->viewport()->repaint();
        qApp->processEvents();

        QPixmap tile = chartView_->grab();
        QRect targetRect((i % cols) * tileW, (i / cols) * tileH, tileW, tileH);
        painter.drawPixmap(targetRect, tile);

        // --- Overlay 시작 ---
        painter.save();

        // 폰트 설정
        QFont f("Segoe UI", reportOverlayFontSize, QFont::Bold);
        painter.setFont(f);
        QFontMetrics fm(f);

        // [데이터 이름 - 좌측 상단]
        QString sName = targetSeries->name();
        int nameW = fm.horizontalAdvance(sName);
        int nameH = fm.height();
        QRect nameBgRect(targetRect.left() + 15, targetRect.top() + 15, nameW + 20, nameH + 10);

        painter.setBrush(QColor(40, 40, 40, 180)); // 약간 더 진한 박스
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(nameBgRect, 5, 5);
        painter.setPen(Qt::white);
        painter.drawText(nameBgRect, Qt::AlignCenter, sName);

        // [Y Range (Delta Y) - 우측 하단]
        qreal dy = axisY_->max() - axisY_->min();
        QString dyText = "ΔY: " + QString::number(dy, 'e', 2).replace('e', 'E');
        int dyW = fm.horizontalAdvance(dyText);
        int dyH = fm.height();
        QRect dyBgRect(targetRect.right() - dyW - 35, targetRect.bottom() - dyH - 30, dyW + 20, dyH + 10);

        painter.setBrush(QColor(0, 0, 0, 160));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(dyBgRect, 5, 5);
        painter.setPen(Qt::yellow);
        painter.drawText(dyBgRect, Qt::AlignCenter, dyText);

        painter.restore();
        // --- Overlay 끝 ---
    }

    // 5. 상태 복구
    // 5. 상태 복구
    for (int i = 0; i < seriesList_.size(); ++i) {
        seriesList_[i]->setVisible(visibilityBackup[i]);
        // [수정] 백업으로 복원한 뒤...
        seriesList_[i]->setPen(penBackup[i]);
    }

    axisX_->setRange(oldXMin, oldXMax);
    axisY_->setRange(oldYMin, oldYMax);
    axisX_->setLabelsFont(oldAxisFont);
    axisY_->setLabelsFont(oldAxisFont);
    chart_->legend()->setVisible(wasLegendVisible);

    // [핵심 해결책] 리포트 생성 과정에서 꼬인 펜 상태를
    // 현재 메인 UI의 스핀박스 값으로 강제 재설정(Kick) 합니다.
    onPlotLineWidthChanged(ui->spinPlotLineWidth->value());

    // 6. 결과 처리 (클립보드 복사)
    QApplication::clipboard()->setImage(reportImage);

    if (hintItem_) {
        hintItem_->setText("Report Copied (Name & ΔY Included)");
        hintItem_->setVisible(true);
        QTimer::singleShot(2000, this, [this](){ if(hintItem_) hintItem_->setVisible(false); });
    }
}

void PlotWindow::onPlotScaleFontChanged(int value)
{
    if (!axisX_ || !axisY_) return;
    QFont font = axisX_->labelsFont();
    font.setPointSize(value);
    axisX_->setLabelsFont(font);
    axisY_->setLabelsFont(font);
}

void PlotWindow::onPlotLineWidthChanged(int value)
{
    if (seriesList_.isEmpty()) return;

    // 차트의 내부 리스트 대신, 우리가 보유한 seriesList_를 직접 순회합니다.
    for (auto* series : seriesList_) {
        // QXYSeries로 캐스팅 (QLineSeries의 부모)
        QXYSeries* xySeries = qobject_cast<QXYSeries*>(series);
        if (xySeries) {
            QPen pen = xySeries->pen();
            pen.setWidthF(static_cast<qreal>(value));

            pen.setStyle(Qt::SolidLine);
            xySeries->setPen(pen);
        }
    }

    // [강력 처방] 뷰포트와 씬(Scene) 전체를 강제로 다시 그리게 합니다.
    if (chartView_) {
        chartView_->viewport()->update();
        if (chartView_->scene()) chartView_->scene()->update();
    }
}

void PlotWindow::onListRowsMoved(const QModelIndex &, int start, int end,
                                 const QModelIndex &, int row)
{
    // Qt의 rowsMoved에서 row는 항목이 삽입될 위치를 의미합니다.
    int dest = row;
    if (start < dest) dest--; // 아래로 이동할 때 인덱스 보정

    // 내부 데이터 구조(QVector)의 순서 변경
    if (start != dest) {
        // seriesList_ 순서 교체
        auto item = seriesList_.takeAt(start);
        seriesList_.insert(dest, item);

        // seriesYCols_ 순서 교체 (매우 중요: 데이터 매칭 유지)
        auto col = seriesYCols_.takeAt(start);
        seriesYCols_.insert(dest, col);

        qDebug() << "Series reordered: " << start << " -> " << dest;
    }
}
