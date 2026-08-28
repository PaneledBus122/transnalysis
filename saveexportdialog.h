#ifndef SAVEEXPORTDIALOG_H
#define SAVEEXPORTDIALOG_H

#include <QDialog>

namespace Ui {
class SaveExportDialog;
}

class SaveExportDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SaveExportDialog(QWidget *parent = nullptr);
    ~SaveExportDialog();

    QString filePath() const;
    bool includeRaw() const;
    bool saveLog() const;

private slots:
    void on_btnBrowse_clicked();

private:
    Ui::SaveExportDialog *ui;
};

#endif // SAVEEXPORTDIALOG_H
