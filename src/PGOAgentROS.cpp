/* ----------------------------------------------------------------------------
 * Copyright 2020, Massachusetts Institute of Technology, * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Yulun Tian, et al. (see README for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

#include <DPGO/DPGO_solver.h>
#include <dpgo_ros/PGOAgentROS.h>
#include <dpgo_ros/utils.h>
#include <geometry_msgs/PoseArray.h>
#include <glog/logging.h>
#include <nav_msgs/Path.h>
#include <pose_graph_tools_msgs/PoseGraphQuery.h>
#include <pose_graph_tools_ros/utils.h>
#include <tf/tf.h>

#include <map>
#include <random>

using namespace DPGO;

namespace dpgo_ros {

PGOAgentROS::PGOAgentROS(const ros::NodeHandle &nh_,
                         unsigned ID,
                         const PGOAgentROSParameters &params)
    : PGOAgent(ID, params),
      nh(nh_),
      mParamsROS(params),
      mClusterID(ID),
      mInitStepsDone(0),
      mTotalBytesReceived(0),
      mIterationElapsedMs(0) {
  mTeamIterRequired.assign(mParams.numRobots, 0);
  mTeamIterReceived.assign(mParams.numRobots, 0);
  mTeamReceivedSharedLoopClosures.assign(mParams.numRobots, false);
  mTeamConnected.assign(mParams.numRobots, true);

  // Load robot names
  for (size_t id = 0; id < mParams.numRobots; id++) {
    std::string robot_name = "kimera" + std::to_string(id);
    ros::param::get("~robot" + std::to_string(id) + "_name", robot_name);
    mRobotNames[id] = robot_name;
  }

  // ROS subscriber
  for (size_t robot_id = 0; robot_id < mParams.numRobots; ++robot_id) {
    std::string topic_prefix = "/" + mRobotNames.at(robot_id) + "/dpgo_ros_node/";
    mLiftingMatrixSubscriber.push_back(nh.subscribe(topic_prefix + "lifting_matrix",
                                                    100,
                                                    &PGOAgentROS::liftingMatrixCallback,
                                                    this));
    mStatusSubscriber.push_back(
        nh.subscribe(topic_prefix + "status", 100, &PGOAgentROS::statusCallback, this));
    mCommandSubscriber.push_back(nh.subscribe(
        topic_prefix + "command", 100, &PGOAgentROS::commandCallback, this));
    mAnchorSubscriber.push_back(
        nh.subscribe(topic_prefix + "anchor", 100, &PGOAgentROS::anchorCallback, this));
    mPublicPosesSubscriber.push_back(nh.subscribe(
        topic_prefix + "public_poses", 100, &PGOAgentROS::publicPosesCallback, this));
    mSharedLoopClosureSubscriber.push_back(
        nh.subscribe(topic_prefix + "public_measurements",
                     100,
                     &PGOAgentROS::publicMeasurementsCallback,
                     this));
  }
  mConnectivitySubscriber =
      nh.subscribe("/" + mRobotNames.at(mID) + "/connected_peer_ids",
                   5,
                   &PGOAgentROS::connectivityCallback,
                   this);

  for (size_t robot_id = 0; robot_id < getID(); ++robot_id) {
    std::string topic_prefix = "/" + mRobotNames.at(robot_id) + "/dpgo_ros_node/";
    mMeasurementWeightsSubscriber.push_back(
        nh.subscribe(topic_prefix + "measurement_weights",
                     100,
                     &PGOAgentROS::measurementWeightsCallback,
                     this));
  }

  // ROS publisher
  mLiftingMatrixPublisher = nh.advertise<MatrixMsg>("lifting_matrix", 1);
  mAnchorPublisher = nh.advertise<PublicPoses>("anchor", 1);
  mStatusPublisher = nh.advertise<Status>("status", 1);
  mCommandPublisher = nh.advertise<Command>("command", 20);
  mPublicPosesPublisher = nh.advertise<PublicPoses>("public_poses", 20);
  mPublicMeasurementsPublisher =
      nh.advertise<RelativeMeasurementList>("public_measurements", 20);
  mMeasurementWeightsPublisher =
      nh.advertise<RelativeMeasurementWeights>("measurement_weights", 20);
  mPoseArrayPublisher = nh.advertise<geometry_msgs::PoseArray>("trajectory", 1);
  mPathPublisher = nh.advertise<nav_msgs::Path>("path", 1);
  mPoseGraphPublisher =
      nh.advertise<pose_graph_tools_msgs::PoseGraph>("optimized_pose_graph", 1);
  mLoopClosureMarkerPublisher =
      nh.advertise<visualization_msgs::Marker>("loop_closures", 1);

  // ROS timer
  timer = nh.createTimer(ros::Duration(3.0), &PGOAgentROS::timerCallback, this);
  mVisualizationTimer = nh.createTimer(
      ros::Duration(30.0), &PGOAgentROS::visualizationTimerCallback, this);

  // Initially, assume each robot is in a separate cluster
  resetRobotClusterIDs();

  // Publish lifting matrix
  for (size_t iter_ = 0; iter_ < 10; ++iter_) {
    publishNoopCommand();
    ros::Duration(0.5).sleep();
  }
  mLastResetTime = ros::Time::now();
  mLaunchTime = ros::Time::now();
  mLastCommandTime = ros::Time::now();
  mLastUpdateTime.reset();
}

void PGOAgentROS::runOnce() {
  if (mParams.asynchronous) {
    runOnceAsynchronous();
  } else {
    runOnceSynchronous();
  }
  // Request to publish public poses PGOAgent::iterate 设置为true
  if (mPublishPublicPosesRequested) {
    publishPublicPoses(false);
    // 如果启用加速，则发布附属的poses
    if (mParams.acceleration) publishPublicPoses(true);
    // 发布后重置请求
    mPublishPublicPosesRequested = false;
  }

  checkTimeout();
  // checkDisconnectedRobot();
}
// Runs the PGO (Pose Graph Optimization) agent once in synchronous mode
void PGOAgentROS::runOnceAsynchronous() {
  if (mPublishAsynchronousRequested) {
    if (isLeader()) publishAnchor();
    publishStatus();
    publishIterate();
    logIteration();
    mPublishAsynchronousRequested = false;
  }
}
/**
 * 同步模式下检查是否可以迭代优化 at every ROS spin
 *
 *
 */
void PGOAgentROS::runOnceSynchronous() {
  // 检查是否不是异步
  CHECK(!mParams.asynchronous);

  // Perform an optimization step
  // 接收到command消息：UPDATE 才为true，开始优化
  if (mSynchronousOptimizationRequested) {
    // Check if ready to perform iterate
    // 验证所有邻居机器人的迭代次数是否满足要求
    bool ready = true;
    for (unsigned neighbor : mPoseGraph->activeNeighborIDs()) {
      int requiredIter = (int)mTeamIterRequired[neighbor];
      if (mParams.acceleration) requiredIter = (int)iteration_number() + 1;
      requiredIter = requiredIter - mParamsROS.maxDelayedIterations;
      if ((int)mTeamIterReceived[neighbor] < requiredIter) {
        ready = false;
        ROS_WARN_THROTTLE(1,
                          "Robot %u iteration %u waits for neighbor %u "
                          "iteration %u (last received %u).",
                          getID(),
                          iteration_number() + 1,
                          neighbor,
                          requiredIter,
                          mTeamIterReceived[neighbor]);
      }
    }

    // Perform iterate with optimization if ready
    if (ready) {
      // Beta feature: Apply stored neighbor poses and edge weights for inactive robots
      // setInactiveNeighborPoses();
      // setInactiveEdgeWeights();
      // mPoseGraph->useInactiveNeighbors(true);

      // Iterate
      auto startTime = std::chrono::high_resolution_clock::now();
      bool success = iterate(true);
      auto counter = std::chrono::high_resolution_clock::now() - startTime;
      mIterationElapsedMs =
          (double)std::chrono::duration_cast<std::chrono::milliseconds>(counter)
              .count();
      mSynchronousOptimizationRequested = false;
      if (success) {
        mLastUpdateTime.emplace(ros::Time::now());
        ROS_INFO(
            "Robot %u iteration %u: success=%d, func_decr=%.1e, grad_init=%.1e, "
            "grad_opt=%.1e.",
            getID(),
            iteration_number(),
            mLocalOptResult.success,
            mLocalOptResult.fInit - mLocalOptResult.fOpt,
            mLocalOptResult.gradNormInit,
            mLocalOptResult.gradNormOpt);
      } else {
        ROS_WARN("Robot %u iteration not successful!", getID());
      }

      // First robot publish anchor
      // Anchor: 1st pose of 1st robot
      if (isLeader()) {
        publishAnchor();
      }

      // Publish status
      publishStatus();

      // Publish iterate (for visualization)
      publishIterate();

      // Log local iteration
      logIteration();

      // Print inforpublishIteratetion
      if (isLeader() && mParams.verbose) {
        ROS_INFO("Num weight updates done: %i, num inner iters: %i.",
                 mWeightUpdateCount,
                 mRobustOptInnerIter);
        for (size_t robot_id = 0; robot_id < mParams.numRobots; ++robot_id) {
          if (!isRobotActive(robot_id)) continue;
          const auto &it = mTeamStatus.find(robot_id);
          if (it != mTeamStatus.end()) {
            const auto &robot_status = it->second;
            ROS_INFO(
                "Robot %zu relative change %f.", robot_id, robot_status.relativeChange);
          } else {
            ROS_INFO("Robot %zu status unavailable.", robot_id);
          }
        }
      }

      // Check termination condition OR notify next robot to update
      if (isLeader()) {
        if (shouldTerminate()) {
          publishTerminateCommand();
        } else if (shouldUpdateMeasurementWeights()) {
          publishUpdateWeightCommand();
        } else {
          publishUpdateCommand();
        }
      } else {
        publishUpdateCommand();
      }
    }
  }
}

