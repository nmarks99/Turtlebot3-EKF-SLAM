#include "turtlelib/kalman.hpp"

namespace turtlelib
{

    std::mt19937 &get_random()
    {
        // Credit Matt Elwin: https://nu-msr.github.io/navigation_site/lectures/gaussian.html

        // static variables inside a function are created once and persist for the remainder of the program
        static std::random_device rd{};
        static std::mt19937 mt{rd()};
        // we return a reference to the pseudo-random number genrator object. This is always the
        // same object every time get_random is called
        return mt;
    }

    LandmarkMeasurement::LandmarkMeasurement()
        : r(0.0), phi(0.0), marker_id(0) {}

    LandmarkMeasurement::LandmarkMeasurement(double _r, double _phi, int _marker_id)
        : r(_r), phi(_phi), marker_id(_marker_id) {}

    LandmarkMeasurement LandmarkMeasurement::from_cartesian(double _x, double _y, int _marker_id)
    {
        LandmarkMeasurement measurement;
        measurement.r = std::sqrt(std::pow(_x, 2.0) + std::pow(_y, 2.0));
        measurement.phi = normalize_angle(std::atan2(_y, _x));
        measurement.marker_id = _marker_id;
        return measurement;
    }

    arma::mat LandmarkMeasurement::to_mat() const
    {
        arma::mat z(2, 1, arma::fill::zeros);
        z(0, 0) = r;
        z(1, 0) = normalize_angle(phi);
        return z;
    }

    KalmanFilter::KalmanFilter()
        : qt_hat(arma::mat{3, 1, arma::fill::zeros}),
          Xi_hat(qt_hat),
          sigma_hat(arma::mat{3, 3, arma::fill::zeros}),
          Q_mat(arma::mat{3, 3, arma::fill::zeros})
    {
    }

    void KalmanFilter::predict(const Twist2D &V)
    {
        // Note for the prediction step here, we set the noise
        // equal to zero and the map stays stationary

        arma::mat A_t(3, 3, arma::fill::zeros);
        arma::mat qt_hat_new(3, 1, arma::fill::zeros);

        // zero rotational velocity
        if (almost_equal(V.thetadot, 0.0))
        {
            qt_hat_new(0, 0) = 0.0;
            qt_hat_new(1, 0) = Xi_hat(1, 0) + V.xdot * std::cos(Xi_hat(0, 0));
            qt_hat_new(2, 0) = Xi_hat(2, 0) + V.xdot * std::sin(Xi_hat(0, 0));

            A_t(1, 0) = -V.xdot * std::sin(Xi_hat(0, 0));
            A_t(2, 0) = V.xdot * std::cos(Xi_hat(0, 0));
        }
        else // non-zero rotational velocity
        {    // may need to do normalize_angle()
            qt_hat_new(0, 0) = normalize_angle(Xi_hat(0, 0) + V.thetadot);
            qt_hat_new(1, 0) = Xi_hat(1, 0) -
                               (V.xdot / V.thetadot) * std::sin(Xi_hat(0, 0)) +
                               (V.xdot / V.thetadot) * std::sin(Xi_hat(0, 0) + V.thetadot);
            qt_hat_new(2, 0) = Xi_hat(2, 0) +
                               (V.xdot / V.thetadot) * std::cos(Xi_hat(0, 0)) -
                               (V.xdot / V.thetadot) * std::cos(Xi_hat(0, 0) + V.thetadot);

            A_t(1, 0) = -(V.xdot / V.thetadot) * std::cos(Xi_hat(0, 0)) +
                        (V.xdot / V.thetadot) * std::cos(Xi_hat(0, 0) + V.thetadot);
            A_t(2, 0) = -(V.xdot / V.thetadot) * std::sin(Xi_hat(0, 0)) +
                        (V.xdot / V.thetadot) * std::sin(Xi_hat(0, 0) + V.thetadot);
        }
        A_t = arma::eye(arma::size(A_t)) + A_t;

        // Save the new prediction of the robot's configuration
        qt_hat = qt_hat_new;

        // Now we propagate the uncertainty using the linear state transition model
        sigma_hat = (A_t * sigma_hat * A_t.t()) + Q_mat;
    }

    void KalmanFilter::update_measurements(const LandmarkMeasurement &measurement)
    {

        auto mx_j = 0.0;
        auto my_j = 0.0;
        auto mt_j = arma::mat(2, 1, arma::fill::zeros);

        // Landmark must be initialized if it hasn't been seen
        if (not landmarks_dict.count(measurement.marker_id))
        {
            // add new landmark, converting to (x,y) from (r,phi)
            mx_j = Xi_hat(1, 0) +
                   measurement.r * std::cos(normalize_angle(measurement.phi + Xi_hat(0, 0)));
            my_j = Xi_hat(2, 0) +
                   measurement.r * std::sin(normalize_angle(measurement.phi + Xi_hat(0, 0)));
            mt_j = arma::mat{mx_j, my_j}.t();

            // Update the complete state estimate by adding in the new mt_j vector
            Xi_hat = arma::join_cols(Xi_hat, mt_j);

            // store the index of the x_j component for this landmark
            landmarks_dict[measurement.marker_id] = Xi_hat.n_rows;

        } // else, Xi_hat (and therefore mt_hat) gets updated in the EKF update step

        // Update dimensions of the covariance matrix Sigma

        // Update the dimensions of the process noise matrix Q
    }

    void KalmanFilter::update(const std::vector<LandmarkMeasurement> &measurements)
    {
        // for each measurement
        for (size_t i = 0; i < measurements.size(); i++)
        {
            // First you must associate incoming measurements with a landmark
            update_measurements(measurements.at(i));

            // 1. Compute theoretical measurement z_t_hat = h_j
            const auto index = landmarks_dict[measurements.at(i).marker_id];
            const auto mj = arma::mat{Xi_hat(index, 0), Xi_hat(index + 1, 0)};
            const auto r_j = std::sqrt(
                std::pow((mj(0, 0) - Xi_hat(1, 0)), 2.0) +
                std::pow((mj(1, 0) - Xi_hat(2, 0)), 2.0));
            const auto phi_j = normalize_angle(
                std::atan2(mj(1, 0) - Xi_hat(2, 0), mj(0, 0) - Xi_hat(1, 0)) - Xi_hat(0, 0));

            const auto h = arma::mat{r_j, phi_j};

            // 2. Compute the Kalman gain (Eq 26)

            // 3. Compute the posterior state update Xi_t_hat
            // 4. Compute the posterior covariance sigma_t
        }
    }

    arma::mat KalmanFilter::pose_prediction() const
    {
        return qt_hat;
    }

    arma::mat KalmanFilter::map_prediction() const
    {
        return mt_hat;
    }

    arma::mat KalmanFilter::state_prediction() const
    {
        return Xi_hat;
    }

}