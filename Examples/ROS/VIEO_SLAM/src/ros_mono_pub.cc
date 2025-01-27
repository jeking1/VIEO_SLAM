/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
* For more information see <https://github.com/leavesnight/VIEO_SLAM>
*
* ORB-SLAM2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM2 is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
*/


#include<iostream>
#include<algorithm>
#include<fstream>
#include<chrono>
#include <time.h>

#include<ros/ros.h>
#include <cv_bridge/cv_bridge.h>
#include "sensor_msgs/PointCloud2.h"
#include "geometry_msgs/PoseStamped.h"
#include "geometry_msgs/PoseArray.h"

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>

#include<opencv2/core/core.hpp>

#include"../../../include/System.h"

#include "MapPoint.h"
#include <opencv2/highgui/highgui_c.h>
#include <opencv2/highgui/highgui.hpp>
#include <Converter.h>

//! parameters
bool read_from_topic = false, read_from_camera = false;
std::string image_topic = "/camera/image_raw";
int all_pts_pub_gap = 0;
bool show_viewer = true;

vector<string> vstrImageFilenames;
vector<double> vTimestamps;
cv::VideoCapture cap_obj;

bool pub_all_pts = false;
int pub_count = 0;

inline bool isInteger(const std::string & s);
void publish(VIEO_SLAM::System &SLAM, ros::Publisher &pub_pts_and_pose,
	ros::Publisher &pub_all_kf_and_pts, int frame_id);

class ImageGrabber{
public:
	ImageGrabber(VIEO_SLAM::System &_SLAM, ros::Publisher &_pub_pts_and_pose,
		ros::Publisher &_pub_all_kf_and_pts) :
		SLAM(_SLAM), pub_pts_and_pose(_pub_pts_and_pose),
		pub_all_kf_and_pts(_pub_all_kf_and_pts), frame_id(0){}

	void GrabImage(const sensor_msgs::ImageConstPtr& msg);

	VIEO_SLAM::System &SLAM;
	ros::Publisher &pub_pts_and_pose;
	ros::Publisher &pub_all_kf_and_pts;
	int frame_id;
};
bool parseParams(int argc, char **argv);

using namespace std;

void LoadImages(const string &strImagePath, const string &strPathTimes,
                vector<string> &vstrImages, vector<double> &vTimeStamps);

//zzh
VIEO_SLAM::System* g_pSLAM;
double g_simulateTimestamp=-1,gDelayCache;
bool g_brgbdFinished=false;
mutex g_mutex;

//a new thread simulating the odom serial threads
void odomRun(ifstream &finOdomdata,int totalNum){//must use &
  //read until reading over
  int nTotalNum=6;//wx~z,ax~z
  if (totalNum!=0) nTotalNum=totalNum;
  double* odomdata=new double[nTotalNum];
  double timestamp,tmstpLast=-1;
  
  while (!g_pSLAM){//if it's NULL
    usleep(15000);//wait 15ms
  }
  while (!finOdomdata.eof()){
    string strTmp;
    getline(finOdomdata,strTmp);
    int posLast=strTmp.find(',');
    timestamp=atof(strTmp.substr(0,posLast).c_str())/1e9;
    ++posLast;
    while (1){//until the image reading time is reached
      {
      unique_lock<mutex> lock(g_mutex);
      if (timestamp<=g_simulateTimestamp+gDelayCache||g_brgbdFinished)
	break;
      }
      usleep(1000);//allow 1ms delay
    }
    for (int i=0;i<nTotalNum;++i){//we should change wxyz,axyz to the order of axyz,wxyz in odomdata
      int pos=strTmp.find(',',posLast);
      string::size_type posNum;if (pos!=string::npos) posNum=pos-posLast;else posNum=string::npos;
      double dtmp=atof(strTmp.substr(posLast,posNum).c_str());
      if (i<nTotalNum/2)
	odomdata[nTotalNum/2+i]=dtmp;
      else
	odomdata[i-nTotalNum/2]=dtmp;
      posLast=pos+1;
    }
    //for (int i=0;i<6;++i) cout<<odomdata[i]<<" ";cout<<endl;
    if (timestamp>tmstpLast)//avoid == condition
      g_pSLAM->TrackOdom(timestamp,odomdata,(char)VIEO_SLAM::System::IMU);//for EuRoC dataset
    //cout<<green<<timestamp<<whiteSTR<<endl;
    tmstpLast=timestamp;
  }
  delete []odomdata;
  finOdomdata.close();
  cout<<greenSTR"Simulation of Odom Data Reading is over."<<whiteSTR<<endl;
}
//zzh over