// 重置PGOAgentROS类的成员变量和状态
void PGOAgentROS::reset() {
  // 调用基类PGOAgent的重置方法
  PGOAgent::reset();

  // 初始化请求的同步优化标志为false
  mSynchronousOptimizationRequested = false;
  // 初始化请求尝试初始化标志为false
  mTryInitializeRequested = false;
  // 初始化初始化步骤计数器为0
  mInitStepsDone = 0;
  // 为每个机器人初始化迭代次数要求和接收计数器
  mTeamIterRequired.assign(mParams.numRobots, 0);
  mTeamIterReceived.assign(mParams.numRobots, 0);
  // 初始化接收到团队共享闭环信息的标志
  mTeamReceivedSharedLoopClosures.assign(mParams.numRobots, false);
  // 初始化接收到的总字节数计数器
  mTotalBytesReceived = 0;
  // 清空团队状态消息
  mTeamStatusMsg.clear();

  // 检查并关闭迭代日志文件
  if (mIterationLog.is_open()) {
    mIterationLog.close();
  }

  // 如果需要完全重置，则执行额外的操作
  if (mParamsROS.completeReset) {
    ROS_WARN("Reset DPGO completely.");
    mPoseGraph = std::make_shared<PoseGraph>(mID, r, d);  // Reset pose graph
    mCachedPoses.reset();  // Reset stored trajectory estimate
    mCachedLoopClosureMarkers.reset();
  }

  // 重置机器人集群ID
  resetRobotClusterIDs();
  // 记录最后重置时间
  mLastResetTime = ros::Time::now();
  // 重置最后更新时间
  mLastUpdateTime.reset();
}

bool PGOAgentROS::requestPoseGraph() {
  // Query local pose graph
  pose_graph_tools_msgs::PoseGraphQuery query;
  query.request.robot_id = getID();
  std::string service_name =
      "/" + mRobotNames.at(getID()) + "/distributed_loop_closure/request_pose_graph";
  if (!ros::service::waitForService(service_name, ros::Duration(5.0))) {
    ROS_ERROR_STREAM("ROS service " << service_name << " does not exist!");
    return false;
  }
  // 调用服务请求位姿图
  if (!ros::service::call(service_name, query)) {
    ROS_ERROR_STREAM("Failed to call ROS service " << service_name);
    return false;
  }

  pose_graph_tools_msgs::PoseGraph pose_graph = query.response.pose_graph;
  // 检查接收到的位姿图是否为空
  if (pose_graph.edges.size() <= 1) {
    ROS_WARN("Received empty pose graph.");
    return false;
  }

  // Process edges
  unsigned int num_measurements_before = mPoseGraph->numMeasurements();
  for (const auto &edge : pose_graph.edges) {
    RelativeSEMeasurement m = RelativeMeasurementFromMsg(edge);
    const PoseID src_id(m.r1, m.p1);
    const PoseID dst_id(m.r2, m.p2);
    // 检查测量是否与当前机器人相关
    if (m.r1 != getID() && m.r2 != getID()) {
      ROS_ERROR("Robot %u received irrelevant measurement! ", getID());
    }
    // 如果测量尚未存在于位姿图中，则添加测量
    if (!mPoseGraph->hasMeasurement(src_id, dst_id)) {
      addMeasurement(m);
    }
  }
  unsigned int num_measurements_after = mPoseGraph->numMeasurements();
  ROS_INFO("Received pose graph from ROS service (%u new measurements).",
           num_measurements_after - num_measurements_before);

  // Process nodes
  // BUG ? No use
  PoseArray initial_poses(dimension(), num_poses());
  if (!pose_graph.nodes.empty()) {
    // Filter nodes that do not belong to this robot
    vector<pose_graph_tools_msgs::PoseGraphNode> nodes_filtered;
    for (const auto &node : pose_graph.nodes) {
      if ((unsigned)node.robot_id == getID()) nodes_filtered.push_back(node);
    }
    // If pose graph contains initial guess for the poses, we will use them
    size_t num_nodes = nodes_filtered.size();
    if (num_nodes == num_poses()) {
      for (const auto &node : nodes_filtered) {
        assert((unsigned)node.robot_id == getID());
        size_t index = node.key;
        assert(index >= 0 && index < num_poses());
        initial_poses.rotation(index) = RotationFromPoseMsg(node.pose);
        initial_poses.translation(index) = TranslationFromPoseMsg(node.pose);
      }
    }
  }
  if (mParamsROS.synchronizeMeasurements) {
    // Synchronize shared measurements with other robots
    mTeamReceivedSharedLoopClosures.assign(mParams.numRobots, false);
    mTeamReceivedSharedLoopClosures[getID()] = true;

    // In Kimera-Multi, we wait for inter-robot loops
    // from robots with smaller ID
    // for (size_t robot_id = getID(); robot_id < mParams.numRobots; ++robot_id)
    //   mTeamReceivedSharedLoopClosures[robot_id] = true;
  } else {
    // Assume measurements are already synchronized by front end
    mTeamReceivedSharedLoopClosures.assign(mParams.numRobots, true);
  }

  mTryInitializeRequested = true;
  return true;
}

bool PGOAgentROS::tryInitialize() {
  // Before initialization, we need to received inter-robot loop closures from
  // all preceeding robots.
  bool ready = true;
  for (unsigned robot_id = 0; robot_id < getID(); ++robot_id) {
    // Skip if this preceeding robot is excluded from optimization
    if (!isRobotActive(robot_id)) {
      continue;
    }
    if (!mTeamReceivedSharedLoopClosures[robot_id]) {
      ROS_INFO("Robot %u waiting for shared loop closures from robot %u.",
               getID(),
               robot_id);
      ready = false;
      break;
    }
  }
  if (ready) {
    ROS_INFO(
        "Robot %u initializes. "
        "num_poses:%u, odom:%u, local_lc:%u, shared_lc:%u.",
        getID(),
        num_poses(),
        mPoseGraph->numOdometry(),
        mPoseGraph->numPrivateLoopClosures(),
        mPoseGraph->numSharedLoopClosures());

    // Perform local initialization
    initialize();

    // Leader first initializes in global frame
    if (isLeader()) {
      if (getID() == 0) {
        initializeInGlobalFrame(Pose(d));
      } else if (getID() != 0 && mCachedPoses.has_value()) {
        ROS_INFO("Leader %u initializes in global frame using previous result.",
                 getID());
        const auto TPrev = mCachedPoses.value();
        const Pose T_world_leader(TPrev.pose(0));
        initializeInGlobalFrame(T_world_leader);
        initializeGlobalAnchor();
        anchorFirstPose();
      }
    }
    mTryInitializeRequested = false;
  }
  return ready;
}

bool PGOAgentROS::isRobotConnected(unsigned robot_id) const {
  if (robot_id >= mParams.numRobots) {
    return false;
  }
  if (robot_id == getID()) {
    return true;
  }
  return mTeamConnected[robot_id];
}

void PGOAgentROS::setActiveRobots() {
  for (unsigned robot_id = 0; robot_id < mParams.numRobots; ++robot_id) {
    if (isRobotConnected(robot_id) && getRobotClusterID(robot_id) == getID()) {
      ROS_INFO("Set robot %u to active.", robot_id);
      setRobotActive(robot_id, true);
    } else {
      ROS_WARN("Set robot %u to inactive.", robot_id);
      setRobotActive(robot_id, false);
    }
  }
}

