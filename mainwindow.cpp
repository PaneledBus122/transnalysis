#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "numerictablemodel.h"
#include "saveexportdialog.h"
#include "plotwindow.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QDebug>
#include <QtCharts/QChart>
#include <QtCharts/QLineSeries>
#include <QDoubleValidator>
#include <QLocale>
#include <QCoreApplication>
#include <QShortcut>
#include <QKeySequence>
#include <QSet>
#include <algorithm>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <limits>
#include <QSpinBox>
#include <QDate>
#include <QSysInfo>
#include <QtGlobal>
#include <QInputDialog>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QUrl>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    setAcceptDrops(true);

    connect(ui->tabWidget, &QTabWidget::currentChanged,
            this, &MainWindow::on_tabWidget_currentChanged);

    // initialize once
    on_tabWidget_currentChanged(ui->tabWidget->currentIndex());
    ui->tabWidget->setCurrentIndex(0);
    ui->logView->setReadOnly(true);          // prevent editing
    ui->logView->setUndoRedoEnabled(false);

    model_ = new NumericTableModel(this);
    ui->rawView->setModel(model_);
    ui->rawView->setSelectionBehavior(QAbstractItemView::SelectColumns);
    ui->rawView->setSelectionMode(QAbstractItemView::ExtendedSelection);

    auto *val = new QDoubleValidator(this);
    val->setNotation(QDoubleValidator::ScientificNotation); // allow 1e-3
    val->setLocale(QLocale::c()); // force dot as decimal separator
    val->setBottom(0.0);          // disallow negative values

    auto *posDoubleValidator = new QDoubleValidator(0.0, 1e300, 12, this);
    posDoubleValidator->setNotation(QDoubleValidator::ScientificNotation);

    ui->editThickness->setValidator(val);
    ui->editWidth->setValidator(val);
    ui->editLength->setValidator(val);
    ui->editRhoI->setValidator(val);
    ui->editRhoAmp->setValidator(posDoubleValidator);

    // Allow only positive doubles for field density
    auto *densValidator = new QDoubleValidator(0.0, 1e9, 6, this);
    densValidator->setNotation(QDoubleValidator::StandardNotation);
    ui->editFSSDensity->setValidator(densValidator);


    // Optional: placeholders
    ui->editThickness->setPlaceholderText("e.g. 1e-3");
    ui->editWidth->setPlaceholderText("e.g. 2.5e-3");
    ui->editLength->setPlaceholderText("e.g. 1e-3");

    auto *delShortcut = new QShortcut(QKeySequence::Delete, ui->rawView);
    connect(delShortcut, &QShortcut::activated, this, [this]() {

        if (!model_)
            return;

        QItemSelectionModel *sel = ui->rawView->selectionModel();
        if (!sel)
            return;

        QSet<int> cols;
        for (const QModelIndex &idx : sel->selectedIndexes())
            cols.insert(idx.column());

        deleteProcessedColumns(cols.values());
    });

    connect(ui->actionRemove_Last_Column_R, &QAction::triggered,
            this, [this]() {

                if (!model_ || model_->columnCount() == 0)
                    return;

                deleteProcessedColumns(
                    QList<int>{ model_->columnCount() - 1 }
                    );
            });

    installEndSpinClamp(ui->spinRhoEnd);
    installEndSpinClamp(ui->spinFSSymEnd);
    installEndSpinClamp(ui->spinTSSymPosEnd);
    installEndSpinClamp(ui->spinTSSymNegEnd);

    ui->spinEndRows->setKeyboardTracking(false);



    connect(ui->actionOpenFile, &QAction::triggered,
            this, &MainWindow::openDataFile);

    connect(ui->btnPlot, &QPushButton::clicked,
            this, &MainWindow::onPlotButtonClicked);

    connect(ui->actionSave_S, &QAction::triggered,
            this, &MainWindow::on_actionSave_S_triggered);

}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::openDataFile()
{
    QString fileName = QFileDialog::getOpenFileName(this, "Open Data File",
                                                    QString(), "Data Files (*.dat *.csv *.txt);;All Files (*)");

    if (!fileName.isEmpty()) {
        loadDataFile(fileName);
    }
}

void MainWindow::on_actionClose_C_triggered()
{
    if (model_) {

        QStringList emptyHeader;
        QVector<QVector<double>> emptyRows;
        model_->setNumericData(emptyHeader, emptyRows);
    }

    loadedFileName_.clear();
    loadedContent_.clear();

    ui->comboX->clear();
    ui->comboRhoX->clear();
    ui->comboRhoV->clear();
    ui->comboRhoI->clear();
    ui->comboFSSymX->clear();
    ui->spinStartRows->setRange(1, 999999999);
    ui->spinStartRows->setValue(1);
    ui->spinEndRows->setRange(1, 999999999);
    ui->spinEndRows->setValue(999999999);
    updateColumnCombos();
    updateRhoCombos();
    //updateRowSpins();
    ui->rawView->reset();

    logMessage("Closed file and cleared dataset.");
    statusBar()->showMessage("Closed dataset.", 2000);
}

void MainWindow::on_actionSave_S_triggered()
{
    if (!model_ || model_->rowCount() == 0 || model_->columnCount() == 0) {
        QMessageBox::information(this, "No data", "Nothing to save.");
        return;
    }

    SaveExportDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString outFile = dlg.filePath();
    if (outFile.isEmpty()) {
        QMessageBox::warning(this, "Invalid path", "Please choose a file name.");
        return;
    }

    saveData(outFile, dlg.includeRaw(), dlg.saveLog());
}

void MainWindow::populateTableFromText(const QString& text)
{
    ui->rawView->setUpdatesEnabled(false);

    QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    if (lines.isEmpty()) {
        ui->rawView->setUpdatesEnabled(true);
        return;
    }

    auto splitLine = [](const QString& line) {
        if (line.contains(','))  return line.split(',', Qt::KeepEmptyParts);
        if (line.contains('\t')) return line.split('\t', Qt::KeepEmptyParts);
        return line.split(' ', Qt::SkipEmptyParts);
    };

    // Parse header
    QStringList headerFields = splitLine(lines.first());
    int numCols = headerFields.size();

    // 통계 계산을 위한 변수들
    QVector<double> minVals(numCols, std::numeric_limits<double>::infinity());
    QVector<double> maxVals(numCols, -std::numeric_limits<double>::infinity());
    QVector<int> nanCounts(numCols, 0);

    QStringList headerOut;
    headerOut.reserve(numCols + 1);
    headerOut << "Row";
    headerOut << headerFields;

    QVector<QVector<double>> rowsOut;
    rowsOut.reserve(lines.size() - 1);

    int rowIndex = 1;
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty()) continue;

        const QStringList fields = splitLine(line);
        QVector<double> row;
        row.reserve(headerOut.size());

        row.push_back(double(rowIndex)); // Row column

        for (int c = 0; c < numCols; ++c) {
            double v = std::numeric_limits<double>::quiet_NaN();
            if (c < fields.size()) {
                bool ok = false;
                v = fields.at(c).trimmed().toDouble(&ok);
                if (!ok) v = std::numeric_limits<double>::quiet_NaN();
            }

            // 수치 통계 업데이트
            if (std::isnan(v)) {
                nanCounts[c]++;
            } else {
                if (v < minVals[c]) minVals[c] = v;
                if (v > maxVals[c]) maxVals[c] = v;
            }
            row.push_back(v);
        }

        rowsOut.push_back(row);
        rowIndex++;

        if ((rowIndex % 1000) == 0) {
            logMessage(QString("Parsing: %1 lines...").arg(rowIndex));
            QCoreApplication::processEvents();
        }
    }

    model_->setNumericData(headerOut, rowsOut);
    ui->rawView->setUpdatesEnabled(true);

    // --- 로그 추가 부분 ---
    logMessage(QString("--- Data Summary ---"));
    logMessage(QString("Total Rows: %1, Total Columns: %2").arg(rowsOut.size()).arg(numCols));
    logMessage("--------------------");
}

void MainWindow::updateColumnCombos()
{
    // 1. 모든 관련 콤보박스 초기화
    ui->comboX->clear();
    ui->comboRhoX->clear();
    ui->comboFSSymX->clear();
    ui->comboTSSymX->clear();
    ui->comboExtractFieldCol->clear();
    ui->comboExtractXCol->clear();

    // Single Hall 관련 콤보박스 초기화
    ui->comboSingleHallField->clear();
    ui->comboSingleHallVLong->clear();
    ui->comboSingleHallVTrans->clear();
    ui->comboSingleHallCurrentCol->clear();

    // [신규 추가] T-Sweep Hall 관련 콤보박스 초기화
    ui->comboTSwpHallTemp->clear();
    ui->comboTSwpHallVLong->clear();
    ui->comboTSwpHallVTrans->clear();
    ui->comboTSwpHallCurrentCol->clear();

    if (!model_)
        return;

    // 2. 모델의 모든 컬럼 순회하며 아이템 추가
    const int cols = model_->columnCount();
    for (int c = 0; c < cols; ++c) {
        QString name = model_->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
        if (name.isEmpty())
            name = QString("Col %1").arg(c);

        ui->comboX->addItem(name, c);
        ui->comboRhoX->addItem(name, c);
        ui->comboFSSymX->addItem(name, c);
        ui->comboTSSymX->addItem(name, c);
        ui->comboExtractFieldCol->addItem(name, c);
        ui->comboExtractXCol->addItem(name, c);

        // Single Hall 관련 아이템 추가
        ui->comboSingleHallField->addItem(name, c);
        ui->comboSingleHallVLong->addItem(name, c);
        ui->comboSingleHallVTrans->addItem(name, c);
        ui->comboSingleHallCurrentCol->addItem(name, c);

        // [신규 추가] T-Sweep Hall 관련 아이템 추가
        ui->comboTSwpHallTemp->addItem(name, c);
        ui->comboTSwpHallVLong->addItem(name, c);
        ui->comboTSwpHallVTrans->addItem(name, c);
        ui->comboTSwpHallCurrentCol->addItem(name, c);
    }

    // 3. 기본 인덱스 설정 (데이터가 있을 경우)
    if (cols >= 1) {
        ui->comboX->setCurrentIndex(0);
        ui->comboRhoX->setCurrentIndex(0);
        ui->comboFSSymX->setCurrentIndex(0);
        ui->comboTSSymX->setCurrentIndex(0);
        ui->comboExtractFieldCol->setCurrentIndex(0);
        ui->comboExtractXCol->setCurrentIndex(0);

        ui->comboSingleHallField->setCurrentIndex(0);
        ui->comboSingleHallVLong->setCurrentIndex(0);
        ui->comboSingleHallVTrans->setCurrentIndex(0);
        ui->comboSingleHallCurrentCol->setCurrentIndex(0);

        // [신규 추가] T-Sweep Hall 기본값 설정
        ui->comboTSwpHallTemp->setCurrentIndex(0);
        ui->comboTSwpHallVLong->setCurrentIndex(0);
        ui->comboTSwpHallVTrans->setCurrentIndex(0);
        ui->comboTSwpHallCurrentCol->setCurrentIndex(0);
    }
}

