/// \file
/// \brief nusim node: a turtlebot3 simulation program
///
/// PARAMETERS:
///     x0 (double): starting x location of the turtlebot in the simulator
///     y0 (double): starting y location of the turtlebot in the simulator
///     theta0 (double): starting yaw angle of the turtlebot in the simulator
///     obstacles/x (std::vector<double>): Array of x locations of obstacles
///     obstacles/y (std::vector<double>): Array of y locations of obstacles
///     obstacles/r (double): Radius of the obtacles
/// PUBLISHES:
///     ~/timestep (std_msgs/msg/UInt64): simulation timestep
///     ~/obstacles (visualization_msgs/msg/MarkerArray): array of Marker messages
///		  /red/sensor_data (nuturtlebot_msgs/msg/SensorData): wheel encoder values
/// SUBSCRIBES:
///     /red/wheel_cmd (nuturtlebot_msgs/msg/WheelCommands): integer valued wheel command speeds
/// SERVERS:
///     ~/reset (std_srvs/srv/Empty): resets the simulation timestep and the robot to its initial pose
///     ~/teleport (nusim/srv/Teleport): teleports the robot to a specified pose
/// CLIENTS:
///     None

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <random>

#include "rclcpp/rclcpp.hpp"
#include <rclcpp/logging.hpp>

#include "std_msgs/msg/u_int64.hpp"
#include "std_msgs/msg/string.hpp"
#include "std_srvs/srv/empty.hpp"
#include "nusim/srv/teleport.hpp"
#include "nuturtlebot_msgs/msg/sensor_data.hpp"
#include "nuturtlebot_msgs/msg/wheel_commands.hpp"

#include "geometry_msgs/msg/transform_stamped.hpp"
#include "geometry_msgs/msg/pose2_d.hpp"
#include "nav_msgs/msg/path.hpp"

#include "turtlelib/diff_drive.hpp"

#include "tf2/LinearMath/Quaternion.h"
#include "tf2_ros/transform_broadcaster.h"

#include "nusim/utils.hpp"

using namespace std::chrono_literals;
using std::placeholders::_1;
using std::placeholders::_2;

