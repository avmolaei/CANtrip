#include "DiagnosticsView.h"

#include <algorithm>
#include <cctype>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTime>
#include <QVBoxLayout>

#include "../common/Uds.h"

namespace cantrip {

namespace {

std::vector<uint8_t> toVector(const QByteArray& bytes) {
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

QString hexText(const QByteArray& bytes) {
    return QString::fromLatin1(bytes.toHex(' ').toUpper());
}

QString idText(uint32_t id) {
    return "0x" + QString::number(id, 16).toUpper();
}

QByteArray bytes(std::initializer_list<int> values) {
    QByteArray out;
    for (int v : values) out.append(static_cast<char>(v));
    return out;
}

} // namespace

QByteArray DiagnosticsView::parseHex(const QString& text, bool* ok) {
    QString digits = text;
    digits.remove("0x", Qt::CaseInsensitive).remove(' ').remove(',').remove('\t');
    bool good = !digits.isEmpty() && digits.size() % 2 == 0;
    for (const QChar c : digits) good = good && c.unicode() < 128 && std::isxdigit(c.unicode());
    if (ok) *ok = good;
    return good ? QByteArray::fromHex(digits.toLatin1()) : QByteArray();
}

DiagnosticsView::DiagnosticsView(UdsClient* client, QWidget* parent) : QWidget(parent), client_(client) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    log_ = new QTreeWidget(this);
    log_->setColumnCount(4);
    log_->setHeaderLabels({"Time", "", "Bytes", "Meaning"});
    log_->setRootIsDecorated(false);
    log_->setUniformRowHeights(true);
    log_->setColumnWidth(0, 100);
    log_->setColumnWidth(1, 80);
    log_->setColumnWidth(2, 320);
    log_->header()->setStretchLastSection(true);
    layout->addWidget(log_, /*stretch=*/1);

    auto* bottom = new QHBoxLayout();
    bottom->setContentsMargins(6, 0, 6, 6);
    bottom->addWidget(new QLabel("Request:", this));
    rawEdit_ = new QLineEdit(this);
    rawEdit_->setPlaceholderText("e.g: 22 F1 90");
    bottom->addWidget(rawEdit_, /*stretch=*/1);
    auto* sendButton = new QPushButton("Send", this);
    bottom->addWidget(sendButton);
    auto* clearButton = new QPushButton("Clear log", this);
    bottom->addWidget(clearButton);
    stateLabel_ = new QLabel(this);
    stateLabel_->setMinimumWidth(170);
    bottom->addWidget(stateLabel_);
    layout->addLayout(bottom);
    senders_ << sendButton;

    connect(sendButton, &QPushButton::clicked, this, &DiagnosticsView::sendRaw);
    connect(rawEdit_, &QLineEdit::returnPressed, this, &DiagnosticsView::sendRaw);
    connect(clearButton, &QPushButton::clicked, log_, &QTreeWidget::clear);

