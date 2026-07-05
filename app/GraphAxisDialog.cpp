#include "GraphAxisDialog.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace cantrip {

GraphAxisBoundsDialog::GraphAxisBoundsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Axis Bounds");

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    nameEdit_ = new QLineEdit(this);
    form->addRow("Name:", nameEdit_);

    autoScaleCheck_ = new QCheckBox("Auto-scale", this);
    autoScaleCheck_->setChecked(true);
    form->addRow(autoScaleCheck_);

    minSpin_ = new QDoubleSpinBox(this);
    minSpin_->setRange(-1e9, 1e9);
    minSpin_->setValue(0.0);
    form->addRow("Min:", minSpin_);

    maxSpin_ = new QDoubleSpinBox(this);
    maxSpin_->setRange(-1e9, 1e9);
    maxSpin_->setValue(100.0);
    form->addRow("Max:", maxSpin_);

    root->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);

    connect(autoScaleCheck_, &QCheckBox::toggled, this, &GraphAxisBoundsDialog::updateEnabled);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updateEnabled();
}

void GraphAxisBoundsDialog::setName(const QString& name) { nameEdit_->setText(name); }
QString GraphAxisBoundsDialog::name() const { return nameEdit_->text(); }

void GraphAxisBoundsDialog::updateEnabled() {
    const bool manual = !autoScaleCheck_->isChecked();
    minSpin_->setEnabled(manual);
    maxSpin_->setEnabled(manual);
}

void GraphAxisBoundsDialog::setBounds(bool autoScale, double min, double max) {
    autoScaleCheck_->setChecked(autoScale);
    minSpin_->setValue(min);
    maxSpin_->setValue(max);
    updateEnabled();
}

bool GraphAxisBoundsDialog::autoScale() const { return autoScaleCheck_->isChecked(); }
double GraphAxisBoundsDialog::minValue() const { return minSpin_->value(); }
double GraphAxisBoundsDialog::maxValue() const { return maxSpin_->value(); }

GraphSignalStyleDialog::GraphSignalStyleDialog(QWidget* parent) : QDialog(parent), color_(Qt::blue) {
    setWindowTitle("Signal Appearance");

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    colorButton_ = new QPushButton(this);
    form->addRow("Color:", colorButton_);

    styleCombo_ = new QComboBox(this);
    styleCombo_->addItem("Solid", static_cast<int>(GraphLineStyle::Solid));
    styleCombo_->addItem("Dashed", static_cast<int>(GraphLineStyle::Dashed));
    styleCombo_->addItem("Dotted", static_cast<int>(GraphLineStyle::Dotted));
    styleCombo_->addItem("Scatter (dots)", static_cast<int>(GraphLineStyle::Scatter));
    form->addRow("Line style:", styleCombo_);

    root->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);

    connect(colorButton_, &QPushButton::clicked, this, &GraphSignalStyleDialog::pickColor);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updateColorButton();
}

void GraphSignalStyleDialog::pickColor() {
    const QColor picked = QColorDialog::getColor(color_, this, "Signal Color");
    if (picked.isValid()) {
        color_ = picked;
        updateColorButton();
    }
}

void GraphSignalStyleDialog::updateColorButton() {
    colorButton_->setText(color_.name());
    colorButton_->setStyleSheet(QString("background-color: %1;").arg(color_.name()));
}

void GraphSignalStyleDialog::setStyle(const QColor& color, GraphLineStyle style) {
    color_ = color;
    updateColorButton();
    const int idx = styleCombo_->findData(static_cast<int>(style));
    if (idx >= 0) styleCombo_->setCurrentIndex(idx);
}

QColor GraphSignalStyleDialog::color() const { return color_; }

GraphLineStyle GraphSignalStyleDialog::lineStyle() const {
    return static_cast<GraphLineStyle>(styleCombo_->currentData().toInt());
}

} // namespace cantrip
