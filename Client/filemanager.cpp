#include "filemanager.h"
#include "metainfo.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <QTimerEvent>
#include <QCryptographicHash>


#include <QsLog/QsLog.h>
#include <QsLog/QsLogDest.h>


FileManager::FileManager(QObject *parent)
    : QThread(parent)
{
    quit = false;
    totalLength = 0;
    readId = 0;
    startVerification = false;
    wokeUp = false;
    newFile = false;
    numPieces = 0;
    verifiedPieces.fill(false);
}

FileManager::~FileManager()
{
    quit = true;
    cond.wakeOne();
    wait();

    foreach (QFile *file, files) {
        file->close();
        delete file;
    }
}

int FileManager::read(int pieceIndex, int offset, int length)
{
    QLOG_DEBUG() << "FileManager::read";

    ReadRequest request;
    request.pieceIndex = pieceIndex;
    request.offset = offset;
    request.length = length;

    QMutexLocker locker(&mutex);
    request.id = readId++;
    readRequests << request;

    if (!wokeUp) {
        wokeUp = true;
        QMetaObject::invokeMethod(this, "wakeUp", Qt::QueuedConnection);
    }

    return request.id;
}

void FileManager::write(int pieceIndex, int offset, const QByteArray &data)
{

    QLOG_DEBUG() << "FileManager::write";

    WriteRequest request;
    request.pieceIndex = pieceIndex;
    request.offset = offset;
    request.data = data;

    QMutexLocker locker(&mutex);
    writeRequests << request;

    if (!wokeUp) {
        wokeUp = true;
        QMetaObject::invokeMethod(this, "wakeUp", Qt::QueuedConnection);
    }
}

void FileManager::verifyPiece(int pieceIndex)
{
    QLOG_DEBUG() << "FileManager::verifyPiece";

    QMutexLocker locker(&mutex);
    pendingVerificationRequests << pieceIndex;
    startVerification = true;

    if (!wokeUp) {
        wokeUp = true;
        QMetaObject::invokeMethod(this, "wakeUp", Qt::QueuedConnection);
    }
}

int FileManager::pieceLengthAt(int pieceIndex) const
{

    QMutexLocker locker(&mutex);
    return (sha1s.size() == pieceIndex + 1)
        ? (totalLength % pieceLength) : pieceLength;
}

QBitArray FileManager::completedPieces() const
{
    QMutexLocker locker(&mutex);
    return verifiedPieces;
}

void FileManager::setCompletedPieces(const QBitArray &pieces)
{
    QMutexLocker locker(&mutex);
    verifiedPieces = pieces;
}

QString FileManager::errorString() const
{
    return errString;
}

void FileManager::run()
{
    QLOG_DEBUG() << "FileManager::run";

    if (!generateFiles())
        return;

    do {
        {
            //QLOG_DEBUG() << "FileManager::run - sleep";
            // Go to sleep if there's nothing to do.
            QMutexLocker locker(&mutex);
            if (!quit && readRequests.isEmpty() && writeRequests.isEmpty() && !startVerification)
                cond.wait(&mutex);            
        }

        // Read pending read requests
        //QLOG_DEBUG() << "FileManager::run - read";
        mutex.lock();
        QList<ReadRequest> newReadRequests = readRequests;
        readRequests.clear();
        mutex.unlock();
        while (!newReadRequests.isEmpty()) {
            ReadRequest request = newReadRequests.takeFirst();
            QByteArray block = readBlock(request.pieceIndex, request.offset, request.length);
            emit dataRead(request.id, request.pieceIndex, request.offset, block);
        }

        // Write pending write requests
        //QLOG_DEBUG() << "FileManager::run - write";
        mutex.lock();
        QList<WriteRequest> newWriteRequests = writeRequests;
        writeRequests.clear();
        while (!quit && !newWriteRequests.isEmpty()) {
            WriteRequest request = newWriteRequests.takeFirst();
            writeBlock(request.pieceIndex, request.offset, request.data);
        }

        // Process pending verification requests
        //QLOG_DEBUG() << "FileManager::run - pending verification";
        if (startVerification) {
            newPendingVerificationRequests = pendingVerificationRequests;
            pendingVerificationRequests.clear();
            verifyFileContents();
            startVerification = false;
        }
        mutex.unlock();
        newPendingVerificationRequests.clear();

    } while (!quit);

    // Write pending write requests
    QLOG_DEBUG() << "FileManager::run - write pending";
    mutex.lock();
    QList<WriteRequest> newWriteRequests = writeRequests;
    writeRequests.clear();
    mutex.unlock();
    while (!newWriteRequests.isEmpty()) {
        WriteRequest request = newWriteRequests.takeFirst();
        writeBlock(request.pieceIndex, request.offset, request.data);
    }
}

void FileManager::startDataVerification()
{
    QMutexLocker locker(&mutex);
    startVerification = true;
    cond.wakeOne();
}

