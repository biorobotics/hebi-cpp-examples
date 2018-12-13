#include <chrono>
#include <thread>
#include <ctime>
#include <fstream>
#include <iostream>

#include "quadruped.hpp"

namespace hebi {
  std::unique_ptr<Quadruped> Quadruped::create(const QuadrupedParameters& params)
  {
    hebi::Lookup lookup;
    std::vector<std::string> names;
    for (int i = 0; i < num_legs_; ++i)
    {
      names.push_back("base" + std::to_string(i + 1));
      names.push_back("shoulder" + std::to_string(i + 1));
      names.push_back("elbow" + std::to_string(i + 1));
    }

    // temporarily still use hexapod as name 
    std::vector<std::string> family = { "hexapod" };

    long timeout_ms = 4000; // use a 4 second timeout
    auto group = lookup.getGroupFromNames(family, names, timeout_ms);
    // omitted a bunch of error checkings which are in orginial code
    if (!group)
    {
      return nullptr;
    }
    group->setCommandLifetimeMs(100);

    return std::unique_ptr<Quadruped>(new Quadruped(group, params));
  }

  Quadruped::Quadruped(std::shared_ptr<Group> group, const QuadrupedParameters& params)
  : group_(group), params_(params), cmd_(group_ ? group_->size() : 1)
  {
    Eigen::Vector3d zero_vec = Eigen::Vector3d::Zero();
    legs_.emplace_back(new QuadLeg(30.0 * M_PI / 180.0, 0.2375, zero_vec, params, 0, QuadLeg::LegConfiguration::Left));
    legs_.emplace_back(new QuadLeg(-30.0 * M_PI / 180.0, 0.2375, zero_vec, params, 1, QuadLeg::LegConfiguration::Right));
    legs_.emplace_back(new QuadLeg(90.0 * M_PI / 180.0, 0.1875, zero_vec, params, 2, QuadLeg::LegConfiguration::Left));
    legs_.emplace_back(new QuadLeg(-90.0 * M_PI / 180.0, 0.1875, zero_vec, params, 3, QuadLeg::LegConfiguration::Right));
    legs_.emplace_back(new QuadLeg(150.0 * M_PI / 180.0, 0.2375, zero_vec, params, 4, QuadLeg::LegConfiguration::Left));
    legs_.emplace_back(new QuadLeg(-150.0 * M_PI / 180.0, 0.2375, zero_vec, params, 5, QuadLeg::LegConfiguration::Right));


    base_stance_ee_xyz = Eigen::Vector4d(0.36f, 0.0f, -0.31f, 0); // expressed in base motor's frame

    // This looks like black magic to me
    if (group_)
    {
      group_->addFeedbackHandler([this] (const GroupFeedback& fbk)
      {
        static bool first_rotation = false;
        static std::vector<Eigen::Matrix3d> init_rotation;
        // FBK 1: get gravity direction
        // Some assistant variables calcuate needed physical quantities
        // A -z vector in a local frame.
        Eigen::Vector3d down(0, 0, -1);
        Eigen::Vector3d avg_grav;
        avg_grav.setZero();

        std::lock_guard<std::mutex> guard(fbk_lock_);
        latest_fbk_time = std::chrono::steady_clock::now();
        assert(fbk.size() == num_joints_);
        
        // average all euler angle from 6 IMUs to get a better estimation
        Eigen::Vector3d single_euler;
        Eigen::Vector3d average_euler;
        std::vector<Eigen::Quaterniond> q_list;
        // std::cout << "angles ";
        // for (int i = 0; i < num_legs_*num_joints_per_leg_; ++i)
        // {
        //     std::cout << fbk[i].actuator().position().get() << " ";
        // }
        // std::cout << std::endl;
        if (updateBodyR) // this only be activated when system goes to third state, so the outside planner will 
                         // call startUpdateBodyR to enable this flag to let the system start to update body R estimation
        {
          // record initial rotations, so later we only calculate relation rotations as body rotation
          if (!first_rotation)
          {
            for (int i = 0; i < num_legs_; ++i)
            {
              Eigen::Matrix4d trans = legs_[i]->getKinematics().getBaseFrame();
              Eigen::Matrix3d trans_mat = trans.topLeftCorner<3,3>();
              auto mod_orientation = fbk[i * num_joints_per_leg_]   // 0  3  6 9 12 15
              .imu().orientation().get();
              Eigen::Quaterniond mod_orientation_eig(
                mod_orientation.getW(),
                mod_orientation.getX(),
                mod_orientation.getY(),
                mod_orientation.getZ());
              Eigen::Matrix3d mod_orientation_mat = mod_orientation_eig.toRotationMatrix();
              
              init_rotation.push_back(mod_orientation_mat);
            }
            
            first_rotation = true;
          }
          else
          {
            int valid_fbk = 0;
            for (int i = 0; i < num_legs_; ++i)
            {
              Eigen::Matrix4d trans = legs_[i]->getKinematics().getBaseFrame();
              Eigen::Matrix3d trans_mat = trans.topLeftCorner<3,3>();
              // HEBI Quaternion
              auto mod_orientation = fbk[i * num_joints_per_leg_]   // 0  3  6 9 12 15
                .imu().orientation().get();
              // Eigen Quaternion
              Eigen::Quaterniond mod_orientation_eig(
                mod_orientation.getW(),
                mod_orientation.getX(),
                mod_orientation.getY(),
                mod_orientation.getZ());
                
              Eigen::Matrix3d mod_orientation_mat = init_rotation[i].transpose() * mod_orientation_eig.toRotationMatrix();
              // transform rotation axis to com of the robot
              Eigen::AngleAxisd tmp_aa = Eigen::AngleAxisd(mod_orientation_mat);
              double new_angle = tmp_aa.angle();
              Eigen::Vector3d axis_aa = tmp_aa.axis();
              axis_aa = trans_mat*axis_aa;
              

              // std::cout << "mod_orientation_mat aa  " << tmp_aa.angle() << " "
              //                                       << axis_aa(0) << " "
              //                                       << axis_aa(1) << " "
              //                                       << axis_aa(2) << " "
              //                                       << std::endl;
              Eigen::AngleAxisd tmp_aa_after = Eigen::AngleAxisd(new_angle, axis_aa);
              mod_orientation_mat = tmp_aa_after.toRotationMatrix();
              
              single_euler = mod_orientation_mat.eulerAngles(2,1,0);
              
              body_R = mod_orientation_mat; // comment this, then uncomment  168-170 to get average rotation
              
              //std::cout << "mod_orientation_mat" << mod_orientation_mat << std::endl;
              // std::cout << "single _euler: " << single_euler(0) << " "
              //                          << single_euler(1) << " "
              //                          << single_euler(2) << 
              //                          std::endl;
              if (!std::isnan(single_euler(0)) && !std::isnan(single_euler(1)) && !std::isnan(single_euler(2)))
              {
                average_euler = average_euler + single_euler;
                valid_fbk += 1;
              }
                

              // Transform
              Eigen::Vector3d my_grav = trans.topLeftCorner<3,3>() * mod_orientation_mat.transpose() * down;
              // If one of the modules isn't reporting valid feedback, ignore this:
              if (!std::isnan(my_grav[0]) && !std::isnan(my_grav[1]) && !std::isnan(my_grav[2]))
                avg_grav += my_grav;
            } 
            // std::cout << "average_euler: " << average_euler(0) << " "
            //                            << average_euler(1) << " "
            //                            << average_euler(2) << std::endl;

            average_euler(0) = average_euler(0)/valid_fbk;
            average_euler(1) = average_euler(1)/valid_fbk;
            average_euler(2) = average_euler(2)/valid_fbk;
            // std::cout << "average_euler: " << average_euler(0) << " "
            //                           << average_euler(1) << " "
            //                           << average_euler(2) <<  std::endl;

            // body_R = Eigen::AngleAxisd(average_euler(0), Eigen::Vector3d::UnitZ()) *
            //         Eigen::AngleAxisd(average_euler(1), Eigen::Vector3d::UnitY()) *
            //         Eigen::AngleAxisd(average_euler(2), Eigen::Vector3d::UnitX());

            // Average the feedback from various modules and normalize.
            avg_grav.normalize();
            {
              std::lock_guard<std::mutex> lg(grav_lock_);
              gravity_direction_ = avg_grav;
            }
          }
        }
        

        // FBK 2 read fbk positions to legs
        for (int i = 0; i < num_legs_; ++i)
        {
          Eigen::VectorXd pos_vec = Eigen::VectorXd::Zero(num_joints_per_leg_);
          for (int j = 0; j < num_joints_per_leg_; ++j)
          {
            auto& pos = fbk[i*num_joints_per_leg_+j].actuator().position();
            if (pos)
            {
              pos_vec(j) = pos.get();
            }
            else
            {
              pos_vec(j) = std::numeric_limits<double>::quiet_NaN();
            }
          }
          legs_[i]->setJointAngles(pos_vec);
        }

      });
      group_->setFeedbackFrequencyHz(fbk_frq_hz_); 
    }
  }

