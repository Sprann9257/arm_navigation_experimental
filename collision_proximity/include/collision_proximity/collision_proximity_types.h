/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2010, Willow Garage, Inc.
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

/** \author E. Gil Jones */

#ifndef COLLISION_PROXIMITY_TYPES_
#define COLLISION_PROXIMITY_TYPES_

#include <vector>
#include <string>
#include <algorithm>
#include <sstream>

#include <geometric_shapes/shapes.h>
#include <geometric_shapes/bodies.h>
#include <LinearMath/btTransform.h>

#include <distance_field/distance_field.h>
#include <distance_field/propagation_distance_field.h>
#include <distance_field/pf_distance_field.h>

namespace collision_proximity
{

struct CollisionType {

  CollisionType() : 
    none(true),
    self(false),
    intra(false),
    environment(false)
  {}
  
  bool none;
  bool self;
  bool intra;
  bool environment;
};

struct CollisionSphere {

  CollisionSphere(btVector3 rel, double radius)
  {
    relative_vec_ = rel;
    radius_ = radius;
  }

  btVector3 center_;
  btVector3 relative_vec_;
  double radius_;
};

struct GradientInfo
{
  GradientInfo() :
    closest_distance(DBL_MAX),
    collision(false)
  {
  }

  double closest_distance;
  bool collision;
  std::vector<btVector3> sphere_locations;
  std::vector<double> distances;
  std::vector<btVector3> gradients;
  std::vector<double> sphere_radii;
  std::string joint_name;

  void clear() {
    closest_distance = DBL_MAX;
    collision = false;
    sphere_locations.clear();
    distances.clear();
    gradients.clear();
  }
};

//determines set of collision spheres given a posed body
std::vector<CollisionSphere> determineCollisionSpheres(const bodies::Body* body, btTransform& relativeTransform);

//determines a set of points at the indicated resolution that are inside the supplied body 
std::vector<btVector3> determineCollisionPoints(const bodies::Body* body, double resolution);

//determines a set of gradients of the given collision spheres in the distance field
bool getCollisionSphereGradients(const distance_field::PropagationDistanceField* distance_field, 
                                 const std::vector<CollisionSphere>& sphere_list, 
                                 GradientInfo& gradient, 
                                 double tolerance, 
                                 bool subtract_radii, 
                                 double maximum_value, 
                                 bool stop_at_first_collision);

bool getCollisionSphereCollision(const distance_field::PropagationDistanceField* distance_field, 
                                 const std::vector<CollisionSphere>& sphere_list,
                                 double tolerance);

//forward declaration required for friending apparently
class BodyDecompositionVector;

class BodyDecomposition {

  friend class BodyDecompositionVector;

public:
    
  BodyDecomposition(const std::string& object_name, const shapes::Shape* shape, double resolution);

  ~BodyDecomposition();

  btTransform relative_cylinder_pose_;

  //assumed to be in reference frame, updates the pose of the body,
  //the collision spheres, and the posed collision points
  void updatePose(const btTransform& linkTransform);

  void updateSpheresPose(const btTransform& linkTransform);
  void updatePointsPose(const btTransform& linkTransform);

  const std::vector<CollisionSphere>& getCollisionSpheres() const 
  {
    return collision_spheres_;
  }
    
  const std::vector<btVector3>& getCollisionPoints() const
  {
    return posed_collision_points_;
  }

  const bodies::Body* getBody() const
  {
    return body_;
  }

private:
    
  std::string object_name_;

  bodies::Body* body_;

  std::vector<CollisionSphere> collision_spheres_;
  std::vector<btVector3> relative_collision_points_;
  std::vector<btVector3> posed_collision_points_;
    
};

class BodyDecompositionVector
{
public:
  BodyDecompositionVector()
  {}

  ~BodyDecompositionVector(){
    for(unsigned int i = 0; i < decomp_vector_.size(); i++) {
      delete decomp_vector_[i];
    }
    decomp_vector_.clear();
  }

  const std::vector<btVector3>& getCollisionPoints() const
  {
    return collision_points_;
  }

  const std::vector<CollisionSphere>& getCollisionSpheres() const
  {
    return collision_spheres_;
  }

  // the decomposition vector keeps its own copies of the points 
  // for efficiency reasons
  void addToVector(BodyDecomposition* bd)
  {
    sphere_index_map_[decomp_vector_.size()] = collision_spheres_.size();
    point_index_map_[decomp_vector_.size()] = collision_points_.size();
    decomp_vector_.push_back(bd);
    collision_spheres_.insert(collision_spheres_.end(), bd->getCollisionSpheres().begin(), bd->getCollisionSpheres().end());
    collision_points_.insert(collision_points_.end(), bd->getCollisionPoints().begin(), bd->getCollisionPoints().end());
  }

  unsigned int getSize() const {
    return decomp_vector_.size();
  }
    
  const BodyDecomposition* getBodyDecomposition(unsigned int i) const {
    if(i >= decomp_vector_.size()) {
      ROS_INFO_STREAM("No body decomposition");
      return NULL;
    }
    return decomp_vector_[i];
  }

  void updateBodyPose(unsigned int ind, const btTransform& pose) {
    if(ind < decomp_vector_.size()) {
      decomp_vector_[ind]->updatePose(pose);
    } else {
      ROS_WARN("Can't update pose");
      return;
    }
    const std::vector<CollisionSphere>& spheres = decomp_vector_[ind]->getCollisionSpheres();
    for(unsigned j = 0; j < spheres.size(); j++) {
      collision_spheres_[sphere_index_map_[ind]+j] = spheres[j];
    }
    const std::vector<btVector3>& points = decomp_vector_[ind]->getCollisionPoints();
    for(unsigned j = 0; j < points.size(); j++) {
      collision_points_[point_index_map_[ind]+j] = points[j];
    }
  }

  void updateSpheresPose(unsigned int ind, const btTransform& pose) {
    if(ind < decomp_vector_.size()) {
      decomp_vector_[ind]->updateSpheresPose(pose);
    } else {
      ROS_WARN("Can't update pose");
      return;
    }
    const std::vector<CollisionSphere>& spheres = decomp_vector_[ind]->getCollisionSpheres();
    for(unsigned j = 0; j < spheres.size(); j++) {
      collision_spheres_[sphere_index_map_[ind]+j] = spheres[j];
    }
  }

private:
  std::map<unsigned int, unsigned int> sphere_index_map_;
  std::map<unsigned int, unsigned int> point_index_map_;
  std::vector<BodyDecomposition*> decomp_vector_;
  std::vector<CollisionSphere> collision_spheres_;
  std::vector<btVector3> collision_points_;
};

struct ProximityInfo 
{
  std::string link_name;
  std::string attached_object_name;
  double proximity;
  unsigned int sphere_index;
  unsigned int att_index;
  btVector3 closest_point;
  btVector3 closest_gradient;
};

}

#endif