/// \brief nusim turtlebot simulation node
class Nusim : public rclcpp::Node
{

public:
	Nusim()
		: Node("nusim"), step(0)
	{

		// Declare parameters
		declare_parameter<int>("rate", RATE);
		declare_parameter<double>("x0", X0);
		declare_parameter<double>("y0", Y0);
		declare_parameter<double>("theta0", THETA0);
		declare_parameter<std::vector<double>>("obstacles/x", obstacles_x);
		declare_parameter<std::vector<double>>("obstacles/y", obstacles_y);
		declare_parameter<double>("obstacles/r", obstacles_r);
		declare_parameter<double>("motor_cmd_per_rad_sec", MOTOR_CMD_PER_RAD_SEC);
		declare_parameter<int>("motor_cmd_max", MOTOR_CMD_MAX);
		declare_parameter<double>("encoder_ticks_per_rad", ENCODER_TICKS_PER_RAD);
		declare_parameter<double>("wall_x_length", X_LENGTH);
		declare_parameter<double>("wall_y_length", Y_LENGTH);
		declare_parameter<double>("input_noise", INPUT_NOISE);
		declare_parameter<double>("slip_fraction", SLIP_FRACTION);
		declare_parameter<double>("basic_sensor_variance", BASIC_SENSOR_VARIANCE);
		declare_parameter<double>("max_range", MAX_RANGE);
		declare_parameter<double>("collision_radius", COLLISION_RADIUS);

		// Get parameters
		obstacles_r = get_parameter("obstacles/r").get_value<double>();
		obstacles_x = get_parameter("obstacles/x").get_value<std::vector<double>>();
		obstacles_y = get_parameter("obstacles/y").get_value<std::vector<double>>();
		assert(obstacles_x.size() == obstacles_y.size());
		MOTOR_CMD_PER_RAD_SEC = get_parameter("motor_cmd_per_rad_sec").get_value<double>();
		MOTOR_CMD_MAX = get_parameter("motor_cmd_max").get_value<int>();
		ENCODER_TICKS_PER_RAD = get_parameter("encoder_ticks_per_rad").get_value<double>();
		X0 = get_parameter("x0").get_value<double>();
		Y0 = get_parameter("y0").get_value<double>();
		THETA0 = get_parameter("theta0").get_value<double>();
		RATE = get_parameter("rate").get_value<int>();
		INPUT_NOISE = get_parameter("input_noise").get_value<double>();
		SLIP_FRACTION = get_parameter("slip_fraction").get_value<double>();
		BASIC_SENSOR_VARIANCE = get_parameter("basic_sensor_variance").get_value<double>();
		COLLISION_RADIUS = get_parameter("collision_radius").get_value<double>();

		// Check for required parameters
		if (turtlelib::almost_equal(MOTOR_CMD_PER_RAD_SEC, 0.0))
		{
			RCLCPP_ERROR_STREAM(get_logger(), "motor_cmd_per_rad_sec parameter missing");
			throw std::runtime_error("motor_cmd_per_rad_sec parameter missing");
		}

		if (MOTOR_CMD_MAX == 0)
		{
			RCLCPP_ERROR_STREAM(get_logger(), "motor_cmd_max parameter missing");
			throw std::runtime_error("motor_cmd_max parameter missing");
		}

		if (turtlelib::almost_equal(ENCODER_TICKS_PER_RAD, 0.0))
		{
			RCLCPP_ERROR_STREAM(get_logger(), "encoder_ticks_per_rad parameter missing");
			throw std::runtime_error("encoder_ticks_per_rad parameter missing");
		}

		/// @brief timestep publisher (std_msgs/msg/UInt64)
		timestep_pub = create_publisher<std_msgs::msg::UInt64>("~/timestep", 10);

		/// @brief marker publisher (visualization_msgs/msg/MarkerArray)
		marker_arr_pub = create_publisher<visualization_msgs::msg::MarkerArray>(
			"~/obstacles", 10);

		/// @brief marker publisher (visualization_msgs/msg/MarkerArray)
		fake_sensor_marker_arr_pub = create_publisher<visualization_msgs::msg::MarkerArray>(
			"/fake_sensor", 10);

		/// @brief red/sensor data publisher which publishes the encoder ticks of the wheels
		sensor_data_pub = create_publisher<nuturtlebot_msgs::msg::SensorData>(
			"red/sensor_data", 10);

		/// @brief publishes the path (nav_msgs/Path)
		path_pub = create_publisher<nav_msgs::msg::Path>(
			"/nusim/path", 10);

		/// @brief subscription ot wheel_cmd to get the commanded
		/// integer values which detemines the wheel velocities
		wheel_cmd_sub = create_subscription<nuturtlebot_msgs::msg::WheelCommands>(
			"red/wheel_cmd",
			10,
			std::bind(&Nusim::wheel_cmd_callback, this, _1));

		/// \brief ~/reset service (std_srvs/srv/Empty)
		/// resets the timestep variable to 0 and resets the turtlebot
		/// pose to its initial location
		_reset_service = create_service<std_srvs::srv::Empty>(
			"~/reset",
			std::bind(&Nusim::reset_callback, this, _1, _2));

		/// \brief ~/teleport service (nusim/srv/Teleport)
		/// teleports the robot in the simulation to the specified pose
		_teleport_service = create_service<nusim::srv::Teleport>(
			"~/teleport",
			std::bind(&Nusim::teleport_callback, this, _1, _2));

		/// \brief transform broadcaster:
		/// used to publish transform on the /tf topic
		tf_broadcaster = std::make_unique<tf2_ros::TransformBroadcaster>(*this);

		/// \brief Timer (frequency defined by node parameter)
		_timer = create_wall_timer(
			std::chrono::milliseconds((int)(1000 / RATE)),
			std::bind(&Nusim::timer_callback, this));

		_fake_sensor_timer = create_wall_timer(
			200ms,
			std::bind(&Nusim::fake_sensor_timer_callback, this));

		// Ground truth pose of the robot known only to the simulator
		// Initial values are passed as parameters to the node
		true_pose.x = X0;
		true_pose.y = Y0;
		true_pose.theta = THETA0;

		// fill in MarkerArray with obstacles and walls
		fill_obstacles(marker_arr, obstacles_x, obstacles_y, obstacles_r);
		fill_walls(marker_arr, X_LENGTH, Y_LENGTH);

		// Define parent and child frame id's
		world_red_tf.header.frame_id = "nusim/world";
		world_red_tf.child_frame_id = "red/base_footprint";

		// Define frame for path message
		path.header.frame_id = "/nusim/world";
	}

private:
	std::vector<double> obstacles_x;
	std::vector<double> obstacles_y;
	double obstacles_r = 0.0;
	double X0 = 0.0;
	double Y0 = 0.0;
	double THETA0 = 0.0;
	int RATE = 200;
	double MOTOR_CMD_PER_RAD_SEC = 0.0;
	double ENCODER_TICKS_PER_RAD = 0.0;
	int MOTOR_CMD_MAX = 0;
	double X_LENGTH = 5.0;
	double Y_LENGTH = 5.0;
	double SLIP_FRACTION = 0.0;
	double INPUT_NOISE = 0.0;
	double BASIC_SENSOR_VARIANCE = 0.001;
	double MAX_RANGE = 1.0; // max basic sensor range
	double COLLISION_RADIUS = 0.105;
	uint64_t step = 0;
	uint64_t count = 0;
	double left_noise = 0.0;
	double right_noise = 0.0;
	double left_slip = 0.0;
	double right_slip = 0.0;

