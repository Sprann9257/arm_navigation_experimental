/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2009, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/** \author Sachin Chitta */

#include <collision_proximity_planner/collision_proximity_planner.h>
#include <collision_proximity_planner/collision_proximity_planner_utils.h>
#include <planning_environment/models/model_utils.h>

using namespace std;

namespace collision_proximity_planner
{

CollisionProximityPlanner::CollisionProximityPlanner(const std::string& robot_description_name):private_handle_("~")
{
  private_handle_.param("use_pseudo_inverse", use_pseudo_inverse_, false);
  private_handle_.param("group_cps", group_name_cps_, std::string(" "));
  private_handle_.param("max_iterations", max_iterations_, 100);
  private_handle_.param("max_joint_update", max_joint_update_, 0.02);

  cps_ = new collision_proximity::CollisionProximitySpace(robot_description_name);

  cps_->setCollisionTolerance(cps_->getCollisionModelsInterface()->getDefaultPadding());
 
  // build the robot model
  chomp_robot_model_.init(cps_->getCollisionModelsInterface());

  // initialize the visualization publisher:
  vis_marker_array_publisher_ = private_handle_.advertise<visualization_msgs::MarkerArray>( "visualization_marker_array", 0 );
  vis_marker_publisher_ = private_handle_.advertise<visualization_msgs::Marker>( "visualization_marker", 0 );

  planning_service_ = private_handle_.advertiseService("plan",&CollisionProximityPlanner::getFreePath,this);

  //  joint_axis_.resize(chomp_robot_model_.getKDLTree()->getNrOfJoints());
  //  joint_pos_.resize(chomp_robot_model_.getKDLTree()->getNrOfJoints());
  //  segment_frames_.resize(chomp_robot_model_.getKDLTree()->getNrOfSegments());
  ROS_INFO("Initalized collision proximity planner");
}

CollisionProximityPlanner::~CollisionProximityPlanner()
{
}

bool CollisionProximityPlanner::initializeForGroup(const std::string& group_name) 
{
  // get the planning group:
  planning_group_ = chomp_robot_model_.getPlanningGroup(group_name);
  if(planning_group_ == NULL) {
    ROS_ERROR_STREAM("No planning group for " << group_name);
    return false;
  }
  num_joints_ = planning_group_->chomp_joints_.size();
  ROS_INFO("Planning for %d joints", num_joints_);
  //  num_collision_points_ = planning_group_->collision_points_.size();
  
  // set up joint index:
  group_joint_to_kdl_joint_index_.clear();
  group_joint_to_kdl_joint_index_.resize(num_joints_);
  for (int i=0; i<num_joints_; ++i)
    group_joint_to_kdl_joint_index_[i] = planning_group_->chomp_joints_[i].kdl_joint_index_;

  robot_state_group_.joint_state.position.clear();
  robot_state_group_.joint_state.name.clear();
  robot_state_group_.joint_state.position.resize(num_joints_);
  robot_state_group_.joint_state.name.resize(num_joints_);
  for(int i=0; i < num_joints_; i++)
    robot_state_group_.joint_state.name[i] = planning_group_->chomp_joints_[i].joint_name_;

  collision_increments_ = Eigen::MatrixXd::Zero(1, num_joints_);
  jacobian_ = Eigen::MatrixXd::Zero(3, num_joints_);
  jacobian_pseudo_inverse_ = Eigen::MatrixXd::Zero(num_joints_, 3);
  jacobian_jacobian_tranpose_ = Eigen::MatrixXd::Zero(3, 3);
  return true;
}

bool CollisionProximityPlanner::getFreePath(arm_navigation_msgs::GetMotionPlan::Request &req,
                                            arm_navigation_msgs::GetMotionPlan::Response &res)
{
  ROS_INFO("Computing free path");
  clear();
  if(!req.motion_plan_request.group_name.empty() && initializeForGroup(req.motion_plan_request.group_name)) {
    cps_->setupForGroupQueries(req.motion_plan_request.group_name,
                               req.motion_plan_request.start_state,
                               current_link_names_,
                               current_attached_body_names_);
  } else {
    return false;
  }
  for(unsigned int i=0; i < current_link_names_.size(); i++)
  {
    ROS_DEBUG("Finding active joints for %s",current_link_names_[i].c_str());
    std::vector<int> ac_j;
    int segment_number;
    chomp_robot_model_.getActiveJointInformation(current_link_names_[i],ac_j,segment_number);
    ROS_DEBUG("Found %zu active joints for %s",ac_j.size(),current_link_names_[i].c_str());
    active_joints_.push_back(ac_j);
  }
  bool result = findPathToFreeState(req.motion_plan_request.start_state,res.trajectory);
  if(result) {
    res.error_code.val = res.error_code.SUCCESS;
  } else {
    res.error_code.val = res.error_code.PLANNING_FAILED;
  }
  return result;
}

void CollisionProximityPlanner::clear()
{
  active_joints_.clear();
  group_state_joint_array_group_mapping_.clear();
  joint_array_group_group_state_mapping_.clear();
}

void CollisionProximityPlanner::fillInGroupState(arm_navigation_msgs::RobotState &robot_state,
                                                 const arm_navigation_msgs::RobotState &group_state)
{
  for(unsigned int i=0; i < group_state.joint_state.name.size(); i++)
  {
    for(unsigned int j=0; j < robot_state.joint_state.name.size(); j++)
    {
      if(group_state.joint_state.name[i] == robot_state.joint_state.name[j])
      {
        ROS_INFO("Filling in group state for %s",robot_state.joint_state.name[j].c_str());
        robot_state.joint_state.position[j] = group_state.joint_state.position[i];
      }
    }
  }
}

void CollisionProximityPlanner::getGroupArray(const KDL::JntArray &jnt_array,
                                              const std::vector<int> &group_joint_to_kdl_joint_index,
                                              KDL::JntArray &jnt_array_group)
{
  for(int i=0; i < num_joints_; i++)
    jnt_array_group(i) = jnt_array(group_joint_to_kdl_joint_index[i]);
}

void CollisionProximityPlanner::fillInGroupArray(const KDL::JntArray &jnt_array_group,
                                                 const std::vector<int> &group_joint_to_kdl_joint_index,
                                                 KDL::JntArray &jnt_array)
{
  for(int i=0; i < num_joints_; i++)
    jnt_array(group_joint_to_kdl_joint_index[i]) = jnt_array_group(i);
}

bool CollisionProximityPlanner::findPathToFreeState(const arm_navigation_msgs::RobotState &robot_state, 
                                                    arm_navigation_msgs::RobotTrajectory &robot_trajectory)
{
  std::vector<KDL::JntArray> jnt_trajectory;
  Eigen::MatrixXd collision_increments;
  KDL::JntArray jnt_array;
  jnt_array.resize(robot_state.joint_state.name.size());
  chomp_robot_model_.jointStateToArray(robot_state.joint_state,jnt_array);
  performForwardKinematics(jnt_array,true);
  bool in_collision = true;
  double distance;

  KDL::JntArray jnt_array_group;
  jnt_array_group.resize(num_joints_);
  getGroupArray(jnt_array,group_joint_to_kdl_joint_index_,jnt_array_group);

  for(int i=0; i < max_iterations_; i++)
  {    
    ROS_DEBUG("Iteration: %d",max_iterations_);
    jnt_trajectory.push_back(jnt_array_group);
    fillInGroupArray(jnt_array_group,group_joint_to_kdl_joint_index_,jnt_array);
    performForwardKinematics(jnt_array,false);
    updateGroupRobotState(jnt_array_group);
    updateCollisionProximitySpace(robot_state_group_);
    in_collision = calculateCollisionIncrements(collision_increments,distance);
    if(!in_collision)
    {
      ROS_INFO("Found state not in collision in %d iterations",i+1);
      break;
    }
    updateJointState(jnt_array_group,collision_increments);
    for(int j=0; j < num_joints_; j++)
    {
      ROS_DEBUG("Joint update: %d %f %f",j,collision_increments(0,j),jnt_array_group(j));
    }
  }
  
  kdlJointTrajectoryToRobotTrajectory(jnt_trajectory,robot_trajectory);
  if(in_collision)
  {
    ROS_WARN("Final position is also in collision");
    return true;
  }
  return true;
}

bool CollisionProximityPlanner::setGroupState(const arm_navigation_msgs::RobotState &group_state)
{
  int joints_found = 0;
  group_state_joint_array_group_mapping_.resize(num_joints_);
  for(unsigned int i=0; i < group_state.joint_state.name.size(); i++)
  {
    for(unsigned int j=0; j < robot_state_group_.joint_state.name.size(); j++)
    {
      if(group_state.joint_state.name[i] == robot_state_group_.joint_state.name[j])
      {
        joints_found++;
        group_state_joint_array_group_mapping_[i] = j;
        joint_array_group_group_state_mapping_[j] = i;
      }
    }
  }
  if(joints_found != num_joints_)
    return false;
  return true;
}

bool CollisionProximityPlanner::mapGroupState(const arm_navigation_msgs::RobotState &group_state,
                                              const std::vector<int>& mapping)
{
  if((int) group_state.joint_state.name.size() < num_joints_ || (int) group_state.joint_state.position.size() < num_joints_)
  {
    ROS_ERROR("Group state needs to contain %zu joints", robot_state_group_.joint_state.name.size());
    return false;
  }
  for(int i=0; i < num_joints_; i++)
    jnt_array_group_(mapping[i]) = group_state.joint_state.position[i];
  return true;
}

bool CollisionProximityPlanner::getStateGradient(const arm_navigation_msgs::RobotState &group_state,
                                                 double &distance,
                                                 arm_navigation_msgs::RobotState &gradient)
{
  Eigen::MatrixXd collision_increments;
  if(!mapGroupState(group_state,group_state_joint_array_group_mapping_))
    return false;
  fillInGroupArray(jnt_array_group_,group_joint_to_kdl_joint_index_,jnt_array_);
  performForwardKinematics(jnt_array_,false);
  updateGroupRobotState(jnt_array_group_);
  updateCollisionProximitySpace(robot_state_group_);
  calculateCollisionIncrements(collision_increments,distance);
  for(int i=0; i < num_joints_; i++)
    gradient.joint_state.position[joint_array_group_group_state_mapping_[i]] = collision_increments(0,i);

  return true;
}

bool CollisionProximityPlanner::refineState(const arm_navigation_msgs::RobotState &group_state,
                                            arm_navigation_msgs::RobotTrajectory &robot_trajectory)
{
  std::vector<KDL::JntArray> jnt_trajectory;
  Eigen::MatrixXd collision_increments;
  bool in_collision = true;
  double distance;
  if(!mapGroupState(group_state,group_state_joint_array_group_mapping_))
    return false;
  for(int i=0; i < max_iterations_; i++)
  {    
    ROS_DEBUG("Iteration: %d",max_iterations_);
    jnt_trajectory.push_back(jnt_array_group_);
    fillInGroupArray(jnt_array_group_,group_joint_to_kdl_joint_index_,jnt_array_);
    performForwardKinematics(jnt_array_,false);
    updateGroupRobotState(jnt_array_group_);
    updateCollisionProximitySpace(robot_state_group_);
    in_collision = calculateCollisionIncrements(collision_increments,distance);
    if(!in_collision)
    {
      ROS_INFO("Found state not in collision in %d iterations",i+1);
      break;
    }
    updateJointState(jnt_array_group_,collision_increments);
    for(int j=0; j < num_joints_; j++)
    {
      ROS_DEBUG("Joint update: %d %f %f",j,collision_increments(0,j),jnt_array_group_(j));
    }
  }  
  kdlJointTrajectoryToRobotTrajectory(jnt_trajectory,robot_trajectory);
  if(in_collision)
  {
    ROS_WARN("Final position is also in collision");
    return false;
  }
  return true;
};

void CollisionProximityPlanner::kdlJointTrajectoryToRobotTrajectory(std::vector<KDL::JntArray> &jnt_trajectory,
                                                                    arm_navigation_msgs::RobotTrajectory &robot_trajectory)
{
  robot_trajectory.joint_trajectory.header.frame_id = reference_frame_;
  robot_trajectory.joint_trajectory.header.stamp = ros::Time::now();
  robot_trajectory.joint_trajectory.joint_names.resize(num_joints_);
  robot_trajectory.joint_trajectory.points.resize(jnt_trajectory.size());
  for(unsigned int i=0; i < robot_trajectory.joint_trajectory.points.size(); i++)
  {
    robot_trajectory.joint_trajectory.points[i].positions.resize(num_joints_);
    for(int j=0; j < num_joints_; j++)
      robot_trajectory.joint_trajectory.points[i].positions[j] = jnt_trajectory[i](j);
  }
  robot_trajectory.joint_trajectory.joint_names = robot_state_group_.joint_state.name;
}

void CollisionProximityPlanner::updateGroupRobotState(const KDL::JntArray &jnt_array)
{
  for(int i=0; i < num_joints_; i++)
    robot_state_group_.joint_state.position[i] = jnt_array(i);
}
void CollisionProximityPlanner::updateCollisionProximitySpace(const arm_navigation_msgs::RobotState &group_state)
{
  planning_environment::setRobotStateAndComputeTransforms(group_state, *cps_->getCollisionModelsInterface()->getPlanningSceneState());
  cps_->setCurrentGroupState(*cps_->getCollisionModelsInterface()->getPlanningSceneState());
}

bool CollisionProximityPlanner::calculateCollisionIncrements(Eigen::MatrixXd &collision_increments,
                                                             double &min_distance)
{
  collision_increments = Eigen::MatrixXd::Zero(1, num_joints_);

  std::vector<collision_proximity::GradientInfo> gradients;

  Eigen::Vector3d cartesian_gradient;
  bool in_collision =   cps_->getStateGradients(gradients, true);  
  bool state_in_collision = cps_->getCollisionModelsInterface()->isKinematicStateInCollision(*cps_->getCollisionModelsInterface()->getPlanningSceneState());

  min_distance = DBL_MAX;
  for(unsigned int i=0; i < current_link_names_.size(); i++)
  {
    ROS_DEBUG("Link: %s",current_link_names_[i].c_str());
    for(unsigned int j=0; j < gradients[i].sphere_locations.size(); j++)
    {      
      ROS_DEBUG("Contact: %d",j);
      Eigen::Vector3d collision_point_pos_eigen;
      collision_point_pos_eigen(0) = gradients[i].sphere_locations[j].x();
      collision_point_pos_eigen(1) = gradients[i].sphere_locations[j].y();
      collision_point_pos_eigen(2) = gradients[i].sphere_locations[j].z();

      cartesian_gradient(0) = -gradients[i].distances[j] * gradients[i].distances[j] * gradients[i].gradients[j].x();
      cartesian_gradient(1) = -gradients[i].distances[j] * gradients[i].distances[j] * gradients[i].gradients[j].y();
      cartesian_gradient(2) = -gradients[i].distances[j] * gradients[i].distances[j] * gradients[i].gradients[j].z();

      if(min_distance > gradients[i].distances[j])
        min_distance = gradients[i].distances[j];

      ROS_DEBUG("Point: %f %f %f",gradients[i].sphere_locations[j].x(),gradients[i].sphere_locations[j].y(),gradients[i].sphere_locations[j].z());
      ROS_DEBUG("Gradient: %f %f %f",gradients[i].gradients[j].x(),gradients[i].gradients[j].y(),gradients[i].gradients[j].z());
      ROS_DEBUG("Environment distance: %f",gradients[i].distances[j]);
      getJacobian((int)i,joint_pos_eigen_,joint_axis_eigen_,collision_point_pos_eigen,jacobian_,group_joint_to_kdl_joint_index_);
      if (use_pseudo_inverse_)
      {
        //        calculatePseudoInverse();
        collision_increments.row(0).transpose() -=
          jacobian_pseudo_inverse_ * cartesian_gradient;
      }
      else
      {
        ROS_DEBUG("Jacobian");
        for(int i=0; i < num_joints_; i++)
          ROS_DEBUG("%f %f %f",jacobian_.col(i)(0),jacobian_.col(i)(1),jacobian_.col(i)(2));
        collision_increments.row(0).transpose() -=
          jacobian_.transpose() * cartesian_gradient;
        ROS_DEBUG("Cartesian gradient: %f %f %f",cartesian_gradient(0),cartesian_gradient(1),cartesian_gradient(2));
        ROS_DEBUG("Collision increment: %f %f %f %f %f %f %f",collision_increments.row(0)(0),collision_increments.row(0)(1),collision_increments.row(0)(2),collision_increments.row(0)(3),collision_increments.row(0)(4),collision_increments.row(0)(5),collision_increments.row(0)(6));
      }
      //     if (point_is_in_collision_[i][j])
      //        break;
    }
  }
  return in_collision || state_in_collision;
}

void CollisionProximityPlanner::performForwardKinematics(KDL::JntArray &jnt_array, const bool& full)
{
  if(full)
    planning_group_->fk_solver_->JntToCartFull(jnt_array, joint_pos_, joint_axis_, segment_frames_);    
  else
    planning_group_->fk_solver_->JntToCartPartial(jnt_array, joint_pos_, joint_axis_, segment_frames_);    

  kdlVecToEigenVec(joint_axis_, joint_axis_eigen_, 3, 1);
  kdlVecToEigenVec(joint_pos_, joint_pos_eigen_, 3, 1);
  kdlVecToEigenVec(collision_point_pos_, collision_point_pos_eigen_, 3, 1);
}

void CollisionProximityPlanner::updateJointState(KDL::JntArray &jnt_array,
                                                 Eigen::MatrixXd &collision_increments)
{
  double diff = collision_increments.row(0).norm();
  double scale = diff/max_joint_update_;
  if(scale > 1.0)
    scale = 1.0/scale;
  for(int i=0; i < num_joints_; i++)
    jnt_array(i) += scale * collision_increments(0,i);
}

/*void ChompOptimizer::addIncrementsToTrajectory()
{
//  double scale = 1.0;
  for (int i=0; i<num_joints_; i++)
  {
    double scale = 1.0;
    double max = final_increments_.col(i).maxCoeff();
    double min = final_increments_.col(i).minCoeff();
    double max_scale = planning_group_->chomp_joints_[i].joint_update_limit_ / fabs(max);
    double min_scale = planning_group_->chomp_joints_[i].joint_update_limit_ / fabs(min);
    if (max_scale < scale)
      scale = max_scale;
    if (min_scale < scale)
      scale = min_scale;
    group_trajectory_.getFreeTrajectoryBlock().col(i) += scale * final_increments_.col(i);
  }
  //ROS_DEBUG("Scale: %f",scale);
  //group_trajectory_.getFreeTrajectoryBlock() += scale * final_increments_;
}
*/


}