void PGOAgentROS::updateActiveRobots(const CommandConstPtr &msg) {
  std::set<unsigned> active_robots_set(msg->active_robots.begin(),
                                       msg->active_robots.end());
  for (unsigned int robot_id = 0; robot_id < mParams.numRobots; robot_id++) {
    if (active_robots_set.find(robot_id) == active_robots_set.end()) {
      setRobotActive(robot_id, false);
    } else {
      setRobotActive(robot_id, true);
    }
  }
}

void PGOAgentROS::publishLiftingMatrix() {
  Matrix YLift;
  if (!getLiftingMatrix(YLift)) {
    ROS_WARN("Lifting matrix does not exist! ");
    return;
  }
  MatrixMsg msg = MatrixToMsg(YLift);
  mLiftingMatrixPublisher.publish(msg);
}

void PGOAgentROS::publishAnchor() {
  // We assume the anchor is always the first pose of the first robot
  if (!isLeader()) {
    ROS_ERROR("Only leader robot should publish anchor!");
    return;
  }
  if (mState != PGOAgentState::INITIALIZED) {
    ROS_WARN("Cannot publish anchor: not initialized.");
    return;
  }
  Matrix T0;
  if (getID() == 0) {
    getSharedPose(0, T0);
  } else {
    if (!globalAnchor.has_value()) {
      return;
    }
    T0 = globalAnchor.value().getData();
  }
  PublicPoses msg;
  msg.robot_id = 0;
  msg.instance_number = instance_number();
  msg.iteration_number = iteration_number();
  msg.cluster_id = getClusterID();
  msg.is_auxiliary = false;
  msg.pose_ids.push_back(0);
  msg.poses.push_back(MatrixToMsg(T0));

  mAnchorPublisher.publish(msg);
}

void PGOAgentROS::publishUpdateCommand() {
  unsigned selected_robot = 0;
  switch (mParamsROS.updateRule) {
    case PGOAgentROSParameters::UpdateRule::Uniform: {
      // Uniform sampling of all active robots
      std::vector<unsigned> active_robots;
      for (unsigned robot_id = 0; robot_id < mParams.numRobots; ++robot_id) {
        if (isRobotActive(robot_id) && isRobotInitialized(robot_id)) {
          active_robots.push_back(robot_id);
        }
      }
      size_t num_active_robots = active_robots.size();
      std::vector<double> weights(num_active_robots, 1.0);
      std::discrete_distribution<int> distribution(weights.begin(), weights.end());
      std::random_device rd;
      std::mt19937 gen(rd());
      selected_robot = active_robots[distribution(gen)];
      break;
    }
    case PGOAgentROSParameters::UpdateRule::RoundRobin: {
      // Round robin updates
      unsigned next_robot_id = (getID() + 1) % mParams.numRobots;
      while (!isRobotActive(next_robot_id) || !isRobotInitialized(next_robot_id)) {
        next_robot_id = (next_robot_id + 1) % mParams.numRobots;
      }
      selected_robot = next_robot_id;
      break;
    }
  }
  if (selected_robot == getID()) {
    ROS_WARN("[publishUpdateCommand] Robot %u selects self to update next!", getID());
  }
  publishUpdateCommand(selected_robot);
}

void PGOAgentROS::publishUpdateCommand(unsigned robot_id) {
  if (mParams.asynchronous) {
    // In asynchronous mode, no need to publish update command
    // because each robot's local optimziation loop is constantly running
    return;
  }
  if (!isRobotActive(robot_id)) {
    ROS_ERROR("Next robot to update %u is not active!", robot_id);
    return;
  }
  if (mParamsROS.interUpdateSleepTime > 1e-3)
    ros::Duration(mParamsROS.interUpdateSleepTime).sleep();
  Command msg;
  msg.header.stamp = ros::Time::now();
  msg.command = Command::UPDATE;
  msg.cluster_id = getClusterID();
  msg.publishing_robot = getID();
  msg.executing_robot = robot_id;
  msg.executing_iteration = iteration_number() + 1;
  ROS_INFO_STREAM("Send UPDATE to robot " << msg.executing_robot
                                          << " to perform iteration "
                                          << msg.executing_iteration << ".");
  mCommandPublisher.publish(msg);
}

void PGOAgentROS::publishRecoverCommand() {
  Command msg;
  msg.header.stamp = ros::Time::now();
  msg.publishing_robot = getID();
  msg.cluster_id = getClusterID();
  msg.command = Command::RECOVER;
  msg.executing_iteration = iteration_number();
  mCommandPublisher.publish(msg);
  ROS_INFO("Robot %u published RECOVER command.", getID());
}

void PGOAgentROS::publishTerminateCommand() {
  Command msg;
  msg.header.stamp = ros::Time::now();
  msg.publishing_robot = getID();
  msg.cluster_id = getClusterID();
  msg.command = Command::TERMINATE;
  mCommandPublisher.publish(msg);
  ROS_INFO("Robot %u published TERMINATE command.", getID());
}

void PGOAgentROS::publishHardTerminateCommand() {
  Command msg;
  msg.header.stamp = ros::Time::now();
  msg.publishing_robot = getID();
  msg.cluster_id = getClusterID();
  msg.command = Command::HARD_TERMINATE;
  mCommandPublisher.publish(msg);
  ROS_INFO("Robot %u published HARD TERMINATE command.", getID());
}

void PGOAgentROS::publishUpdateWeightCommand() {
  Command msg;
  msg.header.stamp = ros::Time::now();
  msg.publishing_robot = getID();
  msg.cluster_id = getClusterID();
  msg.command = Command::UPDATE_WEIGHT;
  mCommandPublisher.publish(msg);
  ROS_INFO("Robot %u published UPDATE_WEIGHT command (num inner iters %i).",
           getID(),
           mRobustOptInnerIter);
}

void PGOAgentROS::publishRequestPoseGraphCommand() {
  if (!isLeader()) {
    ROS_ERROR("Only leader should send request pose graph command! ");
    return;
  }
  setActiveRobots();
  if (numActiveRobots() == 1) {
    ROS_WARN("Not enough active robots. Do not publish request pose graph command.");
    return;
  }
  Command msg;
  msg.header.stamp = ros::Time::now();
  msg.publishing_robot = getID();
  msg.cluster_id = getClusterID();
  msg.command = Command::REQUEST_POSE_GRAPH;
  for (unsigned robot_id = 0; robot_id < mParams.numRobots; ++robot_id) {
    if (isRobotActive(robot_id)) {
      msg.active_robots.push_back(robot_id);
    }
  }
  mCommandPublisher.publish(msg);
  ROS_INFO("Robot %u published REQUEST_POSE_GRAPH command.", getID());
}

void PGOAgentROS::publishInitializeCommand() {
  if (!isLeader()) {
    ROS_ERROR("Only leader should send INITIALIZE command!");
  }
  Command msg;
  msg.header.stamp = ros::Time::now();
  msg.publishing_robot = getID();
  msg.cluster_id = getClusterID();
  msg.command = Command::INITIALIZE;
  mCommandPublisher.publish(msg);
  mInitStepsDone++;
  mPublishInitializeCommandRequested = false;
  ROS_INFO("Robot %u published INITIALIZE command.", getID());
}

/**
 * 发布活动机器人的命令消息
 *
 * 本函数负责生成并发布一个命令消息，该消息指示哪些机器人是当前活动的
 * 它主要执行以下操作：
 * 1. 检查当前机器人是否为领导者，因为只有领导者才应该发布此类命令
 * 2. 创建一个命令消息，并填充必要的信息，如消息头、发布者ID、集群ID和命令类型
 * 3. 收集所有当前活动的机器人ID，并将它们添加到消息的活动机器人列表中
 * 4. 通过ROS通信发布该命令消息
 */
void PGOAgentROS::publishActiveRobotsCommand() {
  // 检查是否为领导者，因为只有领导者应该发布活动机器人命令
  if (!isLeader()) {
    ROS_ERROR("Only leader should publish active robots!");
    return;
  }

  // 初始化命令消息
  Command msg;
  // 设置消息时间戳为当前时间
  msg.header.stamp = ros::Time::now();
  // 设置消息发布者的ID为当前机器人的ID
  msg.publishing_robot = getID();
  // 设置集群ID
  msg.cluster_id = getClusterID();
  // 设置命令类型为设置活动机器人
  msg.command = Command::SET_ACTIVE_ROBOTS;

  // 遍历所有机器人，收集活动机器人的ID
  for (unsigned robot_id = 0; robot_id < mParams.numRobots; ++robot_id) {
    // 如果机器人是活动的，则将其ID添加到活动机器人列表中
    if (isRobotActive(robot_id)) {
      msg.active_robots.push_back(robot_id);
    }
  }

  // 发布命令消息
  mCommandPublisher.publish(msg);
}

