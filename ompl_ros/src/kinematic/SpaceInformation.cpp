/*********************************************************************
* Software License Agreement (BSD License)
* 
*  Copyright (c) 2008, Willow Garage, Inc.
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
*   * Neither the name of the Willow Garage nor the names of its
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

/** \author Ioan Sucan */

#include "ompl_ros/kinematic/SpaceInformation.h"
#include "ompl_ros/kinematic/StateValidator.h"
#include <ros/console.h>

void ompl_ros::ROSSpaceInformationKinematic::configureOMPLSpace(ModelBase *model)
{	   
  kmodel_ = model->planningMonitor->getKinematicModel();
  groupName_ = model->groupName;
  planning_models::KinematicState state(kmodel_);
  const planning_models::KinematicState::JointStateGroup* group_state = state.getJointStateGroup(groupName_);
  divisions_ = 100;

  const std::map<std::string, unsigned int>& map_index = group_state->getKinematicStateIndexMap();
   
  /* compute the state space for this group */
  m_stateDimension = map_index.size();
  m_stateComponent.resize(m_stateDimension);

  for (unsigned int i = 0 ; i < group_state->getJointStateVector().size() ; ++i)
  {	
    const std::map<std::string, std::pair<double,double> >& bounds = group_state->getJointStateVector()[i]->getAllJointValueBounds();
    for(std::map<std::string, std::pair<double,double> >::const_iterator it = bounds.begin();
        it != bounds.end();
        it++) {
      m_stateComponent[map_index.at(it->first)].minValue = it->second.first;
      m_stateComponent[map_index.at(it->first)].maxValue = it->second.second;
      m_stateComponent[map_index.at(it->first)].resolution = (m_stateComponent[i].maxValue - m_stateComponent[i].minValue) / divisions_;
    }

    unsigned int k = map_index.at(group_state->getJointStateVector()[i]->getName());
    
    if (m_stateComponent[i].type == ompl::base::StateComponent::UNKNOWN)
    {
      const planning_models::KinematicModel::RevoluteJointModel *rj = 
        dynamic_cast<const planning_models::KinematicModel::RevoluteJointModel*>(group_state->getJointStateVector()[i]->getJointModel());
      if (rj && rj->continuous_)
        m_stateComponent[k].type = ompl::base::StateComponent::WRAPPING_ANGLE;
      else
        m_stateComponent[k].type = ompl::base::StateComponent::LINEAR;
    }
    
    if (dynamic_cast<const planning_models::KinematicModel::FloatingJointModel*>(group_state->getJointStateVector()[i]->getJointModel()))
    {
      floatingJoints_.push_back(k);
      m_stateComponent[k + 3].type = ompl::base::StateComponent::QUATERNION;
      m_stateComponent[k + 4].type = ompl::base::StateComponent::QUATERNION;
      m_stateComponent[k + 5].type = ompl::base::StateComponent::QUATERNION;
      m_stateComponent[k + 6].type = ompl::base::StateComponent::QUATERNION;
      break;
    }
    
    if (dynamic_cast<const planning_models::KinematicModel::PlanarJointModel*>(group_state->getJointStateVector()[i]->getJointModel()))
    {
      planarJoints_.push_back(k);
      m_stateComponent[k + 2].type = ompl::base::StateComponent::WRAPPING_ANGLE;
      break;		    
    }
  }
  
  // create a backup of this, in case it gets bound by joint constraints
  basicStateComponent_ = m_stateComponent;
  
  checkResolution();
  checkBounds();    
}

void ompl_ros::ROSSpaceInformationKinematic::checkResolution(void)
{    
  /* for movement in plane/space, we want to make sure the resolution is small enough */
  for (unsigned int i = 0 ; i < planarJoints_.size() ; ++i)
    {
      if (m_stateComponent[planarJoints_[i]].resolution > 0.1)
        m_stateComponent[planarJoints_[i]].resolution = 0.1;
      if (m_stateComponent[planarJoints_[i] + 1].resolution > 0.1)
        m_stateComponent[planarJoints_[i] + 1].resolution = 0.1;
    }
  for (unsigned int i = 0 ; i < floatingJoints_.size() ; ++i)
    {
      if (m_stateComponent[floatingJoints_[i]].resolution > 0.1)
        m_stateComponent[floatingJoints_[i]].resolution = 0.1;
      if (m_stateComponent[floatingJoints_[i] + 1].resolution > 0.1)
        m_stateComponent[floatingJoints_[i] + 1].resolution = 0.1;
      if (m_stateComponent[floatingJoints_[i] + 2].resolution > 0.1)
        m_stateComponent[floatingJoints_[i] + 2].resolution = 0.1;
    }
}

