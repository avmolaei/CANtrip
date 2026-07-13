#include "TransmitMessageDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QVBoxLayout>

namespace cantrip {

namespace {

// Real FD byte lengths, matching AscLogWriter.cpp's dlcCodeFromByteLength
// reverse table exactly - not every value 0-64 is a valid CAN FD length.
const int kFdLengths[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};

QString byteToHex(uint8_t b) {
    return QString("%1").arg(b, 2, 16, QChar('0')).toUpper();
}

} // namespace

// ---------------------------------------------------------------- EditDataDialog

EditDataDialog::EditDataDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Edit Data");

    auto* root = new QVBoxLayout(this);
    gridHost_ = new QWidget(this);
    grid_ = new QGridLayout(gridHost_);
    root->addWidget(gridHost_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void EditDataDialog::setData(const QByteArray& data) {
    data_ = data;
    rebuildGrid();
}

QByteArray EditDataDialog::data() const {
    return data_;
}

void EditDataDialog::rebuildGrid() {
    delete gridHost_->layout();
    grid_ = new QGridLayout(gridHost_);
    byteEdits_.clear();

    grid_->addWidget(new QLabel("Offset", gridHost_), 0, 0);
    for (int col = 0; col < 8; ++col) {
        grid_->addWidget(new QLabel(QString("%1").arg(col, 2, 16, QChar('0')).toUpper(), gridHost_), 0, col + 1);
    }

    const int rows = (data_.size() + 7) / 8;
    for (int row = 0; row < rows; ++row) {
        grid_->addWidget(new QLabel(QString("%1").arg(row * 8, 2, 16, QChar('0')).toUpper(), gridHost_), row + 1, 0);
        for (int col = 0; col < 8; ++col) {
            const int index = row * 8 + col;
            if (index >= data_.size()) continue;

            auto* edit = new QLineEdit(gridHost_);
            edit->setMaxLength(2);
            edit->setFixedWidth(28);
            edit->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9A-Fa-f]{0,2}"), edit));
            edit->setText(byteToHex(static_cast<uint8_t>(data_[index])));
            const int capturedIndex = index;
            connect(edit, &QLineEdit::editingFinished, this, [this, capturedIndex, edit]() {
                bool ok = false;
                const int v = edit->text().toInt(&ok, 16);
                if (ok && capturedIndex < data_.size()) data_[capturedIndex] = static_cast<char>(v);
            });
            grid_->addWidget(edit, row + 1, col + 1);
            byteEdits_.push_back(edit);
        }
    }
}

// ---------------------------------------------------------------- TransmitMessageDialog

TransmitMessageDialog::TransmitMessageDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("New Transmit Message");

    auto* root = new QVBoxLayout(this);

    auto* busRow = new QHBoxLayout();
    busInfoLabel_ = new QLabel("Bus: (unknown)", this);
    changeBusButton_ = new QPushButton("Change...", this);
    busRow->addWidget(busInfoLabel_, /*stretch=*/1);
    busRow->addWidget(changeBusButton_);
    root->addLayout(busRow);

    auto* form = new QFormLayout();

    fdFrameCheck_ = new QCheckBox("FD Frame", this);
    form->addRow(fdFrameCheck_);