    connect(client_, &UdsClient::requestSent, this, &DiagnosticsView::onRequestSent);
    connect(client_, &UdsClient::responsePending, this, &DiagnosticsView::onResponsePending);
    connect(client_, &UdsClient::responseReceived, this, &DiagnosticsView::onResponse);
    connect(client_, &UdsClient::failed, this, &DiagnosticsView::onFailed);
    connect(client_, &UdsClient::busyChanged, this, &DiagnosticsView::onBusyChanged);
}

QWidget* DiagnosticsView::ribbonPage() {
    if (ribbonPage_) return ribbonPage_;
    ribbonPage_ = new QWidget();
    auto* layout = new QHBoxLayout(ribbonPage_);

    // Which ECU: its bus and the pair of IDs it is reached on.
    auto* ecuGroup = new QGroupBox("ECU", ribbonPage_);
    auto* ecuLayout = new QGridLayout(ecuGroup);
    busSpin_ = new QSpinBox(ecuGroup);
    busSpin_->setRange(1, 16); // kMaxBuses
    busSpin_->setPrefix("CAN ");
    requestIdEdit_ = new QLineEdit(ecuGroup);
    requestIdEdit_->setFixedWidth(90);
    requestIdEdit_->setToolTip("The ID requests are sent on (tester to ECU), in hex");
    responseIdEdit_ = new QLineEdit(ecuGroup);
    responseIdEdit_->setFixedWidth(90);
    responseIdEdit_->setToolTip("The ID the ECU answers on, in hex");
    extendedCheck_ = new QCheckBox("29-bit IDs", ecuGroup);
    ecuLayout->addWidget(busSpin_, 0, 0);
    ecuLayout->addWidget(new QLabel("Request ID", ecuGroup), 0, 1);
    ecuLayout->addWidget(requestIdEdit_, 0, 2);
    ecuLayout->addWidget(extendedCheck_, 1, 0);
    ecuLayout->addWidget(new QLabel("Response ID", ecuGroup), 1, 1);
    ecuLayout->addWidget(responseIdEdit_, 1, 2);

    auto* sessionGroup = new QGroupBox("Session", ribbonPage_);
    auto* sessionLayout = new QGridLayout(sessionGroup);
    sessionCombo_ = new QComboBox(sessionGroup);
    sessionCombo_->addItem("Default", 0x01);
    sessionCombo_->addItem("Extended", 0x03);
    sessionCombo_->addItem("Programming", 0x02);
    auto* sessionButton = new QPushButton("Switch", sessionGroup);
    testerPresentCheck_ = new QCheckBox("Tester present", sessionGroup);
    testerPresentCheck_->setToolTip("Send Tester Present every two seconds, so the ECU stays in its session");
    sessionLayout->addWidget(sessionCombo_, 0, 0);
    sessionLayout->addWidget(sessionButton, 0, 1);
    sessionLayout->addWidget(testerPresentCheck_, 1, 0, 1, 2);

    auto* dataGroup = new QGroupBox("Data", ribbonPage_);
    auto* dataLayout = new QHBoxLayout(dataGroup);
    auto* readButton = new QPushButton("Read...", dataGroup);
    readButton->setToolTip("Read a data identifier, such as F190 for the VIN");
    auto* writeButton = new QPushButton("Write...", dataGroup);
    dataLayout->addWidget(readButton);
    dataLayout->addWidget(writeButton);

    auto* dtcGroup = new QGroupBox("Trouble Codes", ribbonPage_);
    auto* dtcLayout = new QHBoxLayout(dtcGroup);
    auto* readDtcButton = new QPushButton("Read", dtcGroup);
    auto* clearDtcButton = new QPushButton("Clear", dtcGroup);
    dtcLayout->addWidget(readDtcButton);
    dtcLayout->addWidget(clearDtcButton);

    auto* controlGroup = new QGroupBox("Control", ribbonPage_);
    auto* controlLayout = new QHBoxLayout(controlGroup);
    auto* unlockButton = new QPushButton("Unlock...", controlGroup);
    unlockButton->setToolTip("Security Access: ask for a seed, then send the key");
    auto* resetButton = new QPushButton("Reset ECU", controlGroup);
    controlLayout->addWidget(unlockButton);
    controlLayout->addWidget(resetButton);

    layout->addWidget(ecuGroup);
    layout->addWidget(sessionGroup);
    layout->addWidget(dataGroup);
    layout->addWidget(dtcGroup);
    layout->addWidget(controlGroup);
    layout->addStretch(1);
    senders_ << sessionButton << readButton << writeButton << readDtcButton << clearDtcButton << unlockButton << resetButton;

    setTarget(client_->target());
    connect(busSpin_, &QSpinBox::valueChanged, this, [this]() { applyTarget(); });
    connect(requestIdEdit_, &QLineEdit::editingFinished, this, &DiagnosticsView::applyTarget);
    connect(responseIdEdit_, &QLineEdit::editingFinished, this, &DiagnosticsView::applyTarget);
    connect(extendedCheck_, &QCheckBox::toggled, this, [this]() { applyTarget(); });

    connect(sessionButton, &QPushButton::clicked, this, [this]() {
        send(bytes({Uds::kSessionControl, sessionCombo_->currentData().toInt()}));
    });
    connect(testerPresentCheck_, &QCheckBox::toggled, this, [this](bool on) { client_->setTesterPresent(on); });
    connect(readButton, &QPushButton::clicked, this, &DiagnosticsView::readIdentifier);
    connect(writeButton, &QPushButton::clicked, this, &DiagnosticsView::writeIdentifier);
    // Status mask FF: every code the ECU has, whatever its state.
    connect(readDtcButton, &QPushButton::clicked, this, [this]() { send(bytes({Uds::kReadDtc, 0x02, 0xFF})); });
    connect(clearDtcButton, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::question(this, "Clear Trouble Codes", "Clear every stored trouble code in the ECU?") != QMessageBox::Yes) return;
        send(bytes({Uds::kClearDtc, 0xFF, 0xFF, 0xFF}));
    });
    connect(unlockButton, &QPushButton::clicked, this, &DiagnosticsView::unlock);
    connect(resetButton, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::question(this, "Reset ECU", "Send a hard reset to the ECU?") != QMessageBox::Yes) return;
        send(bytes({Uds::kEcuReset, 0x01}));
    });
    return ribbonPage_;
}