void MainWindow::onPlotButtonClicked()
{
    if (!model_ || model_->rowCount() == 0 || model_->columnCount() == 0) {
        QMessageBox::information(this, "No data", "Load a data file first.");
        return;
    }

    const int xCol = ui->comboX->currentData().toInt();
    const QString xLabel = ui->comboX->currentText();

    const int startRow = ui->spinStartRows->value();
    const int endRow   = ui->spinEndRows->value();
    const int maxRows  = std::max(0, endRow - startRow);

    QVector<int> yCols;
    QStringList yLabels;

    if (auto *sm = ui->rawView->selectionModel()) {
        const auto selCols = sm->selectedColumns();
        for (const auto &mi : selCols) {
            const int c = mi.column();
            if (c == xCol) continue;
            if (c < 0 || c >= model_->columnCount()) continue;
            yCols << c;
            yLabels << model_->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
        }
    }

    if (yCols.isEmpty()) {
        QMessageBox::warning(this, "Invalid selection", "Select a valid Y column.");
        return;
    }

    // --- 1. 데이터 추출 (두 옵션 모두에서 공통으로 사용) ---
    QVector<double> outX;
    QVector<QVector<double>> outYs;
    outYs.resize(yCols.size());

    outX.reserve(maxRows);
    for (auto &v : outYs) v.reserve(maxRows);

    const int rows = model_->rowCount();
    const int r0 = qBound(0, startRow, rows);
    const int r1 = qBound(0, startRow + maxRows, rows);

    for (int r = r0; r < r1; ++r) {
        bool okx = false;
        const double x = model_->index(r, xCol).data(Qt::DisplayRole).toDouble(&okx);
        if (!okx) continue;

        outX.push_back(x);
        for (int k = 0; k < yCols.size(); ++k) {
            bool oky = false;
            const double y = model_->index(r, yCols[k]).data(Qt::DisplayRole).toDouble(&oky);
            outYs[k].push_back(oky ? y : qQNaN());
        }
    }

    // 헤더 구성
    const QString tag = "[p] ";
    const QString userPrefix = ui->editPlotPrefix->toPlainText().trimmed();
    const QString pre = userPrefix.isEmpty() ? "" : (userPrefix + " ");

    QStringList newHeaders;
    newHeaders << (tag + pre + xLabel);
    for (const QString& yl : yLabels) newHeaders << (tag + pre + yl);

    QVector<QVector<double>> newCols;
    newCols << outX;
    for (const auto& v : outYs) newCols << v;

    // --- 2. 옵션별 실행 ---

    // 옵션 A: 메인 테이블에 컬럼 추가
    if (ui->checkPlotNewColumn->isChecked()) {
        model_->appendColumns(newHeaders, newCols);
        updateColumnCombos();
        updateRhoCombos();

        ui->rawView->setUpdatesEnabled(false);
        const int last = model_->columnCount() - 1;
        const int firstNew = last - (newHeaders.size() - 1);
        for (int c = firstNew; c <= last; ++c)
            ui->rawView->resizeColumnToContents(c);
        ui->rawView->setUpdatesEnabled(true);
    }

    // 옵션 B: [수정] Resistivity처럼 새로운 데이터 창 띄우기
    if (ui->checkPlotNewWindow->isChecked()) {
        openResultWindow("Plotted Subset Data", newHeaders, newCols);
    }

    // --- 3. [변경] 체크박스와 상관없이 항상 그래프 창은 띄움 ---
    auto *pw = new PlotWindow(model_, xCol, yCols, xLabel, yLabels, startRow, maxRows, nullptr);
    pw->setAttribute(Qt::WA_DeleteOnClose);
    pw->show();
}

void MainWindow::on_actionExit_X_triggered()
{
    QApplication::quit();
}

void MainWindow::logMessage(const QString& msg)
{
    ui->logView->appendPlainText(msg);

    auto cursor = ui->logView->textCursor();
    cursor.movePosition(QTextCursor::End);
    ui->logView->setTextCursor(cursor);

    QCoreApplication::processEvents();
}

void MainWindow::updateRhoCombos()
{
    ui->comboRhoV->clear();
    ui->comboRhoI->clear();

    if (!model_ || model_->columnCount() == 0)
        return;

    const int cols = model_->columnCount();
    for (int c = 0; c < cols; ++c) {
        QString name = model_->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
        if (name.isEmpty())
            name = QString("Col %1").arg(c);

        // Store column index as userData
        ui->comboRhoX->addItem(name, c);
        ui->comboRhoV->addItem(name, c);
        ui->comboRhoI->addItem(name, c);
        ui->comboFSSymX->addItem(name, c);
    }

    // Optional sensible defaults
    if (cols >= 2) {
        ui->comboRhoV->setCurrentIndex(0);
        ui->comboRhoI->setCurrentIndex(1);
    }
}

void MainWindow::on_btnResistivity_clicked()
{
    if (!model_ || model_->rowCount() == 0 || model_->columnCount() == 0) {
        QMessageBox::information(this, "No data", "Load a data file first.");
        return;
    }

    // [1] 입력 파라미터 수집
    const int xCol = ui->comboRhoX->currentData().toInt();
    const int vCol = ui->comboRhoV->currentData().toInt();
    const int iCol = ui->comboRhoI->currentData().toInt();
    // (Validation 체크 생략 - 기존과 동일)
    if (xCol < 0 || vCol < 0 || iCol < 0) return;

    int rowStart = ui->spinRhoStart->value();
    int rowEnd   = ui->spinRhoEnd->value();
    if (rowStart > rowEnd) std::swap(rowStart, rowEnd);

    bool okT, okW, okL, okAmp;
    const double t = ui->editThickness->text().toDouble(&okT);
    const double w = ui->editWidth->text().toDouble(&okW);
    const double L = ui->editLength->text().toDouble(&okL);
    const double amp = ui->editRhoAmp->text().toDouble(&okAmp);

    if (!okT || !okW || !okL || t <= 0 || w <= 0 || L <= 0) return;

    const double geomFactor = (t * w) / L;
    const QString iText = ui->editRhoI->text().trimmed();
    const double I_const = iText.isEmpty() ? 0.0 : iText.toDouble();

    const QString xName = ui->comboRhoX->currentText();
    const QString vName = ui->comboRhoV->currentText();
    const QString iName = ui->comboRhoI->currentText();

    // [2] 실제 데이터 계산 루프 (로그 출력 없이 조용히 계산)
    QVector<double> outX, outRho, outCond;
    outX.reserve(rowEnd - rowStart);
    int skipped = 0;

    for (int r = rowStart; r < rowEnd; ++r) {
        bool okx=false, okv=false, oki=false;
        const double x = model_->index(r, xCol).data().toDouble(&okx);
        const double V = amp * model_->index(r, vCol).data().toDouble(&okv);
        double I = (I_const != 0.0) ? I_const : model_->index(r, iCol).data().toDouble(&oki);

        if (!okx || !okv || (I_const == 0.0 && !oki) || I == 0.0) {
            skipped++; continue;
        }
        double rho = (V / I) * geomFactor;
        outX.push_back(x);
        outRho.push_back(rho);
        outCond.push_back(rho != 0 ? 1.0/rho : 0.0);
    }

    if (outX.isEmpty()) return;

    // [3] 결과 적용 (컬럼 추가 및 새 창 띄우기)
    QString userPrefix = ui->editRho->toPlainText().trimmed().section('\n', 0, 0).trimmed();
    const QString tag = userPrefix.isEmpty() ? "[p] " : QString("[p] %1 ").arg(userPrefix);
    QStringList outHeaders{ tag + xName + "_X", tag + vName + "_rho", tag + "cond(" + vName + ")" };
    QVector<QVector<double>> outCols{ outX, outRho, outCond };

    if (ui->checkRhoNewColumn->isChecked()) {
        model_->appendColumns(outHeaders, outCols);
        updateColumnCombos(); updateRhoCombos();
    }

    MainWindow* resultWindow = nullptr;
    if (ui->checkRhoNewWindow->isChecked()) {
        resultWindow = openResultWindow("Resistivity Analysis", outHeaders, outCols);
    }

    // [4] 로그 출력 섹션 (하단에 집중)
    auto sendLog = [&](const QString& msg) {
        this->logMessage(msg);             // 메인 창 로그
        if (resultWindow) resultWindow->logMessage(msg); // 새 결과 창 로그
    };

    sendLog("");
    sendLog(QString("Resistivity is obtained from [%1] using the following formula:").arg(QFileInfo(loadedFileName_).fileName()));
    sendLog("  ρ = (V / I) × (t × w / L) and σ = 1 / ρ. ");
    sendLog("  SI units: V (V), I (A), t (m), w (m), L (m).");
    sendLog(QString("  V: Column [%1] (V), Amp: %2").arg(vName).arg(amp));
    sendLog(QString("  I: %1").arg(I_const != 0.0 ? QString::number(I_const, 'e', 3) + " (A) [Fixed]" : "Column [" + iName + "]"));
    sendLog(QString("  Dimensions: L=%1, w=%2, t=%3 (m)").arg(QString::number(L, 'e', 2), QString::number(w, 'e', 2), QString::number(t, 'e', 2)));
    sendLog(QString("  Output: X(%1), Resistivity(ρ), Conductivity(σ)").arg(xName));
    sendLog(QString("  Rows: %1 to %2 used. (%3 points written, %4 skipped)")
                .arg(rowStart).arg(rowEnd).arg(outX.size()).arg(skipped));
    sendLog("------------------------------------------------------------");
}

void MainWindow::on_btnFSSym_clicked()
{
    if (!model_ || model_->rowCount() == 0 || model_->columnCount() == 0) {
        QMessageBox::information(this, "No data", "Load a data file first.");
        return;
    }

    // [1] 입력 파라미터 수집 및 검증
    const QString selectedXName = ui->comboFSSymX->currentText();
    const int xCol = ui->comboFSSymX->currentData().toInt();
    if (xCol < 0 || xCol >= model_->columnCount()) {
        QMessageBox::warning(this, "Invalid selection", "Select a valid Field (X) column.");
        return;
    }

    QVector<int> yCols;
    QStringList yNames;
    if (auto *sm = ui->rawView->selectionModel()) {
        const auto selCols = sm->selectedColumns();
        for (const auto &mi : selCols) {
            const int c = mi.column();
            if (c == xCol) continue;
            if (c < 0 || c >= model_->columnCount()) continue;
            yCols << c;
            yNames << model_->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
        }
    }

    if (yCols.isEmpty()) {
        QMessageBox::warning(this, "No Y selected", "Select one or more Y columns in the table.");
        return;
    }

    int rowStart = ui->spinFSSymStart->value();
    int rowEnd   = ui->spinFSSymEnd->value();
    if (rowStart > rowEnd) std::swap(rowStart, rowEnd);

    const int rows = model_->rowCount();
    rowStart = std::max(0, rowStart);
    rowEnd   = std::min(rows, rowEnd);

    bool okStep = false;
    const double step = ui->editFSSDensity->text().trimmed().toDouble(&okStep);
    if (!okStep || step <= 0.0) {
        QMessageBox::warning(this, "Invalid field density", "Field density must be a positive number.");
        return;
    }

    // [2] 데이터 계산 (조용히 처리)
    QVector<double> xNew;
    QStringList outHeaders;
    QVector<QVector<double>> outCols;
    QString prefix = ui->editFSSym->toPlainText().trimmed().section('\n', 0, 0).trimmed();

    const bool ok = buildFSSymColumns(xCol, yCols, yNames,
                                      rowStart, rowEnd, step,
                                      prefix,
                                      xNew, outHeaders, outCols);
    if (!ok) {
        QMessageBox::warning(this, "FS Symmetrization failed", "Check field range and data validity.");
        return;
    }

    // [3] 결과 적용 (컬럼 추가 및 새 창 생성)
    const bool addColumn = ui->checkFSSNewColumn->isChecked();
    const bool newWindow = ui->checkFSSNewWindow->isChecked();

    if (!addColumn && !newWindow) {
        QMessageBox::information(this, "No output selected", "Select at least one output option.");
        return;
    }

    if (addColumn) {
        model_->appendColumns(outHeaders, outCols);
        updateColumnCombos();
        updateRhoCombos();

        ui->rawView->setUpdatesEnabled(false);
        const int last = model_->columnCount() - 1;
        const int firstNew = qMax(0, last - int(outHeaders.size()) + 1);
        for (int c = firstNew; c <= last; ++c)
            ui->rawView->resizeColumnToContents(c);
        ui->rawView->setUpdatesEnabled(true);
    }

    MainWindow* resultWindow = nullptr;
    if (newWindow) {
        resultWindow = openResultWindow("Field-Sweep Symmetrization", outHeaders, outCols);
    }

    // [4] 로그 출력 섹션 (하단 집중 및 동기화)
    auto sendLog = [&](const QString& msg) {
        this->logMessage(msg);
        if (resultWindow) resultWindow->logMessage(msg);
    };

    sendLog("");
    sendLog(QString("Field-Sweep Symmetrization is obtained from [%1]")
                .arg(QFileInfo(loadedFileName_).fileName()));
    sendLog("");
    sendLog("  Y_sym(B) = ½ [Y(B) + Y(−B)]");
    sendLog("  Y_asym(B) = ½ [Y(B) − Y(−B)]");
    sendLog("");
    sendLog(QString("  Field (X): Column [%1]").arg(selectedXName));
    sendLog(QString("  Input Y: %1").arg(yNames.join(", ")));
    sendLog(QString("  Field Interval (Step): %1").arg(step));
    sendLog("");
    sendLog(QString("  Grid: %1 points generated over the symmetric field range.").arg(xNew.size()));
    sendLog(QString("  Rows from %1 to %2 used.").arg(rowStart).arg(rowEnd));
    sendLog(QString("  Output Prefix: [%1]").arg(prefix.isEmpty() ? "None" : prefix));

    sendLog(QString(" >> Analysis Complete: %1 columns created.").arg(outHeaders.size()));
    sendLog("------------------------------------------------------------");
}