int main(int argc, char **argv){
	ros::init(argc, argv, "Monopub");
	ros::start();
	if (!parseParams(argc, argv)) {
		return EXIT_FAILURE;
	}
	int n_images = vstrImageFilenames.size();

	// Create SLAM system. It initializes all system threads and gets ready to process frames.
	VIEO_SLAM::System SLAM(argv[1], argv[2], VIEO_SLAM::System::MONOCULAR, show_viewer);
	ros::NodeHandle nodeHandler;
	//ros::Publisher pub_cloud = nodeHandler.advertise<sensor_msgs::PointCloud2>("cloud_in", 1000);
	ros::Publisher pub_pts_and_pose = nodeHandler.advertise<geometry_msgs::PoseArray>("pts_and_pose", 1000);
	ros::Publisher pub_all_kf_and_pts = nodeHandler.advertise<geometry_msgs::PoseArray>("all_kf_and_pts", 1000);
	if (read_from_topic) {
		ImageGrabber igb(SLAM, pub_pts_and_pose, pub_all_kf_and_pts);
		ros::Subscriber sub = nodeHandler.subscribe(image_topic, 1, &ImageGrabber::GrabImage, &igb);
		ros::spin();
	}
	else{
		ros::Rate loop_rate(5);
		cv::Mat im;
		double tframe = 0;
#ifdef COMPILEDWITHC11
		std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
#else
		std::chrono::monotonic_clock::time_point t1 = std::chrono::monotonic_clock::now();
#endif
		//cv::namedWindow("Press r to reset");
		int frame_id = 0;
		while (read_from_camera || frame_id < n_images){
			if (read_from_camera) {
				cap_obj.read(im);
#ifdef COMPILEDWITHC11
				std::chrono::steady_clock::time_point t2 = std::chrono::steady_clock::now();
#else
				std::chrono::monotonic_clock::time_point t2 = std::chrono::monotonic_clock::now();
#endif
				tframe = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1).count();
				//printf("fps: %f\n", 1.0 / tframe);
			}
			else {
				// Read image from file
				im = cv::imread(vstrImageFilenames[frame_id], CV_LOAD_IMAGE_UNCHANGED);
				tframe = vTimestamps[frame_id];
			}
			if (im.empty()){
				cerr << endl << "Failed to load image at: " << vstrImageFilenames[frame_id] << endl;
				return 1;
			}
			// Pass the image to the SLAM system
			cv::Mat curr_pose = SLAM.TrackMonocular(im, tframe);

			publish(SLAM, pub_pts_and_pose, pub_all_kf_and_pts, frame_id);

			++frame_id;

			//			int key = cv::waitKey(1);
			//			int key_mod = key % 256;
			//			if (key == 'r' || key_mod == 'r' || key == 'r' || key_mod == 'r') {
			//				printf("Resetting the SLAM system\n");
			//				SLAM.Shutdown();
			//				SLAM.reset(argv[2], show_viewer);
			//#ifdef COMPILEDWITHC11
			//				t1 = std::chrono::steady_clock::now();
			//#else
			//				t1 = std::chrono::monotonic_clock::now();
			//#endif
			//				frame_id = 0;
			//			}
			//cv::imshow("Press escape to exit", im);
			//if (cv::waitKey(1) == 27) {
			//	break;
			//}
			ros::spinOnce();
			loop_rate.sleep();
			if (!ros::ok()){ break; }
		}
	}
	//ros::spin();

	// Stop all threads
	SLAM.Shutdown();
	//geometry_msgs::PoseArray pt_array;
	//pt_array.header.seq = 0;
	//pub_pts_and_pose.publish(pt_array);
	ros::shutdown();
	return 0;
}

