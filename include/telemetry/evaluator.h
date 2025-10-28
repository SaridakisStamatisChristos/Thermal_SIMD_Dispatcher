#pragma once

#include <memory>
#include <vector>

#include <telemetry/history_store.h>
#include <telemetry/sensors.h>

namespace telemetry {

class SensorEvaluator {
public:
    using SensorList = std::vector<SensorAdapterPtr>;

    SensorEvaluator(SensorList sensors, std::shared_ptr<HistoryStore> history_store);

    double evaluate_socket(int socket);

private:
    SensorList sensors_;
    std::shared_ptr<HistoryStore> history_store_;
};

} // namespace telemetry

