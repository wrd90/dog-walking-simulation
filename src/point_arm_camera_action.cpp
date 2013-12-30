#include <ros/ros.h>
#include <moveit/move_group_interface/move_group.h>
#include <actionlib/server/simple_action_server.h>
#include <tf/transform_listener.h>
#include <tf2/LinearMath/btVector3.h>
#include <dogsim/utils.h>

// Generated messages
#include <dogsim/PointArmCameraAction.h>

namespace {
using namespace std;
using namespace ros;

// Distance from the arm to the elbow offset for the camera position on the arm.
static const double CAMERA_DISTANCE_FROM_SHOULDER = 0.5 - 0.044;
static const string MOVE_GROUP_NAME_DEFAULT = "right_arm";
static const double pi = boost::math::constants::pi<double>();
static const string SHOULDER_FRAME = "/r_shoulder_pan_link";

class PointArmCamera {
private:
    NodeHandle nh;
    NodeHandle pnh;
    actionlib::SimpleActionServer<dogsim::PointArmCameraAction> as;
    string actionName;
    move_group_interface::MoveGroup arm;
    tf::TransformListener tf;

    //! Publisher for the look direction
    ros::Publisher lookDirectionPub;
    ros::Publisher targetPub;


public:
    PointArmCamera(const string& name) :
        pnh("~"),
        as(nh, name, boost::bind(&PointArmCamera::moveArmToTarget, this, _1), false),
        actionName(
                name),
                arm("right_arm") {
        lookDirectionPub = nh.advertise<visualization_msgs::Marker>(
                "/point_arm_camera_action/look_direction_viz", 1);
        targetPub = nh.advertise<geometry_msgs::PointStamped>("/point_arm_camera_action/target_vis", 1);

        as.registerPreemptCallback(boost::bind(&PointArmCamera::preemptCB, this));
        as.start();
    }

protected:
    void preemptCB() {
        ROS_DEBUG("Preempting the point arm camera action");

        if (!as.isActive()) {
            ROS_DEBUG("Point arm camera action cancelled prior to start");
            return;
        }

        arm.stop();
        as.setPreempted();
    }

    bool moveArmToTarget(const dogsim::PointArmCameraGoalConstPtr& goal) {
        if (!as.isActive()) {
            ROS_INFO("Point arm camera action cancelled prior to start");
            return false;
        }

        ROS_INFO("Moving arm to point at target %f, %f, %f in frame %s",
                goal->target.point.x, goal->target.point.y, goal->target.point.z, goal->target.header.frame_id.c_str());

        // Transform the goal position to shoulder link, which is fixed during this movement
        // of the arm
        geometry_msgs::PointStamped goalInShoulderFrame;
        try {
            tf.waitForTransform(SHOULDER_FRAME, goal->target.header.frame_id,
                    goal->target.header.stamp, ros::Duration(1.0));
            tf.transformPoint(SHOULDER_FRAME, goal->target.header.stamp, goal->target,
                    goal->target.header.frame_id, goalInShoulderFrame);
        }
        catch (tf::TransformException& ex) {
            ROS_INFO("Failed to transform goal point to %s frame", SHOULDER_FRAME.c_str());
            as.setAborted();
            return false;
        }

        if(targetPub.getNumSubscribers() > 0){
            targetPub.publish(goalInShoulderFrame);
        }

        ROS_DEBUG("Moving arm to point at point %f %f %f in frame %s",
                goalInShoulderFrame.point.x, goalInShoulderFrame.point.y, goalInShoulderFrame.point.z, goalInShoulderFrame.header.frame_id.c_str());

        btVector3 elbowBase = btVector3(CAMERA_DISTANCE_FROM_SHOULDER, 0, 0);

        // Create the unit vector between the two points
        btVector3 direction = btVector3(goalInShoulderFrame.point.x, goalInShoulderFrame.point.y, goalInShoulderFrame.point.z) - elbowBase;
        direction.normalize();

        ROS_DEBUG("Unit vector in shoulder frame %f, %f, %f", direction.x(), direction.y(), direction.z());

        btVector3 planeDirection(direction.x(), direction.y(), 0);

        // Compute RPY
        double yaw = atan2(direction.y(), direction.x());
        double pitch = atan2(-direction.z(), planeDirection.length());

        ROS_DEBUG("Resulting RPY: %f %f %f", 0.0, pitch, yaw);

        if(lookDirectionPub.getNumSubscribers() > 0){
            visualization_msgs::Marker marker;
            marker.pose.orientation = tf::createQuaternionMsgFromRollPitchYaw(0, pitch, yaw);
            marker.pose.position.x = CAMERA_DISTANCE_FROM_SHOULDER;
            marker.header.frame_id = goalInShoulderFrame.header.frame_id;
            marker.header.stamp = goalInShoulderFrame.header.stamp;
            marker.ns = ros::this_node::getName();
            marker.id = 0;
            marker.type = visualization_msgs::Marker::ARROW;
            marker.action = visualization_msgs::Marker::ADD;
            marker.scale.x = 1.0;
            marker.scale.y = 0.05;
            marker.scale.z = 0.05;
            marker.color = utils::createColor(1, 0, 0); // Red
            marker.lifetime = ros::Duration();
            lookDirectionPub.publish(marker);
        }

        vector<double> positions(7);
        positions[0] = -pi / 2.0;
        positions[1] = 0;
        positions[2] = -pi / 2.0 - pitch; // Rotate to match base joint orientation
        positions[3] = -yaw;
        positions[4] = 0;
        positions[5] = 0;
        positions[6] = 0;
        arm.setJointValueTarget(positions);

        if(arm.move()){
            as.setSucceeded();
            return true;
        }
        as.setAborted();
        return false;
    }
};
}

int main(int argc, char** argv) {
    ros::init(argc, argv, "point_arm_camera_action");
    PointArmCamera action(ros::this_node::getName());
    ros::spin();
}
