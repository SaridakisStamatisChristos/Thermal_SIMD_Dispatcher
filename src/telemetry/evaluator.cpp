#include <telemetry/evaluator.h>

#include <algorithm>

namespace telemetry {

SensorEvaluator::SensorEvaluator(SensorList sensors, std::shared_ptr<HistoryStore> history_store)
    : sensors_(std::move(sensors)), history_store_(std::move(history_store)) {}

double SensorEvaluator::evaluate_socket(int socket) {
    double weighted_sum = 0.0;
    double total_weight = 0.0;

    for (const auto &sensor : sensors_) {
        if (!sensor) {
            continue;
        }
        if (!sensor->is_available(socket)) {
            continue;
        }
        SensorSample sample = sensor->sample(socket);
        double health = std::clamp(sample.health, 0.0, 1.0);
        double quality = std::clamp(sample.quality, 0.0, 1.0);
        double weight = health * quality;
        if (weight <= 0.0) {
            continue;
        }
        weighted_sum += sample.value * weight;
        total_weight += weight;
    }

    if (total_weight > 0.0) {
        double aggregated = weighted_sum / total_weight;
        if (history_store_) {
            history_store_->update(socket, aggregated);
        }
        return aggregated;
    }

    if (history_store_) {
        BaselineRecord record = history_store_->get(socket);
        if (record.valid) {
            return record.baseline;
        }
    }

    return 0.0;
}

} // namespace telemetry

