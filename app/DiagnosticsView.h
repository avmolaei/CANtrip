// The Diagnostics tab: UDS requests to one ECU, and a log of what was asked
// and what came back, each line read out in words.
//
// One object builds both halves: ribbonPage() is the row of controls that
// goes in the ribbon, and the widget itself is the log that fills the main
// area while the tab is selected, the same split the Stimulation tab uses.
#pragma once

#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QWidget>

#include "UdsClient.h"

namespace cantrip {

class DiagnosticsView : public QWidget {
    Q_OBJECT
public:
    explicit DiagnosticsView(UdsClient* client, QWidget* parent = nullptr);

    // Built once, owned by whoever it is added to.
    QWidget* ribbonPage();

    // The ECU being talked to, for saving in and loading from a rune.
    UdsClient::Target target() const;
    void setTarget(const UdsClient::Target& target);

    // "22 F1 90", "22F190" or "0x22 0xF1 0x90" as bytes. Null if it is not
    // hex, or has an odd number of digits.
    static QByteArray parseHex(const QString& text, bool* ok);

private slots:
    void onRequestSent(const QByteArray& request);
    void onResponsePending(const QByteArray& request);
    void onResponse(const QByteArray& request, const QByteArray& response);
    void onFailed(const QByteArray& request, const QString& reason);
    void onBusyChanged(bool busy);

private:
    void applyTarget();
    void send(const QByteArray& request);
    void sendRaw();
    void readIdentifier();
    void writeIdentifier();
    void unlock();
    void addRow(const QString& kind, const QByteArray& bytes, const QString& meaning, bool problem);

    UdsClient* client_;
    QWidget* ribbonPage_ = nullptr;

    QSpinBox* busSpin_ = nullptr;
    QLineEdit* requestIdEdit_ = nullptr;
    QLineEdit* responseIdEdit_ = nullptr;
    QCheckBox* extendedCheck_ = nullptr;
    QComboBox* sessionCombo_ = nullptr;
    QCheckBox* testerPresentCheck_ = nullptr;
    // Everything that sends a request, greyed out while one is waiting.
    QList<QWidget*> senders_;

    QTreeWidget* log_;
    QLineEdit* rawEdit_;
    QLabel* stateLabel_;

    // Set between asking for a seed and getting it: the answer then leads
    // straight to the question for the key.
    bool waitingForSeed_ = false;
    QString lastIdentifier_ = "F190";
};

} // namespace cantrip
