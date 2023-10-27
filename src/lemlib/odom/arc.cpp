#include "lemlib/util.hpp"
#include "lemlib/timer.hpp"
#include "lemlib/logger/logger.hpp"
#include "lemlib/odom/arc.hpp"

namespace lemlib {
/**
 * Construct a odometry through tracking arcs
 *
 * This can use vertical tracking wheels, horizontal tracking wheels, and imus.
 * Not all of them are needed however. For example, if you have 2 parallel tracking
 * wheels, you don't need an imu. If you have good traction wheels, you also dont need
 * any horizontal wheels.
 *
 * Vectors are passed since they can have a varying number of sensors in them, allowing for
 * any tracking wheel + imu setup
 */
ArcOdom::ArcOdom(std::vector<TrackingWheel>& verticals, std::vector<TrackingWheel>& horizontals,
                 std::vector<std::shared_ptr<Gyro>>& gyros)
    : verticals(verticals),
      horizontals(horizontals),
      gyros(gyros) {}

/**
 * Calibrate the sensors
 *
 * We have to calibrate tracking wheels and imus. We calibrate them all and remove any that fail
 * calibration. The encoders will output errors if they fail to calibrate.
 */
void ArcOdom::calibrate(bool calibrateGyros) {
    // calibrate vertical tracking wheels
    for (auto it = verticals.begin(); it != verticals.end(); it++) {
        if (it->reset()) {
            infoSink()->warn("Vertical tracker at offset {} failed calibration!", it->getOffset());
            verticals.erase(it);
        }
    }

    // calibrate horizontal tracking wheels
    for (auto it = horizontals.begin(); it != horizontals.end(); it++) {
        if (it->reset()) {
            infoSink()->warn("Horizontal tracker at offset {} failed calibration!", it->getOffset());
            horizontals.erase(it);
        }
    }

    // calibrate gyros
    if (calibrateGyros) {
        for (auto& it : gyros) it->calibrate();
        Timer timer(3000); // try calibrating gyros for 3000 ms
        while (!timer.isDone()) {
            for (auto& gyro : gyros) { // continuously calibrate in case of failure
                if (!gyro->isCalibrating() && !gyro->isCalibrated()) gyro->calibrate();
            }
            pros::delay(10);
        }

        // if a gyro failed to calibrate, output an error and erase the gyro
        for (auto it = gyros.begin(); it != gyros.end(); it++) {
            if (!(**it).isCalibrated()) {
                infoSink()->warn("IMU on port {} failed to calibrate! Removing", (**it).getPort());
                gyros.erase(it);
            }
        }
    }
}

/**
 * @brief Calculate the change in heading given 2 tracking wheels
 *
 * @note positive change in counterclockwise
 *
 * @param tracker1 the first tracking wheel
 * @param tracker2 the second tracking wheel
 * @return float change in angle, in radians
 */
float calcDeltaTheta(TrackingWheel& tracker1, TrackingWheel& tracker2) {
    const float numerator = tracker1.getDistanceDelta(false) - tracker2.getDistanceDelta(false);
    const float denominator = tracker1.getOffset() - tracker2.getOffset();
    return numerator / denominator;
}

/**
 * @brief Calculate the change in heading given a vector of imus
 *
 * @note positive change in counterclockwise
 *
 * @param gyros vector of Gyro shared pointers
 * @return float the average change in heading
 */
float calcDeltaTheta(std::vector<std::shared_ptr<Gyro>>& gyros) {
    float deltaTheta = 0;
    for (const auto& gyro : gyros) deltaTheta += gyro->getRotationDelta() / gyros.size();
    return deltaTheta;
}

/**
 * Update Arc Odom
 *
 * Tracking through arcs works through estimating the robot's change in position between
 * angles as an arc, rather than a straight line, improving accuracy.
 *
 * This alg can either use tracking wheels to calculate angle, or preferably an imu.
 * Theoretically you can get better performance with tracking wheels, but this is very
 * difficult to achieve.
 *
 * 5225A has published a fantastic paper on this odom algorithm:
 * http://thepilons.ca/wp-content/uploads/2018/10/Tracking.pdf
 */
void ArcOdom::update() {
    // calculate theta
    // Priority:
    // 1. IMU
    // 2. Horizontal tracking wheels
    // 3. Vertical tracking wheels
    float theta = pose.theta;
    if (gyros.size() > 0) { // calculate heading with imus if we have enough
        theta += calcDeltaTheta(gyros);
    } else if (horizontals.size() > 1) { // calculate heading with horizontal tracking wheels if we have enough
        theta += calcDeltaTheta(horizontals.at(0), horizontals.at(1));
    } else if (verticals.size() > 1) { // calculate heading with vertical tracking wheels if we have enough
        theta += calcDeltaTheta(verticals.at(0), verticals.at(1));
    } else {
        infoSink()->error("Odom calculation failure! Not enough sensors to calculate heading");
        return;
    }
    const float deltaTheta = theta - pose.theta;
    const float avgTheta = pose.theta + deltaTheta / 2;

    // calculate local change in position
    Pose local(0, 0, deltaTheta);
    // set sinDTheta2 to 1 if deltaTheta is 0. Simplifies local position calculations.
    const float sinDTheta2 = (deltaTheta == 0) ? 1 : 2 * std::sin(deltaTheta / 2);
    // calculate local x position
    for (auto& tracker : horizontals) {
        // prevent divide by 0
        const float radius = (deltaTheta == 0) ? tracker.getDistanceDelta()
                                               : tracker.getDistanceDelta() / deltaTheta + tracker.getOffset();
        local.x += sinDTheta2 * radius / horizontals.size();
    }
    // calculate local y position
    for (auto& tracker : verticals) {
        // prevent divide by 0
        const float radius = (deltaTheta == 0) ? tracker.getDistanceDelta()
                                               : tracker.getDistanceDelta() / deltaTheta + tracker.getOffset();
        local.y += sinDTheta2 * radius / horizontals.size();
    }
    if (verticals.empty()) infoSink()->warn("No vertical tracking wheels! Assuming y movement is 0");

    // calculate global position
    pose += local.rotate(avgTheta);
}
}; // namespace lemlib