  Quadruped::~Quadruped()
  {
    if (group_)
    {
      group_->setFeedbackFrequencyHz(0);
      group_->clearFeedbackHandlers();
    }
  }

  Eigen::Vector3d Quadruped::getGravityDirection()
  {
    std::lock_guard<std::mutex> lg(grav_lock_);
    return gravity_direction_;
  }

  Eigen::VectorXd Quadruped::getLegJointAngles(int index)
  {
    return legs_[index]->getJointAngle();
  }

  bool Quadruped::planStandUpTraj(double duration_time)
  {
    // this is a hexapod movement ...
    for (int i = 0; i < num_legs_; ++i)
    {
      Eigen::VectorXd leg_start = getLegJointAngles(i);
      Eigen::VectorXd leg_end;
      

      auto base_frame = legs_[i] -> getBaseFrame();
      Eigen::VectorXd home_stance_xyz = (base_frame * base_stance_ee_xyz).topLeftCorner<3,1>();
      legs_[i]->computeIK(leg_end, home_stance_xyz);
      // TODO: fix! (quick and dirty -- leg mid is hardcoded as offset from leg end)
      Eigen::VectorXd leg_mid = leg_end;
      leg_mid(1) -= 0.3;
      leg_mid(2) -= 0.15;

      // Convert for trajectories
      int num_waypoints = 5;
      Eigen::MatrixXd positions(num_joints_per_leg_, num_waypoints);
      Eigen::MatrixXd velocities = Eigen::MatrixXd::Zero(num_joints_per_leg_, num_waypoints);
      Eigen::MatrixXd accelerations = Eigen::MatrixXd::Zero(num_joints_per_leg_, num_waypoints);
      Eigen::VectorXd nan_column = Eigen::VectorXd::Constant(num_joints_per_leg_, std::numeric_limits<double>::quiet_NaN());
      // Is this one of the legs that takes a step first?
      bool step_first = (i == 0 || i == 3 || i == 4);

      // Set positions
      positions.col(0) = leg_start;
      positions.col(1) = step_first ? leg_mid : leg_start;
      positions.col(2) = step_first ? leg_end : leg_start;
      positions.col(3) = step_first ? leg_end : leg_mid;
      positions.col(4) = leg_end;

      velocities.col(1) = nan_column;
      velocities.col(3) = nan_column;
      accelerations.col(1) = nan_column;
      accelerations.col(3) = nan_column;

      Eigen::VectorXd times(num_waypoints);
      times << 0,
              0 + duration_time * 0.25,
              0 + duration_time * 0.5,
              0 + duration_time * 0.75,
              0 + duration_time;
      startup_trajectories.push_back(trajectory::Trajectory::createUnconstrainedQp(
        times, positions, &velocities, &accelerations));
    }
  }

