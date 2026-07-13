// The "New/Edit Transmit Message" dialog (Stimulation tab) and its "Edit
// Data" hex-grid popup, modeled on PEAK PCAN-View's own transmit dialog.
#pragma once

#include <vector>

#include <QByteArray>
#include <QDialog>

#include "TransmitMessage.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QWidget;

namespace cantrip {

// Hex-byte grid editor for data longer than the parent dialog's inline 8
// bytes can show - one shared byte array, this is just a fuller view of it,
// not a separate copy that needs reconciling (see TransmitMessageDialog's
// "..." button).
class EditDataDialog : public QDialog {
    Q_OBJECT
public:
    explicit EditDataDialog(QWidget* parent = nullptr);

    void setData(const QByteArray& data);
    QByteArray data() const;

private:
    void rebuildGrid();

    QByteArray data_;
    QGridLayout* grid_;
    QWidget* gridHost_;
    std::vector<QLineEdit*> byteEdits_;
};

class TransmitMessageDialog : public QDialog {
    Q_OBJECT
public:
    explicit TransmitMessageDialog(QWidget* parent = nullptr);

    // Read-only bus summary shown at the top, and whether the FD Frame
    // checkbox should even be offered - an FD-capable bus can still carry
    // classic frames mixed in, so this stays a per-message choice, but a
    // classic-only bus can't send FD frames at all.
    void setBusInfo(const QString& summary, bool fdCapable);

    void setMessage(const TransmitMessage& message);
    TransmitMessage message() const;

signals:
    // "Change..." was clicked - MainWindow switches to the Hardware tab and
    // closes this dialog (editing bus config with this dialog floating open
    // would just be stale the moment it changes).
    void hardwareSettingsRequested();

private slots:
    void onFdFrameToggled(bool fd);
    void onExtendedToggled(bool extended);
    void onEditDataClicked();
    void onByteEditFinished();
    void onAccept();

private:
    void refreshLengthOptions();
    void refreshInlineBytesFromData();
    void applyDataLength(int length);

    QLabel* busInfoLabel_;
    QPushButton* changeBusButton_;

    QCheckBox* fdFrameCheck_;
    bool fdCapable_ = false;

    QLineEdit* idEdit_;
    QComboBox* lengthCombo_;

    static constexpr int kInlineByteCount = 8;
    QLineEdit* byteEdits_[kInlineByteCount];
    QPushButton* editDataButton_;
    QByteArray data_;

    QSpinBox* cycleTimeSpin_;
    QCheckBox* pausedCheck_;

    QCheckBox* extendedCheck_;
    QCheckBox* remoteRequestCheck_;
    QCheckBox* bitRateSwitchCheck_;

    QLineEdit* commentEdit_;

    QDialogButtonBox* buttonBox_;
};

} // namespace cantrip