void PGOAgentROS::publishNoopCommand() {
  Command msg;
  msg.header.stamp = ros::Time::now();
  msg.publishing_robot = getID();
  msg.cluster_id = getClusterID();
  msg.command = Command::NOOP;
  mCommandPublisher.publish(msg);
}

void PGOAgentROS::publishStatus() {
  Status msg = statusToMsg(getStatus());
  msg.cluster_id = getClusterID();
  msg.header.stamp = ros::Time::now();
  mStatusPublisher.publish(msg);
}

void PGOAgentROS::storeOptimizedTrajectory() {
  PoseArray T(dimension(), num_poses());
  if (getTrajectoryInGlobalFrame(T)) {
    mCachedPoses.emplace(T);
  }
}

void PGOAgentROS::publishTrajectory(const PoseArray &T) {
  // Publish as pose array
  geometry_msgs::PoseArray pose_array =
      TrajectoryToPoseArray(T.d(), T.n(), T.getData());
  mPoseArrayPublisher.publish(pose_array);

  // Publish as path
  nav_msgs::Path path = TrajectoryToPath(T.d(), T.n(), T.getData());
  mPathPublisher.publish(path);

  // Publish as optimized pose graph
  pose_graph_tools_msgs::PoseGraph pose_graph =
      TrajectoryToPoseGraphMsg(getID(), T.d(), T.n(), T.getData());
  mPoseGraphPublisher.publish(pose_graph);
}

void PGOAgentROS::publishOptimizedTrajectory() {
  if (!isRobotActive(getID())) return;
  if (!mCachedPoses.has_value()) return;
  publishTrajectory(mCachedPoses.value());
}

void PGOAgentROS::publishIterate() {
  if (!mParamsROS.publishIterate) {
    return;
  }
  PoseArray T(dimension(), num_poses());
  if (getTrajectoryInGlobalFrame(T)) {
    publishTrajectory(T);
  }
}
// Publish latest public poses
/**
 * 发布最新的公共位姿信息
 *
 * 该函数负责向邻居机器人发布当前机器人的位姿信息它会根据是否为辅助位姿信息来选择不同的位姿字典，
 * 并将这些位姿信息打包成PublicPoses消息发布出去
 *
 * @param aux
 * 布尔值，指示是否发布辅助位姿信息如果为true，表示发布辅助位姿；如果为false，表示发布正常位姿
 */
void PGOAgentROS::publishPublicPoses(bool aux) {
  // 遍历邻居ID，
  for (unsigned neighbor : getNeighbors()) {
    PoseDict map;
    if (aux) {
      // 如果是辅助位姿信息，尝试获取与邻居共享的辅助位姿字典 若没有初始化轨迹则return
      // 测量数据中对应自身的“Y”中的ID-frame ID-pose
      if (!getAuxSharedPoseDictWithNeighbor(map, neighbor)) return;
    } else {
      // 如果不是辅助位姿信息，尝试获取与邻居共享的正常位姿字典 若没有初始化轨迹则return
      // 测量数据中对应自身的“X”中的ID-frame ID-pose
      if (!getSharedPoseDictWithNeighbor(map, neighbor)) return;
    }
    // 如果共享的位姿字典为空，则跳过当前邻居
    if (map.empty()) continue;

    // 创建PublicPoses消息，并填充消息头信息
    PublicPoses msg;
    msg.robot_id = getID();
    msg.cluster_id = getClusterID();
    msg.destination_robot_id = neighbor;
    msg.instance_number = instance_number();
    msg.iteration_number = iteration_number();
    msg.is_auxiliary = aux;

    // 遍历共享的位姿字典，将位姿信息添加到消息中
    for (const auto &sharedPose : map) {
      const PoseID nID = sharedPose.first;
      const auto &matrix = sharedPose.second.getData();
      // 确保当前处理的位姿ID与当前机器人的ID一致
      CHECK_EQ(nID.robot_id, getID());
      // 将帧ID添加到消息的位姿ID中
      msg.pose_ids.push_back(nID.frame_id);
      // 将位姿矩阵转换为消息格式，并添加到消息中
      msg.poses.push_back(MatrixToMsg(matrix));
    }
    // 发布PublicPoses消息
    mPublicPosesPublisher.publish(msg);
  }
}

void PGOAgentROS::publishPublicMeasurements() {
  if (!mParamsROS.synchronizeMeasurements) {
    // Do not publish shared measurements
    // when assuming measurements are already synched
    return;
  }
  std::map<unsigned, RelativeMeasurementList> msg_map;
  for (unsigned robot_id = 0; robot_id < mParams.numRobots; ++robot_id) {
    RelativeMeasurementList msg;
    msg.from_robot = getID();
    msg.from_cluster = getClusterID();
    msg.to_robot = robot_id;
    msg_map[robot_id] = msg;
  }
  for (const auto &m : mPoseGraph->sharedLoopClosures()) {
    unsigned otherID = 0;
    if (m.r1 == getID()) {
      otherID = m.r2;
    } else {
      otherID = m.r1;
    }
    CHECK(msg_map.find(otherID) != msg_map.end());
    const auto edge = RelativeMeasurementToMsg(m);
    msg_map[otherID].edges.push_back(edge);
  }
  for (unsigned robot_id = 0; robot_id < mParams.numRobots; ++robot_id)
    mPublicMeasurementsPublisher.publish(msg_map[robot_id]);
}

void PGOAgentROS::publishMeasurementWeights() {
  // if (mState != PGOAgentState::INITIALIZED) return;

  std::map<unsigned, RelativeMeasurementWeights> msg_map;
  for (const auto &m : mPoseGraph->sharedLoopClosures()) {
    unsigned otherID = 0;
    if (m.r1 == getID()) {
      otherID = m.r2;
    } else {
      otherID = m.r1;
    }
    if (otherID > getID()) {
      if (msg_map.find(otherID) == msg_map.end()) {
        RelativeMeasurementWeights msg;
        msg.robot_id = getID();
        msg.cluster_id = getClusterID();
        msg.destination_robot_id = otherID;
        msg_map[otherID] = msg;
      }
      msg_map[otherID].src_robot_ids.push_back(m.r1);
      msg_map[otherID].dst_robot_ids.push_back(m.r2);
      msg_map[otherID].src_pose_ids.push_back(m.p1);
      msg_map[otherID].dst_pose_ids.push_back(m.p2);
      msg_map[otherID].weights.push_back(m.weight);
      msg_map[otherID].fixed_weights.push_back(m.fixedWeight);
    }
  }
  for (const auto &it : msg_map) {
    const auto &msg = it.second;
    if (!msg.weights.empty()) {
      mMeasurementWeightsPublisher.publish(msg);
    }
  }
}

void PGOAgentROS::storeLoopClosureMarkers() {
  if (mState != PGOAgentState::INITIALIZED) return;
  double weight_tol = mParamsROS.weightConvergenceThreshold;
  visualization_msgs::Marker line_list;
  line_list.id = (int)getID();
  line_list.type = visualization_msgs::Marker::LINE_LIST;
  line_list.scale.x = 0.1;
  line_list.header.frame_id = "/world";
  line_list.color.a = 1.0;
  line_list.pose.orientation.x = 0.0;
  line_list.pose.orientation.y = 0.0;
  line_list.pose.orientation.z = 0.0;
  line_list.pose.orientation.w = 1.0;
  line_list.action = visualization_msgs::Marker::ADD;
  for (const auto &measurement : mPoseGraph->privateLoopClosures()) {
    Matrix T1, T2, t1, t2;
    bool b1, b2;
    geometry_msgs::Point p1, p2;
    b1 = getPoseInGlobalFrame(measurement.p1, T1);
    b2 = getPoseInGlobalFrame(measurement.p2, T2);
    if (b1 && b2) {
      t1 = T1.block(0, d, d, 1);
      t2 = T2.block(0, d, d, 1);
      p1.x = t1(0);
      p1.y = t1(1);
      p1.z = t1(2);
      p2.x = t2(0);
      p2.y = t2(1);
      p2.z = t2(2);
      line_list.points.push_back(p1);
      line_list.points.push_back(p2);
      std_msgs::ColorRGBA line_color;
      line_color.a = 1;
      if (measurement.weight > 1 - weight_tol) {
        line_color.g = 1;
      } else if (measurement.weight < weight_tol) {
        line_color.r = 1;
      } else {
        line_color.b = 1;
      }
      line_list.colors.push_back(line_color);
      line_list.colors.push_back(line_color);
    }
  }
  for (const auto &measurement : mPoseGraph->sharedLoopClosures()) {
    Matrix mT, nT;
    Matrix mt, nt;
    bool mb, nb;
    unsigned neighbor_id;
    if (measurement.r1 == getID()) {
      neighbor_id = measurement.r2;
      mb = getPoseInGlobalFrame(measurement.p1, mT);
      nb = getNeighborPoseInGlobalFrame(measurement.r2, measurement.p2, nT);
    } else {
      neighbor_id = measurement.r1;
      mb = getPoseInGlobalFrame(measurement.p2, mT);
      nb = getNeighborPoseInGlobalFrame(measurement.r1, measurement.p1, nT);
    }
    if (mb && nb) {
      mt = mT.block(0, d, d, 1);
      nt = nT.block(0, d, d, 1);
      geometry_msgs::Point mp, np;
      mp.x = mt(0);
      mp.y = mt(1);
      mp.z = mt(2);
      np.x = nt(0);
      np.y = nt(1);
      np.z = nt(2);
      line_list.points.push_back(mp);
      line_list.points.push_back(np);
      std_msgs::ColorRGBA line_color;
      line_color.a = 1;
      if (!isRobotActive(neighbor_id)) {
        // Black
      } else if (measurement.weight > 1 - weight_tol) {
        line_color.g = 1;
      } else if (measurement.weight < weight_tol) {
        line_color.r = 1;
      } else {
        line_color.b = 1;
      }
      line_list.colors.push_back(line_color);
      line_list.colors.push_back(line_color);
    }
  }
  if (!line_list.points.empty()) mCachedLoopClosureMarkers.emplace(line_list);
}