  bool Quadruped::execStandUpTraj(double curr_time)
  {
    // Controls to send to the robot
    Eigen::VectorXd angles(num_joints_per_leg_);
    Eigen::VectorXd vels(num_joints_per_leg_);
    Eigen::VectorXd torques(num_joints_per_leg_); 
    for (int i = 0; i < num_legs_; ++i)
    {
      Eigen::VectorXd a(num_joints_per_leg_); // do not use acceleration
      startup_trajectories[i]->getState(curr_time, &angles, &vels, &a);

      Eigen::Vector3d gravity_vec = getGravityDirection() * 9.8f;

      Eigen::MatrixXd foot_forces(3,num_legs_); // 3 (xyz) by num legs
      computeFootForces(foot_forces);
      double ramp_up_scale = std::min(1.0, (curr_time + 0.001 / 2.0)); // to prevent segementation fault when curr_time ==0
      foot_forces *= ramp_up_scale;
      //foot_forces.setZero();
      Eigen::Vector3d foot_force = foot_forces.block<3,1>(0,i);
      torques = legs_[i]-> computeCompensateTorques(angles, vels, gravity_vec, foot_force); 

      setCommand(i, &angles, &vels, &torques);
    }
    sendCommand();
  }
  
  // this is the hexapod original computation, i need another one for quadruped 
  void Quadruped::computeFootForces(Eigen::MatrixXd& foot_forces)
  {
    Eigen::VectorXd factors(6);
    Eigen::VectorXd blend_factors(6);
    Eigen::Vector3d grav = -gravity_direction_;
    // Get the dot product of gravity with each leg, and then subtract a scaled
    // gravity from the foot stance position.
    // NOTE: Matt is skeptical about this overall approach; but it worked before so we are keeping
    // it for now.
    factors.resize(6); 
    for (int i = 0; i < 6; ++i)
    {
      auto base_frame = legs_[i] -> getBaseFrame();
      Eigen::VectorXd home_stance_xyz = (base_frame * base_stance_ee_xyz).topLeftCorner<3,1>();
      Eigen::Vector3d stance = home_stance_xyz;
      double dot_prod = grav.dot(stance);
      factors(i) = (grav * dot_prod - stance).norm();
    }
    double fact_sum = factors.sum();
    for (int i = 0; i < 6; ++i)
      factors(i) = fact_sum / factors(i);
    for (int i = 0; i < 6; ++i)
    {
      // Redistribute weight to just modules in stance
      blend_factors(i) = 1;
      
    }
    fact_sum = factors.sum();
    factors /= fact_sum;

    // NOTE: here, we have a blend factor for each foot to allow for future gaits;
    // in MATLAB, there was just one scalar for this.  We use "max" here to match
    // the results from MATLAB.
    for (int i = 0; i < 6; ++i)
      factors(i) = factors(i) * (1 + .33 * std::sin(M_PI * blend_factors(i)));

    //  std::cout << "factors: " << factors << std::endl;
    //  std::cout << "grav: " << grav << std::endl;

    foot_forces.resize(3,6);
    for (int i = 0; i < 6; ++i)
      foot_forces.block<3,1>(0,i) = factors(i) * weight_ * grav;
  }

  void Quadruped::setCommand(int index, const VectorXd* angles, const VectorXd* vels, const VectorXd* torques)
  {
    int leg_offset = index * num_joints_per_leg_;
    // I think do not need to check the size of the code
    if (angles != nullptr)
    {
      assert(angles->size() == num_joints_per_leg_);
      for (int i = 0; i < num_joints_per_leg_; ++i)
        cmd_[leg_offset + i].actuator().position().set((*angles)[i]);
    }
    if (vels != nullptr)
    {
      assert(vels->size() == num_joints_per_leg_);
      for (int i = 0; i < num_joints_per_leg_; ++i)
        cmd_[leg_offset + i].actuator().velocity().set((*vels)[i]);
    }
    if (torques != nullptr)
    {
      assert(torques->size() == num_joints_per_leg_);
      for (int i = 0; i < num_joints_per_leg_; ++i)
        cmd_[leg_offset + i].actuator().effort().set((*torques)[i]);
    }
  }

