#pragma once

#include "opendisplay/desktop_backend.hpp"
#include "opendisplay/display_layout.hpp"
#include "opendisplay/wayland_virtual_pointer.hpp"

#include <QDBusConnection>
#include <QObject>
#include <QString>

#include <optional>
#include <vector>

namespace od {

std::vector<DisplayOutput> parseNiriOutputs(std::string_view json);

class NiriPortal final : public QObject, public DesktopBackend {
    Q_OBJECT

public:
    explicit NiriPortal(QObject* parent = nullptr);
    ~NiriPortal() override;

    std::vector<DisplayOutput> outputs() const;
    PortalCapture start(const DesktopRequest& request) override;
    void stop() override;
    void pointer(std::string_view phase, double normalizedX, double normalizedY) override;
    void scroll(double dx, double dy) override;

signals:
    void streamReady();

private slots:
    void pipeWireStreamAdded(uint nodeId);

private:
    QDBusConnection bus_;
    WaylandVirtualPointer pointer_;
    QString sessionPath_;
    QString streamPath_;
    std::optional<std::uint32_t> nodeId_;
    bool inputEnabled_ = false;
};

}  // namespace od