void MainWindow::sortAndAverage(QVector<double>& x, QVector<double>& y)
{
    if (x.isEmpty() || x.size() != y.size()) return;

    // 1. (X, Y) 쌍으로 묶어서 X 기준 정렬
    QVector<std::pair<double, double>> pairs;
    for (int i = 0; i < x.size(); ++i) pairs.push_back({x[i], y[i]});
    std::sort(pairs.begin(), pairs.end());

    QVector<double> newX, newY;
    int i = 0;
    while (i < pairs.size()) {
        double currentX = pairs[i].first;
        double sumY = 0;
        int count = 0;

        // 같은 X값을 가진 구간을 찾아 합산
        while (i < pairs.size() && pairs[i].first == currentX) {
            sumY += pairs[i].second;
            count++;
            i++;
        }

        newX.push_back(currentX);
        newY.push_back(sumY / count); // 산술 평균
    }

    x = newX;
    y = newY;
}

static bool interpLinear(const QVector<double>& x, const QVector<double>& y, double xq, double& yq)
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

bool MainWindow::buildFSSymColumns(int xCol,
                                   const QVector<int>& yCols,
                                   const QStringList& yNames,
                                   int rowStart, int rowEnd,
                                   double step,
                                   const QString& prefix,
                                   QVector<double>& xNew,
                                   QStringList& outHeaders,
                                   QVector<QVector<double>>& outCols)
{
    // Field 범위 확보 (raw에서 유효한 값만)
    double minB = +std::numeric_limits<double>::infinity();
    double maxB = -std::numeric_limits<double>::infinity();

    for (int r = rowStart; r < rowEnd; ++r) {
        bool ok=false;
        const double b = model_->index(r, xCol).data(Qt::DisplayRole).toDouble(&ok);
        if (!ok) continue;
        minB = std::min(minB, b);
        maxB = std::max(maxB, b);
    }
    if (!std::isfinite(minB) || !std::isfinite(maxB)) return false;

    const double maxAbsField = std::min(std::abs(minB), std::abs(maxB)) - 1.0;
    if (!(maxAbsField > 0.0) || !(step > 0.0)) return false;

    // 공통 grid
    xNew.clear();
    const int nSteps = static_cast<int>(
                           std::floor((2.0 * maxAbsField) / step + 1e-12)
                           ) + 1;

    xNew.reserve(nSteps);

    for (int i = 0; i < nSteps; ++i) {
        const double b = -maxAbsField + i * step;
        xNew.push_back(b);
    }


    const QString p = prefix.trimmed();
    const QString tag = p.isEmpty() ? QString("[p] ") : QString("[p] %1 ").arg(p);

    outHeaders.clear();
    outCols.clear();

    // 공통 X 컬럼 하나
    outHeaders << (tag + "FSsym Field");
    outCols << xNew;

    // 각 Y 처리
    for (int k = 0; k < yCols.size(); ++k) {
        const int yCol = yCols[k];

        QVector<double> bx, vy;
        bx.reserve(rowEnd - rowStart);
        vy.reserve(rowEnd - rowStart);

        for (int r = rowStart; r < rowEnd; ++r) {
            bool okb=false, okv=false;
            const double b = model_->index(r, xCol).data(Qt::DisplayRole).toDouble(&okb);
            const double v = model_->index(r, yCol).data(Qt::DisplayRole).toDouble(&okv);
            if (!okb || !okv) continue;
            bx.push_back(b);
            vy.push_back(v);
        }

        sortAndAverage(bx, vy);
        if (bx.size() < 2) continue;

        QVector<double> orig, sym, asym;
        orig.reserve(xNew.size());
        sym.reserve(xNew.size());
        asym.reserve(xNew.size());

        const int n = xNew.size();
        for (int i = 0; i < n; ++i) {
            const double b = xNew[i];

            double vp = 0.0, vn = 0.0;
            if (!interpLinear(bx, vy,  b, vp) || !interpLinear(bx, vy, -b, vn)) {
                orig.push_back(qQNaN());
                sym.push_back(qQNaN());
                asym.push_back(qQNaN());
                continue;
            }

            orig.push_back(vp);
            sym.push_back(0.5 * (vp + vn));
            asym.push_back(0.5 * (vp - vn));
        }


        const QString base = (k < yNames.size() ? yNames[k] : QString("Y%1").arg(k));
        outHeaders << (tag + "FSsym " + base + " orig");
        outHeaders << (tag + "FSsym " + base + " sym");
        outHeaders << (tag + "FSsym " + base + " asym");
        outCols << orig << sym << asym;
    }

    return (outHeaders.size() == outCols.size());
}

void MainWindow::updateRowSpins()
{
    const int rows = model_ ? model_->rowCount() : 0;

    auto setupStart = [&](QSpinBox* s){
        s->blockSignals(true);
        if (rows <= 0) {
            s->setRange(1, 1);
            s->setValue(1);
            s->setEnabled(false);
        } else {
            s->setEnabled(true);
            s->setRange(1, rows);
            s->setValue(qBound(1, s->value(), rows));
        }
        s->blockSignals(false);
    };

    auto clampEndOnly = [&](QSpinBox* e){
        e->blockSignals(true);
        if (rows <= 0) {
            e->setEnabled(false);
            e->setValue(1);
        } else {
            e->setEnabled(true);
            e->setValue(qBound(1, e->value(), rows));
        }
        e->blockSignals(false);
    };

    // Plot row range
    setupStart(ui->spinStartRows);
    clampEndOnly(ui->spinEndRows);

    // FS Sym row range
    setupStart(ui->spinFSSymStart);
    clampEndOnly(ui->spinFSSymEnd);

    // Rho row range
    setupStart(ui->spinRhoStart);
    clampEndOnly(ui->spinRhoEnd);

    // TS +H / -H ranges (1-based UI)
    setupStart(ui->spinTSSymPosStart);
    clampEndOnly(ui->spinTSSymPosEnd);

    setupStart(ui->spinTSSymNegStart);
    clampEndOnly(ui->spinTSSymNegEnd);
}

static int countInRange(const QVector<double>& x, double a, double b)
{
    if (x.isEmpty()) return 0;
    auto it1 = std::lower_bound(x.begin(), x.end(), a);
    auto it2 = std::upper_bound(x.begin(), x.end(), b);
    return int(it2 - it1);
}