void PGOAgentROS::publishLoopClosureMarkers() {
  if (!mParamsROS.visualizeLoopClosures) {
    return;
  }
  if (mCachedLoopClosureMarkers.has_value())
    mLoopClosureMarkerPublisher.publish(mCachedLoopClosureMarkers.value());
}

bool PGOAgentROS::createIterationLog(const std::string &filename) {
  if (mIterationLog.is_open()) mIterationLog.close();
  mIterationLog.open(filename);
  if (!mIterationLog.is_open()) {
    ROS_ERROR_STREAM("Error opening log file: " << filename);
    return false;
  }
  // Robot ID, Cluster ID, global iteration number, Number of poses, total bytes
  // received, iteration time (sec), total elapsed time (sec), relative change
  mIterationLog << "robot_id, cluster_id, num_active_robots, iteration, num_poses, "
                   "bytes_received, "
                   "iter_time_sec, total_time_sec, rel_change \n";
  mIterationLog.flush();
  return true;
}

bool PGOAgentROS::logIteration() {
  if (!mParams.logData) {
    return false;
  }
  if (!mIterationLog.is_open()) {
    ROS_ERROR_STREAM("No iteration log file!");
    return false;
  }

  // Compute total elapsed time since beginning of optimization
  double globalElapsedSec = (ros::Time::now() - mGlobalStartTime).toSec();

  // Robot ID, Cluster ID, global iteration number, Number of poses, total bytes
  // received, iteration time (sec), total elapsed time (sec), relative change
  mIterationLog << getID() << ",";
  mIterationLog << getClusterID() << ",";
  mIterationLog << numActiveRobots() << ",";
  mIterationLog << iteration_number() << ",";
  mIterationLog << num_poses() << ",";
  mIterationLog << mTotalBytesReceived << ",";
  mIterationLog << mIterationElapsedMs / 1e3 << ",";
  mIterationLog << globalElapsedSec << ",";
  mIterationLog << mStatus.relativeChange << "\n";
  mIterationLog.flush();
  return true;
}

bool PGOAgentROS::logString(const std::string &str) {
  if (!mParams.logData) {
    return false;
  }
  if (!mIterationLog.is_open()) {
    ROS_WARN_STREAM("No iteration log file!");
    return false;
  }
  mIterationLog << str << "\n";
  mIterationLog.flush();
  return true;
}

void PGOAgentROS::connectivityCallback(const std_msgs::UInt16MultiArrayConstPtr &msg) {
  std::set<unsigned> connected_ids(msg->data.begin(), msg->data.end());
  for (unsigned robot_id = 0; robot_id < mParams.numRobots; ++robot_id) {
    if (robot_id == getID()) {
      mTeamConnected[robot_id] = true;
    } else if (connected_ids.find(robot_id) != connected_ids.end()) {
      mTeamConnected[robot_id] = true;
    } else {
      // ROS_WARN("Robot %u is disconnected.", robot_id);
      mTeamConnected[robot_id] = false;
    }
  }
}

void PGOAgentROS::liftingMatrixCallback(const MatrixMsgConstPtr &msg) {
  // if (mParams.verbose) {
  //   ROS_INFO("Robot %u receives lifting matrix.", getID());
  // }
  setLiftingMatrix(MatrixFromMsg(*msg));
}

void PGOAgentROS::anchorCallback(const PublicPosesConstPtr &msg) {
  if (msg->robot_id != 0 || msg->pose_ids[0] != 0) {
    ROS_ERROR("Received wrong pose as anchor!");
    return;
  }
  if (msg->cluster_id != getClusterID()) {
    return;
  }
  setGlobalAnchor(MatrixFromMsg(msg->poses[0]));
  // Print anchor error
  // if (YLift.has_value() && globalAnchor.has_value()) {
  //   const Matrix Ya = globalAnchor.value().rotation();
  //   const Matrix pa = globalAnchor.value().translation();
  //   double anchor_rotation_error = (Ya - YLift.value()).norm();
  //   double anchor_translation_error = pa.norm();
  //   ROS_INFO("Anchor rotation error=%.1e, translation error=%.1e.",
  //   anchor_rotation_error, anchor_translation_error);
  // }
}

void PGOAgentROS::statusCallback(const StatusConstPtr &msg) {
  const auto &received_msg = *msg;
  const auto &it = mTeamStatusMsg.find(msg->robot_id);
  // Ignore message with outdated timestamp
  if (it != mTeamStatusMsg.end()) {
    const auto latest_msg = it->second;
    if (latest_msg.header.stamp > received_msg.header.stamp) {
      ROS_WARN("Received outdated status from robot %u.", msg->robot_id);
      return;
    }
  }
  mTeamStatusMsg[msg->robot_id] = received_msg;

  setRobotClusterID(msg->robot_id, msg->cluster_id);
  if (msg->cluster_id == getClusterID()) {
    setNeighborStatus(statusFromMsg(received_msg));
    ;
  }

  // Edge cases in synchronous mode
  if (!mParams.asynchronous) {
    if (isLeader() && isRobotActive(msg->robot_id)) {
      bool should_deactivate = false;
      if (msg->cluster_id != getClusterID()) {
        ROS_WARN("Robot %u joined other cluster %u... set to inactive.",
                 msg->robot_id,
                 msg->cluster_id);
        should_deactivate = true;
      }
      if (iteration_number() > 0 && msg->state != Status::INITIALIZED) {
        ROS_WARN(
            "Robot %u is no longer initialized in global frame... set to inactive.",
            msg->robot_id);
        should_deactivate = true;
      }
      if (should_deactivate) {
        setRobotActive(msg->robot_id, false);
        publishActiveRobotsCommand();
      }
    }
  }
}