	// Wheel states with noise and slipping
	turtlelib::WheelState noisy_wheel_speeds{0.0, 0.0};
	turtlelib::WheelState slippy_wheel_angles{0.0, 0.0};

	// True wheel states and pose known only to the sim
	turtlelib::WheelState true_wheel_angles{0.0, 0.0};
	turtlelib::WheelState true_wheel_speeds{0.0, 0.0};
	turtlelib::Pose2D true_pose{0.0, 0.0, 0.0};

	// DiffDrive object for forward kinematics and stuff
	turtlelib::DiffDrive ddrive;

	// Quaternion object for updating rotational component of tfs
	tf2::Quaternion q;

	// Publishers
	rclcpp::Publisher<std_msgs::msg::UInt64>::SharedPtr timestep_pub;
	rclcpp::Publisher<visualization_msgs::msg::MarkerArray>::SharedPtr marker_arr_pub;
	rclcpp::Publisher<visualization_msgs::msg::MarkerArray>::SharedPtr fake_sensor_marker_arr_pub;
	rclcpp::Publisher<nuturtlebot_msgs::msg::SensorData>::SharedPtr sensor_data_pub;
	rclcpp::Publisher<nav_msgs::msg::Path>::SharedPtr path_pub;

	// Subscribers
	rclcpp::Subscription<nuturtlebot_msgs::msg::WheelCommands>::SharedPtr wheel_cmd_sub;

	// Timers
	rclcpp::TimerBase::SharedPtr _timer;
	rclcpp::TimerBase::SharedPtr _fake_sensor_timer;

	// Services
	rclcpp::Service<std_srvs::srv::Empty>::SharedPtr _reset_service;
	rclcpp::Service<nusim::srv::Teleport>::SharedPtr _teleport_service;

	// tf broadcaster
	std::unique_ptr<tf2_ros::TransformBroadcaster> tf_broadcaster;

	// Declare messages
	geometry_msgs::msg::TransformStamped world_red_tf;
	nuturtlebot_msgs::msg::SensorData sensor_data;
	visualization_msgs::msg::MarkerArray marker_arr;
	nav_msgs::msg::Path path;

