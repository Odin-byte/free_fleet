/*
 * Copyright (C) 2019 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "utilities.hpp"
#include "ClientNode.hpp"
#include "ClientNodeConfig.hpp"
#include <iostream>
#include <vector>

namespace free_fleet
{
namespace ros1
{

ClientNode::SharedPtr ClientNode::make(const ClientNodeConfig& _config)
{
  SharedPtr client_node = SharedPtr(new ClientNode(_config));
  client_node->node.reset(new ros::NodeHandle(_config.robot_name + "_node"));

  /// Starting the free fleet client
  ClientConfig client_config = _config.get_client_config();
  Client::SharedPtr client = Client::make(client_config);
  if (!client)
    return nullptr;

  /// Setting up the move base action client, wait for server
  ROS_INFO("waiting for connection with move base action server: %s",
      _config.move_base_server_name.c_str());
  MoveBaseClientSharedPtr move_base_client(
      new MoveBaseClient(_config.move_base_server_name, true));
  if (!move_base_client->waitForServer(ros::Duration(_config.wait_timeout)))
  {
    ROS_ERROR("timed out waiting for action server: %s",
        _config.move_base_server_name.c_str());
    return nullptr;
  }
  ROS_INFO("connected with move base action server: %s",
      _config.move_base_server_name.c_str());

  /// Setting up the docking server client, if required, wait for server
  std::unique_ptr<ros::ServiceClient> docking_SetString_client = nullptr;
  if (_config.docking_set_string_server_name != "")
  {
    docking_SetString_client =
      std::make_unique<ros::ServiceClient>(
        client_node->node->serviceClient<cob_srvs::SetString>(
          _config.docking_set_string_server_name, false));
    if (!docking_SetString_client->waitForExistence(
      ros::Duration(_config.wait_timeout)))
    {
      ROS_ERROR("timed out waiting for docking SetString server: %s",
        _config.docking_set_string_server_name.c_str());
      return nullptr;
    }
    else
    {
      ROS_INFO("connected with docking service: %s", _config.docking_set_string_server_name.c_str());
    }
  }
  
  /// Setting up the undocking server client, if required, wait for server
  std::unique_ptr<ros::ServiceClient> undocking_SetString_client = nullptr;
  if (_config.undocking_set_string_server_name != "")
  {
    undocking_SetString_client =
      std::make_unique<ros::ServiceClient>(
        client_node->node->serviceClient<cob_srvs::SetString>(
          _config.undocking_set_string_server_name, false));
    if (!undocking_SetString_client->waitForExistence(
      ros::Duration(_config.wait_timeout)))
    {
      ROS_ERROR("timed out waiting for undocking SetString server: %s",
        _config.undocking_set_string_server_name.c_str());
      return nullptr;
    }
    else
    {
      ROS_INFO("connected with docking service: %s", _config.undocking_set_string_server_name.c_str());
    }
  }

  /// Setting up the tool cmd client, if required, wait for server
  std::unique_ptr<ros::ServiceClient> tool_SetString_client = nullptr;
  if (_config.tool_cmd_set_string_server_name != "")
  {
    tool_SetString_client =
      std::make_unique<ros::ServiceClient>(
        client_node->node->serviceClient<cob_srvs::SetString>(
          _config.tool_cmd_set_string_server_name, false));
    if (!tool_SetString_client->waitForExistence(
      ros::Duration(_config.wait_timeout)))
    {
      ROS_ERROR("timed out waiting for tool cmd SetString server: %s",
        _config.tool_cmd_set_string_server_name.c_str());
      return nullptr;
    }
    else
    {
      ROS_INFO("connected with tool cmd service: %s", _config.tool_cmd_set_string_server_name.c_str());
    }
  }

  client_node->start(Fields{
      std::move(client),
      std::move(move_base_client),
      std::move(docking_SetString_client),
      std::move(undocking_SetString_client),
      std::move(tool_SetString_client)
  });

  return client_node;
}

ClientNode::ClientNode(const ClientNodeConfig& _config) :
  tf2_listener(tf2_buffer),
  client_node_config(_config)
{}

ClientNode::~ClientNode()
{
  if (update_thread.joinable())
  {
    update_thread.join();
    ROS_INFO("Client: update_thread joined.");
  }

  if (publish_thread.joinable())
  {
    publish_thread.join();
    ROS_INFO("Client: publish_thread joined.");
  }
}

void ClientNode::start(Fields _fields)
{
  fields = std::move(_fields);

  update_rate.reset(new ros::Rate(client_node_config.update_frequency));
  publish_rate.reset(new ros::Rate(client_node_config.publish_frequency));

  battery_sub = node->subscribe(
      client_node_config.battery_state_topic, 1,
      &ClientNode::battery_state_callback_fn, this);

  request_error = false;
  emergency = false;
  paused = false;

  ROS_INFO("Client: starting update thread.");
  update_thread = std::thread(std::bind(&ClientNode::update_thread_fn, this));

  ROS_INFO("Client: starting publish thread.");
  publish_thread = 
      std::thread(std::bind(&ClientNode::publish_thread_fn, this));
}

void ClientNode::print_config()
{
  client_node_config.print_config();
}

void ClientNode::battery_state_callback_fn(
    const cob_msgs::PowerState& _msg)
{
  WriteLock battery_state_lock(battery_state_mutex);
  current_battery_state = _msg;
}

bool ClientNode::get_robot_transform()
{
  try {
    geometry_msgs::TransformStamped tmp_transform_stamped = 
        tf2_buffer.lookupTransform(
            client_node_config.map_frame,
            client_node_config.robot_frame,
            ros::Time(0));
    WriteLock robot_transform_lock(robot_transform_mutex);
    previous_robot_transform = current_robot_transform;
    current_robot_transform = tmp_transform_stamped;
  }
  catch (tf2::TransformException &ex) {
    ROS_WARN("%s", ex.what());
    return false;
  }
  return true;
}

messages::RobotMode ClientNode::get_robot_mode()
{
  /// Checks if robot has just received a request that causes an adapter error
  if (request_error)
    return messages::RobotMode{messages::RobotMode::MODE_REQUEST_ERROR};

  /// Checks if robot is under emergency
  if (emergency)
    return messages::RobotMode{messages::RobotMode::MODE_EMERGENCY};
  
  // Checks if robot is currently docking
  if (docking)
    return messages::RobotMode{messages::RobotMode::MODE_DOCKING};

  // Checks if robot is currently using its tool  
  if (using_tool)
    return messages::RobotMode{messages::RobotMode::MODE_USE_TOOL};
  
  /// Checks if robot is charging
  {
    ReadLock battery_state_lock(battery_state_mutex);

    if (current_battery_state.charging == true)
      return messages::RobotMode{messages::RobotMode::MODE_CHARGING};
  }

  /// Checks if robot is moving
  {
    ReadLock robot_transform_lock(robot_transform_mutex);

    if (!is_transform_close(
        current_robot_transform, previous_robot_transform))
      return messages::RobotMode{messages::RobotMode::MODE_MOVING};
  }
  
  /// Otherwise, robot is neither charging nor moving,
  /// Checks if the robot is paused
  if (paused)
    return messages::RobotMode{messages::RobotMode::MODE_PAUSED};

  /// Otherwise, robot has queued tasks, it is paused or waiting,
  /// default to use pausing for now
  return messages::RobotMode{messages::RobotMode::MODE_IDLE};
}

void ClientNode::publish_robot_state()
{
  messages::RobotState new_robot_state;
  new_robot_state.name = client_node_config.robot_name;
  new_robot_state.model = client_node_config.robot_model;

  {
    ReadLock task_id_lock(task_id_mutex);
    new_robot_state.task_id = current_task_id;    
  }

  new_robot_state.mode = get_robot_mode();

  {
    ReadLock battery_state_lock(battery_state_mutex);
    /// RMF expects battery to have a percentage in the range for 0-100.
    new_robot_state.battery_percent = current_battery_state.relative_remaining_capacity;
  }

  {
    ReadLock robot_transform_lock(robot_transform_mutex);
    new_robot_state.location.sec = current_robot_transform.header.stamp.sec;
    new_robot_state.location.nanosec = 
        current_robot_transform.header.stamp.nsec;
    new_robot_state.location.x = 
        current_robot_transform.transform.translation.x;
    new_robot_state.location.y = 
        current_robot_transform.transform.translation.y;
    new_robot_state.location.yaw = 
        get_yaw_from_transform(current_robot_transform);
    new_robot_state.location.level_name = client_node_config.level_name;
  }

  new_robot_state.path.clear();
  {
    ReadLock goal_path_lock(goal_path_mutex);

    for (size_t i = 0; i < goal_path.size(); ++i)
    {
      new_robot_state.path.push_back(
          messages::Location{
              (int32_t)goal_path[i].goal.target_pose.header.stamp.sec,
              goal_path[i].goal.target_pose.header.stamp.nsec,
              (float)goal_path[i].goal.target_pose.pose.position.x,
              (float)goal_path[i].goal.target_pose.pose.position.y,
              (float)(get_yaw_from_quat(
                  goal_path[i].goal.target_pose.pose.orientation)),
              goal_path[i].level_name
          });
    }
  }

  if (!fields.client->send_robot_state(new_robot_state))
    ROS_WARN("failed to send robot state: msg sec %u", new_robot_state.location.sec);
}

bool ClientNode::is_valid_request(
    const std::string& _request_fleet_name,
    const std::string& _request_robot_name,
    const std::string& _request_task_id)
{
  ReadLock task_id_lock(task_id_mutex);
  if (current_task_id == _request_task_id ||
      client_node_config.robot_name != _request_robot_name ||
      client_node_config.fleet_name != _request_fleet_name)
    return false;
  return true;
}

ipa_navigation_msgs::MoveBaseGoal ClientNode::location_to_move_base_goal(
    const messages::Location& _location) const
{
  ipa_navigation_msgs::MoveBaseGoal goal;
  goal.target_pose.header.frame_id = client_node_config.map_frame;
  goal.target_pose.header.stamp.sec = _location.sec;
  goal.target_pose.header.stamp.nsec = _location.nanosec;
  goal.target_pose.pose.position.x = _location.x;
  goal.target_pose.pose.position.y = _location.y;
  goal.target_pose.pose.position.z = 0.0; // TODO: handle Z height with level
  goal.target_pose.pose.orientation = get_quat_from_yaw(_location.yaw);
  goal.parameters = "{rotational_goal_tolerance: 3.14}";

  return goal;
}

bool ClientNode::read_mode_request()
{
  messages::ModeRequest mode_request;
  std::vector<messages::ModeParameter> mode_parameters;
  if (fields.client->read_mode_request(mode_request) && 
      is_valid_request(
          mode_request.fleet_name, mode_request.robot_name, 
          mode_request.task_id))
  {
    if (mode_request.mode.mode == messages::RobotMode::MODE_PAUSED)
    {
      ROS_INFO("received a PAUSE command.");

      fields.move_base_client->cancelAllGoals();
      WriteLock goal_path_lock(goal_path_mutex);
      if (!goal_path.empty())
        goal_path[0].sent = false;

      paused = true;
      emergency = false;
    }
    else if (mode_request.mode.mode == messages::RobotMode::MODE_MOVING)
    {
      ROS_INFO("received an explicit RESUME command.");
      paused = false;
      emergency = false;
    }
    else if (mode_request.mode.mode == messages::RobotMode::MODE_EMERGENCY)
    {
      ROS_INFO("received an EMERGENCY command.");
      paused = false;
      emergency = true;
    }
    else if (mode_request.mode.mode == messages::RobotMode::MODE_DOCKING)
    {
      ROS_INFO("received a DOCKING command.");

      if (fields.docking_SetString_client)
      {
        cob_srvs::SetString SetString_srv;

        // See if there is a dock name given
        for (messages::ModeParameter const& param : mode_request.parameters)
        {
            ROS_DEBUG("Parameter name: %s, value: %s", param.name.c_str(), param.value.c_str());
            if (param.name == "docking")
            {
                ROS_INFO("Found param: %s", param.value.c_str());
                SetString_srv.request.data = param.value;
                docked_frame = param.value;
                break;
            }
        }
        docking = true;
        // publish_robot_state();
        ROS_DEBUG("Calling srv with frame_id %s", SetString_srv.request.data.c_str());
        fields.docking_SetString_client->call(SetString_srv);

        if (!SetString_srv.response.success)
        {
          ROS_ERROR("Failed to trigger docking sequence, message: %s.",
            SetString_srv.response.message.c_str());
          request_error = true;
          return false;
        }
      }
      docking = false;
      
      // Remember that we are currently docked
      docked = true;
    }
    else if (mode_request.mode.mode == messages::RobotMode::MODE_USE_TOOL)
    {
      ROS_INFO("recieved a USE TOOL command.");

      if (fields.tool_SetString_client)
      {
        cob_srvs::SetString SetString_srv;

        // See if there is a tool cmd given
        for (messages::ModeParameter const& param : mode_request.parameters)
        {
            ROS_DEBUG("Parameter name: %s, value: %s", param.name.c_str(), param.value.c_str());
            if (param.name == "tool_cmd")
            {
                ROS_INFO("Got command: %s", param.value.c_str());
                SetString_srv.request.data = param.value;
                break;
            }
        }
        using_tool = true;
        publish_robot_state();
        ROS_DEBUG("Calling srv with cmd %s", SetString_srv.request.data.c_str());
        fields.tool_SetString_client->call(SetString_srv);

        if (!SetString_srv.response.success)
        {
          ROS_ERROR("Failed to trigger tool cmd, message: %s.",
            SetString_srv.response.message.c_str());
          request_error = true;
          return false;
        }
      }
      using_tool = false;
    }    

    WriteLock task_id_lock(task_id_mutex);
    current_task_id = mode_request.task_id;

    request_error = false;
    return true;
  }
  return false;
}

bool ClientNode::read_path_request()
{
  messages::PathRequest path_request;
  if (fields.client->read_path_request(path_request) &&
      is_valid_request(
          path_request.fleet_name, path_request.robot_name,
          path_request.task_id))
  {
    ROS_INFO("received a Path command of size %lu.", path_request.path.size());

    if (path_request.path.size() <= 0)
      return false;

    // Sanity check: the first waypoint of the Path must be within N meters of
    // our current position. Otherwise, ignore the request.
    {
      ReadLock robot_transform_lock(robot_transform_mutex);
      const double dx =
          path_request.path[0].x - 
          current_robot_transform.transform.translation.x;
      const double dy =
          path_request.path[0].y -
          current_robot_transform.transform.translation.y;
      const double dist_to_first_waypoint = sqrt(dx*dx + dy*dy);

      ROS_INFO("distance to first waypoint: %.2f\n", dist_to_first_waypoint);

      if (dist_to_first_waypoint > 
          client_node_config.max_dist_to_first_waypoint)
      {
        ROS_WARN("distance was over threshold of %.2f ! Rejecting path,"
            "waiting for next valid request.\n",
            client_node_config.max_dist_to_first_waypoint);
        
        fields.move_base_client->cancelAllGoals();
        WriteLock goal_path_lock(goal_path_mutex);
        goal_path.clear();

        request_error = true;
        emergency = false;
        paused = false;
        return false;
      }
    }

    WriteLock goal_path_lock(goal_path_mutex);
    goal_path.clear();
    for (size_t i = 0; i < path_request.path.size(); ++i)
    {
      goal_path.push_back(
          Goal {
              path_request.path[i].level_name,
              location_to_move_base_goal(path_request.path[i]),
              false,
              0,
              ros::Time(
                  path_request.path[i].sec, path_request.path[i].nanosec)});
    }

    WriteLock task_id_lock(task_id_mutex);
    current_task_id = path_request.task_id;

    if (paused)
      paused = false;

    request_error = false;
    return true;
  }
  return false;
}

bool ClientNode::read_destination_request()
{
  messages::DestinationRequest destination_request;
  if (fields.client->read_destination_request(destination_request) &&
      is_valid_request(
          destination_request.fleet_name, destination_request.robot_name,
          destination_request.task_id))
  {
    ROS_INFO("received a Destination command, x: %.2f, y: %.2f, yaw: %.2f",
        destination_request.destination.x, destination_request.destination.y,
        destination_request.destination.yaw);
    
    WriteLock goal_path_lock(goal_path_mutex);
    goal_path.clear();
    goal_path.push_back(
        Goal {
            destination_request.destination.level_name,
            location_to_move_base_goal(destination_request.destination),
            false,
            0,
            ros::Time(
                destination_request.destination.sec, 
                destination_request.destination.nanosec)});

    WriteLock task_id_lock(task_id_mutex);
    current_task_id = destination_request.task_id;

    if (paused)
      paused = false;

    request_error = false;
    return true;
  }
  return false;
}

void ClientNode::read_requests()
{
  if (read_mode_request() || 
      read_path_request() || 
      read_destination_request())
    return;
}

void ClientNode::handle_requests()
{
  // there is an emergency or the robot is paused
  if (emergency || request_error || paused)
    return;
  // ooooh we have goals
  goal_path_mutex.lock();
  if (!goal_path.empty())
  {
    // Check if we are currently docked
    if (docked)
    {
      // Call undocking service on frame_id we are currently docked at
      if (fields.undocking_SetString_client)
      {
        goal_path_mutex.unlock();
        cob_srvs::SetString SetString_srv;
        std::cout << docked_frame;
        SetString_srv.request.data = docked_frame;

        undocking = true;
        ROS_INFO("Calling srv with frame_id %s", SetString_srv.request.data.c_str());
        fields.undocking_SetString_client->call(SetString_srv);
        undocking = false;
        
        if (!SetString_srv.response.success)
        {
          ROS_ERROR("Failed to trigger docking sequence, message: %s.",
            SetString_srv.response.message.c_str());
          request_error = true;
          goal_path_mutex.unlock();
          return;
        }
      }
      
      // Remember that we are no longer docked
      docked = false;
      docked_frame = "";
      // Get rid of the first point in the nav graph as we should have moved there by undocking
      goal_path.pop_front();
    }
    // Goals must have been updated since last handling, execute them now
    if (!goal_path.front().sent)
    {
      ROS_INFO("sending next goal.");
      fields.move_base_client->sendGoal(goal_path.front().goal);
      goal_path.front().sent = true;
      goal_path_mutex.unlock();
      return;
    }

    // Goals have been sent, check the goal states now
    GoalState current_goal_state = fields.move_base_client->getState();
    if (current_goal_state == GoalState::SUCCEEDED)
    {
      ROS_INFO("current goal state: SUCCEEEDED.");

      // By some stroke of good fortune, we may have arrived at our goal
      // earlier than we were scheduled to reach it. If that is the case,
      // we need to wait here until it's time to proceed.
      if (ros::Time::now() >= goal_path.front().goal_end_time)
      {
        goal_path.pop_front();
      }
      else
      {
        ros::Duration wait_time_remaining =
            goal_path.front().goal_end_time - ros::Time::now();
        ROS_INFO(
            "we reached our goal early! Waiting %.1f more seconds",
            wait_time_remaining.toSec());
      }
      goal_path_mutex.unlock();
      return;
    }
    else if (current_goal_state == GoalState::PENDING)
    {
      goal_path_mutex.unlock();
      return;
    }
    else if (current_goal_state == GoalState::ACTIVE)
    {
      goal_path_mutex.unlock();
      return;
    }
    else if (current_goal_state == GoalState::ABORTED)
    {
      goal_path.front().aborted_count++;

      // TODO: parameterize the maximum number of retries.
      if (goal_path.front().aborted_count < 5)
      {
        ROS_INFO("robot's navigation stack has aborted the current goal %d "
            "times, client will trying again...",
            goal_path.front().aborted_count);
        fields.move_base_client->cancelGoal();
        goal_path.front().sent = false;
        goal_path_mutex.unlock();
        return;
      }
      else
      {
        ROS_INFO("robot's navigation stack has aborted the current goal %d "
            "times, please check that there is nothing in the way of the "
            "robot, client will abort the current path request, and await "
            "further requests.",
            goal_path.front().aborted_count);
        fields.move_base_client->cancelGoal();
        goal_path.clear();
        goal_path_mutex.unlock();
        return;
      }
    }
    else
    {
      ROS_INFO("Undesirable goal state: %s",
          current_goal_state.toString().c_str());
      ROS_INFO("Client will abort the current path request, and await further "
          "requests or manual intervention.");
      fields.move_base_client->cancelGoal();
      goal_path.clear();
      goal_path_mutex.unlock();
      return;
    }
  }
  
  // otherwise, mode is correct, nothing in queue, nothing else to do then
  goal_path_mutex.unlock();
}

void ClientNode::update_thread_fn()
{
  while (node->ok())
  {
    update_rate->sleep();
    ros::spinOnce();

    get_robot_transform();

    read_requests();

    handle_requests();
  }
}

void ClientNode::publish_thread_fn()
{
  while (node->ok())
  {
    publish_rate->sleep();

    publish_robot_state();
  }
}

} // namespace ros1
} // namespace free_fleet