void publish(VIEO_SLAM::System &SLAM, ros::Publisher &pub_pts_and_pose,
	ros::Publisher &pub_all_kf_and_pts, int frame_id) {
	if (all_pts_pub_gap > 0 && pub_count >= all_pts_pub_gap) {
		pub_all_pts = true;
		pub_count = 0;
	}
	if (pub_all_pts || SLAM.GetLoopDetected()) {
		pub_all_pts = false;SLAM.SetLoopDetected(false);
		geometry_msgs::PoseArray kf_pt_array;
		vector<VIEO_SLAM::KeyFrame*> key_frames = SLAM.GetAllKeyFrames();
		//! placeholder for number of keyframes
		kf_pt_array.poses.push_back(geometry_msgs::Pose());
		sort(key_frames.begin(), key_frames.end(), VIEO_SLAM::KeyFrame::lId);
		unsigned int n_kf = 0;
		unsigned int n_pts_id = 0;
		for (auto key_frame : key_frames) {
			// pKF->SetPose(pKF->GetPose()*Two);

			if (!key_frame || key_frame->isBad()) {
				continue;
			}

			cv::Mat R = key_frame->GetRotation().t();
			vector<float> q = VIEO_SLAM::Converter::toQuaternion(R);
			cv::Mat twc = key_frame->GetCameraCenter();
			geometry_msgs::Pose kf_pose;

			kf_pose.position.x = twc.at<float>(0);
			kf_pose.position.y = twc.at<float>(1);
			kf_pose.position.z = twc.at<float>(2);
			kf_pose.orientation.x = q[0];
			kf_pose.orientation.y = q[1];
			kf_pose.orientation.z = q[2];
			kf_pose.orientation.w = q[3];
			kf_pt_array.poses.push_back(kf_pose);

			n_pts_id = kf_pt_array.poses.size();
			//! placeholder for number of points
			kf_pt_array.poses.push_back(geometry_msgs::Pose());
			std::set<VIEO_SLAM::MapPoint*> map_points = key_frame->GetMapPoints();
			unsigned int n_pts = 0;
			for (auto map_pt : map_points) {
				if (!map_pt || map_pt->isBad()) {
					//printf("Point %d is bad\n", pt_id);
					continue;
				}
				cv::Mat pt_pose = map_pt->GetWorldPos();
				if (pt_pose.empty()) {
					//printf("World position for point %d is empty\n", pt_id);
					continue;
				}
				geometry_msgs::Pose curr_pt;
				//printf("wp size: %d, %d\n", wp.rows, wp.cols);
				//pcl_cloud->push_back(pcl::PointXYZ(wp.at<float>(0), wp.at<float>(1), wp.at<float>(2)));
				curr_pt.position.x = pt_pose.at<float>(0);
				curr_pt.position.y = pt_pose.at<float>(1);
				curr_pt.position.z = pt_pose.at<float>(2);
				kf_pt_array.poses.push_back(curr_pt);
				++n_pts;
			}
			kf_pt_array.poses[n_pts_id].position.x = (double)n_pts;
			kf_pt_array.poses[n_pts_id].position.y = (double)n_pts;
			kf_pt_array.poses[n_pts_id].position.z = (double)n_pts;
			++n_kf;
		}
		kf_pt_array.poses[0].position.x = (double)n_kf;
		kf_pt_array.poses[0].position.y = (double)n_kf;
		kf_pt_array.poses[0].position.z = (double)n_kf;
		kf_pt_array.header.frame_id = "1";
		kf_pt_array.header.seq = frame_id + 1;
		printf("Publishing data for %u keyfranmes\n", n_kf);
		pub_all_kf_and_pts.publish(kf_pt_array);
	}
	else if (SLAM.GetKeyFrameCreated()) {
		++pub_count;
		SLAM.SetKeyFrameCreated(false);

		cv::Mat Trw = cv::Mat::eye(4, 4, CV_32F);

		// If the reference keyframe was culled, traverse the spanning tree to get a suitable keyframe.
		//while (pKF->isBad())
		//{
		//	Trw = Trw*pKF->mTcp;
		//	pKF = pKF->GetParent();
		//}

// 		vector<VIEO_SLAM::KeyFrame*> vpKFs = SLAM.GetAllKeyFrames();
// 		sort(vpKFs.begin(), vpKFs.end(), VIEO_SLAM::KeyFrame::lId);

		// Transform all keyframes so that the first keyframe is at the origin.
		// After a loop closure the first keyframe might not be at the origin. but it this is rectified, we should rectify Maps' position as well!
// 		cv::Mat Two = vpKFs[0]->GetPoseInverse();

// 		Trw = Trw*pKF->GetPose()*Two;
// 		cv::Mat lit = SLAM.getTracker()->mlRelativeFramePoses.back();
// 		cv::Mat Tcw = lit*Trw;
		cv::Mat Tcw=SLAM.GetKeyFramePose();
		cv::Mat Rwc = Tcw.rowRange(0, 3).colRange(0, 3).t();
		cv::Mat twc = -Rwc*Tcw.rowRange(0, 3).col(3);

		vector<float> q = VIEO_SLAM::Converter::toQuaternion(Rwc);
		//geometry_msgs::Pose camera_pose;
		//std::vector<VIEO_SLAM::MapPoint*> map_points = SLAM.getMap()->GetAllMapPoints();
		std::vector<VIEO_SLAM::MapPoint*> map_points = SLAM.GetTrackedMapPoints();
		int n_map_pts = map_points.size();

		//printf("n_map_pts: %d\n", n_map_pts);

		//pcl::PointCloud<pcl::PointXYZ>::Ptr pcl_cloud(new pcl::PointCloud<pcl::PointXYZ>);

		geometry_msgs::PoseArray pt_array;
		//pt_array.poses.resize(n_map_pts + 1);

		geometry_msgs::Pose camera_pose;

		camera_pose.position.x = twc.at<float>(0);
		camera_pose.position.y = twc.at<float>(1);
		camera_pose.position.z = twc.at<float>(2);

		camera_pose.orientation.x = q[0];
		camera_pose.orientation.y = q[1];
		camera_pose.orientation.z = q[2];
		camera_pose.orientation.w = q[3];

		pt_array.poses.push_back(camera_pose);

		//printf("Done getting camera pose\n");

		for (int pt_id = 1; pt_id <= n_map_pts; ++pt_id){

			if (!map_points[pt_id - 1] || map_points[pt_id - 1]->isBad()) {
				//printf("Point %d is bad\n", pt_id);
				continue;
			}
			cv::Mat wp = map_points[pt_id - 1]->GetWorldPos();

			if (wp.empty()) {
				//printf("World position for point %d is empty\n", pt_id);
				continue;
			}
			geometry_msgs::Pose curr_pt;
			//printf("wp size: %d, %d\n", wp.rows, wp.cols);
			//pcl_cloud->push_back(pcl::PointXYZ(wp.at<float>(0), wp.at<float>(1), wp.at<float>(2)));
			curr_pt.position.x = wp.at<float>(0);
			curr_pt.position.y = wp.at<float>(1);
			curr_pt.position.z = wp.at<float>(2);
			pt_array.poses.push_back(curr_pt);
			//printf("Done getting map point %d\n", pt_id);
		}
		//sensor_msgs::PointCloud2 ros_cloud;
		//pcl::toROSMsg(*pcl_cloud, ros_cloud);
		//ros_cloud.header.frame_id = "1";
		//ros_cloud.header.seq = ni;

		//printf("valid map pts: %lu\n", pt_array.poses.size()-1);

		//printf("ros_cloud size: %d x %d\n", ros_cloud.height, ros_cloud.width);
		//pub_cloud.publish(ros_cloud);
		pt_array.header.frame_id = "1";
		pt_array.header.seq = frame_id + 1;
		pub_pts_and_pose.publish(pt_array);
		//pub_kf.publish(camera_pose);
	}
}