  bool Quadruped::spreadAllLegs()
  {
    bool isReaching = true;
    is_exec_traj = true;
    Eigen::VectorXd goal;

    // set command angle 
    for (int i = 0; i < num_legs_; ++i)
    {
      auto base_frame = legs_[i] -> getBaseFrame();
      Eigen::Vector4d tmp4(0.55, 0, 0.05, 0); // hard code first
      Eigen::VectorXd home_stance_xyz = (base_frame * tmp4).topLeftCorner<3,1>();
      
      legs_[i]->computeIK(goal, home_stance_xyz);
      int leg_offset = i * num_joints_per_leg_;
      cmd_[leg_offset + 0].actuator().position().set(goal(0));
      cmd_[leg_offset + 1].actuator().position().set(goal(1));
      cmd_[leg_offset + 2].actuator().position().set(goal(2));
    }

    // check if legs reach command angle
    for (int i = 0; i < num_legs_; ++i)
    {
      Eigen::VectorXd curr_angle = legs_[i]->getJointAngle();
      Eigen::VectorXd differece = goal - curr_angle;

      if (differece.norm() > 0.5)
      {
        isReaching = false;
      }
    }
    sendCommand();
    return isReaching;
  }

  bool Quadruped::pushAllLegs(double curr_time, double total_time)
  {
    bool isReaching = true;
    is_exec_traj = true;
    Eigen::VectorXd goal;
    Eigen::Vector3d gravity_vec = getGravityDirection() * 9.8f;

    // set command angle 
    for (int i = 0; i < num_legs_; ++i)
    {
      auto base_frame = legs_[i] -> getBaseFrame();
      Eigen::VectorXd home_stance_xyz = (base_frame * base_stance_ee_xyz).topLeftCorner<3,1>();
      legs_[i]->computeIK(goal, home_stance_xyz);
      int leg_offset = i * num_joints_per_leg_;
      cmd_[leg_offset + 0].actuator().position().set(goal(0));
      cmd_[leg_offset + 1].actuator().position().set(goal(1));
      cmd_[leg_offset + 2].actuator().position().set(goal(2));
   
      Eigen::Vector3d vels(0,0,0);
      // locally compensate foot force, need a dedicated function later
      Eigen::Vector3d foot_force = curr_time/total_time* 1.0f / 6.0f * -gravity_direction_ * weight_;
      Eigen::Vector3d torques = legs_[i]-> computeCompensateTorques(goal, vels, gravity_vec, foot_force); 

      cmd_[leg_offset + 0].actuator().effort().set(torques(0));
      cmd_[leg_offset + 1].actuator().effort().set(torques(1));
      cmd_[leg_offset + 2].actuator().effort().set(torques(2));

    }

    sendCommand();
    return isReaching;   
  }

  bool Quadruped::prepareQuadMode()
  {
    bool isReaching = true;
    is_exec_traj = true;
    Eigen::VectorXd goal;

    Eigen::Vector3d gravity_vec = getGravityDirection() * 9.8f;


    // set command angle 
    // 0 1 4 5 locomote legs  2 3 manipulate
    for (int i = 0; i < num_legs_; i == 1 ? i = i+3 : i++)
    {
      // TODO: consider this as 
      auto base_frame = legs_[i] -> getBaseFrame();
      Eigen::VectorXd home_stance_xyz = (base_frame * base_stance_ee_xyz).topLeftCorner<3,1>();
      legs_[i]->computeIK(goal, home_stance_xyz);
      int leg_offset = i * num_joints_per_leg_;
      cmd_[leg_offset + 0].actuator().position().set(goal(0));
      cmd_[leg_offset + 1].actuator().position().set(goal(1));
      cmd_[leg_offset + 2].actuator().position().set(goal(2));

      Eigen::Vector3d vels(0,0,0);
      // locally compensate foot force, need a dedicated function later
      Eigen::Vector3d foot_force = 0.25* -gravity_direction_ * weight_;
      Eigen::Vector3d torques = legs_[i]-> computeCompensateTorques(goal, vels, gravity_vec, foot_force); 

      cmd_[leg_offset + 0].actuator().effort().set(torques(0));
      cmd_[leg_offset + 1].actuator().effort().set(torques(1));
      cmd_[leg_offset + 2].actuator().effort().set(torques(2));
    }
    for (int i = 2; i < 4; i++)
    {
      auto base_frame = legs_[i] -> getBaseFrame();
      Eigen::Vector4d tmp4(0.35, 0, 0, 0); // hard code first
      Eigen::VectorXd home_stance_xyz = (base_frame * tmp4).topLeftCorner<3,1>();
      Eigen::Vector3d tmp3(0.07, 0, 0);
      home_stance_xyz = home_stance_xyz + tmp3;
      legs_[i]->computeIK(goal, home_stance_xyz);
      int leg_offset = i * num_joints_per_leg_;
      cmd_[leg_offset + 0].actuator().position().set(goal(0));
      cmd_[leg_offset + 1].actuator().position().set(goal(1));
      cmd_[leg_offset + 2].actuator().position().set(goal(2));
    }
    sendCommand();
    return isReaching;   
  }

  // just a test function, will be very nasty, a lot of quick and dirty tricks...
  // may be i will use similar foot force calculation as well

