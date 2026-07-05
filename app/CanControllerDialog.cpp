#include "CanControllerDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace cantrip {

namespace {

// Assumed CAN controller reference clock, matching the default this
// codebase's PEAK/Vector FD init has used since CAN FD support was added.
constexpr uint32_t kFClockHz = 80000000;

void addBitratePresets(QComboBox* combo) {
    combo->addItem("125 kbit/s", 125000);
    combo->addItem("250 kbit/s", 250000);
    combo->addItem("500 kbit/s", 500000);
    combo->addItem("1 Mbit/s", 1000000);
}

void addDataRatePresets(QComboBox* combo) {
    combo->addItem("2 Mbit/s", 2000000);
    combo->addItem("5 Mbit/s", 5000000);
}

// Returns the numeric bps value for an editable QComboBox: the stored
// itemData if the current text matches a preset exactly, otherwise the
// current text parsed directly as a raw bps number (custom entry).
uint32_t comboBpsValue(const QComboBox* combo) {
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemText(i) == combo->currentText()) {
            return combo->itemData(i).toUInt();
        }
    }
    bool ok = false;
    uint32_t v = combo->currentText().toUInt(&ok);
    return ok ? v : 0;
}

void setComboBpsValue(QComboBox* combo, uint32_t bps) {
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemData(i).toUInt() == bps) {
            combo->setCurrentIndex(i);
            return;
        }
    }
    combo->setCurrentText(QString::number(bps));
}

} // namespace

CanControllerDialog::CanControllerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("CAN Controller");

    auto* root = new QVBoxLayout(this);

    auto* topForm = new QFormLayout();
    modeCombo_ = new QComboBox(this);
    modeCombo_->addItem("CAN", static_cast<int>(Mode::Classic));
    modeCombo_->addItem("ISO CAN FD", static_cast<int>(Mode::IsoFd));
    modeCombo_->addItem("Expert CAN FD", static_cast<int>(Mode::ExpertFd));
    topForm->addRow("Mode:", modeCombo_);

    baudrateCombo_ = new QComboBox(this);
    baudrateCombo_->setEditable(true);
    addBitratePresets(baudrateCombo_);
    baudrateCombo_->setCurrentIndex(2); // 500 kbit/s
    topForm->addRow("Baudrate:", baudrateCombo_);
    root->addLayout(topForm);

    fdPanel_ = new QWidget(this);
    auto* fdLayout = new QVBoxLayout(fdPanel_);
    fdLayout->setContentsMargins(0, 0, 0, 0);

    auto* fdForm = new QFormLayout();
    dataRateCombo_ = new QComboBox(fdPanel_);
    dataRateCombo_->setEditable(true);
    addDataRatePresets(dataRateCombo_);
    fdForm->addRow("Data Rate:", dataRateCombo_);
    fdLayout->addLayout(fdForm);

    fdStack_ = new QStackedWidget(fdPanel_);
    fdStack_->addWidget(buildIsoPage());
    fdStack_->addWidget(buildExpertPage());
    fdLayout->addWidget(fdStack_);

    root->addWidget(fdPanel_);

    buttonBox_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttonBox_);

    connect(modeCombo_, &QComboBox::currentIndexChanged, this, &CanControllerDialog::onModeChanged);
    connect(baudrateCombo_, &QComboBox::editTextChanged, this, &CanControllerDialog::updateIsoPreview);
    connect(dataRateCombo_, &QComboBox::editTextChanged, this, &CanControllerDialog::updateIsoPreview);
    connect(nomSamplePointSpin_, &QSpinBox::valueChanged, this, &CanControllerDialog::updateIsoPreview);
    connect(dataSamplePointSpin_, &QSpinBox::valueChanged, this, &CanControllerDialog::updateIsoPreview);
    connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    onModeChanged();
}

QWidget* CanControllerDialog::buildIsoPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    nomSamplePointSpin_ = new QSpinBox(page);
    nomSamplePointSpin_->setRange(50, 95);
    nomSamplePointSpin_->setValue(70);
    nomSamplePointSpin_->setSuffix(" %");
    form->addRow("Nominal Sample Point:", nomSamplePointSpin_);

    dataSamplePointSpin_ = new QSpinBox(page);
    dataSamplePointSpin_->setRange(50, 95);
    dataSamplePointSpin_->setValue(70);
    dataSamplePointSpin_->setSuffix(" %");
    form->addRow("Data Sample Point:", dataSamplePointSpin_);

    achievedLabel_ = new QLabel(page);
    achievedLabel_->setWordWrap(true);
    form->addRow(achievedLabel_);

    return page;
}

QWidget* CanControllerDialog::buildExpertPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    auto makeSpin = [&](int max, int def) {
        auto* spin = new QSpinBox(page);
        spin->setRange(1, max);
        spin->setValue(def);
        return spin;
    };

    nomBrpSpin_ = makeSpin(256, 2);
    nomTseg1Spin_ = makeSpin(128, 63);
    nomTseg2Spin_ = makeSpin(32, 16);
    nomSjwSpin_ = makeSpin(32, 16);
    dataBrpSpin_ = makeSpin(256, 2);
    dataTseg1Spin_ = makeSpin(16, 15);
    dataTseg2Spin_ = makeSpin(8, 4);
    dataSjwSpin_ = makeSpin(8, 4);

    form->addRow("Nominal BRP:", nomBrpSpin_);
    form->addRow("Nominal TSEG1:", nomTseg1Spin_);
    form->addRow("Nominal TSEG2:", nomTseg2Spin_);
    form->addRow("Nominal SJW:", nomSjwSpin_);
    form->addRow("Data BRP:", dataBrpSpin_);
    form->addRow("Data TSEG1:", dataTseg1Spin_);
    form->addRow("Data TSEG2:", dataTseg2Spin_);
    form->addRow("Data SJW:", dataSjwSpin_);

    return page;
}