	/// @brief Checks if there is a collision between the robot and an obstacle assuming there
	/// will only ever be a collision with one obstacle at a time and updates the robot pose
	///
	/// Credit to https://flatredball.com/documentation/tutorials/math/circle-collision/ which
	/// which provides a description of similar motion, "Circle Move Collision"
	void detect_collision()
	{
		if (obstacles_x.size() > 0)
		{
			for (size_t i = 0; i < obstacles_x.size(); i++)
			{
				turtlelib::Vector2D p1{obstacles_x.at(i), obstacles_y.at(i)};
				turtlelib::Vector2D p2{true_pose.x, true_pose.y};
				auto d = turtlelib::distance(p1, p2);
				auto dc = d - (obstacles_r + COLLISION_RADIUS);
				if (dc <= 0.0)
				{
					auto collision_angle = std::atan2((true_pose.y - obstacles_y.at(i)), (true_pose.x - obstacles_x.at(i)));
					const auto distance_to_move = obstacles_r + COLLISION_RADIUS;

					// This makes the robot bump into the obstacle and move along the tangent
					// line between the two collision circles
					true_pose.x = obstacles_x.at(i) + std::cos(collision_angle) * distance_to_move;
					true_pose.y = obstacles_y.at(i) + std::sin(collision_angle) * distance_to_move;
				}
			}
		}
	}

	/// @brief /wheel_cmd topic callback function that reads the integer valued
	/// WheelCommands, converts them to speeds in rad/s, with and without noise + slipping
	void wheel_cmd_callback(const nuturtlebot_msgs::msg::WheelCommands &wheel_cmd)
	{
		// Define normal noise distribution with zero mean and input_noise variance
		std::normal_distribution<> left_noise_d(0.0, INPUT_NOISE);
		std::normal_distribution<> right_noise_d(0.0, INPUT_NOISE);

		// Compute wheel speeds (rad/s) from wheel command message with noise
		// only add in noise if wheel cmds are non-zero
		true_wheel_speeds.left = wheel_cmd.left_velocity * MOTOR_CMD_PER_RAD_SEC;
		true_wheel_speeds.right = wheel_cmd.right_velocity * MOTOR_CMD_PER_RAD_SEC;

		if (turtlelib::almost_equal((double)wheel_cmd.left_velocity, 0.0))
		{
			noisy_wheel_speeds.left = true_wheel_speeds.left;
		}
		else
		{
			left_noise = left_noise_d(get_random());
			noisy_wheel_speeds.left = (wheel_cmd.left_velocity * MOTOR_CMD_PER_RAD_SEC) + left_noise;
		}
		if (turtlelib::almost_equal((double)wheel_cmd.right_velocity, 0.0))
		{
			noisy_wheel_speeds.right = true_wheel_speeds.right;
		}
		else
		{
			right_noise = right_noise_d(get_random());
			noisy_wheel_speeds.right = (wheel_cmd.right_velocity * MOTOR_CMD_PER_RAD_SEC) + right_noise;
		}

		if (!turtlelib::almost_equal(SLIP_FRACTION, 0.0))
		{
			std::uniform_real_distribution<> slip_d(-SLIP_FRACTION, SLIP_FRACTION);
			right_slip = slip_d(get_random());
			left_slip = slip_d(get_random());
		}
	}

	/// @brief ~/reset service callback function:
	/// resets the timestep variable to 0 and resets the
	/// turtlebot pose to its initial location
	void reset_callback(
		const std::shared_ptr<std_srvs::srv::Empty::Request>,
		std::shared_ptr<std_srvs::srv::Empty::Response>)
	{

		true_pose.x = X0;
		true_pose.y = Y0;
		true_pose.theta = THETA0;
	}

	/// @brief ~/teleport service callback function:
	/// teleports the robot to the desired pose by setting
	/// true_pose equal to the input x,y,theta
	/// @param request - nusim/srv/Teleport request which has x,y,theta fields (UInt64)
	void teleport_callback(
		const std::shared_ptr<nusim::srv::Teleport::Request> request,
		std::shared_ptr<nusim::srv::Teleport::Response>)
	{
		// Teleports the robot by redefining the current pose
		true_pose.x = request->x;
		true_pose.y = request->y;
		true_pose.theta = request->theta;
	}

