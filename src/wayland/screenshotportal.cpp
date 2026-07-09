// SPDX-FileCopyrightText: 2024 - 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "screenshotportal.h"
#include "protocols/common.h"
#include "protocols/treelandcapture.h"
#include "loggings.h"
#include "dbushelpers.h"
#include "request2.h"

#include <QApplication>
#include <QDateTime>
#include <QScreen>
#include <QPainter>
#include <QDir>
#include <QRegion>
#include <QStandardPaths>
#include <QEventLoop>
#include <QPointer>
#include <QScopeGuard>

#include <private/qwaylandscreen_p.h>

struct ScreenCaptureInfo {
    QtWaylandClient::QWaylandScreen *screen {nullptr};
    QPointer<ScreenCopyFrame> capturedFrame {nullptr};
    QtWaylandClient::QWaylandShmBuffer *shmBuffer {nullptr};
    QtWayland::zwlr_screencopy_frame_v1::flags flags;
    bool failed {false};
    bool finished {false};
};

static void destroyScreenCaptureInfo(const std::list<std::shared_ptr<ScreenCaptureInfo>> &captureList)
{
    for (const auto &info : std::as_const(captureList)) {
        if (info->shmBuffer) {
            delete info->shmBuffer;
        }

        if (info->capturedFrame) {
            delete info->capturedFrame;
        }
    }
}

static PortalResponse::Response responseFromSourceFailure(uint32_t reason)
{
    using CaptureContext = QtWayland::treeland_capture_context_v1;
    if (reason == CaptureContext::source_failure_user_cancel) {
        return PortalResponse::Cancelled;
    }

    return PortalResponse::OtherError;
}

ScreenshotPortalWayland::ScreenshotPortalWayland(PortalWaylandContext *context)
    : AbstractWaylandPortal(context)
{

}

uint ScreenshotPortalWayland::PickColor(const QDBusObjectPath &handle,
                                 const QString &app_id,
                                 const QString &parent_window, // might just ignore this argument now
                                 const QVariantMap &options,
                                 QVariantMap &results)
{
    // TODO Implement PickColor
    qCDebug(SCREESHOT) << "PickColor called with parameters:";
    qCDebug(SCREESHOT) << "    handle: " << handle.path();
    qCDebug(SCREESHOT) << "    app_id: " << app_id;
    qCDebug(SCREESHOT) << "    parent_window: " << parent_window;
    qCDebug(SCREESHOT) << "    options: " << options;

    return PortalResponse::Cancelled;
}

