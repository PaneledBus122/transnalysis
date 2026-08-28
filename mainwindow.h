#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QScatterSeries>
#include <QtCharts/QValueAxis>
#include <QString>
#include <QSpinBox>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT
protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

public:
    explicit MainWindow(QWidget *parent = nullptr);

    void setTableFromColumns(const QStringList& headers,
                             const QVector<QVector<double>>& cols,
                             const QString& windowTitle = QString(),
                             bool resizeColumns = true);

    ~MainWindow();

private slots:
    void openDataFile();
    void onPlotButtonClicked();
    void on_actionClose_C_triggered();
    void on_actionExit_X_triggered();
    void on_btnResistivity_clicked();
    void on_btnFSSym_clicked();
    void on_btnTSSym_clicked();
    void on_tabWidget_currentChanged(int index);
    void on_actionAbout_triggered();
    void on_actionRemove_Rows_triggered();
    void on_btnExtractMultiSweep_clicked();
    void on_btnSingleHallCalculate_clicked();
    void on_btnTSwpHallCalculate_clicked();
    void loadDataFile(const QString &fileName);

private:
    Ui::MainWindow *ui;
    class NumericTableModel* model_;

    QString loadedFileName_;
    QString loadedContent_;
    void logMessage(const QString& msg);

    void populateTableFromText(const QString& text);
    void updateColumnCombos();
    void updateRhoCombos();
    void updateRowSpins();
    void deleteProcessedColumns(const QList<int>& cols);
    void installEndSpinClamp(QSpinBox* spin);
    MainWindow* processExtractionResult(const QStringList& headers, const QVector<QVector<double>>& cols);

    void on_actionSave_S_triggered();
    void saveData(const QString& outFile, bool includeRaw, bool saveLog);
    QString makeLogFileName(const QString& dataFile) const;

    bool buildFSSymColumns(int xCol,
                           const QVector<int>& yCols,
                           const QStringList& yNames,
                           int rowStart, int rowEnd,
                           double step,
                           const QString& prefix,
                           QVector<double>& xNew,
                           QStringList& outHeaders,
                           QVector<QVector<double>>& outCols);

    bool buildTSSymColumns(int xCol,
                           const QString& xName,
                           const QVector<int>& yCols,
                           const QStringList& yNames,
                           int posStart, int posEnd,
                           int negStart, int negEnd,
                           const QString& prefix,
                           int scanWait,
                           QVector<double>& xNew,
                           QStringList& outHeaders,
                           QVector<QVector<double>>& outCols);

    MainWindow* openResultWindow(const QString& title,
                                 const QStringList& headers,
                                 const QVector<QVector<double>>& cols);

    bool buildMultiSweepColumns(int xCol, int fieldCol, const QVector<int>& yCols,
                                            const QVector<double>& targetFields, double tolerance,
                                            const QString& prefix, int rowStart, int rowEnd,
                                            int windowSize, bool sweepUp,
                                            QVector<double>& xCommon,
                                            QStringList& outHeaders, QVector<QVector<double>>& outCols);

};
#endif // MAINWINDOW_H
