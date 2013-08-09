#include <ros/ros.h>
#include <dogsim/utils.h>
#include <visualization_msgs/Marker.h>
#include <dogsim/DogPosition.h>
#include <dogsim/GetPath.h>
#include <message_filters/subscriber.h>
#include <dogsim/AdjustDogPositionAction.h>
#include <dogsim/MoveRobotAction.h>
#include <actionlib/client/simple_action_client.h>

namespace {
  using namespace std;
  typedef actionlib::SimpleActionClient<dogsim::AdjustDogPositionAction> AdjustDogClient;
  typedef actionlib::SimpleActionClient<dogsim::MoveRobotAction> MoveRobotClient;
class RobotDriver {
private:
  //! Amount of time before starting walk
  static const double DELAY_TIME = 5.0;

  //! The node handle we'll be using
  ros::NodeHandle nh_;

  //! Private nh
  ros::NodeHandle pnh_;

  //! Publisher for command velocities
  ros::Publisher cmdVelocityPub_;

  //! Publisher for goals
  ros::Publisher goalPub_;
    
  //! Time the driver was initialized
  ros::Time initTime;

  //! Timer that controls movement of the robot
  ros::Timer driverTimer_;
  
  //! Dog position subscriber
  auto_ptr<message_filters::Subscriber<dogsim::DogPosition> > dogPositionSub_;
  
  //! Client for the arm to attempt to position the dog
  AdjustDogClient adjustDogClient;

  //! Client for the movement of the robot base
  MoveRobotClient moveRobotClient;
  
  //! Last time we performed an adjustment
  ros::Time lastAdjustmentTime;
  
  //! Length of the leash
  double leashLength;
public:
  //! ROS node initialization
  RobotDriver(): pnh_("~"), adjustDogClient("adjust_dog_position_action", true), moveRobotClient("move_robot_action", true){
    ROS_INFO("Initializing the robot driver @ %f", ros::Time::now().toSec());
    nh_.param("leash_length", leashLength, 2.0);
    dogPositionSub_.reset(new message_filters::Subscriber<dogsim::DogPosition> (nh_, "/dog_position", 1));

    // Set up the publisher for the cmd_vel topic
    cmdVelocityPub_ = nh_.advertise<geometry_msgs::Twist>("base_controller/command", 1);
    goalPub_ = nh_.advertise<visualization_msgs::Marker>("robot_driver/walk_goal_viz", 1);
    
    ros::service::waitForService("/dogsim/get_path");

    dogPositionSub_->registerCallback(boost::bind(&RobotDriver::dogPositionCallback, this, _1));
    adjustDogClient.waitForServer();
    moveRobotClient.waitForServer();
    
    // Only use the steering callback when in solo mode. Otherwise we'll move based on the required positions to
    // move the arm.
    bool soloMode;
    pnh_.param<bool>("solo_mode", soloMode, false);
    if(soloMode){
        ROS_INFO("Running solo move");
        driverTimer_ = nh_.createTimer(ros::Duration(1.0), &RobotDriver::steeringCallback, this);
    }
    else {
      ROS_INFO("Running regular mode");
      driverTimer_ = nh_.createTimer(ros::Duration(1.0), &RobotDriver::displayCallback, this);
    }
    initTime = ros::Time::now();
    driverTimer_.start();
    ROS_INFO("Robot driver initialization complete @ %f", ros::Time::now().toSec());
  }