    idEdit_ = new QLineEdit(this);
    idEdit_->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9A-Fa-f]{1,8}"), idEdit_));
    idEdit_->setText("000");
    form->addRow("ID (hex):", idEdit_);

    lengthCombo_ = new QComboBox(this);
    form->addRow("Length:", lengthCombo_);

    auto* dataRow = new QHBoxLayout();
    for (int i = 0; i < kInlineByteCount; ++i) {
        byteEdits_[i] = new QLineEdit(this);
        byteEdits_[i]->setMaxLength(2);
        byteEdits_[i]->setFixedWidth(28);
        byteEdits_[i]->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9A-Fa-f]{0,2}"), byteEdits_[i]));
        byteEdits_[i]->setText("00");
        connect(byteEdits_[i], &QLineEdit::editingFinished, this, &TransmitMessageDialog::onByteEditFinished);
        dataRow->addWidget(byteEdits_[i]);
    }
    editDataButton_ = new QPushButton("...", this);
    dataRow->addWidget(editDataButton_);
    form->addRow("Data (hex):", dataRow);

    cycleTimeSpin_ = new QSpinBox(this);
    cycleTimeSpin_->setRange(0, 3600000);
    cycleTimeSpin_->setSuffix(" ms");
    cycleTimeSpin_->setSpecialValueText("0 (one-shot)");
    form->addRow("Cycle Time:", cycleTimeSpin_);

    pausedCheck_ = new QCheckBox("Paused", this);
    form->addRow(pausedCheck_);

    extendedCheck_ = new QCheckBox("Extended Frame", this);
    remoteRequestCheck_ = new QCheckBox("Remote Request", this);
    bitRateSwitchCheck_ = new QCheckBox("Bit Rate Switch", this);
    bitRateSwitchCheck_->setVisible(false);
    form->addRow(extendedCheck_);
    form->addRow(remoteRequestCheck_);
    form->addRow(bitRateSwitchCheck_);

    commentEdit_ = new QLineEdit(this);
    form->addRow("Comment:", commentEdit_);

    root->addLayout(form);

    buttonBox_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttonBox_);

    connect(changeBusButton_, &QPushButton::clicked, this, [this]() {
        emit hardwareSettingsRequested();
        reject();
    });
    connect(fdFrameCheck_, &QCheckBox::toggled, this, &TransmitMessageDialog::onFdFrameToggled);
    connect(extendedCheck_, &QCheckBox::toggled, this, &TransmitMessageDialog::onExtendedToggled);
    connect(editDataButton_, &QPushButton::clicked, this, &TransmitMessageDialog::onEditDataClicked);
    connect(lengthCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        applyDataLength(lengthCombo_->currentData().toInt());
    });
    connect(buttonBox_, &QDialogButtonBox::accepted, this, &TransmitMessageDialog::onAccept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    data_ = QByteArray(8, '\0');
    refreshLengthOptions();
    onFdFrameToggled(false);
}

void TransmitMessageDialog::setBusInfo(const QString& summary, bool fdCapable) {
    busInfoLabel_->setText("Bus: " + summary);
    fdCapable_ = fdCapable;
    fdFrameCheck_->setEnabled(fdCapable);
    fdFrameCheck_->setChecked(fdCapable);
}

void TransmitMessageDialog::onFdFrameToggled(bool fd) {
    remoteRequestCheck_->setVisible(!fd);
    bitRateSwitchCheck_->setVisible(fd);
    refreshLengthOptions();
}

void TransmitMessageDialog::onExtendedToggled(bool extended) {
    idEdit_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(extended ? "[0-9A-Fa-f]{1,8}" : "[0-9A-Fa-f]{1,3}"), idEdit_));
}

void TransmitMessageDialog::refreshLengthOptions() {
    const int previous = lengthCombo_->currentIndex() >= 0 ? lengthCombo_->currentData().toInt() : 8;
    lengthCombo_->clear();
    if (fdFrameCheck_->isChecked()) {
        for (int len : kFdLengths) lengthCombo_->addItem(QString::number(len), len);
    } else {
        for (int len = 0; len <= 8; ++len) lengthCombo_->addItem(QString::number(len), len);
    }
    int restoreIndex = lengthCombo_->findData(previous);
    lengthCombo_->setCurrentIndex(restoreIndex >= 0 ? restoreIndex : lengthCombo_->count() - 1);
    applyDataLength(lengthCombo_->currentData().toInt());
}

void TransmitMessageDialog::applyDataLength(int length) {
    if (data_.size() < length) {
        data_.append(QByteArray(length - data_.size(), '\0'));
    } else {
        data_.truncate(length);
    }
    refreshInlineBytesFromData();
}