inline bool isInteger(const std::string & s){
	if (s.empty() || ((!isdigit(s[0])) && (s[0] != '-') && (s[0] != '+'))) return false;

	char * p;
	strtol(s.c_str(), &p, 10);

	return (*p == 0);
}

void LoadImages(const string &strImagePath, const string &strPathTimes,
                vector<string> &vstrImages, vector<double> &vTimeStamps)
{
    ifstream fTimes;
    fTimes.open(strPathTimes.c_str());
    vTimeStamps.reserve(5000);
    vstrImages.reserve(5000);
    while(!fTimes.eof())
    {
        string s;
        getline(fTimes,s);
        if(!s.empty())
        {
            stringstream ss;
            ss << s;
            vstrImages.push_back(strImagePath + "/" + ss.str() + ".png");
            double t;
            ss >> t;
            vTimeStamps.push_back(t/1e9);

        }
    }
}

void ImageGrabber::GrabImage(const sensor_msgs::ImageConstPtr& msg){
	// Copy the ros image message to cv::Mat.
	cv_bridge::CvImageConstPtr cv_ptr;
	try{
		cv_ptr = cv_bridge::toCvShare(msg);
	}
	catch (cv_bridge::Exception& e){
		ROS_ERROR("cv_bridge exception: %s", e.what());
		return;
	}
	SLAM.TrackMonocular(cv_ptr->image, cv_ptr->header.stamp.toSec());
	publish(SLAM, pub_pts_and_pose, pub_all_kf_and_pts, frame_id);
	++frame_id;
}

bool parseParams(int argc, char **argv) {
	if (argc < 5){
		cerr << endl << "Usage: rosrun VIEO_SLAM Monopub path_to_vocabulary path_to_settings path_to_sequence path_to_times_file (all_pts_pub_gap show_viewer)" << endl;
		return 1;
	}
	LoadImages(string(argv[3]),string(argv[4]), vstrImageFilenames, vTimestamps);
	int arg_id = 5;
	if (argc > arg_id) {
		all_pts_pub_gap = atoi(argv[arg_id++]);
	}
	if (argc > arg_id) {
		show_viewer = atoi(argv[arg_id++]);
	}
	printf("all_pts_pub_gap: %d\n", all_pts_pub_gap);
	printf("show_viewer: %d\n", show_viewer);
	return 1;
}