void PGOAgentROS::commandCallback(const CommandConstPtr &msg) {
  // 如果消息的集群ID与当前集群ID不匹配，则忽略该命令
  if (msg->cluster_id != getClusterID()) {
    ROS_WARN_THROTTLE(1,
                      "Ignore command from wrong cluster (recv %u, expect %u).",
                      msg->cluster_id,
                      getClusterID());
    return;
  }
  // Update latest command time
  // Ignore commands that are periodically published
  if (msg->command != Command::NOOP && msg->command != Command::SET_ACTIVE_ROBOTS) {
    mLastCommandTime = ros::Time::now();
  }

  switch (msg->command) {
    /*
    该命令用于请求最新的位姿图。以下是详细步骤：

    集群ID检查：如果消息的集群ID与当前集群ID不匹配，则忽略该命令。
    领导者检查：如果发布该命令的机器人不是集群领导者，则忽略该命令。
    状态检查：如果当前状态不是WAIT_FOR_DATA，则重置机器人状态。

    更新活动机器人：更新当前活动机器人的列表。
    请求位姿图：向自身请求最新的位姿图。
    日志记录：如果启用了日志记录且成功接收到位姿图，则创建一个新的日志文件。
    发布状态：发布机器人的状态。
    初始化：如果机器人是领导者，发布锚点和初始化命令。
    */
    case Command::REQUEST_POSE_GRAPH: {
      // 如果发布该命令的机器人不是集群领导者，则忽略该命令
      if (msg->publishing_robot != getClusterID()) {
        ROS_WARN("Ignore REQUEST_POSE_GRAPH command from non-leader %u.",
                 msg->publishing_robot);
        return;
      }
      ROS_INFO("Robot %u received REQUEST_POSE_GRAPH command.", getID());
      // 如果当前状态不是等待数据，则重置
      if (mState != PGOAgentState::WAIT_FOR_DATA) {
        ROS_WARN_STREAM("Robot " << getID()
                                 << " status is not WAIT_FOR_DATA. Reset...");
        reset();
      }
      // Update local record of currently active robots
      updateActiveRobots(msg);
      // Request latest pose graph
      bool received_pose_graph = requestPoseGraph();
      // Create log file for new round
      if (mParams.logData && received_pose_graph) {
        auto time_since_launch = ros::Time::now() - mLaunchTime;
        int sec_since_launch = int(time_since_launch.toSec());
        std::string log_path = mParams.logDirectory + "dpgo_log_" +
                               std::to_string(sec_since_launch) + ".csv";
        createIterationLog(log_path);
      }
      publishStatus();
      // Enter initialization round
      if (isLeader()) {
        if (!received_pose_graph) {
          publishHardTerminateCommand();
        } else {
          publishAnchor();
          publishInitializeCommand();
        }
      }
      break;
    }

    /*
    该命令用于终止当前操作。以下是详细步骤：

    记录命令：记录该命令。
    活动检查：如果机器人不活跃，则重置其状态。
    记录字符串：记录"TERMINATE"字符串。
    闭环处理：如果运行分布式GNC，修正已收敛的闭环。
    存储数据：存储优化后的轨迹、闭环标记、活动邻居位姿和活动边权重。
    发布数据：发布优化后的轨迹和闭环标记。
    重置：重置机器人的状态。
    */
    case Command::TERMINATE: {
      ROS_INFO("Robot %u received TERMINATE command. ", getID());
      // 如果当前机器人不活跃，则重置
      if (!isRobotActive(getID())) {
        reset();
        break;
      }
      logString("TERMINATE");
      // When running distributed GNC, fix loop closures that have converged
      if (mParams.robustCostParams.costType == RobustCostParameters::Type::GNC_TLS) {
        double residual = 0;
        double weight = 0;
        for (auto &m : mPoseGraph->activeLoopClosures()) {
          if (!m->fixedWeight && computeMeasurementResidual(*m, &residual)) {
            weight = mRobustCost.weight(residual);
            if (weight < mParamsROS.weightConvergenceThreshold) {
              ROS_INFO("Reject measurement with residual %f and weight %f.",
                       residual,
                       weight);
              m->weight = 0;
              m->fixedWeight = true;
            }
          }
        }
        const auto stat = mPoseGraph->statistics();
        ROS_INFO(
            "Robot %u loop closure statistics:\n "
            "accepted: %f\n "
            "rejected: %f\n "
            "undecided: %f\n",
            mID,
            stat.accept_loop_closures,
            stat.reject_loop_closures,
            stat.undecided_loop_closures);
        publishMeasurementWeights();
      }

      // Store and publish optimized trajectory in global frame
      storeOptimizedTrajectory();
      storeLoopClosureMarkers();
      storeActiveNeighborPoses();
      storeActiveEdgeWeights();

      randomSleep(0.1, 5);
      publishOptimizedTrajectory();
      publishLoopClosureMarkers();
      reset();
      break;
    }

    /*
    该命令用于强制终止当前操作。以下是详细步骤：

    记录命令：记录该命令。
    重置：重置机器人的状态。
    */
    case Command::HARD_TERMINATE: {
      ROS_INFO("Robot %u received HARD TERMINATE command. ", getID());
      logString("HARD_TERMINATE");
      reset();
      break;
    }

    /*
    Command::INITIALIZE
    该命令用于初始化操作。以下是详细步骤：

    领导者检查：如果发布该命令的机器人不是集群领导者，则忽略该命令。
    记录全局开始时间：记录全局开始时间。

    发布公共测量数据：发布公共测量数据。
    发布公共位姿：发布公共位姿。
    发布状态：发布机器人的状态。
    领导者操作：如果机器人是领导者，发布提升矩阵和活动机器人命令，并检查所有机器人的状态。如果所有机器人都已初始化，则开始分布式优化，否则继续等待更多机器人初始化。
    */
    case Command::INITIALIZE: {
      // TODO: ignore if status == WAIT_FOR_DATA
      if (msg->publishing_robot != getClusterID()) {
        ROS_WARN("Ignore INITIALIZE command from non-leader %u.",
                 msg->publishing_robot);
        return;
      }
      mGlobalStartTime = ros::Time::now();
      publishPublicMeasurements();
      publishPublicPoses(false);
      publishStatus();
      if (isLeader()) {
        publishLiftingMatrix();
        // updateActiveRobots();
        publishActiveRobotsCommand();
        ros::Duration(0.1).sleep();

        // Check the status of all robots
        bool all_initialized = true;
        int num_initialized_robots = 0;
        for (unsigned robot_id = 0; robot_id < mParams.numRobots; ++robot_id) {
          if (!isRobotActive(robot_id)) {
            // Ignore inactive robots
            continue;
          }
          if (!hasNeighborStatus(robot_id)) {
            ROS_WARN("Robot %u status not available.", robot_id);
            all_initialized = false;
            continue;
          }
          const auto status = getNeighborStatus(robot_id);
          if (status.state == PGOAgentState::WAIT_FOR_DATA) {
            ROS_WARN("Robot %u has not received pose graph.", status.agentID);
            all_initialized = false;
          } else if (status.state == PGOAgentState::WAIT_FOR_INITIALIZATION) {
            ROS_WARN("Robot %u has not initialized in global frame.", status.agentID);
            all_initialized = false;
          } else if (status.state == PGOAgentState::INITIALIZED) {
            num_initialized_robots++;
          }
        }

        if (!all_initialized && mInitStepsDone <= mParamsROS.maxDistributedInitSteps) {
          // Keep waiting for more robots to initialize
          mPublishInitializeCommandRequested = true;
          return;
        } else {
          // Start distributed optimization if more than 1 robot is initialized
          if (num_initialized_robots > 1) {
            ROS_INFO("Start distributed optimization with %i/%zu active robots.",
                     num_initialized_robots,
                     numActiveRobots());
            // Set robots that are not initialized to inactive
            for (unsigned int robot_id = 0; robot_id < mParams.numRobots; ++robot_id) {
              if (isRobotActive(robot_id) && isRobotInitialized(robot_id) &&
                  isRobotConnected(robot_id)) {
                setRobotActive(robot_id, true);
              } else {
                setRobotActive(robot_id, false);
              }
            }
            publishActiveRobotsCommand();
            publishUpdateCommand(getID());  // Kick off optimization
          } else {
            ROS_WARN("Not enough robots initialized.");
            publishHardTerminateCommand();
          }
        }
      }
      break;
    }

    /*
    该命令用于更新操作。迭代号（iteration
    number）用于跟踪优化过程中的每一步。每个机器人在每次迭代中都会执行一些计算，并将结果与其他机器人同步。以下是详细步骤：

    异步检查：检查是否为异步模式。
    活动检查：如果机器人不活跃，则忽略该命令。
    状态检查：如果机器人未初始化，则忽略该命令。

    更新本地记录：更新本地记录的执行机器人和执行迭代。
    迭代检查：检查接收到的迭代是否与本地迭代匹配。
    优化请求：如果执行机器人是当前机器人，请求同步优化，否则立即迭代并发布状态。
    */
    case Command::UPDATE: {
      CHECK(!mParams.asynchronous);
      // Handle case when this robot is not active
      if (!isRobotActive(getID())) {
        ROS_WARN_STREAM("Robot " << getID()
                                 << " is deactivated. Ignore update command... ");
        return;
      }
      // Handle edge case when robots are out of sync
      if (mState != PGOAgentState::INITIALIZED) {
        ROS_WARN_STREAM("Robot " << getID()
                                 << " is not initialized. Ignore update command...");
        return;
      }
      // Update local record
      mTeamIterRequired[msg->executing_robot] = msg->executing_iteration;
      if (msg->executing_iteration != iteration_number() + 1) {
        ROS_WARN(
            "Update iteration does not match local iteration. (received: %u, local: "
            "%u)",
            msg->executing_iteration,
            iteration_number() + 1);
      }
      if (msg->executing_robot == getID()) {
        mSynchronousOptimizationRequested = true;
        if (mParams.verbose)
          ROS_INFO(
              "Robot %u to update at iteration %u.", getID(), msg->executing_iteration);
      } else {
        // Agents that are not selected for optimization can iterate immediately
        iterate(false);
        publishStatus();
      }
      break;
    }

    /*
    该命令用于恢复操作。以下是详细步骤：

    异步检查：检查是否为异步模式。
    活动检查：如果机器人不活跃或未初始化，则忽略该命令。
    重置迭代号：重置迭代号并取消同步优化请求。
    更新邻居状态：更新所有邻居的迭代要求和接收计数器。
    领导者操作：如果机器人是领导者，发布更新(UPDATE)命令。
    */
    case Command::RECOVER: {
      CHECK(!mParams.asynchronous);
      if (!isRobotActive(getID()) || mState != PGOAgentState::INITIALIZED) {
        return;
      }
      mIterationNumber = msg->executing_iteration;
      mSynchronousOptimizationRequested = false;
      for (const auto &neighbor : getNeighbors()) {
        mTeamIterRequired[neighbor] = iteration_number();
        mTeamIterReceived[neighbor] =
            0;  // Force robot to wait for updated public poses from neighbors
      }
      ROS_WARN("Robot %u received RECOVER command and reset iteration number to %u.",
               getID(),
               iteration_number());

      if (isLeader()) {
        ROS_WARN("Leader %u publishes update command.", getID());
        publishUpdateCommand(getID());
      }
      break;
    }

    /*
    该命令用于更新测量权重。以下是详细步骤：

    异步检查：检查是否为异步模式。
    活动检查：如果机器人不活跃，则忽略该命令。
    记录命令：记录"UPDATE_WEIGHT"字符串。
    更新测量权重：更新测量权重。
    要求最新迭代：要求所有邻居提供最新迭代。
    发布权重：发布测量权重和公共位姿。
    领导者操作：如果机器人是领导者，发布更新命令。
    */
    case Command::UPDATE_WEIGHT: {
      CHECK(!mParams.asynchronous);
      if (!isRobotActive(getID())) {
        ROS_WARN_STREAM(
            "Robot " << getID() << " is deactivated. Ignore UPDATE_WEIGHT command... ");
        return;
      }
      logString("UPDATE_WEIGHT");
      updateMeasurementWeights();
      // Require latest iterations from all neighbor robots
      ROS_WARN("Require latest iteration %d from all neighbors.", iteration_number());
      for (const auto &neighbor : getNeighbors()) {
        mTeamIterRequired[neighbor] = iteration_number();
      }
      publishMeasurementWeights();
      publishPublicPoses(false);
      if (mParams.acceleration) publishPublicPoses(true);
      publishStatus();
      // The first resumes optimization by sending UPDATE command
      if (isLeader()) {
        publishUpdateCommand();
      }
      break;
    }

    /*
    该命令用于设置活动机器人。以下是详细步骤：

    领导者检查：如果发布该命令的机器人不是集群领导者，则忽略该命令。
    更新活动机器人：更新当前活动机器人的列表。
    */
    case Command::SET_ACTIVE_ROBOTS: {
      if (msg->publishing_robot != getClusterID()) {
        ROS_WARN("Ignore SET_ACTIVE_ROBOTS command from non-leader %u.",
                 msg->publishing_robot);
        return;
      }
      // Update local record of currently active robots
      updateActiveRobots(msg);
      break;
    }

    case Command::NOOP: {
      // Do nothing
      break;
    }

    default:
      ROS_ERROR("Invalid command!");
  }
}

