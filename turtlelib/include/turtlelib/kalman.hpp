#ifndef KALMAN_INCLUDE_GUARD_HPP
#define KALMAN_INCLUDE_GUARD_HPP
/// @file
/// @brief Implemenation of the Exteneded Kalman Filter algorithm

#include <iosfwd>
#include <cstdlib>
#include <vector>
#include <cmath>
#include <iostream>
#include <armadillo>
#include <map>
#include "turtlelib/rigid2d.hpp"
#include "turtlelib/diff_drive.hpp"

namespace turtlelib
{

    /// @brief Each landmark measurement contains a distance r,
    /// bearing (angle) phi, and marker_id (from ROS Marker msg).
    /// Angles are normalized to be in the range (-pi, pi]
    struct LandmarkMeasurement
    {
        double r;
        double phi;
        int marker_id;

        /// @brief Default constructor that initializes variables to zero
        LandmarkMeasurement();

        /// @brief Constructor which defines the r, phi, and marker_id
        /// @param _r range
        /// @param _phi bearing (angle)
        /// @param _marker_id integer id of the marker in the MarkerArray of landmarks
        LandmarkMeasurement(double _r, double _phi, int _marker_id);

        /// @brief Constructor which defines the r, phi, and marker_id
        /// given cartesian coordinates x and y, as well as the marker_id
        /// @param _x x coordinate
        /// @param _y y coordinate
        /// @param _marker_id integer id of the marker in the MarkerArray of landmarks
        static LandmarkMeasurement from_cartesian(double _x, double _y, int _marker_id);

        /// @brief Stores the r and phi values in a 2x1 arma::mat and returns it
        /// @return 2x1 arma::mat [r phi]
        arma::mat to_mat() const;
    };

    class KalmanFilter
    {

    private:
        arma::mat qt_hat;    // 3x1 predicted robot state vector
        arma::mat mt_hat;    // 2xn predicted map state
        arma::mat Xi_hat;    // Full state prediction. [qt_hat mt_hat]
        arma::mat sigma_hat; // covariance matrix
        arma::mat Q_mat;     // process noise matrix

        // map (dictionary) of id:index key value pairs
        // the index is the index of the x_j compont of mt_j, so index+1 is y_j
        std::map<int, int> landmarks_dict;

    public:
        /// @brief class constructor
        KalmanFilter();

        /// @brief takes a measurement and if it hasn't been seen before, initializes it,
        /// and adds it to the set of known landmark measurments.
        void update_measurements(const LandmarkMeasurement &measurement);

        /// @brief extented Kalman filter prediction step which predicts
        /// the new robot state qt_hat at time t. The process noise is zero here.
        /// @param V a Twist2D at time t
        void predict(const Twist2D &V);

        /// @brief extented Kalman filter update step
        void update(const std::vector<LandmarkMeasurement> &measurements);

        /// @brief returns the current pose prediction, qt_hat
        /// @return an arma::mat of the prediction of the robot's current pose
        arma::mat pose_prediction() const;

        /// @brief returns the current map prediction, mt_hat
        /// @return an arma::mat of the prediction of the state of the map
        arma::mat map_prediction() const;

        /// @brief returns the current full state prediction, Xi_hat
        /// @return an arma::mat of the prediction of the full state (robot+map)
        arma::mat state_prediction() const;
    };

}
#endif