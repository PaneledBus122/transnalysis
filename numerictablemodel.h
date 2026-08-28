#pragma once

#include <QAbstractTableModel>
#include <QVector>
#include <QStringList>
#include <QVariant>
#include <limits>
#include <algorithm>

class NumericTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit NumericTableModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}

    double valueAt(int r, int c) const { return rows_[r][c]; }
    QString headerAt(int c) const { return header_.at(c); }

    void setNumericData(const QStringList& header,
                        const QVector<QVector<double>>& rows)
    {
        beginResetModel();
        header_ = header;
        rows_   = rows;
        endResetModel();
    }

    // Append multiple numeric columns at once.
    // newCols is column-major: newCols[k][r] is value at row r of new column k.
    void appendColumns(const QStringList& newHeaders,
                       const QVector<QVector<double>>& newCols)
    {
        if (newHeaders.isEmpty() || newCols.isEmpty())
            return;
        if (newHeaders.size() != newCols.size())
            return;

        const int oldColCount = header_.size();
        const int addCols = newHeaders.size();

        // Determine required row count
        qsizetype requiredRows = rows_.size();
        for (const auto& col : newCols)
            requiredRows = std::max(requiredRows, col.size());

        // If we need more rows, append them first (rows are row-major in rows_)
        if (requiredRows > rows_.size()) {
            beginInsertRows(QModelIndex(), rows_.size(), requiredRows - 1);

            for (int r = rows_.size(); r < requiredRows; ++r) {
                QVector<double> row;
                row.resize(oldColCount);
                for (int c = 0; c < oldColCount; ++c)
                    row[c] = std::numeric_limits<double>::quiet_NaN();
                rows_.append(row);
            }

            endInsertRows();
        }

        // Now insert new columns
        beginInsertColumns(QModelIndex(), oldColCount, oldColCount + addCols - 1);

        // Add headers
        for (const QString& h : newHeaders)
            header_.append(h);

        // Append new column values to each row
        for (int r = 0; r < rows_.size(); ++r) {
            for (int k = 0; k < addCols; ++k) {
                double v = std::numeric_limits<double>::quiet_NaN();
                if (r < newCols[k].size())
                    v = newCols[k][r];
                rows_[r].append(v);
            }
        }

        endInsertColumns();
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : rows_.size();
    }

    int columnCount(const QModelIndex& parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : header_.size();
    }

    bool removeRows(int row, int count,
                    const QModelIndex& parent = QModelIndex()) override
    {
        if (parent.isValid())
            return false;
        if (count <= 0)
            return false;
        if (row < 0 || row >= rows_.size())
            return false;

        const int last = row + count - 1;
        if (last >= rows_.size())
            return false;

        beginRemoveRows(QModelIndex(), row, last);
        rows_.remove(row, count);
        endRemoveRows();
        return true;
    }

    bool removeColumns(int column, int count,
                       const QModelIndex& parent = QModelIndex()) override
    {
        if (parent.isValid())
            return false;
        if (count <= 0)
            return false;
        if (column < 0 || column >= columnCount())
            return false;

        const int last = column + count - 1;
        if (last >= columnCount())
            return false;

        beginRemoveColumns(QModelIndex(), column, last);

        // headers
        for (int c = last; c >= column; --c)
            header_.removeAt(c);

        // each row: remove the same column range
        for (auto& row : rows_) {
            // safety: row size should match columnCount()+count before removal,
            // but keep it robust for ragged rows.
            if (column < row.size()) {
                const int available = int(row.size()) - column;
                const int removable = std::min(count, std::max(0, available));
                if (removable > 0)
                    row.remove(column, removable);

            }
        }

        endRemoveColumns();
        return true;
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || role != Qt::DisplayRole)
            return QVariant();

        const int r = index.row();
        const int c = index.column();
        if (r < 0 || r >= rows_.size())
            return QVariant();
        if (c < 0 || c >= rows_[r].size())
            return QVariant();

        double v = rows_[r][c];
        return QString::number(v, 'g', 9);
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override
    {
        if (role != Qt::DisplayRole)
            return QVariant();

        if (orientation == Qt::Horizontal) {
            if (section < 0 || section >= header_.size())
                return QVariant();
            return header_.at(section);
        }

        // Vertical header: 1-based row numbers
        return section + 1;
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override
    {
        Q_UNUSED(index);
        return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    }

private:
    QStringList header_;
    QVector<QVector<double>> rows_;
};