CanControllerDialog::Mode CanControllerDialog::currentMode() const {
    return static_cast<Mode>(modeCombo_->currentData().toInt());
}

void CanControllerDialog::onModeChanged() {
    const Mode mode = currentMode();
    fdPanel_->setVisible(mode != Mode::Classic);

    if (mode == Mode::ExpertFd && lastFdResult_) {
        // Give the user a sane, real starting point instead of blank/default
        // fields when they switch over from a working ISO-mode solution.
        nomBrpSpin_->setValue(static_cast<int>(lastFdResult_->nominal.brp));
        nomTseg1Spin_->setValue(static_cast<int>(lastFdResult_->nominal.tseg1));
        nomTseg2Spin_->setValue(static_cast<int>(lastFdResult_->nominal.tseg2));
        nomSjwSpin_->setValue(static_cast<int>(lastFdResult_->nominal.sjw));
        dataBrpSpin_->setValue(static_cast<int>(lastFdResult_->data.brp));
        dataTseg1Spin_->setValue(static_cast<int>(lastFdResult_->data.tseg1));
        dataTseg2Spin_->setValue(static_cast<int>(lastFdResult_->data.tseg2));
        dataSjwSpin_->setValue(static_cast<int>(lastFdResult_->data.sjw));
    }

    if (mode == Mode::IsoFd) {
        fdStack_->setCurrentIndex(0);
        updateIsoPreview();
    } else {
        fdStack_->setCurrentIndex(1);
        buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(true);
    }
}

void CanControllerDialog::updateIsoPreview() {
    if (currentMode() != Mode::IsoFd) return;

    const uint32_t nomBps = comboBpsValue(baudrateCombo_);
    const uint32_t dataBps = comboBpsValue(dataRateCombo_);
    auto result = calculateFdTiming(kFClockHz, nomBps, nomSamplePointSpin_->value(), dataBps, dataSamplePointSpin_->value());

    if (!result) {
        lastFdResult_.reset();
        achievedLabel_->setText("<font color='red'>No valid bit-timing solution for these values.</font>");
        buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(false);
        return;
    }

    lastFdResult_ = result;
    achievedLabel_->setText(QString(
        "Achieved: nominal %1% (BRP %2, TSEG1 %3, TSEG2 %4, SJW %5), "
        "data %6% (BRP %7, TSEG1 %8, TSEG2 %9, SJW %10)")
        .arg(result->achievedNomSamplePoint, 0, 'f', 1)
        .arg(result->nominal.brp).arg(result->nominal.tseg1).arg(result->nominal.tseg2).arg(result->nominal.sjw)
        .arg(result->achievedDataSamplePoint, 0, 'f', 1)
        .arg(result->data.brp).arg(result->data.tseg1).arg(result->data.tseg2).arg(result->data.sjw));
    buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(true);
}

void CanControllerDialog::setConfig(const CanBitrateConfig& config) {
    setComboBpsValue(baudrateCombo_, config.nominalBitrateBps);
    if (!config.fd) {
        modeCombo_->setCurrentIndex(static_cast<int>(Mode::Classic));
        onModeChanged();
        return;
    }

    setComboBpsValue(dataRateCombo_, config.dataBitrateBps);
    // We only get here with a previously-accepted FD config's raw timing
    // fields, not which mode (ISO vs. Expert) produced them - Expert mode
    // shows those exact values directly, which is the more transparent
    // choice for "what will actually be sent to the hardware".
    modeCombo_->setCurrentIndex(static_cast<int>(Mode::ExpertFd));
    nomBrpSpin_->setValue(static_cast<int>(config.nominalTiming.brp));
    nomTseg1Spin_->setValue(static_cast<int>(config.nominalTiming.tseg1));
    nomTseg2Spin_->setValue(static_cast<int>(config.nominalTiming.tseg2));
    nomSjwSpin_->setValue(static_cast<int>(config.nominalTiming.sjw));
    dataBrpSpin_->setValue(static_cast<int>(config.dataTiming.brp));
    dataTseg1Spin_->setValue(static_cast<int>(config.dataTiming.tseg1));
    dataTseg2Spin_->setValue(static_cast<int>(config.dataTiming.tseg2));
    dataSjwSpin_->setValue(static_cast<int>(config.dataTiming.sjw));
    onModeChanged();
}

CanBitrateConfig CanControllerDialog::config() const {
    CanBitrateConfig cfg;
    cfg.nominalBitrateBps = comboBpsValue(baudrateCombo_);

    const Mode mode = currentMode();
    if (mode == Mode::Classic) {
        cfg.fd = false;
        return cfg;
    }

    cfg.fd = true;
    cfg.dataBitrateBps = comboBpsValue(dataRateCombo_);

    if (mode == Mode::IsoFd && lastFdResult_) {
        cfg.nominalTiming = lastFdResult_->nominal;
        cfg.dataTiming = lastFdResult_->data;
    } else {
        cfg.nominalTiming = CanTimingValues{
            static_cast<uint32_t>(nomBrpSpin_->value()), static_cast<uint32_t>(nomTseg1Spin_->value()),
            static_cast<uint32_t>(nomTseg2Spin_->value()), static_cast<uint32_t>(nomSjwSpin_->value())};
        cfg.dataTiming = CanTimingValues{
            static_cast<uint32_t>(dataBrpSpin_->value()), static_cast<uint32_t>(dataTseg1Spin_->value()),
            static_cast<uint32_t>(dataTseg2Spin_->value()), static_cast<uint32_t>(dataSjwSpin_->value())};
    }
    return cfg;
}

} // namespace cantrip