UdsClient::Target DiagnosticsView::target() const {
    return client_->target();
}

void DiagnosticsView::setTarget(const UdsClient::Target& target) {
    client_->setTarget(target);
    if (!busSpin_) return;
    const QSignalBlocker b1(busSpin_), b2(extendedCheck_);
    busSpin_->setValue(target.bus);
    requestIdEdit_->setText(idText(target.requestId));
    responseIdEdit_->setText(idText(target.responseId));
    extendedCheck_->setChecked(target.extended);
}

void DiagnosticsView::applyTarget() {
    UdsClient::Target target = client_->target();
    target.bus = busSpin_->value();
    target.extended = extendedCheck_->isChecked();
    const uint32_t maxId = target.extended ? 0x1FFFFFFFu : 0x7FFu;
    bool okRequest = false, okResponse = false;
    const uint32_t requestId = requestIdEdit_->text().trimmed().toUInt(&okRequest, 16);
    const uint32_t responseId = responseIdEdit_->text().trimmed().toUInt(&okResponse, 16);
    // An ID that does not parse or does not fit keeps its previous value.
    if (okRequest && requestId <= maxId) target.requestId = requestId;
    if (okResponse && responseId <= maxId) target.responseId = responseId;
    target.requestId = std::min(target.requestId, maxId);
    target.responseId = std::min(target.responseId, maxId);
    setTarget(target);
}

void DiagnosticsView::send(const QByteArray& request) {
    if (busSpin_) applyTarget();
    QString error;
    if (!client_->request(request, &error)) {
        waitingForSeed_ = false;
        addRow("Problem", request, error, /*problem=*/true);
    }
}

void DiagnosticsView::sendRaw() {
    bool ok = false;
    const QByteArray request = parseHex(rawEdit_->text(), &ok);
    if (!ok) {
        addRow("Problem", {}, QString("\"%1\" is not a list of hex bytes. Write it like 22 F1 90.").arg(rawEdit_->text()), true);
        return;
    }
    send(request);
}

void DiagnosticsView::readIdentifier() {
    bool accepted = false;
    const QString text = QInputDialog::getText(this, "Read Data", "Identifier, in hex (F190 is the VIN):", QLineEdit::Normal,
                                               lastIdentifier_, &accepted);
    if (!accepted) return;
    bool ok = false;
    const QByteArray did = parseHex(text, &ok);
    if (!ok || did.size() != 2) {
        QMessageBox::warning(this, "Read Data", "An identifier is two bytes, four hex digits, such as F190.");
        return;
    }
    lastIdentifier_ = text.trimmed();
    send(bytes({Uds::kReadDataById}) + did);
}

void DiagnosticsView::writeIdentifier() {
    QDialog dialog(this);
    dialog.setWindowTitle("Write Data");
    auto* form = new QFormLayout(&dialog);
    auto* didEdit = new QLineEdit(lastIdentifier_, &dialog);
    auto* dataEdit = new QLineEdit(&dialog);
    dataEdit->setMinimumWidth(320);
    dataEdit->setPlaceholderText("Hex bytes, or text in \"quotes\"");
    form->addRow("Identifier (hex):", didEdit);
    form->addRow("Data:", dataEdit);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QByteArray request;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        bool ok = false;
        const QByteArray did = parseHex(didEdit->text(), &ok);
        if (!ok || did.size() != 2) {
            QMessageBox::warning(&dialog, "Write Data", "An identifier is two bytes, four hex digits, such as F190.");
            return;
        }
        const QString text = dataEdit->text().trimmed();
        QByteArray data;
        if (text.size() >= 2 && text.startsWith('"') && text.endsWith('"')) {
            data = text.mid(1, text.size() - 2).toLatin1();
        } else {
            data = parseHex(text, &ok);
            if (!ok) {
                QMessageBox::warning(&dialog, "Write Data", "The data is neither hex bytes nor text in quotes.");
                return;
            }
        }
        if (data.isEmpty()) {
            QMessageBox::warning(&dialog, "Write Data", "There is no data to write.");
            return;
        }
        lastIdentifier_ = didEdit->text().trimmed();
        request = bytes({Uds::kWriteDataById}) + did + data;
        dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted) send(request);
}