  /* 
    describe convention for quadruped here
    may move to other location later
    For the four locomotion legs, we denote them as 
    0(LF)      1(RF)
    
    4(LH)      5(RH)
    F is front,  H is hind.  This is ETH people's notation
    
    I will first use virtual leg strategy, LF-RH is virtual leg 1, and RF-LH is virtual leg 2.
    In this function, mode means which virtal leg is in swing mode and which is stance.
    Outside state machine calls runTest periodically with different mode argument, then 
    in this function, legs execute trajectories


    here the function does not actually use virtual leg placement strategy yet because we not yet have 
    body velocity measurement, let me work out a open loop gait first
  */
  void Quadruped::runTest(SwingMode mode, double curr_time, double total_time)
  {
    Eigen::VectorXd goal;
    Eigen::Vector3d gravity_vec = getGravityDirection() * 9.8f;
    // won't use these manipulate legs for a while so just hold them up
    for (int i = 2; i < 4; i++)
    {
      auto base_frame = legs_[i] -> getBaseFrame();
      Eigen::Vector4d tmp4(0.35, 0, 0, 0); // hard code first
      Eigen::VectorXd home_stance_xyz = (base_frame * tmp4).topLeftCorner<3,1>();
      Eigen::Vector3d tmp3(0.07, 0, 0);
      home_stance_xyz = home_stance_xyz + tmp3;
      legs_[i]->computeIK(goal, home_stance_xyz);
      int leg_offset = i * num_joints_per_leg_;
      cmd_[leg_offset + 0].actuator().position().set(goal(0));
      cmd_[leg_offset + 1].actuator().position().set(goal(1));
      cmd_[leg_offset + 2].actuator().position().set(goal(2));
    }

    // id of legs
    int swing_vleg[2], stance_vleg[2];
    if (mode == Quadruped::SwingMode::swing_mode_virtualLeg1)
    {
      swing_vleg[0] = 0;
      swing_vleg[1] = 5;
      stance_vleg[0] = 1;
      stance_vleg[1] = 4;
    }
    else
    {
      swing_vleg[0] = 1;
      swing_vleg[1] = 4;
      stance_vleg[0] = 0;
      stance_vleg[1] = 5;
    }
    // for swing leg
    for (int i = 0; i<2;i++)
    {
      Eigen::VectorXd traj_angles(3);
      Eigen::VectorXd traj_vels(3);
      Eigen::VectorXd traj_accs(3);
      Eigen::Vector3d foot_force = 0* -gravity_direction_ * weight_;
      // if (i == 0 && swing_vleg[0] == 0)
      // {
        swing_trajectories[i]->getState(curr_time, &traj_angles, &traj_vels, &traj_accs);
              std::cout << "traj_angles is " << traj_angles(0) << " " 
                                         << traj_angles(1) << " "
                                         << traj_angles(2) <<std::endl; 
      // }
      // else
      // {
      //   auto base_frame = legs_[swing_vleg[i]] -> getBaseFrame();
      //   Eigen::Vector4d tmp4(0.55, 0, -0.31, 0); // hard code first
      //   Eigen::VectorXd home_stance_xyz = (base_frame * tmp4).topLeftCorner<3,1>();
      //   legs_[swing_vleg[i]]->computeIK(traj_angles, home_stance_xyz);
      // }      
      int leg_offset = swing_vleg[i] * num_joints_per_leg_;
      cmd_[leg_offset + 0].actuator().position().set(traj_angles(0));
      cmd_[leg_offset + 1].actuator().position().set(traj_angles(1));
      cmd_[leg_offset + 2].actuator().position().set(traj_angles(2));

      // swing leg does not compensate foot force
      // if (i == 0 && swing_vleg[0] == 0)
      // {
      double normed_time = curr_time/total_time;
      double coefficient = -2*normed_time*normed_time + 2* normed_time +0.5;
      foot_force = (-0.25*0 + 0.2)* -gravity_direction_ * weight_;
      // }
      //Eigen::Vector3d vels(0,0,0);
      Eigen::Vector3d torques = legs_[swing_vleg[i]]-> computeCompensateTorques(traj_angles, traj_vels, gravity_vec, foot_force); 

      cmd_[leg_offset + 0].actuator().effort().set(torques(0));
      cmd_[leg_offset + 1].actuator().effort().set(torques(1));
      cmd_[leg_offset + 2].actuator().effort().set(torques(2));
    }
    // for stance leg
    // first calcuate foot force distribution
    for (int i = 0; i<2;i++)
    {
      Eigen::VectorXd traj_angles(3);
      Eigen::VectorXd traj_vels(3);
      Eigen::VectorXd traj_accs(3);
      
      // if (i == 0 && stance_vleg[0] == 0)
      // {
        stance_trajectories[i]->getState(curr_time, &traj_angles, &traj_vels, &traj_accs);
      // }
      // auto base_frame = legs_[stance_vleg[i]] -> getBaseFrame();
      // Eigen::Vector4d tmp4(0.55, 0, -0.31, 0); // hard code first
      // Eigen::VectorXd home_stance_xyz = (base_frame * tmp4).topLeftCorner<3,1>();
      // legs_[stance_vleg[i]]->computeIK(traj_angles, home_stance_xyz);
      
      int leg_offset = stance_vleg[i] * num_joints_per_leg_;
      cmd_[leg_offset + 0].actuator().position().set(traj_angles(0));
      cmd_[leg_offset + 1].actuator().position().set(traj_angles(1));
      cmd_[leg_offset + 2].actuator().position().set(traj_angles(2));

      
      // during a swing, change foot force distribution and ratio for stance leg
      double normed_time = curr_time/total_time;
      double coefficient = -2*normed_time*normed_time + 2* normed_time +0.5;
      Eigen::Vector3d foot_force = 0.0* -gravity_direction_ * weight_;
      Eigen::Vector3d torques = legs_[stance_vleg[i]]-> computeCompensateTorques(traj_angles, traj_vels, gravity_vec, foot_force); 

      cmd_[leg_offset + 0].actuator().effort().set(torques(0));
      cmd_[leg_offset + 1].actuator().effort().set(torques(1));
      cmd_[leg_offset + 2].actuator().effort().set(torques(2));
    }

    sendCommand();
  }

