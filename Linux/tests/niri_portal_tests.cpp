#include "opendisplay/niri_portal.hpp"

#include <cassert>
#include <cmath>
#include <stdexcept>
#include <string>

namespace {

void parsesConfiguredVirtualOutput() {
    const auto outputs = od::parseNiriOutputs(R"({"ipad": {
        "name": "ipad", "make": "Unknown", "model": "Virtual", "serial": null,
        "physical_size": null,
        "modes": [
            {"width": 1280, "height": 800, "refresh_rate": 60000, "is_preferred": false},
            {"width": 1920, "height": 1200, "refresh_rate": 59940, "is_preferred": true}
        ],
        "current_mode": 1,
        "logical": {"x": -960, "y": 25, "width": 960, "height": 600,
                    "scale": 2.0, "transform": "Normal"}
    }})");
    assert(outputs.size() == 1);
    const auto& output = outputs.front();
    assert(output.id == "ipad" && output.name == "ipad");
    assert(output.connected && output.enabled);
    assert(output.resolution.width == 1920 && output.resolution.height == 1200);
    assert(std::abs(output.refreshRate - 59.94) < 0.001);
    assert(output.logicalGeometry.x == -960 && output.logicalGeometry.y == 25);
    assert(output.logicalGeometry.width == 960 && output.logicalGeometry.height == 600);
    assert(output.scale == 2.0 && output.transform == 0);
    assert(!output.physicalSize && !output.focused);
    assert(output.modes.size() == 2 && output.modes[1].id == "1");
    assert(output.modes[0].size.width == 1280 && output.modes[0].size.height == 800);
    assert(output.modes[0].refreshRate == 60.0);
}

void parsesDisabledOutput() {
    const auto outputs = od::parseNiriOutputs(R"({"DP-1": {
        "name": "DP-1", "physical_size": [0, 0],
        "modes": [{"width": 1920, "height": 1080, "refresh_rate": 60000}],
        "current_mode": null, "logical": null
    }})");
    assert(outputs.size() == 1);
    const auto& output = outputs.front();
    assert(!output.connected && !output.enabled);
    assert(output.resolution.width == 0 && output.resolution.height == 0);
    assert(output.logicalGeometry.width == 0 && output.logicalGeometry.height == 0);
    assert(output.scale == 1.0 && !output.physicalSize);
    assert(output.modes.size() == 1);
}

void preservesLogicalGeometryAndScaleWithoutPhysicalHeuristics() {
    const auto outputs = od::parseNiriOutputs(R"({"DP-2": {
        "physical_size": [300, 200],
        "modes": [{"width": 1500, "height": 900, "refresh_rate": 75000}],
        "current_mode": 0,
        "logical": {"x": 0, "y": -1200, "width": 720, "height": 1200,
                    "scale": 1.25, "transform": "90"}
    }})");
    const auto& output = outputs.front();
    assert(output.name == "DP-2" && output.id == "DP-2");
    assert(output.scale == 1.25 && output.transform == 1);
    assert(output.resolution.width == 1500 && output.resolution.height == 900);
    assert(output.logicalGeometry.width == 720 && output.logicalGeometry.height == 1200);
    assert(output.logicalGeometry.y == -1200 && !output.physicalSize);
    assert(output.refreshRate == 75.0);
}

void toleratesIncompleteOutputs() {
    const auto outputs = od::parseNiriOutputs(R"({"ipad": {
        "modes": [{"width": 1920, "height": 1200}], "current_mode": 99,
        "logical": {"scale": 0, "transform": "FutureTransform"}
    }, "unavailable": null})");
    assert(outputs.size() == 1);
    assert(outputs.front().connected && outputs.front().enabled);
    assert(outputs.front().resolution.width == 0);
    assert(outputs.front().scale == 1.0 && outputs.front().transform == 0);
    const auto missing = od::parseNiriOutputs(R"({"ipad": {}})");
    assert(!missing.front().enabled && missing.front().modes.empty());
    assert(od::parseNiriOutputs("{}").empty());
}

void rejectsInvalidJson() {
    for (const auto* json : {"not json", "[]", "null"}) {
        bool rejected = false;
        try {
            od::parseNiriOutputs(json);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected);
    }
}

}  // namespace

int main() {
    parsesConfiguredVirtualOutput();
    parsesDisabledOutput();
    preservesLogicalGeometryAndScaleWithoutPhysicalHeuristics();
    toleratesIncompleteOutputs();
    rejectsInvalidJson();
}