void DiagnosticsView::unlock() {
    // Level 1: 27 01 asks for the seed, 27 02 carries the key. The key
    // question comes up when the seed arrives (see onResponse).
    waitingForSeed_ = true;
    send(bytes({Uds::kSecurityAccess, 0x01}));
}

void DiagnosticsView::addRow(const QString& kind, const QByteArray& bytes, const QString& meaning, bool problem) {
    auto* item = new QTreeWidgetItem(log_);
    item->setText(0, QTime::currentTime().toString("HH:mm:ss.zzz"));
    item->setText(1, kind);
    item->setText(2, hexText(bytes));
    item->setText(3, meaning);
    item->setToolTip(2, hexText(bytes));
    item->setToolTip(3, meaning);
    if (problem) {
        for (int col = 0; col < 4; ++col) item->setForeground(col, QColor(0xC6, 0x28, 0x28));
    }
    log_->scrollToItem(item);
}

void DiagnosticsView::onRequestSent(const QByteArray& request) {
    addRow("Request", request, QString::fromStdString(udsDescribe(toVector(request))), false);
}

void DiagnosticsView::onResponsePending(const QByteArray& request) {
    addRow("Response", bytes({Uds::kNegativeResponse, static_cast<uint8_t>(request[0]), Uds::kNrcResponsePending}),
           "The ECU is working on it and will answer later", false);
}

void DiagnosticsView::onResponse(const QByteArray& request, const QByteArray& response) {
    const bool seedExpected = waitingForSeed_;
    waitingForSeed_ = false;
    if (response.isEmpty()) return; // the request asked for no answer

    const std::vector<uint8_t> message = toVector(response);
    const bool negative = message[0] == Uds::kNegativeResponse;
    QString meaning = QString::fromStdString(udsDescribe(message));
    if (negative && message.size() >= 3) {
        const QString hint = QString::fromStdString(udsNrcHint(message[2]));
        if (!hint.isEmpty()) meaning += ". " + hint;
    }
    addRow("Response", response, meaning, negative);

    // A seed came back for Unlock: ask for the key and send it.
    if (!seedExpected || negative || response.size() < 3 || static_cast<uint8_t>(request[0]) != Uds::kSecurityAccess) return;
    const QByteArray seed = response.mid(2);
    if (seed.count('\0') == seed.size()) return; // all zero: unlocked already
    bool accepted = false;
    const QString text = QInputDialog::getText(this, "Unlock",
        QString("The ECU's seed is %1.\n\nKey, in hex:").arg(hexText(seed)), QLineEdit::Normal, QString(), &accepted);
    if (!accepted) return;
    bool ok = false;
    const QByteArray key = parseHex(text, &ok);
    if (!ok) {
        addRow("Problem", {}, QString("\"%1\" is not a list of hex bytes, so no key was sent.").arg(text), true);
        return;
    }
    send(bytes({Uds::kSecurityAccess, 0x02}) + key);
}

void DiagnosticsView::onFailed(const QByteArray& request, const QString& reason) {
    waitingForSeed_ = false;
    QString text = reason;
    // Wrong IDs are the likeliest cause by far. And one thing worth saying
    // about the test source: its pretend ECU is not on the default pair,
    // because the source sends a made-up periodic frame on 0x7E8.
    if (reason.startsWith("No answer") || reason.startsWith("The transfer broke")) {
        text += " Check the bus and the two IDs.";
        if (client_->target().responseId == 0x7E8) text += " The synthetic test source's ECU is on 0x7E1 / 0x7E9.";
    }
    addRow("Problem", request, text, true);
}

void DiagnosticsView::onBusyChanged(bool busy) {
    for (QWidget* sender : senders_) sender->setEnabled(!busy);
    stateLabel_->setText(busy ? "Waiting for the ECU..." : QString());
}

} // namespace cantrip
