/**
 *
 *  \file
 *  \brief      Main entry point for jackal base.
 *  \author     Mike Purvis <mpurvis@clearpathrobotics.com>
 *  \copyright  Copyright (c) 2013, Clearpath Robotics, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Clearpath Robotics, Inc. nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL CLEARPATH ROBOTICS, INC. BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Please send comments, questions, or patches to code@clearpathrobotics.com
 *
 */

#include <iostream>
#include <boost/asio.hpp>
#include <boost/assign.hpp>

#include "ros/ros.h"
#include "ros/callback_queue.h"
#include "jackal_msgs/Drive.h"
#include "jackal_msgs/Feedback.h"
#include "sensor_msgs/JointState.h"

#include "rosserial_server/serial_session.h"

#include "controller_manager/controller_manager.h"
#include "hardware_interface/robot_hw.h"
#include "hardware_interface/joint_state_interface.h"
#include "hardware_interface/joint_command_interface.h"


class JackalRobot : public hardware_interface::RobotHW
{
public:
  JackalRobot()
  {
    joint_state_msg_.name.resize(4);
    joint_state_msg_.position.resize(4);
    joint_state_msg_.velocity.resize(4);
    joint_state_msg_.effort.resize(4);
    joint_state_msg_.name = boost::assign::list_of("front_left_wheel_joint")
        ("front_right_wheel_joint")("rear_left_wheel_joint")("rear_right_wheel_joint");

    for (unsigned int i = 0; i < joint_state_msg_.name.size(); i++) {
      hardware_interface::JointStateHandle joint_state_handle(joint_state_msg_.name[i],
          &joint_state_msg_.position[i], &joint_state_msg_.velocity[i], &joint_state_msg_.effort[i]);
      joint_state_interface_.registerHandle(joint_state_handle);
    }
    registerInterface(&joint_state_interface_);

    hardware_interface::JointHandle left_velocity_handle(
        joint_state_interface_.getHandle("front_left_wheel_joint"), &drive_commands_[jackal_msgs::Drive::LEFT]);
    velocity_joint_interface_.registerHandle(left_velocity_handle);
    hardware_interface::JointHandle right_velocity_handle(
        joint_state_interface_.getHandle("front_right_wheel_joint"), &drive_commands_[jackal_msgs::Drive::RIGHT]);
    velocity_joint_interface_.registerHandle(right_velocity_handle);
    registerInterface(&velocity_joint_interface_);

    feedback_sub_ = nh_.subscribe("feedback", 1, &JackalRobot::feedbackCallback, this);
    cmd_drive_pub_ = nh_.advertise<jackal_msgs::Drive>("cmd_drive", 1);
    joint_state_pub_ = nh_.advertise<sensor_msgs::JointState>("/joint_states", 1);
  }

  /**
   * Populates and publishes a JointState based on the most recent Feedback message
   * received from the MCU.
   *
   * Called from the controller thread.
   */
  void publishJointStateFromHardware()
  {
    if (feedback_msg_)
    {
      // Grab a local reference to the last received message, so that we're
      // working with a consistent snapshot.
      const jackal_msgs::Feedback::ConstPtr msg = feedback_msg_;

      for (int i = 0; i < 4; i++)
      {
        joint_state_msg_.position[i] = msg->drivers[i % 2].measured_travel;
        joint_state_msg_.velocity[i] = msg->drivers[i % 2].measured_velocity;
        joint_state_msg_.effort[i] = 0;  // TODO
      }

      joint_state_msg_.header.seq++;
      joint_state_msg_.header.stamp = feedback_msg_->header.stamp;
      joint_state_pub_.publish(joint_state_msg_);
    }
  }

  /**
   * Populates and publishes Drive message based on the controller outputs.
   *
   * Called from the controller thread.
   */
  void publishDriveFromController()
  {
    jackal_msgs::Drive drive_msg;
    drive_msg.mode = jackal_msgs::Drive::MODE_VELOCITY;
    drive_msg.drivers[jackal_msgs::Drive::LEFT] = drive_commands_[jackal_msgs::Drive::LEFT];
    drive_msg.drivers[jackal_msgs::Drive::RIGHT] = drive_commands_[jackal_msgs::Drive::RIGHT];
    cmd_drive_pub_.publish(drive_msg);
  }

private:
  void feedbackCallback(const jackal_msgs::Feedback::ConstPtr& msg)
  {
    // Update the feedback message pointer to go to the current message.
    feedback_msg_ = msg;
  }

  ros::NodeHandle nh_;
  ros::Subscriber feedback_sub_;
  ros::Publisher cmd_drive_pub_;
  ros::Publisher joint_state_pub_;

  hardware_interface::JointStateInterface joint_state_interface_;
  hardware_interface::VelocityJointInterface velocity_joint_interface_;

  // These are mutated on the controls thread only.
  double drive_commands_[2];
  sensor_msgs::JointState joint_state_msg_;

  // This pointer is set from the ROS thread.
  jackal_msgs::Feedback::ConstPtr feedback_msg_;
};

void controlThread(ros::Rate rate, JackalRobot* robot, controller_manager::ControllerManager* cm)
{
  ros::Time last_time;

  while(1)
  {
    ros::Time this_time = ros::Time::now();
    robot->publishJointStateFromHardware();
    cm->update(this_time, this_time - last_time);
    robot->publishDriveFromController();
    last_time = this_time;
    rate.sleep();
  }
}

int main(int argc, char* argv[])
{
  // Initialize ROS node.
  ros::init(argc, argv, "jackal_node");
  JackalRobot jackal;

  // Create the serial rosserial server in a background ASIO event loop.
  int baud = 115200;  //< This is a dummy, since the USB-ACM connection negotiates its own rate.
  std::string port;
  ros::param::param<std::string>("~port", port, "/dev/jackal");
  boost::asio::io_service io_service;
  new rosserial_server::SerialSession(io_service, port, baud);
  boost::thread(boost::bind(&boost::asio::io_service::run, &io_service));

  // Background thread for the controls callback.
  ros::NodeHandle controller_nh("");
  controller_manager::ControllerManager cm(&jackal, controller_nh);
  boost::thread(boost::bind(controlThread, ros::Rate(50), &jackal, &cm));

  // Foreground ROS spinner for ROS callbacks, including rosserial, joy teleop.
  ros::spin();

  return 0;
}