  // assistant function for runTest, it should be called when it is about to switch state
  void Quadruped::prepareTrajectories(SwingMode mode, double leg_swing_time)
  {
    // id of legs
    int swing_vleg[2], stance_vleg[2];
    if (mode == Quadruped::SwingMode::swing_mode_virtualLeg1)
    {
      swing_vleg[0] = 0;
      swing_vleg[1] = 5;
      stance_vleg[0] = 1;
      stance_vleg[1] = 4;
    }
    else
    {
      swing_vleg[0] = 1;
      swing_vleg[1] = 4;
      stance_vleg[0] = 0;
      stance_vleg[1] = 5;
    }
    // first swing legs
    swing_trajectories.clear();
    for (int i = 0; i<2;i++)
    {
      // Eigen::VectorXd start_leg_angles = legs_[swing_vleg[i]] -> getJointAngle();
      Eigen::VectorXd start_leg_angles;
      auto base_frame = legs_[swing_vleg[i]] -> getBaseFrame();
      Eigen::VectorXd home_stance_xyz = (base_frame * base_stance_ee_xyz).topLeftCorner<3,1>();
      
      legs_[swing_vleg[i]] -> computeIK(start_leg_angles, home_stance_xyz);

      hebi::robot_model::Matrix4dVector frames;
      // endeffector only one frame, take me very long time to figure out this frame thing
      // all FKs are represented in base frame, here the "frametype" essentially means point of interets
      legs_[swing_vleg[i]] -> getKinematics().getFK(HebiFrameTypeEndEffector, start_leg_angles, frames); // I assume this is in the frame of base frame
      Eigen::Vector3d start_leg_ee_xyz = frames[0].topRightCorner<3,1>();  // make sure this is in com frame
      int numFrame = legs_[swing_vleg[i]] -> getKinematics().getFrameCount(HebiFrameTypeEndEffector);
      std::cout << "prepare trajectories for leg " << swing_vleg[i] << " (frame " << numFrame << " )" << std::endl;
      std::cout << "start_leg_ee_xyz is " << start_leg_ee_xyz(0) << " " 
                                         << start_leg_ee_xyz(1) << " "
                                         << start_leg_ee_xyz(2) <<std::endl; 
      Eigen::VectorXd mid_leg_ee_xyz = start_leg_ee_xyz + Eigen::Vector3d(0.05,0.0,0.08);

      std::cout << "mid_leg_ee_xyz is " << mid_leg_ee_xyz(0) << " " 
                                         << mid_leg_ee_xyz(1) << " "
                                         << mid_leg_ee_xyz(2) <<std::endl; 
      Eigen::VectorXd end_leg_ee_xyz = start_leg_ee_xyz + Eigen::Vector3d(0.10,0.0,0.0);
      std::cout << "end_leg_ee_xyz is " << end_leg_ee_xyz(0) << " " 
                                         << end_leg_ee_xyz(1) << " "
                                         << end_leg_ee_xyz(2) <<std::endl; 

      std::cout << "start_leg_angle is " << start_leg_angles(0) << " " 
                                         << start_leg_angles(1) << " "
                                         << start_leg_angles(2) <<std::endl; 
      Eigen::VectorXd mid_leg_angles;
      Eigen::VectorXd end_leg_angles;
      legs_[swing_vleg[i]] -> computeIK(mid_leg_angles, mid_leg_ee_xyz);
      std::cout << "mid_leg_angles is " << mid_leg_angles(0) << " " 
                                         << mid_leg_angles(1) << " "
                                         << mid_leg_angles(2) <<std::endl; 
      legs_[swing_vleg[i]] -> computeIK(end_leg_angles, end_leg_ee_xyz);
      std::cout << "end_leg_angles is " << end_leg_angles(0) << " " 
                                         << end_leg_angles(1) << " "
                                         << end_leg_angles(2) <<std::endl; 

      // std::cout << "leg fk" << i << std:endl;
      // Convert for trajectories
      int num_waypoints = 3;
      Eigen::MatrixXd positions(num_joints_per_leg_, num_waypoints);
      Eigen::MatrixXd velocities = Eigen::MatrixXd::Zero(num_joints_per_leg_, num_waypoints);
      Eigen::MatrixXd accelerations = Eigen::MatrixXd::Zero(num_joints_per_leg_, num_waypoints);
      Eigen::VectorXd nan_column = Eigen::VectorXd::Constant(num_joints_per_leg_, std::numeric_limits<double>::quiet_NaN());

      // Set positions
      positions.col(0) = start_leg_angles;
      positions.col(1) = mid_leg_angles;
      positions.col(2) = end_leg_angles;

      velocities.col(1) = nan_column;
      accelerations.col(1) = nan_column;

      Eigen::VectorXd times(num_waypoints);
      double local_start = 0; //fine tune later
      double total = leg_swing_time;     //fine tune later
      times << local_start,
              local_start + total * 0.5,
              local_start + total;
      swing_trajectories.push_back(trajectory::Trajectory::createUnconstrainedQp(
        times, positions, &velocities, &accelerations));
    }

    // second stance leg, temporarily use similar trajectory, but I guess do not need to do so, we will see
    stance_trajectories.clear();
    for (int i = 0; i<2;i++)
    {
      //Eigen::VectorXd start_leg_angles;
      auto base_frame = legs_[stance_vleg[i]] -> getBaseFrame();
      Eigen::VectorXd home_stance_xyz = (base_frame * base_stance_ee_xyz).topLeftCorner<3,1>();

      Eigen::VectorXd start_leg_angles = legs_[stance_vleg[i]] -> getJointAngle();
      //legs_[stance_vleg[i]]->computeIK(start_leg_angles, home_stance_xyz);
      // 12-9 before left, have a plan for 12-10
      // need to read HexapodView2D tomorrow
      // test if getFK is also world frame, caclulate FK use this angle, see if it agree with base*tmp4 before
      
      hebi::robot_model::Matrix4dVector frames;
      // endeffector only one frame
      legs_[stance_vleg[i]] -> getKinematics().getFK(HebiFrameTypeEndEffector, start_leg_angles, frames); // I assume this is in the frame of base frame
      Eigen::VectorXd start_leg_ee_xyz = frames[0].topRightCorner<3,1>();  // make sure this is in com frame
      //Eigen::VectorXd mid_leg_ee_xyz = start_leg_ee_xyz + Eigen::Vector3d(-0.00,0.0,0.0);
      Eigen::VectorXd mid_leg_ee_xyz = 0.5*start_leg_ee_xyz + 0.5*home_stance_xyz+ Eigen::Vector3d(0.0,0.0,-0.01);
      //Eigen::VectorXd end_leg_ee_xyz = start_leg_ee_xyz + Eigen::Vector3d(-0.00,0.0,0.0);
      Eigen::VectorXd end_leg_ee_xyz = home_stance_xyz;
      Eigen::VectorXd mid_leg_angles;
      Eigen::VectorXd end_leg_angles;
      //legs_[stance_vleg[i]] -> computeIK(start_leg_angles, start_leg_ee_xyz);
      legs_[stance_vleg[i]] -> computeIK(mid_leg_angles, mid_leg_ee_xyz);
      legs_[stance_vleg[i]] -> computeIK(end_leg_angles, end_leg_ee_xyz);

      // std::cout << "leg fk" << i << std:endl;
      // Convert for trajectories
      int num_waypoints = 3;
      Eigen::MatrixXd positions(num_joints_per_leg_, num_waypoints);
      Eigen::MatrixXd velocities = Eigen::MatrixXd::Zero(num_joints_per_leg_, num_waypoints);
      Eigen::MatrixXd accelerations = Eigen::MatrixXd::Zero(num_joints_per_leg_, num_waypoints);
      Eigen::VectorXd nan_column = Eigen::VectorXd::Constant(num_joints_per_leg_, std::numeric_limits<double>::quiet_NaN());

      // Set positions
      positions.col(0) = start_leg_angles;
      positions.col(1) = mid_leg_angles;
      positions.col(2) = end_leg_angles;

      velocities.col(1) = nan_column;
      accelerations.col(1) = nan_column;

      Eigen::VectorXd times(num_waypoints);
      double local_start = 0; //fine tune later
      double total = leg_swing_time;     //fine tune later
      times << local_start,
              local_start + total * 0.5,
              local_start + total;
      stance_trajectories.push_back(trajectory::Trajectory::createUnconstrainedQp(
        times, positions, &velocities, &accelerations));
    }

  }

