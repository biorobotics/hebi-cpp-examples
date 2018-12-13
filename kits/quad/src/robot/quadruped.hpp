#pragma once

#include <memory>
#include <set>
#include <chrono>

#include <Eigen/Dense>

#include "lookup.hpp"
#include "group.hpp"
#include "trajectory.hpp"
#include "group_command.hpp"
#include "group_feedback.hpp"
#include "group_info.hpp"

#include "quadruped_parameters.hpp"
#include "quadruped_leg.hpp"

/* 
  This class defines a quadruped robot using Hebi's daisy robot (or Mat6)
  The idea is to use leg 1 2 5 6 to run and use 3 4 to grab objects
*/

// Leg Numbering / Chassis Coordinate convention
// This should match ROS wheeled vehicle convention
//
//  1 \   / 2         +x
//     \ /            ^
//  3 ----- 4         |
//     / \      +y <--o
//  5 /   \ 6          +z
// different from hexapod, we use 3 4 to manipulate objects

using namespace Eigen;

namespace hebi {

class Quadruped 
{
  public:
    // 1 2 5 6 are locomote legs, 3 4 are manipulate legs
    enum struct CtrlLegType { all, locomote, manipulate };
    enum struct SwingMode {swing_mode_virtualLeg1, swing_mode_virtualLeg2};
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW // Allow Eigen member variables

    // learn from hebi source code to do this fancy construction method
    static std::unique_ptr<Quadruped> create(const QuadrupedParameters& params);
    virtual ~Quadruped() noexcept;

    Eigen::Vector3d getGravityDirection();
    Eigen::VectorXd getLegJointAngles(int index);

    void computeFootForces(Eigen::MatrixXd& foot_forces);

    // these functions are called in outside state machine, they must be called every iteration
    // and, in all these modes they will call sendCommand. Which is not good because we may want to combine 
    // functions in a state, so the cmd will be refreshed twice.
    // I think a better design methdology would either be 
    // 1. make sure outside state machine always only call one and only one
    //    function to enter quadruped, and combine state logics all in this function
    // 2. at state machine, explicitly call sendCommand after call functions that set cmd 
    //  It is just different levels of abstraction, need more thinking 
    bool planStandUpTraj(double duration_time);
    bool execStandUpTraj(double curr_time);
    bool spreadAllLegs();
    bool pushAllLegs(double curr_time, double total_time);
    bool prepareQuadMode();
    void runTest(SwingMode mode, double curr_time, double total_time);
    void prepareTrajectories(SwingMode mode, double leg_swing_time);
    bool reOrient(Matrix3d target_body_R);

    Eigen::Matrix3d getBodyR() {return body_R;}
    void startBodyRUpdate() {updateBodyR = true;}

    bool isExecution() {return is_exec_traj;}

    void setCommand(int index, const VectorXd* angles, const VectorXd* vels, const VectorXd* torques);
    void sendCommand();
    bool setGains();

  private:
    // private constructor, it make sense because before construct must make sure group is successfully created
    Quadruped(std::shared_ptr<Group> group, const QuadrupedParameters& params);

    // private functions
    Eigen::Quaterniond average_quat(Eigen::Quaterniond average_q_, std::vector<Eigen::Quaterniond> q_list_);
    Eigen::Vector3d quat_log(Eigen::Quaterniond q);
    Eigen::Quaterniond quat_exp(Eigen::Vector3d qv);

    // hebi middleware to communicate with real hardware
    std::shared_ptr<Group> group_;
    GroupCommand cmd_;

    // leg info
    std::vector<std::unique_ptr<QuadLeg> > legs_;
    Eigen::VectorXd joint_angles; //get joint angles from fbk and put them into leg

    std::chrono::time_point<std::chrono::steady_clock> latest_fbk_time;
    QuadrupedParameters params_;

    // feedback physical quantities
    Eigen::Vector3d gravity_direction_;
    Eigen::Matrix3d body_R;

    bool updateBodyR = false;

    // two locks to get feedback
    std::mutex fbk_lock_;
    std::mutex grav_lock_;

    // planner trajectories
    std::vector<std::shared_ptr<trajectory::Trajectory>> startup_trajectories;
    std::vector<std::shared_ptr<trajectory::Trajectory>> stance_trajectories;  // used in runTest
    std::vector<std::shared_ptr<trajectory::Trajectory>> swing_trajectories;   // used in runTest
    bool is_exec_traj; // flag to show that it is still running trajectories 

    // control constants
    const float fbk_frq_hz_ = 200.0f;
    const float fbk_frq_ms_ = 5.0f;
    const float ctrl_frq_hz_ = 200.0f;
    // structural constants
    static const int num_legs_ = 6;
    const int num_locomote_legs_ = 4;
    const int num_manipulate_legs_ = 2;
    const int num_joints_per_leg_ = 3;
    const int num_joints_ = num_legs_ * num_joints_per_leg_;
    const int num_locomote_joints_ = num_locomote_legs_ * num_joints_per_leg_;
    const int num_manipulate_joints_ = num_manipulate_legs_ * num_joints_per_leg_;
    static constexpr double weight_ = 9.8f * 21.0f; // mass = 21 kg

    // structural and stance constants to make system stand
    // these bars need to be tuned
    const double bar_y = 0.1187; // distance fromt com of the robot to motor 0, y direction
    const double bar_x = 0.2057; // distance fromt com of the robot to motor 0, x direction
    const double foot_bar_y = 0.1187 + 0.20; // distance fromt com of the robot to foot 0, y direction
    const double foot_bar_x = 0.2057 + 0.34; // distance fromt com of the robot to foot 0, x direction
    const double nominal_height_z = 0.3;


    Eigen::Vector4d base_stance_ee_xyz; // expressed in base motor's frame
    Eigen::Vector3d com_stance_ee_xyz;  // expressed in com of the robot's frame
};

} // namespace hebi