bool MainWindow::buildTSSymColumns(int xCol,
                                   const QString& xName,
                                   const QVector<int>& yCols,
                                   const QStringList& yNames,
                                   int posStart, int posEnd,
                                   int negStart, int negEnd,
                                   const QString& prefix,
                                   int scanWait, // [신규] 안정화를 위해 무시할 포인트 수
                                   QVector<double>& xNew,
                                   QStringList& outHeaders,
                                   QVector<QVector<double>>& outCols)
{
    const QString p = prefix.trimmed();
    const QString tag = p.isEmpty() ? QString("[p] ") : QString("[p] %1 ").arg(p);

    outHeaders.clear();
    outCols.clear();
    xNew.clear();

    // --- 1) 데이터 수집 및 필터링/평균화를 위한 람다 함수 정의 ---
    auto processSegment = [&](int start, int end, int targetYCol, QVector<double>& outX, QVector<double>& outY) {
        QVector<double> tempX, tempY;
        double prevX = -1e30;
        int counter = -1;

        for (int r = start; r < end; ++r) {
            bool okX = false, okY = false;
            const double cx = model_->index(r, xCol).data().toDouble(&okX);
            const double cy = model_->index(r, targetYCol).data().toDouble(&okY);
            if (!okX || !okY) continue;

            // [수정] scanWait가 0이면 필터링 없이 모든 데이터를 수집 (연속 스윕 모드)
            if (scanWait == 0) {
                tempX.push_back(cx);
                tempY.push_back(cy);
                continue;
            }

            // [기존 필터 로직] scanWait > 0인 경우 (Step-and-Scan 모드)
            if (cx != prevX) {
                // 모터 이동 중: 카운터 리셋 및 현재 데이터 스킵
                counter = scanWait;
                prevX = cx;
            } else {
                // 모터 정지 상태
                if (counter > 0) {
                    counter--; // 안정화 대기 포인트 소진
                } else if (counter == 0) {
                    // 안정화 완료: 데이터 수집
                    tempX.push_back(cx);
                    tempY.push_back(cy);
                }
            }
        }

        // 수집된 데이터를 X별로 정렬하고 평균 내기
        sortAndAverage(tempX, tempY);
        outX = tempX;
        outY = tempY;
    };

    // --- 2) Overlap 영역 결정을 위한 대표 X축 추출 ---
    // 첫 번째 Y 컬럼을 기준으로 Positive/Negative 구간의 유효 X 범위를 파악합니다.
    QVector<double> xP_ref, yP_ref, xN_ref, yN_ref;
    processSegment(posStart, posEnd, yCols[0], xP_ref, yP_ref);
    processSegment(negStart, negEnd, yCols[0], xN_ref, yN_ref);

    if (xP_ref.size() < 2 || xN_ref.size() < 2) return false;

    const double xMin = std::max(xP_ref.front(), xN_ref.front());
    const double xMax = std::min(xP_ref.back(),  xN_ref.back());
    if (!(xMax > xMin)) return false;

    // 공통 그리드 밀도는 필터링/평균화된 후의 포인트 수 중 작은 쪽을 기준으로 합니다.
    const int nGrid = std::min(countInRange(xP_ref, xMin, xMax), countInRange(xN_ref, xMin, xMax));
    if (nGrid < 2) return false;

    // 공통 그리드 xNew 생성
    xNew.reserve(nGrid);
    const double dx = (xMax - xMin) / double(nGrid - 1);
    for (int i = 0; i < nGrid; ++i)
        xNew.push_back(xMin + dx * double(i));

    outHeaders << (tag + "TSsym " + xName);
    outCols << xNew;

    // --- 3) 각 Y 채널에 대해 필터링 -> 평균 -> 보간 -> 대칭화 수행 ---
    for (int k = 0; k < yCols.size(); ++k) {
        const int yCol = yCols[k];
        if (yCol < 0 || yCol >= model_->columnCount()) continue;

        QVector<double> txP, yP, txN, yN;
        processSegment(posStart, posEnd, yCol, txP, yP);
        processSegment(negStart, negEnd, yCol, txN, yN);

        if (txP.size() < 2 || txN.size() < 2) continue;

        QVector<double> sym, asym;
        sym.reserve(xNew.size());
        asym.reserve(xNew.size());

        for (double xVal : xNew) {
            double yp = 0.0, yn = 0.0;
            // 평균화된 (tx, y) 데이터를 기반으로 선형 보간
            if (!interpLinear(txP, yP, xVal, yp) || !interpLinear(txN, yN, xVal, yn)) {
                sym.push_back(qQNaN());
                asym.push_back(qQNaN());
                continue;
            }
            sym.push_back(0.5 * (yp + yn));
            asym.push_back(0.5 * (yp - yn));
        }

        const QString base = (k < yNames.size() ? yNames[k] : QString("Y%1").arg(k));
        outHeaders << (tag + "TSsym " + base + " sym");
        outHeaders << (tag + "TSsym " + base + " asym");
        outCols << sym << asym;
    }

    return (outHeaders.size() == outCols.size());
}
void MainWindow::on_btnTSSym_clicked()
{
    if (!model_ || model_->rowCount() == 0 || model_->columnCount() == 0) {
        QMessageBox::information(this, "No data", "Load a data file first.");
        return;
    }

    // [1] 입력 파라미터 수집
    const QString selectedXName = ui->comboTSSymX->currentText();
    const int xCol = ui->comboTSSymX->currentData().toInt();

    // [신규] Scan Filter 설정값 가져오기
    const int scanWait = ui->spinScanFilterWait->value();

    if (xCol < 0 || xCol >= model_->columnCount()) {
        QMessageBox::warning(this, "Invalid selection", "Select a valid Sweep Variable (X) column.");
        return;
    }

    // Y columns 수집 로직
    QVector<int> yCols;
    QStringList yNames;
    if (auto *sm = ui->rawView->selectionModel()) {
        const auto selCols = sm->selectedColumns();
        for (const auto &mi : selCols) {
            const int c = mi.column();
            if (c == xCol) continue;
            if (c < 0 || c >= model_->columnCount()) continue;
            yCols << c;
            yNames << model_->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
        }
    }

    if (yCols.isEmpty()) {
        QMessageBox::warning(this, "No Y selected", "Select one or more Y columns in the table.");
        return;
    }

    QString prefix = ui->editTSSym->toPlainText().trimmed().section('\n', 0, 0).trimmed();

    // Row ranges 계산
    auto toZeroBasedExclusive = [&](int s1, int e1, int rows, int& s0, int& e0)->bool {
        if (s1 > e1) std::swap(s1, e1);
        s1 = std::max(1, s1); e1 = std::min(rows, e1);
        s0 = s1 - 1; e0 = e1;
        return (s0 < e0);
    };

    const int rows = model_->rowCount();
    int posStart = 0, posEnd = 0, negStart = 0, negEnd = 0;
    if (!toZeroBasedExclusive(ui->spinTSSymPosStart->value(), ui->spinTSSymPosEnd->value(), rows, posStart, posEnd) ||
        !toZeroBasedExclusive(ui->spinTSSymNegStart->value(), ui->spinTSSymNegEnd->value(), rows, negStart, negEnd)) {
        QMessageBox::warning(this, "Invalid range", "Check if +H or -H row ranges are valid.");
        return;
    }

    // [2] 데이터 계산 (scanWait 인자 추가)
    QVector<double> xNew;
    QStringList outHeaders;
    QVector<QVector<double>> outCols;

    const bool ok = buildTSSymColumns(xCol, selectedXName, yCols, yNames,
                                      posStart, posEnd, negStart, negEnd,
                                      prefix, scanWait, xNew, outHeaders, outCols);
    if (!ok) {
        QMessageBox::warning(this, "Symmetrization failed", "Check overlap range of the sweep variable.");
        return;
    }

    // [3] 결과 적용
    const bool addColumn = ui->checkTSSNewColumn->isChecked();
    const bool newWindow = ui->checkTSSNewWindow->isChecked();

    if (addColumn) {
        model_->appendColumns(outHeaders, outCols);
        updateColumnCombos();
        updateRhoCombos();

        // 테이블 뷰 갱신 처리
        ui->rawView->setUpdatesEnabled(false);
        const int last = model_->columnCount() - 1;
        const int firstNew = qMax(0, last - (int)outHeaders.size() + 1);
        for(int c = firstNew; c <= last; ++c) ui->rawView->resizeColumnToContents(c);
        ui->rawView->setUpdatesEnabled(true);
    }

    MainWindow* resultWindow = nullptr;
    if (newWindow) {
        resultWindow = openResultWindow("Variable-Sweep Symmetrization", outHeaders, outCols);
    }

    // [4] 로그 출력 섹션 (필터 정보 보강)
    auto sendLog = [&](const QString& msg) {
        this->logMessage(msg);
        if (resultWindow) resultWindow->logMessage(msg);
    };

    sendLog("");
    sendLog(QString("Sweep-Variable Symmetrization is obtained from [%1]")
                .arg(QFileInfo(loadedFileName_).fileName()));
    sendLog("");
    sendLog("  Symmetric part: $Y_{sym}(X) = \\frac{1}{2} [Y_{+H}(X) + Y_{-H}(X)]$");
    sendLog("  Asymmetric part: $Y_{asym}(X) = \\frac{1}{2} [Y_{+H}(X) - Y_{-H}(X)]$");
    sendLog("");
    sendLog(QString("  Sweep Variable (X): Column [%1]").arg(selectedXName));
    sendLog(QString("  Input Y channels: %1").arg(yNames.join(", ")));

    // Scan Filter 로그 추가
    if (scanWait > 0) {
        sendLog(QString("  Scan Filter applied: Skipping first %1 points in each step for stabilization.")
                    .arg(scanWait));
    } else {
        sendLog("  Scan Filter: Disabled (all points used).");
    }

    sendLog(QString("  +H Segment Rows: %1 to %2").arg(posStart + 1).arg(posEnd));
    sendLog(QString("  -H Segment Rows: %1 to %2").arg(negStart + 1).arg(negEnd));
    sendLog("");

    if (!xNew.isEmpty()) {
        sendLog(QString("  Common Overlap Range: %1 to %2")
                    .arg(QString::number(xNew.front(), 'g', 5))
                    .arg(QString::number(xNew.back(), 'g', 5)));
        sendLog(QString("  Interpolation: %1 stable points generated on a common grid.")
                    .arg(xNew.size()));
    }

    sendLog(QString("  Output Prefix: [%1]").arg(prefix.isEmpty() ? "None" : prefix));
    sendLog(QString(" >> Analysis Complete: %1 columns created.").arg(outHeaders.size()));
    sendLog("------------------------------------------------------------");
}

QString MainWindow::makeLogFileName(const QString& dataFile) const
{
    QFileInfo fi(dataFile);
    return fi.path() + "/" + fi.completeBaseName() + ".log";
}

void MainWindow::saveData(const QString& outFile, bool includeRaw, bool saveLog)
{
    const QString procPrefix = "[p]";

    // 확장자에 따른 구분자 결정
    const QString lower = outFile.toLower();
    const QChar sep = lower.endsWith(".tsv") ? '\t' : ',';

    // 저장할 컬럼 결정
    QVector<int> cols;
    cols.reserve(model_->columnCount());

    for (int c = 0; c < model_->columnCount(); ++c) {
        const QString h = model_->headerAt(c);
        const bool isProc = h.startsWith(procPrefix);

        if (includeRaw) {
            cols.push_back(c);
        } else {
            if (isProc)
                cols.push_back(c);
        }
    }

    if (cols.isEmpty()) {
        QMessageBox::warning(this, "No columns",
                             "No columns matched the selected save option.");
        return;
    }

    // [1] 데이터 파일 저장 (기존과 동일하게 새로 쓰기)
    QFile f(outFile);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Error", "Failed to open output file.");
        return;
    }

    QTextStream out(&f);
    for (int i = 0; i < cols.size(); ++i) {
        if (i) out << sep;
        out << model_->headerAt(cols[i]);
    }
    out << "\n";

    for (int r = 0; r < model_->rowCount(); ++r) {
        for (int i = 0; i < cols.size(); ++i) {
            if (i) out << sep;
            const double v = model_->valueAt(r, cols[i]);
            out << QString::number(v, 'g', 12);
        }
        out << "\n";
    }
    f.close();

    logMessage(QString("Saved data: %1").arg(outFile));
    statusBar()->showMessage("Saved: " + outFile, 3000);

    // [2] 로그 파일 저장 (Append 모드 및 타임스탬프 추가)
    if (saveLog) {
        const QString logFile = makeLogFileName(outFile);
        QFile lf(logFile);

        // QIODevice::Append 모드를 사용하여 기존 파일 끝에 덧붙입니다.
        if (lf.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream lout(&lf);

            // 현재 날짜 및 시간 획득
            QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");

            // 가독성을 위한 헤더 삽입
            lout << "\n";
            lout << "------------------------------------------------------------\n";
            lout << " [LOG APPENDED] " << timestamp << "\n";
            lout << " Data File: " << QFileInfo(outFile).fileName() << "\n";
            lout << "------------------------------------------------------------\n";

            // 현재 logView의 내용 전체 기록
            lout << ui->logView->toPlainText();
            lout << "\n";

            lf.close();
            logMessage(QString("Log successfully appended to: %1").arg(logFile));
        } else {
            logMessage(QString("Failed to open log file for appending: %1").arg(logFile));
        }
    }
}

