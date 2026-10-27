#include "opendisplay/niri_portal.hpp"

#include "opendisplay/log.hpp"

#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QVariant>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace od {
namespace {

constexpr auto screenCastService = "org.gnome.Mutter.ScreenCast";
constexpr auto screenCastPath = "/org/gnome/Mutter/ScreenCast";
constexpr auto sessionInterface = "org.gnome.Mutter.ScreenCast.Session";
constexpr auto streamInterface = "org.gnome.Mutter.ScreenCast.Stream";

QDBusMessage call(const QDBusConnection& bus, const QString& path,
                   const QString& interface, const QString& method,
                   const QVariantList& arguments = {}) {
    auto message = QDBusMessage::createMethodCall(screenCastService, path, interface, method);
    message.setArguments(arguments);
    const auto reply = bus.call(message, QDBus::Block, 30'000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        throw std::runtime_error((method + QStringLiteral(" failed: ")
                                  + reply.errorMessage()).toStdString());
    }
    return reply;
}

QString objectPath(const QDBusMessage& reply) {
    if (!reply.arguments().isEmpty()) {
        const auto path = reply.arguments().front().value<QDBusObjectPath>().path();
        if (!path.isEmpty() && path != QStringLiteral("/")) return path;
    }
    throw std::runtime_error("Mutter ScreenCast did not return an object path");
}

}  // namespace

std::vector<DisplayOutput> parseNiriOutputs(const std::string_view json) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(json.data(), static_cast<qsizetype>(json.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        throw std::runtime_error("niri returned invalid output JSON: "
                                 + error.errorString().toStdString());
    }
    std::vector<DisplayOutput> outputs;
    const auto nativeOutputs = document.object();
    for (auto it = nativeOutputs.begin(); it != nativeOutputs.end(); ++it) {
        if (!it.value().isObject()) continue;
        const auto native = it.value().toObject();
        DisplayOutput output;
        output.name = native.value(QStringLiteral("name")).toString(it.key()).toStdString();
        output.id = output.name;
        const auto modes = native.value(QStringLiteral("modes")).toArray();
        for (qsizetype index = 0; index < modes.size(); ++index) {
            const auto mode = modes.at(index).toObject();
            output.modes.push_back({
                .id = QString::number(index).toStdString(),
                .size = {.width = mode.value(QStringLiteral("width")).toInt(),
                         .height = mode.value(QStringLiteral("height")).toInt()},
                .refreshRate = mode.value(QStringLiteral("refresh_rate")).toDouble() / 1000.0,
            });
        }
        const auto current = native.value(QStringLiteral("current_mode"));
        const int index = current.toInt(-1);
        if (current.isDouble() && current.toDouble() == index
            && index >= 0 && index < modes.size()) {
            output.resolution = output.modes[static_cast<std::size_t>(index)].size;
            output.refreshRate = output.modes[static_cast<std::size_t>(index)].refreshRate;
        }
        const auto logicalValue = native.value(QStringLiteral("logical"));
        output.connected = logicalValue.isObject();
        output.enabled = output.connected;
        if (output.enabled) {
            const auto logical = logicalValue.toObject();
            output.logicalGeometry = {
                .x = logical.value(QStringLiteral("x")).toInt(),
                .y = logical.value(QStringLiteral("y")).toInt(),
                .width = logical.value(QStringLiteral("width")).toInt(),
                .height = logical.value(QStringLiteral("height")).toInt(),
            };
            output.scale = logical.value(QStringLiteral("scale")).toDouble(1.0);
            if (!std::isfinite(output.scale) || output.scale <= 0) output.scale = 1.0;
            const auto transform = logical.value(QStringLiteral("transform")).toString();
            const QStringList transforms = {QStringLiteral("Normal"), QStringLiteral("90"),
                QStringLiteral("180"), QStringLiteral("270"), QStringLiteral("Flipped"),
                QStringLiteral("Flipped90"), QStringLiteral("Flipped180"),
                QStringLiteral("Flipped270")};
            const auto transformIndex = transforms.indexOf(transform);
            if (transformIndex >= 0) output.transform = static_cast<int>(transformIndex);
        }
        // Physical size is deliberately unused: virtual outputs report null or zero.
        outputs.push_back(std::move(output));
    }
    return outputs;
}

NiriPortal::NiriPortal(QObject* parent)
    : QObject(parent), bus_(QDBusConnection::sessionBus()) {
    if (!bus_.isConnected()) {
        throw std::runtime_error("cannot connect to the D-Bus session bus");
    }
}