  bool Quadruped::reOrient(Eigen::Matrix3d target_body_R)
  {
    Eigen::VectorXd goal;
    Eigen::Vector3d gravity_vec = getGravityDirection() * 9.8f;
    // won't use these manipulate legs for a while so just hold them up
    for (int i = 2; i < 4; i++)
    {
      auto base_frame = legs_[i] -> getBaseFrame();
      Eigen::Vector4d tmp4(0.35, 0, 0, 0); // hard code first
      Eigen::VectorXd home_stance_xyz = (base_frame * tmp4).topLeftCorner<3,1>();
      Eigen::Vector3d tmp3(0.07, 0, 0);
      home_stance_xyz = home_stance_xyz + tmp3;
      legs_[i]->computeIK(goal, home_stance_xyz);
      int leg_offset = i * num_joints_per_leg_;
      cmd_[leg_offset + 0].actuator().position().set(goal(0));
      cmd_[leg_offset + 1].actuator().position().set(goal(1));
      cmd_[leg_offset + 2].actuator().position().set(goal(2));
    }
    // std::cout << "ready to get leg angles" << std::endl;

    // id of legs            and bar positions, to save some space
    int support_vleg[4];    double bar_x_list[4], bar_y_list[4];
    support_vleg[0] = 0;    bar_x_list[0] =  bar_x ; bar_y_list[0] =   bar_y;
    support_vleg[1] = 1;    bar_x_list[1] =  bar_x ; bar_y_list[1] =  -bar_y;
    support_vleg[2] = 4;    bar_x_list[2] = -bar_x ; bar_y_list[2] =  bar_y;
    support_vleg[3] = 5;    bar_x_list[3] = -bar_x ; bar_y_list[3] = -bar_y;
    
    double foot_bar_x_list[4], foot_bar_y_list[4];
    foot_bar_x_list[0] =  foot_bar_x ; foot_bar_y_list[0] =   foot_bar_y;
    foot_bar_x_list[1] =  foot_bar_x ; foot_bar_y_list[1] =  -foot_bar_y;
    foot_bar_x_list[2] = -foot_bar_x ; foot_bar_y_list[2] =  foot_bar_y;
    foot_bar_x_list[3] = -foot_bar_x ; foot_bar_y_list[3] = -foot_bar_y;

    Eigen::MatrixXd R_ec = target_body_R;
    Eigen::Vector3d p_ec(0,0,nominal_height_z);

    for (int i = 0; i<num_locomote_legs_;i++)
    {

      // some of my ugly notations...
      // frame definitions: 
      // e is earth frame or ground frame
      // b is base motor frame
      // c is the frame at CoM of the robot
      // f is the foot frame 
      
      int leg_offset = support_vleg[i] * num_joints_per_leg_;
      auto base_frame = legs_[support_vleg[i]] -> getBaseFrame();
      Eigen::MatrixXd R_cb = base_frame.topLeftCorner<3,3>();
      Eigen::VectorXd p_cb = base_frame.topRightCorner<3,1>();
      // std::cout << p_cb << std::endl;
      Eigen::VectorXd p_eb = R_ec*p_cb;
      Eigen::Vector3d p_ef(foot_bar_x_list[i],foot_bar_y_list[i],0);
      Eigen::VectorXd p_e = p_ef - p_ec - p_eb;
      Eigen::MatrixXd R_eb = R_ec*R_cb;
      Eigen::VectorXd p_b = R_eb.transpose()*p_e;

      // solve IK to get leg pose
      legs_[support_vleg[i]]->computeIK(goal, p_e);
      
      // std::cout << "pose for leg " << support_vleg[i] << " :" << p_e(0) << " "
      //                           << p_e(1) << " "
      //                           << p_e(2) <<  std::endl;

      cmd_[leg_offset + 0].actuator().position().set(goal(0));
      cmd_[leg_offset + 1].actuator().position().set(goal(1));
      cmd_[leg_offset + 2].actuator().position().set(goal(2));
      

      // constant footforce compensation
      Eigen::Vector3d traj_vels(0,0,0);
      Eigen::Vector3d foot_force = 0.25* -gravity_direction_ * weight_;
      Eigen::Vector3d torques = legs_[support_vleg[i]]-> computeCompensateTorques(goal, traj_vels, gravity_vec, foot_force); 

      cmd_[leg_offset + 0].actuator().effort().set(torques(0));
      cmd_[leg_offset + 1].actuator().effort().set(torques(1));
      cmd_[leg_offset + 2].actuator().effort().set(torques(2));
    }

    sendCommand();

  }