	/// @brief timer callback function:
	/// publises the simulation timestep, updates the transform between
	/// the nusim/world and red/base_footprint frames, and published obstacle MarkerArray
	void timer_callback()
	{

		// Compute new wheel angles (rad)
		true_wheel_angles.left = true_wheel_angles.left + true_wheel_speeds.left * (1.0 / RATE);
		true_wheel_angles.right = true_wheel_angles.right + true_wheel_speeds.right * (1.0 / RATE);

		// Compute wheel angles with slipping model
		slippy_wheel_angles.left = slippy_wheel_angles.left + noisy_wheel_speeds.left * (1 + left_slip) * (1.0 / RATE);
		slippy_wheel_angles.right = slippy_wheel_angles.right + noisy_wheel_speeds.right * (1 + right_slip) * (1.0 / RATE);

		// Convert angle to encoder ticks to fill in sensor_data message with noise and slipping
		sensor_data.left_encoder = (int)(slippy_wheel_angles.left * ENCODER_TICKS_PER_RAD);
		sensor_data.right_encoder = (int)(slippy_wheel_angles.right * ENCODER_TICKS_PER_RAD);

		// Use new wheel angles with forward kinematics to obtain new pose of red robot
		true_pose = ddrive.forward_kinematics(true_pose, true_wheel_angles);

		// Check if there is a collision and update pose accordingly
		detect_collision();

		// Publish timestep
		auto timestep_message = std_msgs::msg::UInt64();
		timestep_message.data = step++;
		timestep_pub->publish(timestep_message);

		// Set the translation of the red robot
		world_red_tf.transform.translation.x = true_pose.x;
		world_red_tf.transform.translation.y = true_pose.y;
		world_red_tf.transform.translation.z = 0.0;

		// Set the rotation of the red robot
		q.setRPY(0.0, 0.0, true_pose.theta);
		world_red_tf.transform.rotation.x = q.x();
		world_red_tf.transform.rotation.y = q.y();
		world_red_tf.transform.rotation.z = q.z();
		world_red_tf.transform.rotation.w = q.w();

		auto time_now = get_clock()->now();

		// Stamp and broadcast the transform
		world_red_tf.header.stamp = time_now;
		tf_broadcaster->sendTransform(world_red_tf);

		// Publish MarkerArray of obstacles
		marker_arr_pub->publish(marker_arr);

		// Publish sensor data
		sensor_data_pub->publish(sensor_data);

		// Publish path at a slower rate than the loop
		constexpr int PATH_PUB_RATE = 100;
		if (count >= PATH_PUB_RATE)
		{
			count = 0;
			geometry_msgs::msg::PoseStamped temp_pose;
			temp_pose.header.stamp = time_now;
			temp_pose.pose.position.x = true_pose.x;
			temp_pose.pose.position.y = true_pose.y;
			temp_pose.pose.position.z = 0.0;
			temp_pose.pose.orientation.x = q.x();
			temp_pose.pose.orientation.y = q.y();
			temp_pose.pose.orientation.z = q.z();
			temp_pose.pose.orientation.w = q.w();

			path.header.stamp = time_now;
			path.poses.push_back(temp_pose);
			path_pub->publish(path);
		}
		else
		{
			count++;
		}
	}

	/// @brief timer callback for fake sensor:
	/// publishes a MarkerArray of the "sensed" positions of the obstacles at 5Hz
	void fake_sensor_timer_callback()
	{
		// Publish MarkerArray of fake sensor data
		visualization_msgs::msg::MarkerArray fake_sensor_marker_arr;
		fill_basic_sensor_obstacles(fake_sensor_marker_arr, obstacles_x, obstacles_y,
									obstacles_r, true_pose, MAX_RANGE, BASIC_SENSOR_VARIANCE);

		fake_sensor_marker_arr_pub->publish(fake_sensor_marker_arr);
	}
};

/// @brief the main function to run the nusim node
int main(int argc, char *argv[])
{
	rclcpp::init(argc, argv);
	rclcpp::spin(std::make_shared<Nusim>());
	rclcpp::shutdown();
	return 0;
}