NiriPortal::~NiriPortal() { stop(); }

std::vector<DisplayOutput> NiriPortal::outputs() const {
    QProcess process;
    process.start(QStringLiteral("niri"),
                  {QStringLiteral("msg"), QStringLiteral("--json"), QStringLiteral("outputs")});
    if (!process.waitForStarted(3'000)) {
        throw std::runtime_error("cannot start niri: " + process.errorString().toStdString());
    }
    if (!process.waitForFinished(5'000)) {
        process.kill();
        process.waitForFinished();
        throw std::runtime_error("niri output query timed out");
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        throw std::runtime_error("cannot query niri outputs: "
                                 + process.readAllStandardError().toStdString());
    }
    return parseNiriOutputs(process.readAllStandardOutput().toStdString());
}

PortalCapture NiriPortal::start(const DesktopRequest& request) {
    stop();
    if (request.mode != CaptureMode::Mirror) {
        throw std::runtime_error(
            "the niri backend supports only --mode mirror; configure the output in config.kdl");
    }
    try {
        const auto output = selectReferenceOutput(outputs(), request.display.referenceMonitor);
        if (output.resolution.width <= 0 || output.resolution.height <= 0
            || output.logicalGeometry.width <= 0 || output.logicalGeometry.height <= 0) {
            throw std::runtime_error("niri output has no valid current mode or logical geometry");
        }
        sessionPath_ = objectPath(call(bus_, screenCastPath, screenCastService,
                                       QStringLiteral("CreateSession"), {QVariantMap{}}));
        QVariantMap properties;
        properties.insert(QStringLiteral("cursor-mode"), 1U);  // Embedded
        streamPath_ = objectPath(call(bus_, sessionPath_, sessionInterface,
            QStringLiteral("RecordMonitor"), {QString::fromStdString(output.name), properties}));
        if (!bus_.connect(screenCastService, streamPath_, streamInterface,
                          QStringLiteral("PipeWireStreamAdded"), this,
                          SLOT(pipeWireStreamAdded(uint)))) {
            throw std::runtime_error("cannot subscribe to Mutter PipeWireStreamAdded");
        }
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(this, &NiriPortal::streamReady, &loop, &QEventLoop::quit);
        call(bus_, sessionPath_, sessionInterface, QStringLiteral("Start"));
        timeout.start(30'000);
        if (!nodeId_) loop.exec();
        if (!nodeId_) {
            throw std::runtime_error("timed out waiting for Mutter PipeWire stream");
        }
        if (request.requestInput) {
            pointer_.start(output.name);
            inputEnabled_ = true;
        }
        log("niri ScreenCast session ready; PipeWire node " + std::to_string(*nodeId_));
        return {
            .sessionPath = sessionPath_.toStdString(),
            .stream = {.nodeId = *nodeId_,
                       .logicalWidth = output.logicalGeometry.width,
                       .logicalHeight = output.logicalGeometry.height},
            .pipewireFd = -1,
            .captureWidth = output.resolution.width,
            .captureHeight = output.resolution.height,
        };
    } catch (...) {
        stop();
        throw;
    }
}

void NiriPortal::stop() {
    inputEnabled_ = false;
    pointer_.stop();
    if (!streamPath_.isEmpty()) {
        bus_.disconnect(screenCastService, streamPath_, streamInterface,
                        QStringLiteral("PipeWireStreamAdded"), this,
                        SLOT(pipeWireStreamAdded(uint)));
        streamPath_.clear();
    }
    if (!sessionPath_.isEmpty()) {
        // Mutter sessions use Stop, rather than the portal Session.Close method.
        try {
            call(bus_, sessionPath_, sessionInterface, QStringLiteral("Stop"));
        } catch (const std::exception& error) {
            debug(std::string("Cannot stop niri ScreenCast session: ") + error.what());
        }
        sessionPath_.clear();
    }
    nodeId_.reset();
}

void NiriPortal::pipeWireStreamAdded(const uint nodeId) {
    nodeId_ = nodeId;
    emit streamReady();
}

void NiriPortal::pointer(const std::string_view phase, const double normalizedX,
                          const double normalizedY) {
    if (inputEnabled_) pointer_.pointer(phase, normalizedX, normalizedY);
}

void NiriPortal::scroll(const double dx, const double dy) {
    if (inputEnabled_) pointer_.scroll(dx, dy);
}

}  // namespace od