  void Quadruped::sendCommand()
  {
    if (group_)
      group_->sendCommand(cmd_);
  }

  bool Quadruped::setGains()
  {
    if (!group_)
      return true;

    hebi::GroupCommand gains(group_->size());
    std::string gains_file = std::string("quad_gains") + std::to_string(group_->size()) + ".xml";
    std::cout << "Loading gains from: " << gains_file << std::endl;
    bool success = gains.readGains(gains_file);
    return success && group_->sendCommandWithAcknowledgement(gains, 4000);
  }

  // private individual function, calcuate average of quaternions, but not correct yet
  Eigen::Quaterniond Quadruped::average_quat(Eigen::Quaterniond average_q_, std::vector<Eigen::Quaterniond> q_list_)
  {
    int q_list_size = q_list_.size(); // should be 6
    Eigen::Vector3d u(0,0,0);
    Eigen::Quaterniond out_q;
    out_q = average_q_;
    int run_count = 5, curr_count = 0; // prevent infinite loop
    do
    {
      curr_count ++;
      for (int i = 0; i < q_list_size; i++)
      {
        Eigen::Quaterniond q_tmp = q_list_[i]*out_q;
        // calculate log of q_tmp
        Eigen::Vector3d qv = quat_log(q_tmp);

        u += 1.0f/q_list_size*qv;
      }

      Eigen::Vector3d log_out_q = quat_log(out_q);
      out_q = quat_exp(log_out_q + u);
      if (curr_count > run_count)
      {
        break;
      }
    } while (u.norm()>0.1);

    return out_q;

  }

  Eigen::Vector3d Quadruped::quat_log(Eigen::Quaterniond q)
  {
    Eigen::Vector3d qv = q.vec();
    double angle;
    double sinha = qv.norm();
    if (sinha > 0)
    {
      angle = 2*atan2(sinha, q.w());
      qv = qv * (angle/sinha);
    }
    else
    {
      qv = qv * (2/q.w());
    }
    return qv;
  }
  Eigen::Quaterniond Quadruped::quat_exp(Eigen::Vector3d qv)
  {
    Eigen::Quaterniond out_q;
    double angle = qv.norm();
    Eigen::Vector3d u_vec = qv / qv.norm();

    out_q = Eigen::Quaterniond(cos(angle), sin(angle)*u_vec(0), sin(angle)*u_vec(1), sin(angle)*u_vec(2));

    return out_q;
  }
} // namespace hebi