ScreenshotPortalWayland::CaptureResult ScreenshotPortalWayland::fullScreenShot()
{
    std::list<std::shared_ptr<ScreenCaptureInfo>> captureList;
    const auto cleanup = qScopeGuard([&captureList] {
        destroyScreenCaptureInfo(captureList);
    });
    int pendingCapture = 0;
    auto screenCopyManager = context()->screenCopyManager();
    if (!screenCopyManager || !screenCopyManager->isActive()) {
        qCWarning(SCREESHOT) << "Screen copy manager is not active";
        return {PortalResponse::OtherError, QString()};
    }

    const auto screens = waylandDisplay()->screens();
    if (screens.isEmpty()) {
        qCWarning(SCREESHOT) << "No Wayland screens available for screenshot";
        return {PortalResponse::OtherError, QString()};
    }

    QEventLoop eventLoop;
    QRegion outputRegion;

    if (m_currentScreenshotRequest) {
        connect(m_currentScreenshotRequest, &Request2::closeRequested, &eventLoop, [this, &eventLoop] {
            m_currentScreenshotCancelled = true;
            eventLoop.quit();
        });
    }

    auto finishCapture = [&pendingCapture, &eventLoop](const std::shared_ptr<ScreenCaptureInfo> &info, bool failed) {
        if (info->finished) {
            return;
        }

        info->failed = failed;
        info->finished = true;
        if (--pendingCapture == 0) {
            eventLoop.quit();
        }
    };

    // Capture each output
    for (auto screen : screens) {
        auto info = std::make_shared<ScreenCaptureInfo>();
        outputRegion += screen->geometry();
        auto output = screen->output();
        info->capturedFrame = screenCopyManager->captureOutput(false, output);
        if (!info->capturedFrame || !info->capturedFrame->isInitialized()) {
            qCWarning(SCREESHOT) << "Failed to create screencopy frame for output" << screen->name();
            return {PortalResponse::OtherError, QString()};
        }
        info->screen = screen;
        ++pendingCapture;
        captureList.push_back(info);
        connect(info->capturedFrame, &ScreenCopyFrame::buffer, this,
                [info, &finishCapture](uint32_t format, uint32_t width, uint32_t height, uint32_t stride){
                    // Create a new wl_buffer for reception
                    // For some reason, Qt regards stride == width * 4, and it creates buffer likewise, we must check this
                    if (stride != width * 4) {
                        qCWarning(SCREESHOT)
                                << "Receive a buffer format which is not compatible with QWaylandShmBuffer."
                                << "format:" << format << "width:" << width << "height:" << height
                                << "stride:" << stride;
                        finishCapture(info, true);
                        return;
                    }
                    if (info->shmBuffer)
                        return; // We only need one supported format

                    info->shmBuffer = new QtWaylandClient::QWaylandShmBuffer(
                            waylandDisplay(),
                            QSize(width, height),
                            QtWaylandClient::QWaylandShm::formatFrom(static_cast<::wl_shm_format>(format)));
                    info->capturedFrame->copy(info->shmBuffer->buffer());
                });
        connect(info->capturedFrame, &ScreenCopyFrame::frameFlags, this,
                [info](uint32_t flags){
                    info->flags = static_cast<QtWayland::zwlr_screencopy_frame_v1::flags>(flags);
                });
        connect(info->capturedFrame, &ScreenCopyFrame::ready, this,
                [info, &finishCapture](uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
                    Q_UNUSED(tv_sec_hi);
                    Q_UNUSED(tv_sec_lo);
                    Q_UNUSED(tv_nsec);
                    auto image = info->shmBuffer ? info->shmBuffer->image() : nullptr;
                    if (!image || image->isNull()) {
                        qCWarning(SCREESHOT) << "Screencopy frame is ready without a valid image";
                        finishCapture(info, true);
                        return;
                    }
                    finishCapture(info, false);
                });
        connect(info->capturedFrame, &ScreenCopyFrame::failed, this, [info, &finishCapture]{
            qCWarning(SCREESHOT) << "Screencopy frame failed";
            finishCapture(info, true);
        });
    }

    eventLoop.exec();
    if (m_currentScreenshotCancelled) {
        return {PortalResponse::Cancelled, QString()};
    }
    if (outputRegion.isEmpty()) {
        qCWarning(SCREESHOT) << "Captured output region is empty";
        return {PortalResponse::OtherError, QString()};
    }
    for (const auto &info : std::as_const(captureList)) {
        auto image = info->shmBuffer ? info->shmBuffer->image() : nullptr;
        if (info->failed || !image || image->isNull()) {
            qCWarning(SCREESHOT) << "Unable to compose screenshot because an output image is invalid";
            return {PortalResponse::OtherError, QString()};
        }
    }

    // Cat them according to layout
    const QRect boundingRect = outputRegion.boundingRect();
    QImage image(boundingRect.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    for (const auto &info : std::as_const(captureList)) {
        if (info->shmBuffer) {
            QRect targetRect = info->screen->geometry().translated(-boundingRect.topLeft());
            // Convert to screen image local coordinates
            auto sourceRect = targetRect;
            sourceRect.moveTo(QPoint(0, 0));
            p.drawImage(targetRect, *info->shmBuffer->image(), sourceRect);
        } else {
            qCWarning(SCREESHOT) << "image is null!!!";
        }
    }
    static const char *SaveFormat = "PNG";
    auto saveBasePath = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    QDir saveBaseDir(saveBasePath);
    if (!saveBaseDir.exists()) return {PortalResponse::OtherError, QString()};
    QString picName = "portal screenshot - " + QDateTime::currentDateTime().toString() + ".png";
    if (image.save(saveBaseDir.absoluteFilePath(picName), SaveFormat)) {
        return {PortalResponse::Success, saveBaseDir.absoluteFilePath(picName)};
    } else {
        return {PortalResponse::OtherError, QString()};
    }
}

ScreenshotPortalWayland::CaptureResult ScreenshotPortalWayland::captureInteractively()
{
    auto captureManager = context()->treelandCaptureManager();
    if (!captureManager || !captureManager->isActive()) {
        qCWarning(SCREESHOT) << "Treeland capture manager is not active";
        return {PortalResponse::OtherError, QString()};
    }

    auto captureContext = captureManager->getContext();
    if (!captureContext) {
        return {PortalResponse::OtherError, QString()};
    }
    const auto releaseContext = qScopeGuard([captureManager, captureContext] {
        if (captureManager && captureContext) {
            captureManager->releaseCaptureContext(captureContext);
        }
    });

    QEventLoop loop;
    PortalResponse::Response sourceResponse = PortalResponse::OtherError;
    connect(captureContext, &TreeLandCaptureContext::sourceReady, &loop, [&] {
        sourceResponse = PortalResponse::Success;
        loop.quit();
    });
    connect(captureContext, &TreeLandCaptureContext::sourceFailed, &loop, [&](uint32_t reason) {
        qCWarning(SCREESHOT) << "Treeland source selection failed with reason" << reason;
        sourceResponse = responseFromSourceFailure(reason);
        loop.quit();
    });
    if (m_currentScreenshotRequest) {
        connect(m_currentScreenshotRequest, &Request2::closeRequested, &loop, [this, &loop] {
            m_currentScreenshotCancelled = true;
            loop.quit();
        });
    }
    captureContext->selectSource(QtWayland::treeland_capture_context_v1::source_type_output
                                         | QtWayland::treeland_capture_context_v1::source_type_window
                                         | QtWayland::treeland_capture_context_v1::source_type_region
                                 ,true
                                 , false
                                 ,nullptr);
    loop.exec();
    if (m_currentScreenshotCancelled) {
        return {PortalResponse::Cancelled, QString()};
    }
    if (sourceResponse != PortalResponse::Success) {
        return {sourceResponse, QString()};
    }

    auto frame = captureContext->frame();
    if (!frame || !frame->isInitialized()) {
        qCWarning(SCREESHOT) << "Failed to create Treeland capture frame";
        return {PortalResponse::OtherError, QString()};
    }

    QImage result;
    PortalResponse::Response frameResponse = PortalResponse::OtherError;
    connect(frame, &TreeLandCaptureFrame::ready, this, [this, &result, &frameResponse, &loop](QImage image) {
        result = image;
        frameResponse = PortalResponse::Success;
        loop.quit();
    });
    connect(frame, &TreeLandCaptureFrame::failed, &loop, [&] {
        qCWarning(SCREESHOT) << "Treeland capture frame failed";
        frameResponse = PortalResponse::OtherError;
        loop.quit();
    });
    if (m_currentScreenshotRequest) {
        connect(m_currentScreenshotRequest, &Request2::closeRequested, &loop, [this, &loop] {
            m_currentScreenshotCancelled = true;
            loop.quit();
        });
    }
    loop.exec();
    if (m_currentScreenshotCancelled) {
        return {PortalResponse::Cancelled, QString()};
    }
    if (frameResponse != PortalResponse::Success) {
        return {frameResponse, QString()};
    }
    if (result.isNull()) return {PortalResponse::OtherError, QString()};
    auto saveBasePath = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    QDir saveBaseDir(saveBasePath);
    if (!saveBaseDir.exists()) return {PortalResponse::OtherError, QString()};
    QString picName = "portal screenshot - " + QDateTime::currentDateTime().toString() + ".png";
    if (result.save(saveBaseDir.absoluteFilePath(picName), "PNG")) {
        return {PortalResponse::Success, saveBaseDir.absoluteFilePath(picName)};
    } else {
        return {PortalResponse::OtherError, QString()};
    }
}

uint ScreenshotPortalWayland::Screenshot(const QDBusObjectPath &handle,
                                  const QString &app_id,
                                  const QString &parent_window,
                                  const QVariantMap &options,
                                  QVariantMap &results)
{
    uint response = PortalResponse::Success;

    if (m_screenshotInProgress) {
        qCWarning(SCREESHOT) << "Rejecting concurrent screenshot request";
        return PortalResponse::OtherError;
    }
    m_screenshotInProgress = true;
    m_currentScreenshotCancelled = false;

    m_currentScreenshotRequest = new Request2(handle, this, QStringLiteral("Screenshot"));
    const auto cleanup = qScopeGuard([this] {
        if (m_currentScreenshotRequest) {
            m_currentScreenshotRequest->deleteLater();
        }
        m_currentScreenshotRequest.clear();
        m_currentScreenshotCancelled = false;
        m_screenshotInProgress = false;
    });

    if (options["modal"].toBool()) {
        // TODO if modal, we should block parent_window
    }
    CaptureResult captureResult;
    if (options["interactive"].toBool()) {
        captureResult = captureInteractively();
    } else {
        captureResult = fullScreenShot();
    }
    response = captureResult.response;
    if (response == PortalResponse::Success && captureResult.filePath.isEmpty()) {
        response = PortalResponse::OtherError;
    }

    if (response == PortalResponse::Success) {
        results.insert(QStringLiteral("uri"), QUrl::fromLocalFile(captureResult.filePath).toString(QUrl::FullyEncoded));
    } else {
        results.clear();
    }
    return response;
}
