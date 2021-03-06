#include <ros/ros.h>
#include <dogsim/GetPath.h>
#include <dogsim/StartPath.h>
#include <dogsim/MaximumTime.h>
#include <geometry_msgs/Point.h>
#include "path_provider.h"
#include "lissajous_path_provider.h"
#include "rectangle_path_provider.h"
#include "block_walk_path_provider.h"
#include "random_walk_path_provider.h"
#include <dogsim/GetEntirePath.h>
#include <dogsim/GetEntireRobotPath.h>
#include <tf/transform_listener.h>

namespace {
using namespace ros;
using namespace std;

const double TRAILING_DISTANCE = 0;

//! Shift distance from base to desired arm position
//! Calculated as the negative of /base_footprint to /r_wrist_roll_link in x axis
const double SHIFT_DISTANCE = 0.6;

class GetPathServer {

private:
	NodeHandle nh;
	NodeHandle pnh;
	ros::ServiceServer service;
	ros::ServiceServer startService;
	ros::ServiceServer maxService;
	ros::ServiceServer entirePathService;
	ros::ServiceServer entireRobotPathService;

	bool started;
	ros::Time startTime;
	auto_ptr<PathProvider> pathProvider;

public:

	GetPathServer() :
		pnh("~"), started(false) {
		service = nh.advertiseService("/dogsim/get_path",
				&GetPathServer::getPath, this);
		entirePathService = nh.advertiseService("/dogsim/get_entire_path",
				&GetPathServer::getEntirePath, this);
		entireRobotPathService = nh.advertiseService("/dogsim/get_entire_robot_path",
                &GetPathServer::getEntireRobotPath, this);

		startService = nh.advertiseService("/dogsim/start",
				&GetPathServer::start, this);
		maxService = nh.advertiseService("/dogsim/maximum_time",
				&GetPathServer::maximumTime, this);

		string pathType;
		pnh.param<string>("path_type", pathType, "lissajous");
		if (pathType == "lissajous") {
			pathProvider.reset(new LissajousPathProvider());
		} else if (pathType == "rectangle") {
			pathProvider.reset(new RectanglePathProvider());
		} else if (pathType == "blockwalk") {
			pathProvider.reset(new BlockWalkPathProvider());
		} else if (pathType == "randomwalk") {
            pathProvider.reset(new RandomWalkPathProvider());
		} else {
			ROS_ERROR("Unknown path provider type: %s", pathType.c_str());
			return;
		}
		ROS_INFO("%s path type selected", pathType.c_str());
		pathProvider->init();
	}

private:
	bool start(dogsim::StartPath::Request& req,
			dogsim::StartPath::Response& res) {
		assert(!started);
		started = true;
		startTime = req.time;
		ROS_DEBUG("Starting path @ time: %f", startTime.toSec());
		return true;
	}

	bool maximumTime(dogsim::MaximumTime::Request& req,
			dogsim::MaximumTime::Response& res) {
		res.maximumTime = pathProvider->getMaximumTime();
		ROS_DEBUG("Returning maximum time: %f", res.maximumTime.toSec());
		return true;
	}

	bool getEntirePath(dogsim::GetEntirePath::Request& req,
			dogsim::GetEntirePath::Response& res) {
		ROS_DEBUG(
				"Getting entire path for max time %f and increment %f", pathProvider->getMaximumTime().toSec(), req.increment);
		for (double t = 0; t < pathProvider->getMaximumTime().toSec();
				t += req.increment) {
		    geometry_msgs::PoseStamped pose = pathProvider->poseAtTime(ros::Duration(t));
		    // Set the time to the moment of the goal
		    pose.header.stamp = startTime + ros::Duration(t);
			res.poses.push_back(pose);
		}
		return true;
	}

    bool getEntireRobotPath(dogsim::GetEntireRobotPath::Request& req,
            dogsim::GetEntireRobotPath::Response& res) {
        ROS_DEBUG(
                "Getting entire robot path for max time %f and increment %f", pathProvider->getMaximumTime().toSec(), req.increment);
        for (double t = 0; t < pathProvider->getMaximumTime().toSec();
                t += req.increment) {
            geometry_msgs::PoseStamped pose = getPlannedRobotPose(ros::Duration(t));
            // Set the time to the moment of the goal
            pose.header.stamp = startTime + ros::Duration(t);
            res.poses.push_back(pose);
        }
        return true;
    }

	geometry_msgs::PoseStamped getPlannedRobotPose(ros::Duration t) {

	    const geometry_msgs::PoseStamped dogGoal = pathProvider->poseAtTime(t);

	    const geometry_msgs::PoseStamped goal2 = pathProvider->poseAtTime(t + SLOPE_DELTA);

	    // Calculate the vector of the tangent line.
	    btVector3 tangent = btVector3(goal2.pose.position.x, goal2.pose.position.y, 0)
	    - btVector3(dogGoal.pose.position.x, dogGoal.pose.position.y, 0);
	    tangent.normalize();

	    // Now select a point on the vector but slightly behind.
	    btVector3 backGoal = btVector3(dogGoal.pose.position.x, dogGoal.pose.position.y, 0)
	    - tangent * tfScalar(TRAILING_DISTANCE);

	    // Rotate the vector to perpendicular
	    btVector3 perp = tangent.rotate(btVector3(0, 0, 1),
	            tfScalar(boost::math::constants::pi<double>() / 2.0));

	    // Select a point on the perpendicular line.
	    btVector3 finalGoal = backGoal + perp * tfScalar(SHIFT_DISTANCE);

	    geometry_msgs::PoseStamped robotGoal;
	    robotGoal.pose.position.x = finalGoal.x();
	    robotGoal.pose.position.y = finalGoal.y();
	    robotGoal.pose.position.z = finalGoal.z();
	    robotGoal.header = dogGoal.header;

	    // Calculate the yaw so we can create an orientation.
	    tfScalar yaw = tfAtan2(tangent.y(), tangent.x());
	    robotGoal.pose.orientation = tf::createQuaternionMsgFromYaw(yaw);

	    return robotGoal;
	}

	bool getPath(dogsim::GetPath::Request& req,
			dogsim::GetPath::Response& res) {

	    ROS_DEBUG("Getting path position for time %f", startTime.toSec());
		res.elapsedTime = req.time - startTime;
		ROS_DEBUG("Elapsed time is %f", res.elapsedTime.toSec());

		computeStartAndEnd(ros::Time(req.time), res.started, res.ended);

		// Allow calling get path prior to starting and return the begin position.
		geometry_msgs::PoseStamped result = pathProvider->poseAtTime(res.started ? (req.time - startTime): ros::Duration(0));
		res.point.header = result.header;
		res.point.point = result.pose.position;
		assert(res.point.header.frame_id.size() > 0);
		return true;
	}

	void computeStartAndEnd(const ros::Time& time, uint8_t& rStarted,
			uint8_t& rEnded) const {
		if (!this->started) {
		    ROS_DEBUG("Path not started yet");
			// Not started yet.
			rEnded = false;
			rStarted = false;
		} else if ((time - startTime) > pathProvider->getMaximumTime()) {
			rEnded = true;
			rStarted = true;
		} else {
			rStarted = true;
			rEnded = false;
		}
	}
};
}

int main(int argc, char** argv) {
	ros::init(argc, argv, "get_path");
	GetPathServer getPathServer;
	ros::MultiThreadedSpinner spinner(4);
	spinner.spin();
	return 0;
}

