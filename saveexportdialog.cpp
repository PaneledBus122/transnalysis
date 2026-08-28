#include "saveexportdialog.h"
#include "ui_saveexportdialog.h"

#include <QFileDialog>

SaveExportDialog::SaveExportDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::SaveExportDialog)
{
    ui->setupUi(this);

    setWindowTitle("Save Data");

    // sensible defaults
    ui->chkIncludeRaw->setChecked(true);
    ui->chkSaveLog->setChecked(true);
}

SaveExportDialog::~SaveExportDialog()
{
    delete ui;
}

void SaveExportDialog::on_btnBrowse_clicked()
{
    const QString fn = QFileDialog::getSaveFileName(
        this,
        "Save Data",
        ui->editPath->text(),
        "CSV (*.csv);;TSV (*.tsv);;Data (*.dat);;All Files (*)"
        );

    if (!fn.isEmpty())
        ui->editPath->setText(fn);
}

QString SaveExportDialog::filePath() const
{
    return ui->editPath->text().trimmed();
}

bool SaveExportDialog::includeRaw() const
{
    return ui->chkIncludeRaw->isChecked();
}

bool SaveExportDialog::saveLog() const
{
    return ui->chkSaveLog->isChecked();
}
