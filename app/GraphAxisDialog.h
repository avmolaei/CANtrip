// Two small dialogs for the Graph view: one to set a Y axis's shared
// bounds, one to set a single plotted signal's color/line style.
#pragma once

#include <QColor>
#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QPushButton;

namespace cantrip {

enum class GraphLineStyle { Solid, Dashed, Dotted, Scatter };

class GraphAxisBoundsDialog : public QDialog {
    Q_OBJECT
public:
    explicit GraphAxisBoundsDialog(QWidget* parent = nullptr);

    void setName(const QString& name);
    QString name() const;
    void setBounds(bool autoScale, double min, double max);
    bool autoScale() const;
    double minValue() const;
    double maxValue() const;

private slots:
    void updateEnabled();

private:
    QLineEdit* nameEdit_;
    QCheckBox* autoScaleCheck_;
    QDoubleSpinBox* minSpin_;
    QDoubleSpinBox* maxSpin_;
};

class GraphSignalStyleDialog : public QDialog {
    Q_OBJECT
public:
    explicit GraphSignalStyleDialog(QWidget* parent = nullptr);

    void setStyle(const QColor& color, GraphLineStyle style);
    QColor color() const;
    GraphLineStyle lineStyle() const;

private slots:
    void pickColor();

private:
    void updateColorButton();

    QColor color_;
    QPushButton* colorButton_;
    QComboBox* styleCombo_;
};

} // namespace cantrip
