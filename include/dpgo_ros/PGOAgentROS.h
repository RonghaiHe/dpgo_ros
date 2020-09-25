/* ----------------------------------------------------------------------------
 * Copyright 2020, Massachusetts Institute of Technology, * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Yulun Tian, et al. (see README for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

#ifndef PGOAGENTROS_H
#define PGOAGENTROS_H

#include <DPGO/PGOAgent.h>
#include <dpgo_ros/Command.h>
#include <dpgo_ros/QueryLiftingMatrix.h>
#include <dpgo_ros/QueryPoses.h>
#include <dpgo_ros/Status.h>
#include <pose_graph_tools/PoseGraph.h>
#include <ros/console.h>
#include <ros/ros.h>

using namespace DPGO;

namespace dpgo_ros {

class PGOAgentROS : public PGOAgent {
 public:
  PGOAgentROS(ros::NodeHandle nh_, unsigned ID,
              const PGOAgentParameters& params);

  ~PGOAgentROS();

 private:
  // ROS node handle
  ros::NodeHandle nh;

  // Current instance number
  unsigned instance_number;

  // Current iteration number
  unsigned iteration_number;

  // Latest relative changes of all robots
  std::vector<double> relativeChanges;

  // Termination condition
  double RelativeChangeTolerance;

  // Latest optimization result
  ROPTResult OptResult;

  // Apply local update
  void update();

  // Request latest public poses from a neighboring agent
  bool requestPublicPosesFromAgent(const unsigned& neighborID);

  // Publish status
  void publishStatus();

  // Publish command
  void publishCommand();

  // Publish trajectory
  bool publishTrajectory();

  // ROS callbacks
  void statusCallback(const StatusConstPtr& msg);
  void commandCallback(const CommandConstPtr& msg);
  void poseGraphCallback(const pose_graph_tools::PoseGraphConstPtr& msg);
  bool queryLiftingMatrixCallback(QueryLiftingMatrixRequest& request,
                                  QueryLiftingMatrixResponse& response);
  bool queryPosesCallback(QueryPosesRequest& request,
                          QueryPosesResponse& response);

  // ROS publisher
  ros::Publisher statusPublisher;
  ros::Publisher commandPublisher;
  ros::Publisher poseArrayPublisher;
  ros::Publisher pathPublisher;

  // ROS subscriber
  ros::Subscriber statusSubscriber;
  ros::Subscriber commandSubscriber;
  ros::Subscriber poseGraphSubscriber;

  // ROS service server
  ros::ServiceServer queryLiftingMatrixServer;
  ros::ServiceServer queryPoseServer;
};

}  // namespace dpgo_ros

#endif