  void dogPositionCallback(const dogsim::DogPositionConstPtr& dogPosition){
    ROS_DEBUG("Received a dog position callback @ %f and init time is: %f", ros::Time::now().toSec(), initTime.toSec());
    
    if(ros::Time::now().toSec() - initTime.toSec() < DELAY_TIME){
        ROS_DEBUG("Start time not reached");
        return;
    }
      
    // Assumes messages is in robot frame
    // Check if we are likely to collide with the dog and go around it.
    const double AVOIDANCE_V = 2.5;
    const double AVOIDANCE_THRESHOLD = 1.25;

    bool ended = false;
    bool started = false;
    const geometry_msgs::PointStamped goal = getDogPosition(ros::Time::now(), true /* should start */, started, ended);
   
    if(ended){
        // TODO: Unsubscribe here.
        return;
    }

    // Determine if our base movement should be to avoid the dog. First priority.
    // TODO: Move this to separate action so it can more intelligently avoid the dog. Potentially.
    assert(dogPosition->pose.header.frame_id == "/base_footprint");
    geometry_msgs::Point robotPosition;
    ROS_INFO("Current distance for avoidance: %f", utils::pointToPointXYDistance(dogPosition->pose.pose.position, robotPosition));
    if(utils::pointToPointXYDistance(dogPosition->pose.pose.position, robotPosition) < AVOIDANCE_THRESHOLD){
      ROS_INFO("Attempting to avoid dog @ distance %f", utils::pointToPointXYDistance(dogPosition->pose.pose.position, robotPosition));
      // Stop any planned movement.
      adjustDogClient.cancelGoal();
      geometry_msgs::Twist baseCmd;
      // Move in the opposite direction of the position of the dog.
      // TODO: This needs to take dog velocity into account.
      baseCmd.linear.x = 0.0;
      baseCmd.linear.y = -1.0 * copysign(AVOIDANCE_V, dogPosition->pose.pose.position.y);
      // Publish the command to the base
      cmdVelocityPub_.publish(baseCmd);
      return;
    }
    
    // Only adjust dog position if the 
    if(ros::Time::now().toSec() - lastAdjustmentTime.toSec() > 0.25){
        ROS_INFO("Adjusting dog position");
        dogsim::AdjustDogPositionGoal adjustGoal;
        adjustGoal.dogPose = dogPosition->pose;
        adjustGoal.goalPosition = goal;
        adjustDogClient.sendGoal(adjustGoal);
        lastAdjustmentTime = ros::Time::now();
    }
    ROS_DEBUG("Completed dog position callback");
  }

  geometry_msgs::PointStamped getDogPosition(const ros::Time& time, bool shouldStart, bool& started, bool& ended){
      // Determine the goal.
      ros::ServiceClient getPathClient = nh_.serviceClient<dogsim::GetPath>("/dogsim/get_path");
      dogsim::GetPath getPath;
      getPath.request.time = time.toSec();
      getPath.request.start = shouldStart;
      getPathClient.call(getPath);
      started = getPath.response.started;
      ended = getPath.response.ended;
      return getPath.response.point;
  }

  void displayCallback(const ros::TimerEvent& event){
      ROS_DEBUG("Received display callback");
      bool started;
      bool ended;
      const geometry_msgs::PointStamped goal = getDogPosition(event.current_real, false /* should start */, started, ended);
      
      if(!started){
          ROS_INFO("Walk has not started yet");
          return;
      }
      if(ended){
          ROS_INFO("Walk is ended");
          driverTimer_.stop();
          return;
      }

      // Visualize the goal.
      if(goalPub_.getNumSubscribers() > 0){
          ROS_DEBUG("Publishing the goal position");
          std_msgs::ColorRGBA RED = utils::createColor(1, 0, 0);
          goalPub_.publish(utils::createMarker(goal.point, goal.header, RED, true));
      }
  }
  
  void steeringCallback(const ros::TimerEvent& event){
      ROS_DEBUG("Received callback @ %f : %f", event.current_real.toSec(), event.current_expected.toSec());

      if(event.current_real.toSec() - initTime.toSec() < DELAY_TIME){
          ROS_DEBUG("Start time not reached");
          return;
      }

      bool ended = false;
      bool started = false;
      const geometry_msgs::PointStamped goal = getDogPosition(event.current_real, true /* should start */, started, ended);

      if(ended){
          ROS_INFO("Walk ended");
          if(adjustDogClient.getState() == actionlib::SimpleClientGoalState::ACTIVE){
              adjustDogClient.cancelGoal();
          }
          if(moveRobotClient.getState() == actionlib::SimpleClientGoalState::ACTIVE){
              moveRobotClient.cancelGoal();
          }
          driverTimer_.stop();
          return;
      }
      
      dogsim::MoveRobotGoal moveRobotGoal;
      moveRobotGoal.position = goal;
      
      // This will automatically cancel the last goal.
      moveRobotClient.sendGoal(moveRobotGoal);
      ROS_DEBUG("Completed robot driver callback");
    }
};
}

int main(int argc, char** argv){
  ros::init(argc, argv, "robot_driver");

  RobotDriver driver;
  ros::spin();
  ROS_INFO("Exiting robot driver");
}