bool FileManager::generateFiles()
{
    QLOG_DEBUG() << "FileManager::generateFiles";

    numPieces = -1;

    QMutexLocker locker(&mutex);
    MetaInfoSingleFile singleFile = metaInfo.singleFile();

    QString prefix;
    if (!destinationPath.isEmpty()) {
        prefix = destinationPath;
        if (!prefix.endsWith('/'))
            prefix += '/';
        QDir dir;
        if (!dir.mkpath(prefix)) {
            errString = tr("Failed to create directory %1").arg(prefix);
            QLOG_ERROR() << "Failed to create directory " << prefix;
            emit error();
            return false;
        }
    }
    QFile *file = new QFile(prefix + singleFile.name);
    if (!file->open(QFile::ReadWrite)) {
        errString = tr("Failed to open/create file %1: %2")
                    .arg(file->fileName()).arg(file->errorString());

        QLOG_ERROR() << "Failed to open/create file " << file->fileName() << ": " << file->errorString() ;

        emit error();
        delete file;
        return false;
    }

    if (file->size() != singleFile.length) {
        newFile = true;
        if (!file->resize(singleFile.length)) {
            errString = tr("Failed to resize file %1: %2")
                        .arg(file->fileName()).arg(file->errorString());
            QLOG_ERROR() << "Failed to resize file " << file->fileName() << ":" << file->errorString();
            delete file;
            emit error();
            return false;
        }
    }
    fileSizes << file->size();
    files << file;
    file->close();

    pieceLength = singleFile.pieceLength;
    totalLength = singleFile.length;
    sha1s = singleFile.sha1Sums;

    numPieces = sha1s.size();
    return true;
}

QByteArray FileManager::readBlock(int pieceIndex, int offset, int length)
{
    QLOG_DEBUG() << "FileManager::readBlock";

    QByteArray block;
    qint64 startReadIndex = (quint64(pieceIndex) * pieceLength) + offset;
    qint64 currentIndex = 0;

    for (int i = 0; !quit && i < files.size() && length > 0; ++i) {
        QFile *file = files[i];
        qint64 currentFileSize = fileSizes.at(i);
        if ((currentIndex + currentFileSize) > startReadIndex) {
            if (!file->isOpen()) {
                if (!file->open(QFile::ReadWrite)) {
                    errString = tr("Failed to read from file %1: %2")
                        .arg(file->fileName()).arg(file->errorString());
                    emit error();
                    break;
                }
            }

            file->seek(startReadIndex - currentIndex);
            QByteArray chunk = file->read(qMin<qint64>(length, currentFileSize - file->pos()));
            file->close();

            block += chunk;
            length -= chunk.size();
            startReadIndex += chunk.size();
            if (length < 0) {
                errString = tr("Failed to read from file %1 (read %3 bytes): %2")
                            .arg(file->fileName()).arg(file->errorString()).arg(length);
                emit error();
                break;
            }
        }
        currentIndex += currentFileSize;
    }
    return block;
}

bool FileManager::writeBlock(int pieceIndex, int offset, const QByteArray &data)
{
    QLOG_DEBUG() << "FileManager::writeBlock";

    qint64 startWriteIndex = (qint64(pieceIndex) * pieceLength) + offset;
    qint64 currentIndex = 0;
    int bytesToWrite = data.size();
    int written = 0;

    for (int i = 0; !quit && i < files.size(); ++i) {
        QFile *file = files[i];
        qint64 currentFileSize = fileSizes.at(i);

        if ((currentIndex + currentFileSize) > startWriteIndex) {
            if (!file->isOpen()) {
                if (!file->open(QFile::ReadWrite)) {
                    errString = tr("Failed to write to file %1: %2")
                        .arg(file->fileName()).arg(file->errorString());
                    emit error();
                    break;
                }
            }

            file->seek(startWriteIndex - currentIndex);
            qint64 bytesWritten = file->write(data.constData() + written,
                                              qMin<qint64>(bytesToWrite, currentFileSize - file->pos()));
            file->close();

            if (bytesWritten <= 0) {
                errString = tr("Failed to write to file %1: %2")
                            .arg(file->fileName()).arg(file->errorString());
                emit error();
                return false;
            }

            written += bytesWritten;
            startWriteIndex += bytesWritten;
            bytesToWrite -= bytesWritten;
            if (bytesToWrite == 0)
                break;
        }
        currentIndex += currentFileSize;
    }
    return true;
}

void FileManager::verifyFileContents()
{
    QLOG_DEBUG() << "FileManager::verifyFileContents";

    // Verify all pieces the first time
    if (newPendingVerificationRequests.isEmpty()) {

        //QLOG_DEBUG() << "FileManager::verifyFileContents - first time";

        if (verifiedPieces.count(true) == 0) {
            verifiedPieces.resize(sha1s.size());

            int oldPercent = 0;
            if (!newFile) {
                int numPieces = sha1s.size();

                for (int index = 0; index < numPieces; ++index) {
                    verifySinglePiece(index);

                    int percent = ((index + 1) * 100) / numPieces;
                    if (oldPercent != percent) {
                        emit verificationProgress(percent);
                        oldPercent = percent;
                    }
                }
            }
        }
        emit verificationDone();
        return;
    }

    // Verify all pending pieces
    foreach (int index, newPendingVerificationRequests)
    {
        QLOG_DEBUG() << "FileManager::verifyFileContents - piece:" << index;
        emit pieceVerified(index, verifySinglePiece(index));
    }
}

bool FileManager::verifySinglePiece(int pieceIndex)
{
    QLOG_DEBUG() << "FileManager::verifySinglePiece";

    QByteArray block = readBlock(pieceIndex, 0, pieceLength);
    QByteArray sha1Sum = QCryptographicHash::hash(block, QCryptographicHash::Sha1);

    if (sha1Sum != sha1s.at(pieceIndex))
        return false;
    verifiedPieces.setBit(pieceIndex);
    return true;
}

void FileManager::wakeUp()
{
    QMutexLocker locker(&mutex);
    wokeUp = false;
    cond.wakeOne();
}
