#include <iostream>
#include <dogsim/utils.h>
#include <ros/ros.h>
#include <geometry_msgs/Twist.h>
#include <tf/transform_listener.h>
#include <visualization_msgs/MarkerArray.h>
#include <gazebo_msgs/GetModelState.h>

namespace {
  using namespace std;
class RobotDriver {
private:
  //! The node handle we'll be using
  ros::NodeHandle nh_;

  //! We will be publishing to the "cmd_vel" topic to issue commands
  ros::Publisher cmdVelocityPub_;

  //! We will be listening to TF transforms as well
  tf::TransformListener tf_;

  //! Publisher for goals
  ros::Publisher goalPub_;

  //! Publisher for the dog position.
  ros::Publisher dogPub_;

  //! Publisher for movement
  ros::Publisher movePub_;

  //! Drives adjustments
  ros::Timer driverTimer;
public:
  //! ROS node initialization
  RobotDriver(ros::NodeHandle &nh):nh_(nh){

    // Set up the publisher for the cmd_vel topic
    cmdVelocityPub_ = nh_.advertise<geometry_msgs::Twist>("base_controller/command", 1);

    // Setup a visualization publisher for our goals.
    goalPub_ = nh_.advertise<visualization_msgs::Marker>("path_goal", 1);
    movePub_ = nh_.advertise<visualization_msgs::Marker>("planned_move", 1);
    dogPub_ = nh_.advertise<visualization_msgs::Marker>("dog_position", 1);

    driverTimer = nh_.createTimer(ros::Duration(0.25), &RobotDriver::callback, this);
    // Wait for the service that will provide us simulated object locations.
    ros::service::waitForService("/gazebo/get_model_state");

    driverTimer.start();
  }

  static visualization_msgs::Marker createMarker(const geometry_msgs::Point& position, const std_msgs::Header& header, std_msgs::ColorRGBA& color){

      visualization_msgs::Marker marker;
      marker.header = header;
      marker.ns = "dogsim";
      marker.id = 0;
      marker.type = visualization_msgs::Marker::SPHERE;
      marker.action = visualization_msgs::Marker::ADD;
      marker.pose.position = position;
      marker.color = color;
      marker.scale.x = marker.scale.y = marker.scale.z = 0.2;
      return marker;
  }

  static std_msgs::ColorRGBA createColor(float r, float g, float b){
    std_msgs::ColorRGBA color;
    color.r = r;
    color.g = g;
    color.b = b;
    color.a = 1;
    return color;
  }

  static visualization_msgs::Marker createArrow(const double yaw, const std_msgs::Header& header, const std_msgs::ColorRGBA& color){
     // Publish a visualization arrow.
     visualization_msgs::Marker arrow;
     arrow.header = header;
     arrow.ns = "dogsim";
     arrow.id = 0;
     arrow.type = visualization_msgs::Marker::ARROW;
     arrow.action = visualization_msgs::Marker::ADD;
     arrow.pose.position.x = arrow.pose.position.y = arrow.pose.position.z = 0;
     arrow.pose.orientation = tf::createQuaternionMsgFromRollPitchYaw(0, 0, yaw);
     arrow.scale.x = arrow.scale.y = arrow.scale.z = 1.0;
     arrow.color = color;
     return arrow;
 }