void TransmitMessageDialog::refreshInlineBytesFromData() {
    for (int i = 0; i < kInlineByteCount; ++i) {
        byteEdits_[i]->setText(i < data_.size() ? byteToHex(static_cast<uint8_t>(data_[i])) : QString());
        byteEdits_[i]->setEnabled(i < data_.size());
    }
}

void TransmitMessageDialog::onByteEditFinished() {
    for (int i = 0; i < kInlineByteCount; ++i) {
        if (i >= data_.size()) continue;
        bool ok = false;
        const int v = byteEdits_[i]->text().toInt(&ok, 16);
        if (ok) data_[i] = static_cast<char>(v);
    }
}

void TransmitMessageDialog::onEditDataClicked() {
    // Sync the canonical data_ from whatever's currently in the inline boxes
    // before opening the popup - the two are one shared model, not separate
    // copies, so this and refreshInlineBytesFromData() below are the only
    // two sync points needed (the dialog is modal, so there's no moment
    // where both are simultaneously visible/interactive to keep in sync).
    onByteEditFinished();

    EditDataDialog dialog(this);
    dialog.setData(data_);
    if (dialog.exec() != QDialog::Accepted) return;

    data_ = dialog.data();
    refreshInlineBytesFromData();
}

void TransmitMessageDialog::onAccept() {
    bool ok = false;
    const uint32_t id = idEdit_->text().toUInt(&ok, 16);
    const uint32_t maxId = extendedCheck_->isChecked() ? 0x1FFFFFFFu : 0x7FFu;
    if (!ok || id > maxId) {
        QMessageBox::warning(this, "New Transmit Message",
            QString("ID must be a hex value up to 0x%1 for %2 frames.")
                .arg(maxId, 0, 16).arg(extendedCheck_->isChecked() ? "extended" : "standard"));
        return;
    }
    onByteEditFinished();
    accept();
}

void TransmitMessageDialog::setMessage(const TransmitMessage& message) {
    extendedCheck_->setChecked(message.extended);
    idEdit_->setText(QString("%1").arg(message.id, 0, 16).toUpper());
    fdFrameCheck_->setChecked(message.fd);
    onFdFrameToggled(message.fd);

    // Set the length combo first (which resizes data_ via the connected
    // signal/applyDataLength), then overwrite data_ with the real loaded
    // bytes - avoids the resize logic clobbering what we're about to load.
    const int idx = lengthCombo_->findData(static_cast<int>(message.dlc));
    lengthCombo_->setCurrentIndex(idx >= 0 ? idx : lengthCombo_->count() - 1);
    applyDataLength(static_cast<int>(message.dlc));
    data_ = message.data;
    if (data_.size() != message.dlc) data_.resize(message.dlc);
    refreshInlineBytesFromData();

    cycleTimeSpin_->setValue(message.cycleTimeMs);
    pausedCheck_->setChecked(message.paused);
    remoteRequestCheck_->setChecked(message.rtr);
    bitRateSwitchCheck_->setChecked(message.brs);
    commentEdit_->setText(message.comment);
}

TransmitMessage TransmitMessageDialog::message() const {
    TransmitMessage msg;
    msg.extended = extendedCheck_->isChecked();
    msg.id = idEdit_->text().toUInt(nullptr, 16);
    msg.fd = fdFrameCheck_->isChecked();
    msg.dlc = static_cast<uint8_t>(lengthCombo_->currentData().toInt());
    msg.data = data_;
    msg.cycleTimeMs = cycleTimeSpin_->value();
    msg.paused = pausedCheck_->isChecked();
    msg.rtr = msg.fd ? false : remoteRequestCheck_->isChecked();
    msg.brs = msg.fd ? bitRateSwitchCheck_->isChecked() : false;
    msg.comment = commentEdit_->text();
    return msg;
}

} // namespace cantrip
