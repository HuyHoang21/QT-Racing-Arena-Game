#include "chatserver.h"
#include "serverworker.h"
#include <QThread>
#include <functional>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTimer>
#include <QRandomGenerator>

ChatServer::ChatServer(QObject *parent)
    : QTcpServer(parent)
    , timer(new QTimer)
{
    timer->setInterval(10000);

    connect(timer, SIGNAL(timeout()), this, SLOT(automaticSendQuestion()));
    timer->start();
}

void ChatServer::incomingConnection(qintptr socketDescriptor)
{
    ServerWorker *worker = new ServerWorker(this);
    if (!worker->setSocketDescriptor(socketDescriptor)) {
        worker->deleteLater();
        return;
    }
    connect(worker, &ServerWorker::disconnectedFromClient, this, std::bind(&ChatServer::userDisconnected, this, worker));
    connect(worker, &ServerWorker::error, this, std::bind(&ChatServer::userError, this, worker));
    connect(worker, &ServerWorker::jsonReceived, this, std::bind(&ChatServer::jsonReceived, this, worker, std::placeholders::_1));
    connect(worker, &ServerWorker::logMessage, this, &ChatServer::logMessage);
    m_clients.append(worker);
    emit logMessage(QStringLiteral("New client Connected"));
}
void ChatServer::sendJson(ServerWorker *destination, const QJsonObject &message)
{
    Q_ASSERT(destination);
    destination->sendJson(message);
}
void ChatServer::broadcast(const QJsonObject &message, ServerWorker *exclude)
{
    for (ServerWorker *worker : m_clients) {
        Q_ASSERT(worker);
        if (worker == exclude)
            continue;
        sendJson(worker, message);
    }
}

void ChatServer::jsonReceived(ServerWorker *sender, const QJsonObject &doc)
{
    Q_ASSERT(sender);
    emit logMessage(QLatin1String("JSON received ") + QString::fromUtf8(QJsonDocument(doc).toJson()));
    if (sender->userName().isEmpty())
        return jsonFromLoggedOut(sender, doc);
    jsonFromLoggedIn(sender, doc);
}

void ChatServer::userDisconnected(ServerWorker *sender)
{
    m_clients.removeAll(sender);
    const QString userName = sender->userName();
    if (!userName.isEmpty()) {
        QJsonObject disconnectedMessage;
        disconnectedMessage[QStringLiteral("type")] = QStringLiteral("userdisconnected");
        disconnectedMessage[QStringLiteral("username")] = userName;
        broadcast(disconnectedMessage, nullptr);
        emit logMessage(userName + QLatin1String(" disconnected"));
    }
    sender->deleteLater();
}

void ChatServer::userError(ServerWorker *sender)
{
    Q_UNUSED(sender)
    emit logMessage(QLatin1String("Error from ") + sender->userName());
}

void ChatServer::stopServer()
{
    for (ServerWorker *worker : m_clients) {
        worker->disconnectFromClient();
    }
    close();
}

void ChatServer::jsonFromLoggedOut(ServerWorker *sender, const QJsonObject &docObj)
{
    Q_ASSERT(sender);
    const QJsonValue typeVal = docObj.value(QLatin1String("type"));
    if (typeVal.isNull() || !typeVal.isString())
        return;
    if (typeVal.toString().compare(QLatin1String("login"), Qt::CaseInsensitive) != 0)
        return;
    const QJsonValue usernameVal = docObj.value(QLatin1String("username"));
    if (usernameVal.isNull() || !usernameVal.isString())
        return;
    const QString newUserName = usernameVal.toString().simplified();
    if (newUserName.isEmpty())
        return;
    for (ServerWorker *worker : qAsConst(m_clients)) {
        if (worker == sender)
            continue;
        if (worker->userName().compare(newUserName, Qt::CaseInsensitive) == 0) {
            QJsonObject message;
            message[QStringLiteral("type")] = QStringLiteral("login");
            message[QStringLiteral("success")] = false;
            message[QStringLiteral("reason")] = QStringLiteral("duplicate username");
            sendJson(sender, message);
            return;
        }
    }
    sender->setUserName(newUserName);
    QJsonObject successMessage;
    successMessage[QStringLiteral("type")] = QStringLiteral("login");
    successMessage[QStringLiteral("success")] = true;
    sendJson(sender, successMessage);
    QJsonObject connectedMessage;
    connectedMessage[QStringLiteral("type")] = QStringLiteral("newuser");
    connectedMessage[QStringLiteral("username")] = newUserName;
    broadcast(connectedMessage, sender);
}

void ChatServer::jsonFromLoggedIn(ServerWorker *sender, const QJsonObject &docObj)
{
    Q_ASSERT(sender);
    const QJsonValue typeVal = docObj.value(QLatin1String("type"));
    if (typeVal.isNull() || !typeVal.isString())
        return;
    if (typeVal.toString().compare(QLatin1String("message"), Qt::CaseInsensitive) == 0){
        const QJsonValue textVal = docObj.value(QLatin1String("text"));
        if (textVal.isNull() || !textVal.isString())
            return;
        const QString text = textVal.toString().trimmed();
        if (text.isEmpty())
            return;
        QJsonObject message;
        message[QStringLiteral("type")] = QStringLiteral("message");
        message[QStringLiteral("text")] = text;
        message[QStringLiteral("sender")] = sender->userName();
        broadcast(message, sender);
    }
    else if(typeVal.toString().compare(QLatin1String("answer"), Qt::CaseInsensitive) == 0){
        const QJsonValue answerVal = docObj.value(QLatin1String("answer"));
        sender->setAnswer(answerVal.toString().trimmed());
    }
}


QString ChatServer::getQuestion(){
    int arr[5] = {QRandomGenerator::global()->bounded(1, 20), QRandomGenerator::global()->bounded(1, 20),
                  QRandomGenerator::global()->bounded(1, 20), QRandomGenerator::global()->bounded(1, 20)};
    QString operArr[4] = {"+", "+","*", "="};
    arr[4] = arr[0] + arr[1] + arr[2] * arr[3];

    int mask = QRandomGenerator::global()->bounded(5);
    m_answer = QString::number(arr[mask]);

    arr[mask] = -1;
    QString question = "";
    for(int i=0;i<4;i++){
        QString number;
        if(arr[i] == -1){
            number = "?";
        } else {
            number = QString::number(arr[i]);
        }

        question += number + " " + operArr[i] + " ";
    }
    if(arr[4] == -1){
        question += "?";
    } else {
        question += QString::number(arr[4]);
    }

    return question;
}

void ChatServer::automaticSendQuestion(){
    if(QString::compare(m_answer, QStringLiteral("NAN")) != 0){ // if not the first time
        QJsonObject resultMessage;
        resultMessage[QStringLiteral("type")] = QStringLiteral("result");

        for (ServerWorker *worker : m_clients) {
            Q_ASSERT(worker);
            if(QString::compare(worker->getLastAnswer(), m_answer) == 0){
                resultMessage[QStringLiteral("result")] = true;
            } else{
                resultMessage[QStringLiteral("result")] = false;
            }
            sendJson(worker, resultMessage);
        }
    }

    QString sendQuestion = getQuestion();
    QJsonObject questionMessage;
    questionMessage[QStringLiteral("type")] = QStringLiteral("questionarrive");
    questionMessage[QStringLiteral("question")] = sendQuestion;
    broadcast(questionMessage, nullptr);
}