  void callback(const ros::TimerEvent& event){
      ROS_DEBUG("Received callback @ %f", event.current_real.toSec());

      // Lookup the current position.
      bool gotTransform = false;
      const ros::Time startTime = event.current_real;
      for(unsigned int i = 0; i < 5 && !gotTransform; ++i){
        gotTransform = tf_.waitForTransform("/base_footprint", "/map", 
                                            startTime, ros::Duration(1.0));
      }
      if(!gotTransform){
        ROS_WARN("Failed to get transform. Aborting cycle");
        return;
      }
      
      // Lookup the current position of the dog.
      ros::ServiceClient modelStateServ = nh_.serviceClient<gazebo_msgs::GetModelState>("/gazebo/get_model_state");
      gazebo_msgs::GetModelState modelState;
      modelState.request.model_name = "dog";
      modelStateServ.call(modelState);
      geometry_msgs::PoseStamped dogPose;
      dogPose.header.stamp = startTime;
      dogPose.header.frame_id = "/map";
      dogPose.pose = modelState.response.pose;
    
      // Visualize the dog.
      std_msgs::ColorRGBA BLUE = createColor(0, 0, 1);
      dogPub_.publish(createMarker(dogPose.pose.position, dogPose.header, BLUE));

      // Determine the goal.
      geometry_msgs::PointStamped goal;

      // TODO: Be smarter about making time scale based on velocity.
      gazebo::math::Vector3 gazeboGoal = utils::lissajous(startTime.toSec() / 50.0);

      goal.point.x = gazeboGoal.x;
      goal.point.y = gazeboGoal.y;
      goal.point.z = gazeboGoal.z;
      goal.header.stamp = startTime;
      goal.header.frame_id = "/map";
      
      // Visualize the goal.
      std_msgs::ColorRGBA RED = createColor(1, 0, 0);
      goalPub_.publish(createMarker(goal.point, goal.header, RED));

      // Determine the angle from the robot to the target.
      geometry_msgs::PointStamped normalStamped;
      tf_.transformPoint("/base_footprint", goal, normalStamped);

      // Determine the relative dog position
      geometry_msgs::PoseStamped dogInBaseFrame;
      tf_.transformPose("/base_footprint", dogPose, dogInBaseFrame);

      // Now calculate a point that is 1 meter behind the dog on the vector
      // between the robot and the dog. This is the desired position of 
      // the robot.
      btVector3 goalVector(normalStamped.point.x, normalStamped.point.y, 0);
      goalVector -= btScalar(1.0) * goalVector.normalized();
 
      // Now calculate the point.
      // atan gives us the yaw between the positive x axis and the point.
      btScalar yaw = btAtan2(goalVector.y(), goalVector.x());
      
      geometry_msgs::Twist baseCmd;

      bool shouldMove = true;
      const double MAX_V = 2.0;
      const double AVOIDANCE_V = 0.5;
      const double AVOIDANCE_THRESHOLD = 1.0;
      const double DEACC_DISTANCE = 1.0;
      const double DISTANCE_THRESH = 0.01;

      // Robot location is at the root of frame.
      double distance = goalVector.distance(btVector3(0, 0, 0));
      if(distance > DEACC_DISTANCE){
        baseCmd.linear.x = MAX_V;
      }
      // Don't attempt to close on the target
      else if(distance < DISTANCE_THRESH){
        shouldMove = false;
      }
      else {
        baseCmd.linear.x = distance / DEACC_DISTANCE * MAX_V;
      }
     
      // Check if we are likely to collide with the dog and go around it.
      if(abs(dogInBaseFrame.pose.position.y) < AVOIDANCE_THRESHOLD && (dogInBaseFrame.pose.position.x > 0 && dogInBaseFrame.pose.position.x < AVOIDANCE_THRESHOLD)){
        ROS_INFO("Attempting to avoid dog");
        // Move in the opposite direction of the position of the dog.
        baseCmd.linear.y = -1.0 * copysign(AVOIDANCE_V, dogInBaseFrame.pose.position.y);
      }
      // We will naturally correct back to the goal.
      
      baseCmd.angular.z = yaw;
      
      if(shouldMove){
        // Visualize the movement.
        std_msgs::Header arrowHeader;
        arrowHeader.frame_id = "base_footprint";
        arrowHeader.stamp = startTime;
        const std_msgs::ColorRGBA GREEN = createColor(0, 1, 0);
        movePub_.publish(createArrow(yaw, arrowHeader, GREEN));
        
        // Publish the command to the base
        cmdVelocityPub_.publish(baseCmd);
      }
      else {
        ROS_DEBUG("Skipping movement. Below distance tolerance");
      } 
    }
};
}

int main(int argc, char** argv){
  ros::init(argc, argv, "robot_driver");
  ros::NodeHandle nh;

  RobotDriver driver(nh);
  ROS_INFO("Spinning the event loop");
  ros::spin();
  ROS_INFO("Exiting");
}
