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

    // One bus the message can go out on, as listed in the Bus dropdown at
    // the top. `summary` is its configuration ("CAN FD, 500000 bps /
    // 2000000 bps"), shown under the dropdown. fdCapable says whether the
    // FD Frame checkbox is offered: an FD bus can still carry classic
    // frames, but a classic-only bus can't send FD frames at all.
    struct BusChoice {
        int bus = 1;
        QString hardware; // the channel it is on
        QString summary;
        bool fdCapable = false;
    };
    // Call before setMessage(). The first bus is selected.
    void setBuses(const std::vector<BusChoice>& buses);

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

    void onBusChanged();

    QComboBox* busCombo_;
    std::vector<BusChoice> buses_;
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