void PGOAgentROS::publicPosesCallback(const PublicPosesConstPtr &msg) {
  // Discard message sent by robots in other clusters
  if (msg->cluster_id != getClusterID()) {
    return;
  }

  std::vector<unsigned> neighbors = getNeighbors();
  if (std::find(neighbors.begin(), neighbors.end(), msg->robot_id) == neighbors.end()) {
    // Discard messages send by non-neighbors
    return;
  }

  // Generate a random permutation of indices
  PoseDict poseDict;
  for (size_t index = 0; index < msg->pose_ids.size(); ++index) {
    const PoseID nID(msg->robot_id, msg->pose_ids.at(index));
    const auto matrix = MatrixFromMsg(msg->poses.at(index));
    poseDict.emplace(nID, matrix);
  }
  if (!msg->is_auxiliary) {
    updateNeighborPoses(msg->robot_id, poseDict);
  } else {
    updateAuxNeighborPoses(msg->robot_id, poseDict);
  }

  // Update local bookkeeping
  mTeamIterReceived[msg->robot_id] = msg->iteration_number;
  mTotalBytesReceived += computePublicPosesMsgSize(*msg);
}

void PGOAgentROS::publicMeasurementsCallback(
    const RelativeMeasurementListConstPtr &msg) {
  // Ignore if message not addressed to this robot
  if (msg->to_robot != getID()) {
    return;
  }
  // Ignore if does not have local odometry
  if (mPoseGraph->numOdometry() == 0) return;
  // Ignore if already received inter-robot loop closures from this robot
  if (mTeamReceivedSharedLoopClosures[msg->from_robot]) return;
  // Ignore if from another cluster
  if (msg->from_cluster != getClusterID()) return;
  mTeamReceivedSharedLoopClosures[msg->from_robot] = true;

  // Add inter-robot loop closures that involve this robot
  const auto num_before = mPoseGraph->numSharedLoopClosures();
  for (const auto &e : msg->edges) {
    if (e.robot_from == (int)getID() || e.robot_to == (int)getID()) {
      const auto measurement = RelativeMeasurementFromMsg(e);
      addMeasurement(measurement);
    }
  }
  const auto num_after = mPoseGraph->numSharedLoopClosures();
  ROS_INFO(
      "Robot %u received measurements from %u: "
      "added %u missing measurements.",
      getID(),
      msg->from_robot,
      num_after - num_before);
}

void PGOAgentROS::measurementWeightsCallback(
    const RelativeMeasurementWeightsConstPtr &msg) {
  // if (mState != PGOAgentState::INITIALIZED) return;
  if (msg->destination_robot_id != getID()) return;
  if (msg->cluster_id != getClusterID()) return;
  bool weights_updated = false;
  for (size_t k = 0; k < msg->weights.size(); ++k) {
    const unsigned robotSrc = msg->src_robot_ids[k];
    const unsigned robotDst = msg->dst_robot_ids[k];
    const unsigned poseSrc = msg->src_pose_ids[k];
    const unsigned poseDst = msg->dst_pose_ids[k];
    const PoseID srcID(robotSrc, poseSrc);
    const PoseID dstID(robotDst, poseDst);
    double w = msg->weights[k];
    bool fixed = msg->fixed_weights[k];

    unsigned otherID;
    if (robotSrc == getID() && robotDst != getID()) {
      otherID = robotDst;
    } else if (robotDst == getID() && robotSrc != getID()) {
      otherID = robotSrc;
    } else {
      ROS_ERROR("Received weight for irrelevant measurement!");
      continue;
    }
    if (!isRobotActive(otherID)) continue;
    if (otherID < getID()) {
      if (setMeasurementWeight(srcID, dstID, w, fixed))
        weights_updated = true;
      else {
        ROS_WARN("Cannot find specified shared loop closure (%u, %u) -> (%u, %u)",
                 robotSrc,
                 poseSrc,
                 robotDst,
                 poseDst);
      }
    }
  }
  if (weights_updated) {
    // Need to recompute data matrices in the pose graph
    mPoseGraph->clearDataMatrices();
  }
}

void PGOAgentROS::timerCallback(const ros::TimerEvent &event) {
  publishNoopCommand();
  publishLiftingMatrix();
  if (mPublishInitializeCommandRequested) {
    publishInitializeCommand();
  }
  if (mTryInitializeRequested) {
    tryInitialize();
  }
  if (mState == PGOAgentState::WAIT_FOR_DATA) {
    // Update leader robot when idle
    updateCluster();
    // Initialize a new round of dpgo
    int elapsed_sec = (ros::Time::now() - mLastResetTime).toSec();
    if (isLeader() && elapsed_sec > 10) {
      publishRequestPoseGraphCommand();
    }
  }
  if (mState == PGOAgentState::INITIALIZED) {
    publishPublicPoses(false);
    if (mParamsROS.acceleration) publishPublicPoses(true);
    publishMeasurementWeights();
    if (isLeader()) {
      publishAnchor();
      publishActiveRobotsCommand();
    }
  }
  publishStatus();
}

void PGOAgentROS::visualizationTimerCallback(const ros::TimerEvent &event) {
  publishOptimizedTrajectory();
  publishLoopClosureMarkers();
}