void ompl_ros::ROSSpaceInformationKinematic::setPlanningVolume(double x0, double y0, double z0, double x1, double y1, double z1)
{
  for (unsigned int i = 0 ; i < floatingJoints_.size() ; ++i)
    {
      int id = floatingJoints_[i];		
      m_stateComponent[id    ].minValue = x0;
      m_stateComponent[id    ].maxValue = x1;
      m_stateComponent[id + 1].minValue = y0;
      m_stateComponent[id + 1].maxValue = y1;
      m_stateComponent[id + 2].minValue = z0;
      m_stateComponent[id + 2].maxValue = z1;
      for (int j = 0 ; j < 3 ; ++j)
        m_stateComponent[j + id].resolution = (m_stateComponent[j + id].maxValue - m_stateComponent[j + id].minValue) / divisions_;
    }
  checkResolution();
  checkBounds();    
}

void ompl_ros::ROSSpaceInformationKinematic::setPlanningArea(double x0, double y0, double x1, double y1)
{
    for (unsigned int i = 0 ; i < planarJoints_.size() ; ++i)
    {
	int id = planarJoints_[i];		
	m_stateComponent[id    ].minValue = x0;
	m_stateComponent[id    ].maxValue = x1;
	m_stateComponent[id + 1].minValue = y0;
	m_stateComponent[id + 1].maxValue = y1;
	for (int j = 0 ; j < 2 ; ++j)
	    m_stateComponent[j + id].resolution = (m_stateComponent[j + id].maxValue - m_stateComponent[j + id].minValue) / divisions_;
    }
    checkResolution();
    checkBounds();    
}

void ompl_ros::ROSSpaceInformationKinematic::clearPathConstraints(void)
{
    m_stateComponent = basicStateComponent_;
    ROSStateValidityPredicateKinematic *svp = dynamic_cast<ROSStateValidityPredicateKinematic*>(getStateValidityChecker());
    svp->clearConstraints();
}

void ompl_ros::ROSSpaceInformationKinematic::setPathConstraints(const motion_planning_msgs::Constraints &kc)
{   
    const std::vector<motion_planning_msgs::JointConstraint> &jc = kc.joint_constraints;
    
    planning_models::KinematicState state(kmodel_);
    const planning_models::KinematicState::JointStateGroup* group_state = state.getJointStateGroup(groupName_);

    // tighten the bounds based on the constraints
    for (unsigned int i = 0 ; i < jc.size() ; ++i)
    {
      const std::map<std::string, unsigned int>& all_group_order = group_state->getKinematicStateIndexMap();
      
      if(all_group_order.find(jc[i].joint_name) != all_group_order.end()) {
        int idx = all_group_order.find(jc[i].joint_name)->second;
        if (m_stateComponent[idx].minValue < jc[i].position - jc[i].tolerance_below)
          m_stateComponent[idx].minValue = jc[i].position - jc[i].tolerance_below;
        if (m_stateComponent[idx].maxValue > jc[i].position + jc[i].tolerance_above)
          m_stateComponent[idx].maxValue = jc[i].position + jc[i].tolerance_above;
      }
    }
    checkBounds();    
    motion_planning_msgs::Constraints temp_kc = kc;
    temp_kc.joint_constraints.clear();
    ROSStateValidityPredicateKinematic *svp = dynamic_cast<ROSStateValidityPredicateKinematic*>(getStateValidityChecker());
    svp->setConstraints(temp_kc);
}

bool ompl_ros::ROSSpaceInformationKinematic::checkBounds(void)
{
  // check if joint bounds are feasible
  bool valid = true;
  for (unsigned int i = 0 ; i < m_stateDimension ; ++i)
    if (m_stateComponent[i].minValue > m_stateComponent[i].maxValue)
      {
        valid = false;
        ROS_ERROR("Inconsistent set of joint constraints imposed on path at index %d. Sampling will not find any valid states between %f and %f", i,
                  m_stateComponent[i].minValue, m_stateComponent[i].maxValue);
        break;
      }
  return valid;
}

void ompl_ros::ROSSpaceInformationKinematic::printSettings(std::ostream &out) const
{
    ompl::kinematic::SpaceInformationKinematic::printSettings(out);
    const ROSStateValidityPredicateKinematic *svp = dynamic_cast<const ROSStateValidityPredicateKinematic*>(getStateValidityChecker());
    svp->printSettings(out);
}