void MainWindow::on_tabWidget_currentChanged(int index)
{
    if (!ui->editHelp) return;

    switch (index) {
    case 0:
        ui->editHelp->setHtml(R"(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8" />
<style>
  body { font-family:'Segoe UI'; font-size:10pt; color:#999; }
  p { margin: 6px 0; line-height: 1.35; }
  .eqbox{
    margin: 8px 0 10px 0;
    padding: 8px 10px;
    border: 1px solid #d6d6d6;
    border-left: 4px solid #2b6cb0;
    border-radius: 6px;
  }
  .eq{
    font-size: 14pt;          /* ~2x vs 9pt */
    font-weight: 700;
    color: #999;
    letter-spacing: 0.2px;
  }
  .note { color:#999; }
  .key  { font-weight:600; color:#444; }
  .pill{
    display:inline-block;
    padding: 0px 6px;
    border: 1px solid #cfcfcf;
    border-radius: 10px;
    font-weight:600;
  }
</style>
</head>

<body>

<p class="note">
  <span class="key">File Open: </span> Ctrl+O
</p>

<p class="note">
  <span class="key">File Save: </span> Ctrl+S
</p>

<p class="note">
  <span class="key">File Close: </span> Ctrl+W
</p>

<p class="note">
  <span class="key">Delete Selected Column: </span> Delete
</p>

<p class="note">
  <span class="key">Delete Last Column: </span> Ctrl+Delete
</p>

<p class="note">
  <span class="key">Delete Last Rows: </span> In the Edit menu
</p>
</body>
</html>
)");
        break;

    case 1:
        ui->editHelp->setHtml(R"(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8" />
<style>
  body { font-family:'Segoe UI'; font-size:10pt; color:#999; }
  p { margin: 6px 0; line-height: 1.35; }
  .eqbox{
    margin: 8px 0 10px 0;
    padding: 8px 10px;
    border: 1px solid #d6d6d6;
    border-left: 4px solid #2b6cb0;
    border-radius: 6px;
  }
  .eq{
    font-size: 14pt;          /* ~2x vs 9pt */
    font-weight: 700;
    color: #999;
    letter-spacing: 0.2px;
  }
  .note { color:#999; }
  .key  { font-weight:600; color:#444; }
  .pill{
    display:inline-block;
    padding: 0px 6px;
    border: 1px solid #cfcfcf;
    border-radius: 10px;
    font-weight:600;
  }
</style>
</head>

<body>

<div class="eqbox">
  <div class="eq">
    &rho; = (V / I) &times; (t &times; w / L)
  </div>
</div>

<p class="note">
  <span class="key">SI units:</span> V (V), I (A), t (m), w (m), L (m).
</p>

<p class="note">
  <span class="key">Amplification:</span> if specified, voltage is corrected as
  <b>V = amplification &times; V<sub>original</sub></b>.
</p>

<p class="note">
  <span class="key">Current source:</span> a non-zero constant current overrides the column selection;
  otherwise the selected <b>I</b> column is used.
</p>

<p class="note">
  <span class="key">Conductivity:</span> <b>&sigma; = 1 / &rho;</b>.
</p>

<p class="note">
  <span class="key">Output:</span> results are appended with prefix <span class="pill">[p]</span>.
</p>

</body>
</html>

)");
        break;

    case 2:
        ui->editHelp->setHtml(R"(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8" />
<style>
  body { font-family:'Segoe UI'; font-size:10pt; color:#999; }
  p { margin: 6px 0; line-height: 1.35; }
  .eqbox{
    margin: 8px 0 10px 0;
    padding: 8px 10px;
    border: 1px solid #d6d6d6;
    border-left: 4px solid #2b6cb0;
    border-radius: 6px;
  }
  .eq{
    font-size: 14pt;
    font-weight: 700;
    color: #999;
    letter-spacing: 0.2px;
  }
  .note { color:#999; }
  .key  { font-weight:600; color:#444; }
  .pill{
    display:inline-block;
    padding: 0px 6px;
    border: 1px solid #cfcfcf;
    border-radius: 10px;
    font-weight:600;
  }
</style>
</head>

<body>

<div class="eqbox">
  <div class="eq">
    Y<sub>sym</sub>(B) = ½ [Y(B) + Y(−B)] &nbsp;&nbsp;|&nbsp;&nbsp;
    Y<sub>asym</sub>(B) = ½ [Y(B) − Y(−B)]
  </div>
</div>

<p class="note">
  <span class="key">Inputs:</span> choose <b>Field</b> column for <b>X</b>, then select one or more (Ctrl+select) <b>Y</b> columns from the table.
</p>

<p class="note">
  <span class="key">Row range:</span> only rows within <b>Start..End</b> are used (End is exclusive).
</p>

<p class="note">
  <span class="key">Field interval:</span> a uniform field grid is generated with the Field Interval.
</p>

<p class="note">
  <span class="key">Processing:</span> for each selected Y, the data are interpolated onto the uniform grid, then split into <b>sym</b> and <b>asym</b> components using the formula.
</p>

<p class="note">
  <span class="key">Prefix:</span> will be added to the output column and are marked with <span class="pill">[p]</span>.
</p>

</body>
</html>

)");
        break;
    case 4: // Multi-Sweep Extraction (신규 추가)
        ui->editHelp->setHtml(R"(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8" />
<style>
  body { font-family:'Segoe UI'; font-size:10pt; color:#999; }
  p { margin: 6px 0; line-height: 1.35; }
  .eqbox{
    margin: 8px 0 10px 0;
    padding: 8px 10px;
    border: 1px solid #d6d6d6;
    border-left: 4px solid #2b6cb0;
    border-radius: 6px;
  }
  .eq{
    font-size: 14pt;
    font-weight: 700;
    color: #999;
    letter-spacing: 0.2px;
  }
  .note { color:#999; }
  .key  { font-weight:600; color:#444; }
  .pill{
    display:inline-block;
    padding: 0px 6px;
    border: 1px solid #cfcfcf;
    border-radius: 10px;
    font-weight:600;
  }
</style>
</head>

<body>

<div class="eqbox">
  <div class="eq">
    |B<sub>actual</sub> − B<sub>target</sub>| &le; Tolerance
  </div>
</div>

<p class="note">
  <span class="key">Sweep Direction:</span> Use  to isolate only one sweeping direction.
</p>

<p class="note">
  <span class="key">Sweep Check Points:</span> The number of points used to determine the sweep direction (e.g., 100) (effectively filters out thermal fluctuations.)
</p>

<p class="note">
  <span class="key">Fields(T):</span> Enter values in <b>Tesla (T)</b> separated by comma.
  Internally converted to <b>Oe</b>.
</p>

<p class="note">
  <span class="key">Resolution:</span> The common grid is automatically generated based on the <b>densest segment</b>.
</p>

<p class="note">
  <span class="key">Processing:</span> Selected field segments are extracted, filtered by direction,
  and aligned via linear interpolation onto the common grid.
</p>

</body>
</html>
)");
        break;

    case 5: // Single-point Hall Analysis (최종 보강 버전)
        ui->editHelp->setHtml(R"(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8" />
<style>
  body { font-family:'Segoe UI'; font-size:10pt; color:#999; }
  p { margin: 6px 0; line-height: 1.35; }
  .eqbox{
    margin: 8px 0 10px 0;
    padding: 8px 10px;
    border: 1px solid #d6d6d6;
    border-left: 4px solid #2b6cb0;
    border-radius: 6px;
  }
  .eq{
    font-size: 14pt;
    font-weight: 700;
    color: #999;
    letter-spacing: 0.2px;
  }
  .note { color:#999; }
  .key  { font-weight:600; color:#444; }
  .pill{
    display:inline-block;
    padding: 0px 6px;
    border: 1px solid #cfcfcf;
    border-radius: 10px;
    font-weight:600;
  }
</style>
</head>

<body>

<div class="eqbox">
  <div class="eq">
    1. &rho;<sub>xy</sub> = (V<sub>H</sub> &times; Gain / I) &times; t [ &Omega;&middot;m ] |
    2. R<sub>H</sub> = &Delta;&rho;<sub>xy</sub> / &Delta;B [ m³/C ]<br>
    3. n = 1 / (|R<sub>H</sub>|e) &nbsp;&nbsp; | &nbsp;&nbsp; 4. &mu; = |R<sub>H</sub>| / &rho;<sub>xx</sub>(0T)
  </div>
</div>

<p class="note">
  <span class="key">1. Hall Resistivity (&rho;<sub>xy</sub>):</span> Calculated using the
  <span class="pill">Asymmetric</span> voltage component, current (I), and sample thickness (t).
</p>

<p class="note">
  <span class="key">2. Unit Conversion:</span> Magnetic field (Oe) is converted to <b>Tesla</b>
  internally (1T = 10,000 Oe) to yield R<sub>H</sub> in SI units.
</p>

<p class="note">
  <span class="key">3. Longitudinal Resistivity (&rho;<sub>xx</sub>):</span> Determined at <b>B = 0</b>
  using sample width (w), length (L), and thickness (t).
</p>

<p class="note">
  <span class="key">4. Lab Units:</span> Results are dual-reported in <b>&Omega;&middot;cm</b>,
  <b>cm⁻³</b>, and <b>cm²/Vs</b> for direct use in publications.
</p>

</body>
</html>
)");
    break;

    case 6: // T-Sweep Hall Analysis (신규 추가)
        ui->editHelp->setHtml(R"(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8" />
<style>
  body { font-family:'Segoe UI'; font-size:10pt; color:#999; }
  p { margin: 6px 0; line-height: 1.35; }
  .eqbox{
    margin: 8px 0 10px 0;
    padding: 8px 10px;
    border: 1px solid #d6d6d6;
    border-left: 4px solid #2b6cb0;
    border-radius: 6px;
  }
  .eq{
    font-size: 14pt;
    font-weight: 700;
    color: #999;
    letter-spacing: 0.2px;
  }
  .note { color:#999; }
  .key  { font-weight:600; color:#444; }
  .pill{
    display:inline-block;
    padding: 0px 6px;
    border: 1px solid #cfcfcf;
    border-radius: 10px;
    font-weight:600;
  }
</style>
</head>

<body>

<div class="eqbox">
  <div class="eq">
    V<sub>H</sub>(T) = ½ [V<sub>+H</sub>(T) − V<sub>−H</sub>(T)]<br>
    n(T) = 1 / (|R<sub>H</sub>(T)|e) &nbsp;&nbsp; | &nbsp;&nbsp; &mu;(T) = |R<sub>H</sub>(T)| / &rho;<sub>xx</sub>(T, 0T)
  </div>
</div>

<p class="note">
  <span class="key">1. Triple Segment Alignment:</span> Synchronizes <b>Zero</b>, <b>Positive (+H)</b>, and <b>Negative (-H)</b> field sweeps onto a common temperature grid using linear interpolation.
</p>

<p class="note">
  <span class="key">2. Anti-symmetrization:</span> Extracts the pure Hall voltage <b>V<sub>H</sub>(T)</b> from the transverse voltage sweeps to eliminate longitudinal resistance (MR) artifacts.
</p>

<p class="note">
  <span class="key">3. Fixed Field (B):</span> The magnetic field entered in <b>Field(Oe)</b> is converted to <b>Tesla</b> and used as a constant to calculate the Hall coefficient <b>R<sub>H</sub>(T) = &rho;<sub>xy</sub>(T) / B</b>.
</p>

<p class="note">
  <span class="key">4. Zero Field Reference:</span> <b>&rho;<sub>xx</sub>(T, 0T)</b> is extracted from the Zero Field segment to ensure mobility <b>&mu;(T)</b> is calculated without magnetoresistance interference.
</p>

<p class="note">
  <span class="key">5. Lab Units:</span> Results for <b>n(T)</b> and <b>&mu;(T)</b> are dual-reported in both <b>SI</b> and <b>cm-based</b> units (cm⁻³, cm²/Vs).
</p>

</body>
</html>
)");
    break;

    default:
        ui->editHelp->setHtml(R"(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8" />
<style>
  body { font-family:'Segoe UI'; font-size:10pt; color:#999; }
  p { margin: 6px 0; line-height: 1.35; }
  .eqbox{
    margin: 8px 0 10px 0;
    padding: 8px 10px;
    border: 1px solid #d6d6d6;
    border-left: 4px solid #2b6cb0;
    border-radius: 6px;
  }
  .eq{
    font-size: 14pt;
    font-weight: 700;
    color: #999;
    letter-spacing: 0.2px;
  }
  .note { color:#999; }
  .key  { font-weight:600; color:#444; }
  .pill{
    display:inline-block;
    padding: 0px 6px;
    border: 1px solid #cfcfcf;
    border-radius: 10px;
    font-weight:600;
  }
</style>
</head>

<body>

<div class="eqbox">
  <div class="eq">
    Y<sub>sym</sub>(T) = ½ [Y<sub>+H</sub>(T) + Y<sub>−H</sub>(T)] &nbsp;&nbsp;|&nbsp;&nbsp;
    Y<sub>asym</sub>(T) = ½ [Y<sub>+H</sub>(T) − Y<sub>−H</sub>(T)]
  </div>
</div>

<p class="note">
  <span class="key">Inputs:</span> choose <b>Temperature</b> column for <b>X</b>, then select one or more (Ctrl+select) <b>Y</b> columns from the table.
</p>

<p class="note">
  <span class="key">Row ranges:</span> define two segments, one for <b>+H</b> and one for <b>−H</b>
</p>

<p class="note">
  <span class="key">Processing:</span> both +H and −H traces are aligned over the <b>overlapping temperature range</b>;
  for each selected Y, the tool builds <b>orig(+H)</b>, <b>sym</b>, and <b>asym</b> columns as functions of T.
</p>

<p class="note">
  <span class="key">Prefix:</span> will be added to the column names and are marked with <span class="pill">[p]</span>.
</p>

</body>
</html>

)");
        break;
    }
}

void MainWindow::setTableFromColumns(const QStringList& headers,
                                     const QVector<QVector<double>>& cols,
                                     const QString& windowTitle,
                                     bool resizeColumns)
{
    // Basic validation
    const int nCols = headers.size();
    if (nCols <= 0 || cols.size() != nCols) {
        logMessage("setTableFromColumns: invalid headers/cols size.");
        return;
    }

    // Determine row count = max length among columns
    int nRows = 0;
    for (const auto& c : cols)
        nRows = std::max(nRows, int(c.size()));

    if (nRows <= 0) {
        logMessage("setTableFromColumns: empty data.");
        // Clear model anyway
        model_->setNumericData(headers, QVector<QVector<double>>{});
        updateColumnCombos();
        updateRhoCombos();
        updateRowSpins();
        return;
    }

    // Convert column-major -> row-major for NumericTableModel
    QVector<QVector<double>> rows;
    rows.resize(nRows);
    for (int r = 0; r < nRows; ++r)
        rows[r].resize(nCols);

    for (int c = 0; c < nCols; ++c) {
        const auto& col = cols[c];
        const int m = std::min(nRows, int(col.size()));
        for (int r = 0; r < m; ++r)
            rows[r][c] = col[r];
        // If a column is shorter, remaining cells stay default-initialized (0.0).
        // If you prefer NaN instead, tell me and I’ll switch it.
    }

    model_->setNumericData(headers, rows);

    // Ensure view is attached (in case this window was newly constructed)
    ui->rawView->setModel(model_);

    updateColumnCombos();
    updateRhoCombos();
    updateRowSpins();

    if (resizeColumns)
        ui->rawView->resizeColumnsToContents();

    if (!windowTitle.isEmpty())
        setWindowTitle(windowTitle);
}

MainWindow* MainWindow::openResultWindow(const QString& title,
                                         const QStringList& headers,
                                         const QVector<QVector<double>>& cols)
{
    auto* w = new MainWindow(nullptr);
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->setTableFromColumns(headers, cols, title, /*resizeColumns=*/true);
    w->show();
    return w;
}

void MainWindow::deleteProcessedColumns(const QList<int>& cols)
{
    if (!model_ || cols.isEmpty())
        return;

    const QString prefix = "[p]";

    // Validate columns

    QList<int> validCols;
    for (int c : cols) {
        if (c < 0 || c >= model_->columnCount())
            continue;
        const QString h = model_->headerData(c, Qt::Horizontal,
                                             Qt::DisplayRole).toString();
        if (h.startsWith(prefix))
            validCols.push_back(c);
    }

    if (validCols.isEmpty()) {
        QMessageBox::information(this, "Delete column",
                                 "Only columns starting with [p] can be deleted.");
        return;
    }

    std::sort(validCols.begin(), validCols.end());

    if (QMessageBox::question(this, "Delete column",
                              QString("Delete %1 [p] column(s)?")
                                  .arg(validCols.size()))
        != QMessageBox::Yes) {
        return;
    }

    // Remove from right to left
    for (int i = validCols.size() - 1; i >= 0; --i) {
        model_->removeColumns(validCols[i], 1);
    }

    updateColumnCombos();
    updateRhoCombos();

    logMessage(QString("Deleted %1 [p] column(s).")
                   .arg(validCols.size()));
}

void MainWindow::installEndSpinClamp(QSpinBox* spin)
{
    if (!spin) return;

    // Allow typing beyond the current max without key rejection
    spin->setRange(1, std::numeric_limits<int>::max());

    // Optional: avoid jitter while typing
    spin->setKeyboardTracking(false);

    connect(spin, &QSpinBox::editingFinished, this, [this, spin]() {
        if (!model_) return;

        const int maxRow = std::max(1, model_->rowCount());

        spin->blockSignals(true);
        spin->setValue(qBound(1, spin->value(), maxRow));
        spin->blockSignals(false);
    });
}

void MainWindow::on_actionAbout_triggered()
{
    // 1. Initialize the Dialog
    auto *dlg = new QDialog(this);
    dlg->setWindowTitle("About Transnalysis Suite");

    // Ensure the memory is freed automatically when the window is closed
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->resize(520, 400);

    // 2. Set up the Layout
    auto *layout = new QVBoxLayout(dlg);

    // 3. Configure the Text Display
    auto *text = new QTextEdit(dlg);
    text->setReadOnly(true);

    // Using a Raw String Literal R"( ... )" to embed HTML/CSS content
    text->setHtml(R"(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8"/>
<style>
  body {
    font-family: 'Segoe UI', sans-serif;
    font-size: 10pt;
    color: #2f2f2f;
    background-color: #f6f7f9;
    margin: 12px;
  }
  h2 { margin: 0 0 6px 0; font-weight: 600; color: #2b6cb0; }
  p { margin: 6px 0; line-height: 1.45; }
  .subtitle { color: #555; margin-bottom: 12px; }
  .card {
    margin-top: 12px;
    padding: 12px 14px;
    background-color: #ffffff;
    border: 1px solid #dfe3e8;
    border-left: 4px solid #90b4e8;
    border-radius: 8px;
  }
  .key { font-weight: 600; color: #444; }
  .license { font-size: 9.5pt; color: #555; }
</style>
</head>
<body>
  <h2>Transnalysis Suite</h2>
  <p class="subtitle">
    A lightweight analysis and visualization tool for electric transport data,
    designed for interactive inspection, processing, and symmetry-based analysis.
  </p>
  <div class="card">
    <p><span class="key">Author:</span> Seongjoon Lim</p>
    <p><span class="key">Developed:</span> 2025</p>
    <p><span class="key">Technology:</span> C++ / Qt Widgets / Qt Charts</p>
  </div>
  <div class="card license">
    <p><span class="key">License:</span> Apache License 2.0</p>
    <p>This software is open source and may be used, modified, and redistributed.</p>
    <p>Copyright © 2026 Seongjoon Lim</p>
  </div>
</body>
</html>
)");

    layout->addWidget(text);

    // 4. Add a Close Button
    auto *btnClose = new QPushButton("Close", dlg);

    // Connect the button click to the dialog's accept slot to close it
    connect(btnClose, &QPushButton::clicked, dlg, &QDialog::accept);

    // Align the button to the right for a standard UI feel
    layout->addWidget(btnClose, 0, Qt::AlignRight);

    // 5. Apply the layout and launch as a modal dialog
    dlg->setLayout(layout);
    dlg->exec();
}

void MainWindow::on_actionRemove_Rows_triggered()
{
    if (!model_ || model_->rowCount() == 0) {
        QMessageBox::information(this, "No data", "There is no data to remove.");
        return;
    }

    const int rows = model_->rowCount();

    bool ok = false;
    const int row1 = QInputDialog::getInt(
        this,
        "Remove rows",
        QString("Remove rows starting from which row?\n(1 – %1)").arg(rows),
        rows + 1,     // default: suggest "beyond end" = no-op
        1,
        rows,
        1,
        &ok
        );

    if (!ok)
        return;

    // Convert to 0-based
    const int row0 = row1 - 1;

    if (row0 < 0 || row0 >= rows) {
        QMessageBox::information(this, "Nothing removed",
                                 "No rows were removed.");
        return;
    }

    const int nRemove = rows - row0;

    if (QMessageBox::question(
            this,
            "Confirm row removal",
            QString("Remove %1 row(s)\n(from row %2 to %3)?")
                .arg(nRemove)
                .arg(row1)
                .arg(rows))
        != QMessageBox::Yes)
        return;

    // --- actual removal ---
    model_->removeRows(row0, nRemove);

    updateRowSpins();
    updateColumnCombos();
    updateRhoCombos();

    logMessage(QString("Removed rows %1..%2 (%3 rows).")
                   .arg(QString::number(row1),
                        QString::number(rows),
                        QString::number(nRemove)));
}

void MainWindow::on_btnExtractMultiSweep_clicked()
{
    if (!model_ || model_->rowCount() == 0) return;

    // [1] 기본 파라미터 수집
    int xCol = ui->comboExtractXCol->currentData().toInt();
    int fieldCol = ui->comboExtractFieldCol->currentData().toInt();

    // Tolerance와 Prefix 수집 (QPlainTextEdit 및 QLineEdit 대응)
    double toleranceOe = ui->editExtractTolerance->text().toDouble();
    QString prefix = ui->editExtractPrefix->toPlainText().trimmed();
    if (prefix.isEmpty()) prefix = "Ext_";

    // [신규] 방향성 필터 파라미터 수집
    int windowSize = ui->spinExtractCheckWindow->value();
    bool sweepUp = ui->checkExtractSweepUp->isChecked();

    // [2] 행 범위(Row Range) 수집 및 보정
    int rows = model_->rowCount();
    int s1 = ui->spinExtractStart->value();
    int e1 = ui->spinExtractEnd->value();
    if (s1 > e1) std::swap(s1, e1);

    int rowStart = qBound(0, s1 - 1, rows - 1);
    int rowEnd = qBound(0, e1, rows);

    if (rowStart >= rowEnd) {
        QMessageBox::warning(this, "Range Error", "Invalid row range selected.");
        return;
    }

    // [3] 필드값 수집 (Tesla 입력 -> Oersted 변환: 1 T = 10,000 Oe)
    QString fieldStr = ui->editExtractFieldValues->toPlainText();
    QVector<double> targetFieldsOe;
    for (const QString &s : fieldStr.split(',')) {
        bool ok;
        double valT = s.trimmed().toDouble(&ok);
        if (ok) targetFieldsOe.push_back(valT * 10000.0);
    }

    // Y 컬럼 수집 (선택된 컬럼들 중 X와 Field 제외)
    QVector<int> yCols;
    QStringList yNames;
    if (auto *sm = ui->rawView->selectionModel()) {
        const auto selCols = sm->selectedColumns();
        for (const auto &mi : selCols) {
            int c = mi.column();
            if (c == xCol || c == fieldCol) continue;
            yCols << c;
            yNames << model_->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
        }
    }

    if (yCols.isEmpty()) {
        QMessageBox::warning(this, "No Y Selected", "Please select one or more Y columns in the table.");
        return;
    }

    // [4] 계산 수행 (확장된 파라미터 전달)
    QVector<double> xCommon;
    QStringList outHeaders;
    QVector<QVector<double>> outCols;

    bool ok = buildMultiSweepColumns(xCol, fieldCol, yCols, targetFieldsOe, toleranceOe,
                                     prefix, rowStart, rowEnd,
                                     windowSize, sweepUp, // 윈도우 크기와 방향 추가
                                     xCommon, outHeaders, outCols);

    if (!ok) {
        QMessageBox::warning(this, "Error", "No valid data found in the specified range/field with the current sweep direction.");
        return;
    }

    // [5] 결과 적용 및 로그 출력
    MainWindow* resultWindow = processExtractionResult(outHeaders, outCols);

    auto sendLog = [&](const QString& msg) {
        this->logMessage(msg);
        if (resultWindow) resultWindow->logMessage(msg);
    };

    sendLog("");
    sendLog(QString("Multi-Sweep Extraction is obtained from [%1]")
                .arg(QFileInfo(loadedFileName_).fileName()));
    sendLog("");

    // 필드 매칭 및 스윗 방향 물리적 기준 명시
    sendLog("  Selection criteria:");
    sendLog(QString("    - Field: $|B_{actual} - B_{target}| \\le %1$ Oe").arg(toleranceOe));
    sendLog(QString("    - Direction: %1 (Window: %2 pts)")
                .arg(sweepUp ? "Sweep Up (Warming)" : "Sweep Down (Cooling)")
                .arg(windowSize));
    sendLog("");

    sendLog(QString("  Sweep Variable (X): Column [%1]").arg(ui->comboExtractXCol->currentText()));
    sendLog(QString("  Field Source Column: [%1]").arg(ui->comboExtractFieldCol->currentText()));
    sendLog(QString("  Input Y channels: %1").arg(yNames.join(", ")));
    sendLog("");

    // 추출된 필드 리스트 (T와 Oe 병기)
    QStringList fieldInfo;
    for (double fOe : targetFieldsOe) {
        fieldInfo << QString("%1T (%2 Oe)").arg(fOe / 10000.0).arg(fOe);
    }
    sendLog(QString("  Target Fields: %1").arg(fieldInfo.join(", ")));

    if (!xCommon.isEmpty()) {
        // [수정] 실제 계산된 Grid Step 산출
        double actualStep = 0.0;
        if (xCommon.size() > 1) {
            actualStep = (xCommon.last() - xCommon.first()) / (xCommon.size() - 1);
        }

        sendLog(QString("  Common Overlap: %1 to %2")
                    .arg(QString::number(xCommon.front(), 'g', 5))
                    .arg(QString::number(xCommon.back(), 'g', 5)));

        // [수정] 하드코딩된 0.1 K를 실제 actualStep으로 교체
        sendLog(QString("  Grid: %1 points aligned via linear interpolation (Step: %2 K).")
                    .arg(xCommon.size())
                    .arg(QString::number(actualStep, 'f', 4)));
    }

    sendLog(QString("  Rows from %1 to %2 processed.").arg(rowStart + 1).arg(rowEnd));
    sendLog(QString("  Output Prefix: [%1]").arg(prefix));
    sendLog(QString(" >> Extraction Complete: %1 columns created.").arg(outHeaders.size()));
    sendLog("------------------------------------------------------------");
}

bool MainWindow::buildMultiSweepColumns(int xCol, int fieldCol, const QVector<int>& yCols,
                                        const QVector<double>& targetFields, double tolerance,
                                        const QString& prefix, int rowStart, int rowEnd,
                                        int windowSize, bool sweepUp,
                                        QVector<double>& xCommon,
                                        QStringList& outHeaders, QVector<QVector<double>>& outCols)
{
    struct DataPoint {
        double x;
        QMap<int, double> ys;
        bool operator<(const DataPoint& other) const { return x < other.x; }
    };

    struct Segment {
        double fieldVal;
        QVector<DataPoint> points;
    };
    QVector<Segment> segments;

    double commonMin = -std::numeric_limits<double>::infinity();
    double commonMax = std::numeric_limits<double>::infinity();

    // [1단계] 방향성 필터링 및 데이터 수집
    for (double targetF : targetFields) {
        Segment seg;
        seg.fieldVal = targetF;

        for (int r = rowStart; r < rowEnd; ++r) {
            double currentF = model_->index(r, fieldCol).data().toDouble();
            if (std::abs(currentF - targetF) <= tolerance) {
                double currentT = model_->index(r, xCol).data().toDouble();

                // 100점 윈도우 기반 방향 판별 (Macro-trend)
                bool isDirectionMatch = true;
                if (r >= rowStart + windowSize) {
                    double sumStartT = 0, sumEndT = 0;
                    int subSamples = qBound(1, windowSize / 10, 10);
                    for (int i = 0; i < subSamples; ++i) {
                        sumStartT += model_->index(r - windowSize + i, xCol).data().toDouble();
                        sumEndT += model_->index(r - subSamples + 1 + i, xCol).data().toDouble();
                    }
                    if (sweepUp ? (sumEndT / subSamples <= sumStartT / subSamples)
                                : (sumEndT / subSamples >= sumStartT / subSamples))
                        isDirectionMatch = false;
                }

                if (isDirectionMatch) {
                    DataPoint dp;
                    dp.x = currentT;
                    for (int yCol : yCols) dp.ys[yCol] = model_->index(r, yCol).data().toDouble();
                    seg.points.push_back(dp);
                }
            }
        }
        if (!seg.points.isEmpty()) {
            std::sort(seg.points.begin(), seg.points.end());
            segments.push_back(seg);
        }
    }

    if (segments.isEmpty()) return false;

    // [2단계] 가장 촘촘한 세그먼트를 기준으로 그리드 간격 결정 (정보 손실 방지)
    int maxPoints = 0;
    int densestSegIdx = 0;
    for (int i = 0; i < segments.size(); ++i) {
        // 공통 범위(Overlap) 계산
        commonMin = std::max(commonMin, segments[i].points.first().x);
        commonMax = std::min(commonMax, segments[i].points.last().x);

        // 가장 포인트가 많은(촘촘한) 세그먼트 찾기
        if (segments[i].points.size() > maxPoints) {
            maxPoints = segments[i].points.size();
            densestSegIdx = i;
        }
    }

    if (commonMin >= commonMax) return false;

    // [핵심] 가장 촘촘한 세그먼트의 평균 간격을 그리드 간격으로 사용
    double gridSpacing = 0.1; // Fallback
    const auto& refPoints = segments[densestSegIdx].points;
    if (refPoints.size() > 1) {
        gridSpacing = (refPoints.last().x - refPoints.first().x) / (refPoints.size() - 1);
    }

    xCommon.clear();
    for (double x = commonMin; x <= commonMax; x += gridSpacing) {
        xCommon.push_back(x);
    }

    // [3단계] 보간 및 출력 데이터 구성 (기존 interpolate 재사용)
    outHeaders << model_->headerData(xCol, Qt::Horizontal, Qt::DisplayRole).toString() + "_Common";
    outCols.push_back(xCommon);

    for (int yCol : yCols) {
        QString originalName = model_->headerData(yCol, Qt::Horizontal, Qt::DisplayRole).toString();
        for (const auto& seg : segments) {
            outHeaders << QString("%1%2_%3T").arg(prefix, originalName).arg(seg.fieldVal / 10000.0);
            QVector<double> segX, segY;
            for (const auto& p : seg.points) {
                segX << p.x;
                segY << p.ys[yCol];
            }
            QVector<double> interpolatedY;
            interpolatedY.reserve(xCommon.size());
            for (double x : xCommon) {
                interpolatedY.push_back(interpolate(x, segX, segY));
            }
            outCols.push_back(interpolatedY);
        }
    }
    return true;
}

double MainWindow::interpolate(double x, const QVector<double>& xData, const QVector<double>& yData) {
    if (xData.size() < 2) return (yData.isEmpty() ? 0.0 : yData[0]);

    auto it = std::lower_bound(xData.begin(), xData.end(), x);
    if (it == xData.begin()) return yData.front();
    if (it == xData.end()) return yData.back();

    int idx1 = std::distance(xData.begin(), it) - 1;
    int idx2 = idx1 + 1;

    double x1 = xData[idx1], x2 = xData[idx2];
    double y1 = yData[idx1], y2 = yData[idx2];

    if (std::abs(x1 - x2) < 1e-12) return y1;
    return y1 + (x - x1) * (y2 - y1) / (x2 - x1);
}
MainWindow* MainWindow::processExtractionResult(const QStringList& headers, const QVector<QVector<double>>& cols)
{
    if (headers.isEmpty() || cols.isEmpty()) return nullptr;

    const bool addColumn = ui->checkExtractNewColumn->isChecked();
    const bool newWindow = ui->checkExtractNewWindow->isChecked();
    MainWindow* w = nullptr;

    if (!addColumn && !newWindow) {
        QMessageBox::information(this, "No output selected", "Select 'New Column' or 'New Window'.");
        return nullptr;
    }

    // 1. 현재 테이블에 새 컬럼으로 추가
    if (addColumn && model_) {
        model_->appendColumns(headers, cols);
        updateColumnCombos(); // 콤보박스 목록 갱신
        updateRhoCombos();

        ui->rawView->setUpdatesEnabled(false);
        const int last = model_->columnCount() - 1;
        const int firstNew = qMax(0, last - int(headers.size()) + 1);
        for (int c = firstNew; c <= last; ++c)
            ui->rawView->resizeColumnToContents(c);
        ui->rawView->setUpdatesEnabled(true);
    }

    // 2. 결과용 새 윈도우 생성
    if (newWindow) {
        w = openResultWindow("Multi-Sweep Extraction Result", headers, cols);
    }

    return w;
}

void MainWindow::on_btnSingleHallCalculate_clicked()
{
    if (!model_ || model_->rowCount() == 0) return;

    // [1] UI 파라미터 수집 및 출력 옵션 확인
    const bool addColumn = ui->checkSingleHallNewColumn->isChecked();
    const bool newWindow = ui->checkSingleHallNewWindow->isChecked();

    if (!addColumn && !newWindow) {
        QMessageBox::information(this, "No output selected", "Select at least one output option (New Column or New Window).");
        return;
    }

    const int xCol = ui->comboSingleHallField->currentData().toInt();
    const int vLongCol = ui->comboSingleHallVLong->currentData().toInt();
    const int vTransCol = ui->comboSingleHallVTrans->currentData().toInt();
    const int iCol = ui->comboSingleHallCurrentCol->currentData().toInt();
    QString prefix = ui->editSingleHallPrefix->toPlainText().trimmed();

    int rowStart = ui->spinSingleHallStartRow->value();
    int rowEnd = ui->spinSingleHallEndRow->value();
    if (rowStart > rowEnd) std::swap(rowStart, rowEnd);

    bool okT, okW, okL, okAmp, okI;
    const double t = ui->editHallThickness->text().toDouble(&okT);
    const double w = ui->editHallWidth->text().toDouble(&okW);
    const double L = ui->editHallLength->text().toDouble(&okL);
    const double amp = ui->editSingleHallAmp->text().toDouble(&okAmp);

    const double I_input = ui->editSingleHallCurrentConst->text().toDouble(&okI);
    const bool useFixedI = (okI && I_input != 0.0);

    if (!okT || !okW || !okL || t <= 0) {
        QMessageBox::warning(this, "Input Error", "Check Sample Dimensions and Voltage Gain.");
        return;
    }

    // [2] Symmetrization 엔진 가동
    QVector<int> yCols = { vLongCol, vTransCol };
    QStringList yNames = { "VLong", "VTrans" };
    double step = ui->editFSSDensity->text().toDouble();
    if (step <= 0) step = 0.05;

    QVector<double> xNew;
    QStringList symHeaders;
    QVector<QVector<double>> symCols;

    bool ok = buildFSSymColumns(xCol, yCols, yNames, rowStart, rowEnd, step,
                                prefix, xNew, symHeaders, symCols);

    if (!ok || xNew.isEmpty() || symCols.size() < 7) return;

    // [3] Hall 효과 물리량 산출
    const QVector<double>& vLongSym = symCols[2];
    const QVector<double>& vTransAsym = symCols[6];

    double current = useFixedI ? I_input : 1.0;
    if (!useFixedI) {
        double sumI = 0; int countI = 0;
        for (int r = rowStart; r <= rowEnd; ++r) {
            sumI += model_->index(r, iCol).data().toDouble();
            countI++;
        }
        current = (countI > 0) ? sumI / countI : 1.0;
    }

    double sumB2 = 0, sumBRho = 0;
    QVector<double> rhoXY;
    for (int i = 0; i < xNew.size(); ++i) {
        double B_tesla = xNew[i] / 10000.0; // Oe -> T
        double rxy = (vTransAsym[i] * amp / current) * t;
        rhoXY.push_back(rxy);
        sumB2 += B_tesla * B_tesla;
        sumBRho += B_tesla * rxy;
    }

    double RH = (sumB2 != 0) ? sumBRho / sumB2 : 0;
    const double eCharge = 1.602176634e-19;
    double nCarrier = (RH != 0) ? 1.0 / (std::abs(RH) * eCharge) : 0;

    // [4] Mobility 및 Resistivity 산출
    int midIdx = xNew.size() / 2;
    double rhoXX_SI = (vLongSym[midIdx] * amp / current) * (w * t / L);
    double rhoXX_cm = rhoXX_SI * 100.0;
    double mu_SI = (rhoXX_SI != 0) ? std::abs(RH) / rhoXX_SI : 0;
    double mu_cm = mu_SI * 10000.0;

    // [5] 결과 데이터 구성 (Prefix 적용)
    QString tag = prefix.isEmpty() ? "" : prefix + "_";
    QStringList outHeaders = {
        tag + "Field(Oe)",
        tag + "VLong_Sym",
        tag + "VTrans_Asym",
        tag + "Rho_xy(Ohm-m)"
    };
    QVector<QVector<double>> outCols = { xNew, symCols[2], symCols[6], rhoXY };

    // [6] 결과 적용 (컬럼 추가 및 새 창 생성)
    if (addColumn) {
        model_->appendColumns(outHeaders, outCols);
        updateColumnCombos(); // 콤보박스 갱신
        updateRhoCombos();

        // 테이블 뷰 강제 갱신 및 컬럼 사이즈 조정
        ui->rawView->setUpdatesEnabled(false);
        const int last = model_->columnCount() - 1;
        const int firstNew = qMax(0, last - int(outHeaders.size()) + 1);
        for (int c = firstNew; c <= last; ++c)
            ui->rawView->resizeColumnToContents(c);
        ui->rawView->setUpdatesEnabled(true);
    }

    MainWindow* resWin = nullptr;
    if (newWindow) {
        resWin = openResultWindow("Hall Analysis: " + prefix, outHeaders, outCols);
    }

    // [7] 상세 로그 출력 (두 곳 모두 전송)
    auto sendLog = [&](const QString& msg) {
        this->logMessage(msg);
        if (resWin) resWin->logMessage(msg);
    };

    sendLog("============================================================");
    sendLog(QString(">>> %1 Analysis Result").arg(prefix.isEmpty() ? "Hall Effect" : prefix));
    sendLog(QString("  - Current: %1 A, Thickness: %2 m").arg(current).arg(t));
    sendLog("------------------------------------------------------------");
    sendLog(QString("  - Hall Coeff. (RH): %1 m³/C (%2 cm³/C)")
                .arg(QString::number(RH, 'e', 4)).arg(QString::number(RH * 1e6, 'e', 4)));
    sendLog(QString("  - Carrier Density (n): %1 m⁻³ (%2 cm⁻³)")
                .arg(QString::number(nCarrier, 'e', 4)).arg(QString::number(nCarrier * 1e-6, 'e', 4)));
    sendLog(QString("  - Resistivity (ρxx): %1 Ω·m (%2 Ω·cm)")
                .arg(QString::number(rhoXX_SI, 'e', 4)).arg(QString::number(rhoXX_cm, 'e', 4)));
    sendLog(QString("  - Mobility (μ): %1 m²/Vs (%2 cm²/Vs)")
                .arg(QString::number(mu_SI, 'e', 4)).arg(QString::number(mu_cm, 'e', 4)));
    sendLog(QString("  - Carrier Type: %1").arg(RH > 0 ? "P-type (Hole)" : "N-type (Electron)"));
    sendLog("============================================================");
}

void MainWindow::on_btnTSwpHallCalculate_clicked()
{
    if (!model_ || model_->rowCount() == 0) return;

    // [1] 입력 파라미터 및 옵션 수집
    const int tCol = ui->comboTSwpHallTemp->currentData().toInt();
    const int vLongCol = ui->comboTSwpHallVLong->currentData().toInt();
    const int vTransCol = ui->comboTSwpHallVTrans->currentData().toInt();
    const int iCol = ui->comboTSwpHallCurrentCol->currentData().toInt();

    // [신규] Scan Filter 설정값 (TSSym 탭과 공유하거나 별도 지정된 값 사용)
    const int scanWait = ui->spinScanFilterWait->value();

    bool okH, okL, okW, okT, okAmp, okI;
    const double H_oe = ui->editTSwpHallField->text().toDouble(&okH);
    const double L = ui->editTSwpHallLength->text().toDouble(&okL);
    const double w = ui->editTSwpHallWidth->text().toDouble(&okW);
    const double t = ui->editTSwpHallThickness->text().toDouble(&okT);
    const double amp = ui->editTSwpHallAmp->text().toDouble(&okAmp);
    const double I_input = ui->editTSwpHallConstI->text().toDouble(&okI);
    const bool useFixedI = (okI && I_input != 0.0);
    const double H_tesla = H_oe / 10000.0;

    if (!okH || !okT || t <= 0) {
        QMessageBox::warning(this, "Input Error", "Check Field(Oe), Thickness, and Geometry.");
        return;
    }

    // [2] 데이터 추출 및 동기화된 평균화 로직 (핵심 수정 지점)
    auto getSegment = [&](int s, int e, QVector<double>& tx, QVector<double>& vl, QVector<double>& vt, QVector<double>& vi) {
        if (s > e) std::swap(s, e);

        QVector<double> rawT, rawVl, rawVt, rawVi;
        double prevT = -1e30;
        int counter = -1;

        // 1단계: Scan Filter 적용하여 데이터 수집
        for (int r = s - 1; r < e; ++r) {
            bool ok;
            double temp = model_->index(r, tCol).data().toDouble(&ok);
            if (!ok) continue;

            if (temp != prevT) {
                counter = scanWait; // 온도 변화 감지 (Step 이동)
                prevT = temp;
            } else {
                if (counter > 0) counter--;
                else if (counter == 0) {
                    // 안정화된 포인트만 임시 저장
                    rawT << temp;
                    rawVl << model_->index(r, vLongCol).data().toDouble();
                    rawVt << model_->index(r, vTransCol).data().toDouble();
                    if (!useFixedI) rawVi << model_->index(r, iCol).data().toDouble();
                }
            }
        }

        // 2단계: 동일 온도에 대해 모든 물리량 평균화 (동기화 보장)
        // tx와 각각의 Y를 쌍으로 묶어 평균화하여 데이터 개수를 일치시킵니다.
        if (!rawT.isEmpty()) {
            tx = rawT; vl = rawVl; vt = rawVt;
            sortAndAverage(tx, vl); // tx도 여기서 정렬/중복제거됨

            // 나머지 물리량들도 동일한 tx 기준으로 평균화
            QVector<double> dummyT = rawT;
            sortAndAverage(dummyT, vt);
            vt = vt; // tx와 개수 일치됨

            if (!useFixedI) {
                QVector<double> dummyT2 = rawT;
                vi = rawVi;
                sortAndAverage(dummyT2, vi);
            }
        }
    };

    QVector<double> T_z, Vl_z, Vt_z, I_z;
    QVector<double> T_p, Vl_p, Vt_p, I_p;
    QVector<double> T_n, Vl_n, Vt_n, I_n;

    getSegment(ui->spinTSwpHallZeroStart->value(), ui->spinTSwpHallZeroEnd->value(), T_z, Vl_z, Vt_z, I_z);
    getSegment(ui->spinTSwpHallPosStart->value(), ui->spinTSwpHallPosEnd->value(), T_p, Vl_p, Vt_p, I_p);
    getSegment(ui->spinTSwpHallNegStart->value(), ui->spinTSwpHallNegEnd->value(), T_n, Vl_n, Vt_n, I_n);

    if (T_z.isEmpty() || T_p.isEmpty() || T_n.isEmpty()) {
        QMessageBox::warning(this, "Data Error", "One or more segments are empty after Scan Filter.");
        return;
    }

    // [3] 공통 온도 그리드 생성 (중첩 구간)
    double tMin = std::max({T_z.front(), T_p.front(), T_n.front()});
    double tMax = std::min({T_z.last(), T_p.last(), T_n.last()});

    if (tMin >= tMax) {
        QMessageBox::warning(this, "Range Error", "No overlapping temperature range found.");
        return;
    }

    QVector<double> T_common;
    double tStep = 0.5; // UI 확장이 가능하다면 ui->editTSwpStep->text().toDouble() 추천
    for (double curT = tMin; curT <= tMax; curT += tStep) T_common << curT;

    // [4] 물리량 계산 루프
    QVector<double> nList, muList, rhoXXList, rhoXYList;
    const double eCharge = 1.602176634e-19;

    for (double t_val : T_common) {
        double vl_z = interpolate(t_val, T_z, Vl_z);
        double vt_p = interpolate(t_val, T_p, Vt_p);
        double vt_n = interpolate(t_val, T_n, Vt_n);
        double current = useFixedI ? I_input : interpolate(t_val, T_z, I_z);

        // Anti-symmetrization
        double v_hall = 0.5 * (vt_p - vt_n);

        // 물리량 산출 (Ohm-m, m^-3, m^2/Vs)
        double rho_xy = (v_hall * amp / current) * t;
        double rho_xx = (vl_z * amp / current) * (w * t / L);
        double RH = (H_tesla != 0) ? rho_xy / H_tesla : 0;
        double n = (RH != 0) ? 1.0 / (std::abs(RH) * eCharge) : 0;
        double mu = (rho_xx != 0) ? std::abs(RH) / rho_xx : 0;

        rhoXYList << rho_xy; rhoXXList << rho_xx;
        nList << n; muList << mu;
    }

    // [5] 결과 출력 및 탭 동기화
    QString prefix = ui->editTSwpHallPrefix->toPlainText().trimmed();
    QString tag = prefix.isEmpty() ? "" : prefix + "_";
    QStringList headers = { tag+"Temp(K)", tag+"Rho_xx", tag+"Rho_xy", tag+"n", tag+"mu" };
    QVector<QVector<double>> cols = { T_common, rhoXXList, rhoXYList, nList, muList };

    if (ui->checkTSwpHallNewColumn->isChecked()) {
        model_->appendColumns(headers, cols);
        updateColumnCombos();
    }

    MainWindow* resWin = nullptr;
    if (ui->checkTSwpHallNewWindow->isChecked()) {
        resWin = openResultWindow("T-Sweep Hall Analysis", headers, cols);
    }

    // [6] 상세 로그
    auto sendLog = [&](const QString& msg) {
        this->logMessage(msg);
        if (resWin) resWin->logMessage(msg);
    };

    sendLog("============================================================");
    sendLog(QString(">>> T-Sweep Hall Analysis: %1").arg(prefix));
    if (scanWait > 0) sendLog(QString("  - Scan Filter: Skipped first %1 points per step.").arg(scanWait));
    sendLog(QString("  - Averaging: All stable points per temperature were averaged."));
    sendLog(QString("  - Common Range: %1 K to %2 K").arg(tMin).arg(tMax));
    sendLog(QString("  - Points on common grid: %1").arg(T_common.size()));
    sendLog("============================================================");
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    // 드롭하려는 데이터가 URL(파일 경로 포함)을 가지고 있는지 확인
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction(); // 받아주겠다고 응답
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    if (mimeData->hasUrls()) {
        QList<QUrl> urlList = mimeData->urls();
        if (!urlList.isEmpty()) {
            QString filePath = urlList.at(0).toLocalFile();
            loadDataFile(filePath); // 공통 로직 호출!
        }
    }
}

void MainWindow::loadDataFile(const QString &fileName)
{
    if (fileName.isEmpty()) return;

    // 이미 파일이 로드되어 있는지 확인
    if (model_ && model_->rowCount() > 0) {
        QMessageBox::warning(this, tr("File Already Loaded"),
                             tr("A file is already open. Please close it first."));
        return;
    }

    // --- 파일 정보 및 예상 시간 계산 ---
    QFileInfo fileInfo(fileName);
    double fileSizeMB = static_cast<double>(fileInfo.size()) / (1024.0 * 1024.0);
    double estimatedSeconds = fileSizeMB * 1.0; // 성준님 기준: 1MB당 1초

    logMessage(QString("Target File: %1").arg(fileName));
    logMessage(QString("File Size: %1 MB").arg(QString::number(fileSizeMB, 'f', 2)));

    if (estimatedSeconds > 1.0) {
        logMessage(QString("Estimated processing time: approx. %1 seconds...")
                       .arg(QString::number(estimatedSeconds, 'f', 1)));
    }
    QCoreApplication::processEvents();

    // --- 파일 읽기 시작 ---
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Error", "Failed to open file.");
        return;
    }

    QTextStream in(&file);
    QString content = in.readAll();
    file.close();

    logMessage("Memory load complete. Starting parse...");
    QCoreApplication::processEvents();

    // 개행 문자 정규화 및 데이터 저장
    content.replace("\r\n", "\n");
    content.replace("\r", "\n");

    loadedContent_  = content;
    loadedFileName_ = fileName;

    // 테이블 채우기 및 UI 갱신
    populateTableFromText(loadedContent_);
    updateColumnCombos();
    updateRhoCombos();
    updateRowSpins();
    ui->spinEndRows->setRange(1, std::numeric_limits<int>::max());
    ui->rawView->resizeColumnsToContents();

    statusBar()->showMessage("Loaded: " + fileName, 3000);
    logMessage("Finished parsing file.\n");
}