void PGOAgentROS::storeActiveNeighborPoses() {
  Matrix matrix;
  int num_poses_stored = 0;
  for (const auto &nbr_pose_id : mPoseGraph->activeNeighborPublicPoseIDs()) {
    if (getNeighborPoseInGlobalFrame(
            nbr_pose_id.robot_id, nbr_pose_id.frame_id, matrix)) {
      Pose T(dimension());
      T.setData(matrix);
      mCachedNeighborPoses[nbr_pose_id] = T;
      num_poses_stored++;
    }
  }
  ROS_INFO("Stored %i neighbor poses in world frame.", num_poses_stored);
}

void PGOAgentROS::setInactiveNeighborPoses() {
  if (!YLift) {
    ROS_WARN("Missing lifting matrix! Cannot apply neighbor poses.");
    return;
  }
  int num_poses_initialized = 0;
  for (const auto &it : mCachedNeighborPoses) {
    const auto &pose_id = it.first;
    // Active neighbors will transmit their poses
    // Therefore we only use stored poses for inactive neighbors
    if (!isRobotActive(pose_id.robot_id)) {
      const auto &Ti = it.second;
      Matrix Xi_mat = YLift.value() * Ti.getData();
      LiftedPose Xi(r, d);
      Xi.setData(Xi_mat);
      neighborPoseDict[pose_id] = Xi;
      num_poses_initialized++;
    }
  }
  ROS_INFO("Set %i inactive neighbor poses.", num_poses_initialized);
}

void PGOAgentROS::storeActiveEdgeWeights() {
  int num_edges_stored = 0;
  for (const RelativeSEMeasurement *m : mPoseGraph->activeLoopClosures()) {
    const PoseID src_id(m->r1, m->p1);
    const PoseID dst_id(m->r2, m->p2);
    const EdgeID edge_id(src_id, dst_id);
    if (edge_id.isSharedLoopClosure()) {
      mCachedEdgeWeights[edge_id] = m->weight;
      num_edges_stored++;
    }
  }
  ROS_INFO("Stored %i active edge weights.", num_edges_stored);
}

void PGOAgentROS::setInactiveEdgeWeights() {
  int num_edges_set = 0;
  for (RelativeSEMeasurement *m : mPoseGraph->inactiveLoopClosures()) {
    const PoseID src_id(m->r1, m->p1);
    const PoseID dst_id(m->r2, m->p2);
    const EdgeID edge_id(src_id, dst_id);
    const auto &it = mCachedEdgeWeights.find(edge_id);
    if (it != mCachedEdgeWeights.end()) {
      m->weight = it->second;
      num_edges_set++;
    }
  }
  ROS_INFO("Set %i inactive edge weights.", num_edges_set);
}

void PGOAgentROS::initializeGlobalAnchor() {
  if (!YLift) {
    ROS_WARN("Missing lifting matrix! Cannot initialize global anchor.");
    return;
  }
  LiftedPose X(r, d);
  X.rotation() = YLift.value();
  X.translation() = Vector::Zero(r);
  setGlobalAnchor(X.getData());
  ROS_INFO("Initialized global anchor.");
}
// Get the ID of the current cluster
unsigned PGOAgentROS::getClusterID() const { return mClusterID; }

bool PGOAgentROS::isLeader() const { return getID() == getClusterID(); }

void PGOAgentROS::updateCluster() {
  for (unsigned int robot_id = 0; robot_id < mParams.numRobots; ++robot_id) {
    if (isRobotConnected(robot_id)) {
      mClusterID = robot_id;
      break;
    }
  }
  ROS_INFO("Robot %u joins cluster %u.", getID(), mClusterID);
}

unsigned PGOAgentROS::getRobotClusterID(unsigned robot_id) const {
  if (robot_id > mParams.numRobots) {
    ROS_ERROR("Robot ID %u larger than number of robots.", robot_id);
    return robot_id;
  }
  return mTeamClusterID[robot_id];
}

void PGOAgentROS::setRobotClusterID(unsigned robot_id, unsigned cluster_id) {
  if (robot_id > mParams.numRobots) {
    ROS_ERROR("Robot ID %u larger than number of robots.", robot_id);
    return;
  }
  if (cluster_id > mParams.numRobots) {
    ROS_ERROR("Cluster ID %u larger than number of robots.", cluster_id);
    return;
  }
  mTeamClusterID[robot_id] = cluster_id;
}

void PGOAgentROS::resetRobotClusterIDs() {
  mTeamClusterID.assign(mParams.numRobots, 0);
  for (unsigned robot_id = 0; robot_id < mParams.numRobots; ++robot_id) {
    mTeamClusterID[robot_id] = robot_id;
  }
}

void PGOAgentROS::checkTimeout() {
  // 只用于同步模式
  if (mParams.asynchronous) {
    return;
  }

  // Timeout if command channel quiet for long time
  // This usually happen when robots get disconnected
  double elapsedSecond = (ros::Time::now() - mLastCommandTime).toSec();
  if (elapsedSecond > mParamsROS.timeoutThreshold) {
    // 若轨迹初始化完成且迭代次数大于0
    if (mState == PGOAgentState::INITIALIZED && iteration_number() > 0) {
      ROS_WARN("Robot %u timeout during optimization: last command was %.1f sec ago.",
               getID(),
               elapsedSecond);
      // 如果是领导者机器人(ID=cluster_ID)
      if (isLeader()) {
        // 若存在其他断开的机器人
        if (checkDisconnectedRobot()) {
          // 重新发布集群中活跃机器人的指令
          publishActiveRobotsCommand();
          ros::Duration(3).sleep();
        }
        ROS_WARN("Number of active robots: %zu.", numActiveRobots());
        if (numActiveRobots() > 1) {
          // 若活跃的机器人数大于1
          if (mParamsROS.enableRecovery) {
            // ROS_WARN("Attempt to resume optimization with %zu robots.",
            // numActiveRobots()); 若允许恢复，发布恢复指令
            publishRecoverCommand();
          } else {
            // ROS_WARN("Terminate with %zu robots.", numActiveRobots());
            // 若不允许恢复，发布硬终止指令
            publishHardTerminateCommand();
          }
        } else {
          // ROS_WARN("Terminate... Not enough active robots.");
          // 若活跃的机器人数=1即自己，发布硬终止指令
          publishHardTerminateCommand();
        }
      } else {
        if (!isRobotConnected(getClusterID())) {
          // 若自己不是领导者机器人，但是与自己所在的集群中的领导者机器人断开连接
          ROS_WARN("Disconnected from current cluster... reset.");
          reset();
        }
      }
    } else {
      reset();
      if (isLeader()) {
        publishHardTerminateCommand();
      }
    }
    mLastCommandTime = ros::Time::now();
  }

  // Check hard timeout
  if (mState == PGOAgentState::INITIALIZED && iteration_number() > 0) {
    if (mLastUpdateTime.has_value()) {
      double sec_idle = (ros::Time::now() - mLastUpdateTime.value()).toSec();
      if (sec_idle > 1) {
        ROS_WARN_THROTTLE(
            1, "Robot %u last successful update is %.1f sec ago.", getID(), sec_idle);
      }
      if (sec_idle > 3 * mParamsROS.timeoutThreshold) {
        ROS_ERROR("Hard timeout!");
        logString("TIMEOUT");
        if (isLeader()) publishHardTerminateCommand();
        reset();
      }
    }
  }
}

/**
 * 检查并处理断开连接的机器人
 *
 * 此函数遍历所有机器人，识别出当前活跃但已断开连接的机器人，并记录断开连接的状态
 * 如果发现机器人断开连接，会打印警告信息并将其设为非活跃状态
 *
 * @return bool 表示是否有机器人在检查期间断开连接
 */
bool PGOAgentROS::checkDisconnectedRobot() {
  // 初始化一个标志变量，用于指示是否有机器人断开连接
  bool robot_disconnected = false;

  // 遍历所有机器人（除开自己，因为自己一定能连接上）
  for (unsigned robot_id = 0; robot_id < mParams.numRobots; ++robot_id) {
    // 检查其他机器人是否活跃且已断开连接
    if (isRobotActive(robot_id) && !isRobotConnected(robot_id)) {
      ROS_WARN("Active robot %u is disconnected.", robot_id);
      // 断开连接后将该机器人的活跃状态设置为false，以防止进一步的操作
      setRobotActive(robot_id, false);
      // 设置标志变量为true，表示有机器人断开连接
      robot_disconnected = true;
    }
  }

  // 返回是否有机器人断开连接的状态
  return robot_disconnected;
}

}  // namespace dpgo_ros
