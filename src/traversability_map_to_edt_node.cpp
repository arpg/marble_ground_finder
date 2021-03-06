#include <math.h>
#include "edt.hpp"
// Octomap libaries
#include <octomap/octomap.h>
#include <octomap/ColorOcTree.h>
#include <rough_octomap/RoughOcTree.h>
#include <octomap_msgs/Octomap.h>
#include <octomap_msgs/conversions.h>
// ROS Libraries
#include <ros/ros.h>
#include <tf/transform_listener.h>
#include <sensor_msgs/PointCloud2.h>
#include <geometry_msgs/PoseStamped.h>
#include <nav_msgs/Odometry.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/features/normal_3d.h>
#include <pcl/kdtree/kdtree.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/filters/crop_box.h>
#include <pcl/filters/voxel_grid.h>
// Eigen
#include <Eigen/Core>

octomap::OcTree* map_octree;
bool map_updated = false;
octomap::RoughOcTree* rough_octree;
pcl::PointCloud<pcl::PointXYZI>::Ptr rough_cloud (new pcl::PointCloud<pcl::PointXYZI>);
bool rough_updated = false;

void index3_xyz(const int index, double point[3], double min[3], int size[3], double voxel_size)
{
  // x+y*sizx+z*sizx*sizy
  point[2] = min[2] + (index/(size[1]*size[0]))*voxel_size;
  point[1] = min[1] + ((index % (size[1]*size[0]))/size[0])*voxel_size;
  point[0] = min[0] + ((index % (size[1]*size[0])) % size[0])*voxel_size;
}

int xyz_index3(const double point[3], double min[3], int size[3], double voxel_size)
{
  int ind[3];
  for (int i=0; i<3; i++) ind[i] = round((point[i]-min[i])/voxel_size);
  return (ind[0] + ind[1]*size[0] + ind[2]*size[0]*size[1]);
}

bool CheckPointInBounds(double p[3], double min[3], double max[3])
{
  for (int i=0; i<3; i++) {
    if ((p[i] <= min[i]) || (p[i] >= max[i])) return false;
  }
  return true;
}

struct RobotState
{
  Eigen::Vector3f position;
  double sensor_range;
};

class NodeManager
{
  public:
    NodeManager():
    ground_cloud (new pcl::PointCloud<pcl::PointXYZI>),
    edt_cloud (new pcl::PointCloud<pcl::PointXYZI>)
    {
      // map_octree = new octomap::OcTree(0.1);
    }
    bool use_tf = false;
    tf::TransformListener listener;
    std::string robot_frame_id;
    std::string fixed_frame_id;
    sensor_msgs::PointCloud2 ground_msg;
    sensor_msgs::PointCloud2 edt_msg;
    std::vector<ros::Publisher> debug_publishers;
    bool position_updated = false;
    int min_cluster_size;
    RobotState robot;
    float normal_z_threshold;
    float normal_curvature_threshold;
    float truncation_distance = 100.0; // meters
    float inflate_distance = 0.0; // meters
    float max_roughness = 0.5; // [0.0, 1.0]
    bool filter_holes = false;
    int padding = 1;
    pcl::PointCloud<pcl::PointXYZI>::Ptr ground_cloud;
    pcl::PointCloud<pcl::PointXYZI>::Ptr edt_cloud;
    // void CallbackOctomap(const octomap_msgs::Octomap::ConstPtr msg);
    void CallbackOdometry(const nav_msgs::Odometry msg);
    void FindGroundVoxels(std::string map_size);
    void FindGroundVoxels_RoughOctomap(std::string map_size);
    void UpdateRobotState();
    void GetGroundMsg();
    void GetEdtMsg();
    // void FilterNormals();
    // void FilterContiguous();
};

sensor_msgs::PointCloud2 ConvertCloudToMsg(pcl::PointCloud<pcl::PointXYZ>::Ptr cloud, std::string frame_id)
{
  sensor_msgs::PointCloud2 msg;
  pcl::toROSMsg(*cloud, msg);
  msg.header.seq = 1;
  msg.header.stamp = ros::Time();
  msg.header.frame_id = frame_id;
  return msg;
}
sensor_msgs::PointCloud2 ConvertCloudToMsg(pcl::PointCloud<pcl::PointXYZI>::Ptr cloud, std::string frame_id)
{
  sensor_msgs::PointCloud2 msg;
  pcl::toROSMsg(*cloud, msg);
  msg.header.seq = 1;
  msg.header.stamp = ros::Time();
  msg.header.frame_id = frame_id;
  return msg;
}

void CalculatePointCloudEDT(bool *occupied_mat, pcl::PointCloud<pcl::PointXYZI>::Ptr edt_cloud, double min[3], int size[3], double voxel_size, float truncation_distance)
{
  // Call EDT function
  float* dt = edt::edt<bool>(occupied_mat, /*sx=*/size[0], /*sy=*/size[1], /*sz=*/size[2],
  /*wx=*/1.0, /*wy=*/1.0, /*wz=*/1.0, /*black_border=*/false);

  // Parse EDT result into output PointCloud
  double max[3];
  for (int i=0; i<3; i++) max[i] = min[i] + (size[i]-1)*voxel_size;
  for (int i=0; i<edt_cloud->points.size(); i++) {
    double query[3] = {(double)edt_cloud->points[i].x, (double)edt_cloud->points[i].y, (double)edt_cloud->points[i].z};
    if (CheckPointInBounds(query, min, max)) {
      int idx = xyz_index3(query, min, size, voxel_size);
      float distance = (float)dt[idx]*voxel_size;
      // if (distance < edt_cloud->points[i].intensity) edt_cloud->points[i].intensity = distance;
      edt_cloud->points[i].intensity = std::min(distance, truncation_distance);
    }
  }

  delete[] dt;
  return;
}

void InflateObstacles(pcl::PointCloud<pcl::PointXYZI>::Ptr edt_cloud, float inflate_distance)
{
  for (int i=0; i<edt_cloud->points.size(); i++) {
    edt_cloud->points[i].intensity = edt_cloud->points[i].intensity - inflate_distance;
  }
  return;
}

void CallbackOctomap(const octomap_msgs::Octomap::ConstPtr msg)
{
  ROS_INFO("Subscribing to Octomap...");
  if (msg->data.size() == 0) return;
  delete map_octree;
  map_octree = (octomap::OcTree*)octomap_msgs::fullMsgToMap(*msg);
  map_updated = true;
}

void CallbackRoughOctomap(const octomap_msgs::Octomap::ConstPtr msg)
{
  ROS_INFO("Subscribing to Rough Octomap...");
  if (msg->data.size() == 0) return;
  delete rough_octree;
  rough_octree = (octomap::RoughOcTree*)octomap_msgs::fullMsgToMap(*msg);
  map_updated = true;
}

void CallbackRoughCloud(const sensor_msgs::PointCloud2::ConstPtr msg)
{
  ROS_INFO("Subscribing to surface roughness cloud...");
  if (msg->data.size() == 0) return;
  pcl::fromROSMsg(*msg, *rough_cloud);
  rough_updated = true;
}

void NodeManager::CallbackOdometry(nav_msgs::Odometry msg)
{
  if (use_tf) return;
  robot.position[0] = msg.pose.pose.position.x;
  robot.position[1] = msg.pose.pose.position.y;
  robot.position[2] = msg.pose.pose.position.z;
  position_updated = true;
  return;
}

void NodeManager::UpdateRobotState()
{
  ROS_INFO("Updating robot state through tf listener...");
  if (!(use_tf)) return;
  tf::StampedTransform transform;
  try{
    listener.lookupTransform(fixed_frame_id, robot_frame_id,
                              ros::Time(0), transform);
    robot.position[0] = transform.getOrigin().x();
    robot.position[1] = transform.getOrigin().y();
    robot.position[2] = transform.getOrigin().z();
    position_updated = true;
    ROS_INFO("Robot @ (%0.2f, %0.2f, %0.2f) relative to world.", robot.position[0], robot.position[1], robot.position[2]);
  }
  catch (tf::TransformException ex){
    ROS_ERROR("%s",ex.what());
    robot.position[0] = 0.0;
    robot.position[1] = 0.0;
    robot.position[2] = 0.0;
  }
}

void NodeManager::GetGroundMsg()
{
  sensor_msgs::PointCloud2 msg;
  pcl::toROSMsg(*ground_cloud, msg);
  msg.header.seq = 1;
  msg.header.stamp = ros::Time();
  msg.header.frame_id = fixed_frame_id;
  ground_msg = msg;
}

void NodeManager::GetEdtMsg()
{
  sensor_msgs::PointCloud2 msg;
  pcl::toROSMsg(*edt_cloud, msg);
  msg.header.seq = 1;
  msg.header.stamp = ros::Time();
  msg.header.frame_id = fixed_frame_id;
  edt_msg = msg;
}

void NodeManager::FindGroundVoxels(std::string map_size)
{
  if (map_updated) {
    map_updated = false;
  } else {
    return;
  }

  if (rough_updated) {
    rough_updated = false;
  } else {
    return;
  }

  UpdateRobotState();
  if (!(position_updated)) return;

  // Store voxel_size, this shouldn't change throughout the node running,
  // but something weird will happen if it does.
  double voxel_size = map_octree->getResolution();

  // Get map minimum and maximum dimensions
  double x_min_tree, y_min_tree, z_min_tree;
  map_octree->getMetricMin(x_min_tree, y_min_tree, z_min_tree);
  double min_tree[3] = {x_min_tree, y_min_tree, z_min_tree};
  double x_max_tree, y_max_tree, z_max_tree;
  map_octree->getMetricMax(x_max_tree, y_max_tree, z_max_tree);
  double max_tree[3] = {x_max_tree, y_max_tree, z_max_tree};

  ROS_INFO("Calculating bounding box. Map size =");
  std::cout << map_size << std::endl;
  // ***** //
  // Find a bounding box around the robot's current position to limit the map queries and cap compute/memory
  double bbx_min_array[3];
  double bbx_max_array[3];

  // Check if this iteration requires the full map.
  if (map_size == "bbx") {
    for (int i=0; i<3; i++) {
      bbx_min_array[i] = min_tree[i] + std::round((robot.position[i] - 2.0*robot.sensor_range - min_tree[i])/voxel_size)*voxel_size - 2.95*voxel_size;
      // bbx_min_array[i] = std::max(bbx_min_array[i], min_tree[i] - 1.5*voxel_size);
      bbx_max_array[i] = min_tree[i] + std::round((robot.position[i] + 2.0*robot.sensor_range - min_tree[i])/voxel_size)*voxel_size + 2.95*voxel_size;
      // bbx_max_array[i] = std::min(bbx_max_array[i], max_tree[i] + 1.5*voxel_size);
    }
  }
  else {
    for (int i=0; i<3; i++) {
      bbx_min_array[i] = min_tree[i] - 1.5*voxel_size;
      bbx_max_array[i] = max_tree[i] + 1.5*voxel_size;
    }
  }

  Eigen::Vector4f bbx_min(bbx_min_array[0], bbx_min_array[1], bbx_min_array[2], 1.0);
  Eigen::Vector4f bbx_max(bbx_max_array[0], bbx_max_array[1], bbx_max_array[2], 1.0);
  octomap::point3d bbx_min_octomap(bbx_min_array[0], bbx_min_array[1], bbx_min_array[2]);
  octomap::point3d bbx_max_octomap(bbx_max_array[0], bbx_max_array[1], bbx_max_array[2]);
  // ***** //
  ROS_INFO("Box has x,y,z limits of [%0.1f to %0.1f, %0.1f to %0.1f, and %0.1f to %0.1f] meters.",
  bbx_min_array[0], bbx_max_array[0], bbx_min_array[1], bbx_max_array[1], bbx_min_array[2], bbx_max_array[2]);

  ROS_INFO("Allocating occupied bool flat matrix memory.");
  // Allocate memory for the occupied cells within the bounding box in a flat 3D boolean array
  int bbx_size[3];
  for (int i=0; i<3; i++) {
    bbx_size[i] = (int)std::round((bbx_max[i] - bbx_min[i])/voxel_size) + 1;
  }
  int bbx_mat_length = bbx_size[0]*bbx_size[1]*bbx_size[2];
  // bool occupied_mat[bbx_mat_length];
  bool* occupied_mat = new bool[bbx_mat_length]; // Allows for more memory allocation
  for (int i=0; i<bbx_mat_length; i++) occupied_mat[i] = true;

  ROS_INFO("Removing the voxels within the bounding box from the ground_cloud of length %d", (int)ground_cloud->points.size());

  // ***** //
  // Iterate through that box
  ROS_INFO("Beginning tree iteration through map of size %d", (int)map_octree->size());
  // Initialize a PCL object to hold preliminary ground voxels
  pcl::PointCloud<pcl::PointXYZI>::Ptr ground_cloud_prefilter(new pcl::PointCloud<pcl::PointXYZI>);
  pcl::PointCloud<pcl::PointXYZI>::Ptr ground_cloud_traversable(new pcl::PointCloud<pcl::PointXYZI>); // input points that have already been labeled untraversable
  pcl::PointCloud<pcl::PointXYZI>::Ptr ground_cloud_free(new pcl::PointCloud<pcl::PointXYZI>); // free voxels with unseen below.
  pcl::PointCloud<pcl::PointXYZ>::Ptr obstacle_cloud(new pcl::PointCloud<pcl::PointXYZ>);

  // Expand octomap 
  map_octree->expand();

  for(octomap::OcTree::leaf_bbx_iterator
  it = map_octree->begin_leafs_bbx(bbx_min_octomap, bbx_max_octomap);
  it != map_octree->end_leafs_bbx(); ++it)
  {
    double query[3];
    query[0] = it.getX(); query[1] = it.getY(); query[2] = it.getZ();
    if (it->getOccupancy() <= 0.4) { // free
      // Check if the cell below it is unseen
     octomap::OcTreeNode* node = map_octree->search(query[0], query[1], query[2] - voxel_size);
      if (node) {
        if ((node->getOccupancy() <= 0.55) && (node->getOccupancy() >= 0.45)) {
          pcl::PointXYZI query_point;
          query_point.x = query[0]; query_point.y = query[1]; query_point.z = query[2];
          query_point.intensity = (float)-1.0; // (.intensity == -1.0) --> free voxel
          ground_cloud_prefilter->points.push_back(query_point);
          ground_cloud_free->points.push_back(query_point);
        }
      } else {
        pcl::PointXYZI query_point;
        query_point.x = query[0]; query_point.y = query[1]; query_point.z = query[2];
        query_point.intensity = (float)-1.0; // (.intensity == -1.0) --> free voxel
        ground_cloud_prefilter->points.push_back(query_point);
        ground_cloud_free->points.push_back(query_point);
      }
    }
  }
  // ***** //

  // Iterate through roughness pointcloud and add all points below max_roughness to the initial ground_cloud and all others to the occupied_mat
  pcl::PointCloud<pcl::PointXYZI>::Ptr rough_cloud_bbx (new pcl::PointCloud<pcl::PointXYZI>);
  pcl::CropBox<pcl::PointXYZI> box_filter_rough;
  box_filter_rough.setMin(bbx_min);
  box_filter_rough.setMax(bbx_max);
  box_filter_rough.setNegative(false);
  box_filter_rough.setInputCloud(rough_cloud);
  box_filter_rough.filter(*rough_cloud_bbx);
  for (int i=0; i<rough_cloud_bbx->points.size(); i++) {
    pcl::PointXYZI rough_voxel = rough_cloud_bbx->points[i];
    if ((rough_voxel.intensity <= max_roughness) || (std::isnan(rough_voxel.intensity)))
    {
      ground_cloud_traversable->points.push_back(rough_voxel);
      ground_cloud_prefilter->points.push_back(rough_voxel);
    } else if ((rough_voxel.intensity >= max_roughness) && (rough_voxel.intensity <= 1.1)) {
      double query[3] = {rough_voxel.x, rough_voxel.y, rough_voxel.z};
      int id = xyz_index3(query, bbx_min_array, bbx_size, voxel_size);
      occupied_mat[id] = false;
      pcl::PointXYZ query_point;
      query_point.x = query[0]; query_point.y = query[1]; query_point.z = query[2];
      obstacle_cloud->points.push_back(query_point);
    }
  }

  // Publish the initial ground cloud and the negative ground only cloud
  debug_publishers[0].publish(ConvertCloudToMsg(ground_cloud_free, fixed_frame_id));
  debug_publishers[1].publish(ConvertCloudToMsg(ground_cloud_prefilter, fixed_frame_id));
  debug_publishers[2].publish(ConvertCloudToMsg(ground_cloud_traversable, fixed_frame_id));
  ROS_INFO("Published points initially labeled ground before filtering.");
  // sleep(5.0);

  ROS_INFO("Normal vector filtering initial ground PointCloud of length %d", (int)ground_cloud_prefilter->points.size());
  // ***** //
  // Filter ground by local normal vector
  pcl::PointCloud<pcl::PointXYZI>::Ptr ground_cloud_normal_filtered (new pcl::PointCloud<pcl::PointXYZI>);
  pcl::PointCloud<pcl::PointXYZI>::Ptr negative_obstacle_cloud (new pcl::PointCloud<pcl::PointXYZI>);
  pcl::NormalEstimation<pcl::PointXYZI, pcl::Normal> normal_filter;
  normal_filter.setInputCloud(ground_cloud_prefilter);
  pcl::search::KdTree<pcl::PointXYZI>::Ptr kdtree_normal (new pcl::search::KdTree<pcl::PointXYZI>());
  kdtree_normal->setInputCloud(ground_cloud_prefilter);
  normal_filter.setSearchMethod(kdtree_normal);
  pcl::PointCloud<pcl::Normal>::Ptr cloud_normals (new pcl::PointCloud<pcl::Normal>);
  normal_filter.setRadiusSearch(5.0*map_octree->getResolution());
  normal_filter.setViewPoint(0.0, 0.0, 2.0);
  normal_filter.compute(*cloud_normals);

  for (int i=0; i<cloud_normals->points.size(); i++) {
    pcl::PointXYZI query = ground_cloud_prefilter->points[i];
    pcl::Normal query_normal = cloud_normals->points[i];
    if ((std::abs(query_normal.normal_z) >= normal_z_threshold) && (std::abs(query_normal.curvature) <= normal_curvature_threshold)) {
        ground_cloud_normal_filtered->points.push_back(query);
    } else {
      if (query.intensity <= -0.5) {
        negative_obstacle_cloud->points.push_back(query);
      }
    }
  }
  debug_publishers[3].publish(ConvertCloudToMsg(ground_cloud_normal_filtered, fixed_frame_id));
  debug_publishers[5].publish(ConvertCloudToMsg(negative_obstacle_cloud, fixed_frame_id));
  debug_publishers[6].publish(ConvertCloudToMsg(obstacle_cloud, fixed_frame_id));
  // ***** //

  ROS_INFO("Contiguity filtering normal filtered cloud of length %d...", (int)ground_cloud_normal_filtered->points.size());
  // ROS_INFO("Contiguity filtering normal filtered cloud of length %d...", (int)ground_cloud_prefilter->points.size());
  // ***** //
  // Filter ground by contiguity (is this necessary?)
  pcl::search::KdTree<pcl::PointXYZI>::Ptr kdtree(new pcl::search::KdTree<pcl::PointXYZI>);
  kdtree->setInputCloud(ground_cloud_normal_filtered);
  // kdtree->setInputCloud(ground_cloud_prefilter);

  // Initialize euclidean cluster extraction object
  std::vector<pcl::PointIndices> cluster_indices;
  pcl::EuclideanClusterExtraction<pcl::PointXYZI> euclidean_cluster_extractor;
  euclidean_cluster_extractor.setClusterTolerance(1.8*voxel_size); // Clusters must be made of contiguous sections of ground (within sqrt(2)*voxel_size of each other)
  euclidean_cluster_extractor.setMinClusterSize(min_cluster_size); // Cluster must be at least 15 voxels in size
  euclidean_cluster_extractor.setSearchMethod(kdtree);
  euclidean_cluster_extractor.setInputCloud(ground_cloud_normal_filtered);
  // euclidean_cluster_extractor.setInputCloud(ground_cloud_prefilter); // try w/o normal filtering
  euclidean_cluster_extractor.extract(cluster_indices);
  ROS_INFO("Clusters extracted.");

  // Extract a local bounding box from the ground_cloud
  pcl::PointCloud<pcl::PointXYZI>::Ptr ground_cloud_local (new pcl::PointCloud<pcl::PointXYZI>);
  pcl::CropBox<pcl::PointXYZI> box_filter;
  box_filter.setMin(bbx_min);
  box_filter.setMax(bbx_max);
  box_filter.setNegative(true);
  box_filter.setInputCloud(ground_cloud);
  box_filter.filter(*ground_cloud_local);

  // Extract local bounding box from the edt_cloud
  pcl::PointCloud<pcl::PointXYZI>::Ptr edt_cloud_local (new pcl::PointCloud<pcl::PointXYZI>);
  pcl::PointCloud<pcl::PointXYZI>::Ptr edt_cloud_bbx (new pcl::PointCloud<pcl::PointXYZI>);
  pcl::PointCloud<pcl::PointXYZI>::Ptr edt_cloud_bbx_smaller (new pcl::PointCloud<pcl::PointXYZI>);

  ROS_INFO("Copying clusters above minimum cluster size...");
  // Add the biggest (or the one with the robot in it) to the ground_cloud.
  if (cluster_indices.size() > 0) {
    for (int j=0; j< cluster_indices.size(); j++){
      if (cluster_indices[j].indices.size() >= min_cluster_size) {
        for (int i=0; i<cluster_indices[j].indices.size(); i++) {
          pcl::PointXYZI ground_point = ground_cloud_normal_filtered->points[cluster_indices[j].indices[i]];
          // pcl::PointXYZI ground_point = ground_cloud_prefilter->points[cluster_indices[j].indices[i]];
          if (ground_point.intensity <= -0.5) {
            ground_cloud_local->points.push_back(ground_point);
            pcl::PointXYZI edt_point = ground_point;
            edt_point.intensity = 0.0;
            edt_cloud_bbx->points.push_back(edt_point);
            for (int i=0; i<padding; i++) {
              edt_point.z = edt_point.z + voxel_size; // Padding
              edt_cloud_bbx->points.push_back(edt_point); // Padding
            }
          }
        }
      }
    }
  } else {
    ROS_INFO("No new cloud entries, publishing previous cloud msg");
    return;
  }

  // Add in all the "traversable" voxels from the RoughOcTree
  for (int i=0; i<ground_cloud_traversable->points.size(); i++) {
    pcl::PointXYZI ground_point = ground_cloud_traversable->points[i];
    ground_cloud_local->points.push_back(ground_point);
    pcl::PointXYZI edt_point = ground_point;
    edt_point.intensity = 0.0; // Consider passing traversability in to penalize rougher points.
    edt_cloud_bbx->points.push_back(edt_point);
    for (int i=0; i<padding; i++) {
      edt_point.z = edt_point.z + voxel_size; // Padding
      edt_cloud_bbx->points.push_back(edt_point); // Padding
    }
  }

  // Use a voxel_grid filter to remove duplicate voxel entries.
  pcl::PointCloud<pcl::PointXYZI>::Ptr edt_cloud_bbx_filtered (new pcl::PointCloud<pcl::PointXYZI>);
  pcl::VoxelGrid<pcl::PointXYZI> sor;
  sor.setInputCloud (edt_cloud_bbx);
  sor.setLeafSize (0.1*voxel_size, 0.1*voxel_size, 0.1*voxel_size);
  sor.filter (*edt_cloud_bbx_filtered);
  edt_cloud_bbx->points.clear();
  pcl::PointCloud<pcl::PointXYZI>::Ptr ground_cloud_local_filtered (new pcl::PointCloud<pcl::PointXYZI>);
  sor.setInputCloud (ground_cloud_local);
  sor.setLeafSize (0.1*voxel_size, 0.1*voxel_size, 0.1*voxel_size);
  sor.filter (*ground_cloud_local_filtered);


  // Clear ground_cloud and deep copy it from ground_cloud_local
  ground_cloud->points.clear();
  for (int i=0; i<ground_cloud_local_filtered->points.size(); i++) {
    ground_cloud->points.push_back(ground_cloud_local_filtered->points[i]);
  }

  // Calculate EDT bbx
  double bbx_min_array_edt[3];
  double bbx_max_array_edt[3];
  if (map_size == "bbx") {
    for (int i=0; i<3; i++) {
      double bbx_center = (bbx_min_array[i] + bbx_max_array[i])/2.0;
      bbx_min_array_edt[i] = bbx_center - (bbx_size[i]/4.0)*voxel_size;
      bbx_max_array_edt[i] = bbx_center + (bbx_size[i]/4.0)*voxel_size;
    }
  }
  else {
    for (int i=0; i<3; i++) {
      bbx_min_array_edt[i] = bbx_min_array[i];
      bbx_max_array_edt[i] = bbx_max_array[i];
    }
  }
  Eigen::Vector4f bbx_min_edt(bbx_min_array_edt[0], bbx_min_array_edt[1], bbx_min_array_edt[2], 0.0);
  Eigen::Vector4f bbx_max_edt(bbx_max_array_edt[0], bbx_max_array_edt[1], bbx_max_array_edt[2], 0.0);

  pcl::CropBox<pcl::PointXYZI> box_filter2;
  box_filter2.setMin(bbx_min_edt);
  box_filter2.setMax(bbx_max_edt);
  box_filter2.setNegative(false);
  box_filter2.setInputCloud(edt_cloud_bbx_filtered);
  box_filter2.filter(*edt_cloud_bbx_smaller);

  // EDT Calculation
  ROS_INFO("Calculating EDT.");
  CalculatePointCloudEDT(occupied_mat, edt_cloud_bbx_smaller, bbx_min_array, bbx_size, voxel_size, truncation_distance);
  InflateObstacles(edt_cloud_bbx_smaller, inflate_distance);
  ROS_INFO("EDT calculated.");

  // Copy to edt_cloud

  if (map_size == "bbx") {
    pcl::CropBox<pcl::PointXYZI> box_filter3;
    box_filter3.setMin(bbx_min_edt);
    box_filter3.setMax(bbx_max_edt);
    box_filter3.setNegative(true);
    box_filter3.setInputCloud(edt_cloud);
    box_filter3.filter(*edt_cloud_local);

    for (int i=0; i<edt_cloud_bbx_smaller->points.size(); i++) {
      edt_cloud_local->points.push_back(edt_cloud_bbx_smaller->points[i]);
    }

    edt_cloud->points.clear();
    for (int i=0; i<edt_cloud_local->points.size(); i++) {
      edt_cloud->points.push_back(edt_cloud_local->points[i]);
    }
  }
  else {
    edt_cloud->points.clear();
    for (int i=0; i<edt_cloud_bbx_smaller->points.size(); i++) {
      edt_cloud->points.push_back(edt_cloud_bbx_smaller->points[i]);
    }
  }
  

  GetGroundMsg();
  GetEdtMsg();
  delete[] occupied_mat;
  ROS_INFO("Publishing edt");
  return;
}

void NodeManager::FindGroundVoxels_RoughOctomap(std::string map_size)
{
  if (map_updated) {
    map_updated = false;
  } else {
    return;
  }

  UpdateRobotState();
  if (!(position_updated)) return;

  // Store voxel_size, this shouldn't change throughout the node running,
  // but something weird will happen if it does.
  double voxel_size = rough_octree->getResolution();

  // Get map minimum and maximum dimensions
  double x_min_tree, y_min_tree, z_min_tree;
  rough_octree->getMetricMin(x_min_tree, y_min_tree, z_min_tree);
  double min_tree[3] = {x_min_tree, y_min_tree, z_min_tree};
  double x_max_tree, y_max_tree, z_max_tree;
  rough_octree->getMetricMax(x_max_tree, y_max_tree, z_max_tree);
  double max_tree[3] = {x_max_tree, y_max_tree, z_max_tree};

  ROS_INFO("Calculating bounding box. Map size =");
  std::cout << map_size << std::endl;
  // ***** //
  // Find a bounding box around the robot's current position to limit the map queries and cap compute/memory
  double bbx_min_array[3];
  double bbx_max_array[3];

  // Check if this iteration requires the full map.
  if (map_size == "bbx") {
    for (int i=0; i<3; i++) {
      bbx_min_array[i] = min_tree[i] + std::round((robot.position[i] - 2.0*robot.sensor_range - min_tree[i])/voxel_size)*voxel_size - 2.95*voxel_size;
      // bbx_min_array[i] = std::max(bbx_min_array[i], min_tree[i] - 1.5*voxel_size);
      bbx_max_array[i] = min_tree[i] + std::round((robot.position[i] + 2.0*robot.sensor_range - min_tree[i])/voxel_size)*voxel_size + 2.95*voxel_size;
      // bbx_max_array[i] = std::min(bbx_max_array[i], max_tree[i] + 1.5*voxel_size);
    }
  }
  else {
    for (int i=0; i<3; i++) {
      bbx_min_array[i] = min_tree[i] - 1.5*voxel_size;
      bbx_max_array[i] = max_tree[i] + 1.5*voxel_size;
    }
  }

  Eigen::Vector4f bbx_min(bbx_min_array[0], bbx_min_array[1], bbx_min_array[2], 1.0);
  Eigen::Vector4f bbx_max(bbx_max_array[0], bbx_max_array[1], bbx_max_array[2], 1.0);
  octomap::point3d bbx_min_octomap(bbx_min_array[0], bbx_min_array[1], bbx_min_array[2]);
  octomap::point3d bbx_max_octomap(bbx_max_array[0], bbx_max_array[1], bbx_max_array[2]);
  // ***** //
  ROS_INFO("Box has x,y,z limits of [%0.1f to %0.1f, %0.1f to %0.1f, and %0.1f to %0.1f] meters.",
  bbx_min_array[0], bbx_max_array[0], bbx_min_array[1], bbx_max_array[1], bbx_min_array[2], bbx_max_array[2]);

  ROS_INFO("Allocating occupied bool flat matrix memory.");
  // Allocate memory for the occupied cells within the bounding box in a flat 3D boolean array
  int bbx_size[3];
  for (int i=0; i<3; i++) {
    bbx_size[i] = (int)std::round((bbx_max[i] - bbx_min[i])/voxel_size) + 1;
  }
  int bbx_mat_length = bbx_size[0]*bbx_size[1]*bbx_size[2];
  // bool occupied_mat[bbx_mat_length];
  bool* occupied_mat = new bool[bbx_mat_length]; // Allows for more memory allocation
  for (int i=0; i<bbx_mat_length; i++) occupied_mat[i] = true;

  ROS_INFO("Removing the voxels within the bounding box from the ground_cloud of length %d", (int)ground_cloud->points.size());

  // ***** //
  // Iterate through that box
  ROS_INFO("Beginning tree iteration through map of size %d", (int)rough_octree->size());
  // Initialize a PCL object to hold preliminary ground voxels
  pcl::PointCloud<pcl::PointXYZI>::Ptr ground_cloud_prefilter(new pcl::PointCloud<pcl::PointXYZI>);
  pcl::PointCloud<pcl::PointXYZI>::Ptr ground_cloud_traversable(new pcl::PointCloud<pcl::PointXYZI>); // input points that have already been labeled untraversable
  pcl::PointCloud<pcl::PointXYZI>::Ptr ground_cloud_free(new pcl::PointCloud<pcl::PointXYZI>); // free voxels with unseen below.
  pcl::PointCloud<pcl::PointXYZ>::Ptr obstacle_cloud(new pcl::PointCloud<pcl::PointXYZ>);

  // Expand octomap 
  rough_octree->expand();

  for(octomap::RoughOcTree::leaf_bbx_iterator
  it = rough_octree->begin_leafs_bbx(bbx_min_octomap, bbx_max_octomap);
  it != rough_octree->end_leafs_bbx(); ++it)
  {
    double query[3];
    query[0] = it.getX(); query[1] = it.getY(); query[2] = it.getZ();
    if (it->getOccupancy() <= 0.4) { // free
      // Check if the cell below it is unseen
     octomap::OcTreeNode* node = rough_octree->search(query[0], query[1], query[2] - voxel_size);
      if (node) {
        if ((node->getOccupancy() <= 0.55) && (node->getOccupancy() >= 0.45)) {
          pcl::PointXYZI query_point;
          query_point.x = query[0]; query_point.y = query[1]; query_point.z = query[2];
          query_point.intensity = (float)-1.0; // (.intensity == -1.0) --> free voxel
          ground_cloud_prefilter->points.push_back(query_point);
          ground_cloud_free->points.push_back(query_point);
        }
      } else {
        pcl::PointXYZI query_point;
        query_point.x = query[0]; query_point.y = query[1]; query_point.z = query[2];
        query_point.intensity = (float)-1.0; // (.intensity == -1.0) --> free voxel
        ground_cloud_prefilter->points.push_back(query_point);
        ground_cloud_free->points.push_back(query_point);
      }
    }
    else if (it->getOccupancy() >= 0.6) { // occupied
      if ((it->getRough() <= max_roughness) || (std::isnan(it->getRough()))) {
        pcl::PointXYZI rough_voxel;
        rough_voxel.x = query[0]; rough_voxel.y = query[1]; rough_voxel.z = query[2];
        rough_voxel.intensity = it->getRough();
        ground_cloud_traversable->points.push_back(rough_voxel);
        ground_cloud_prefilter->points.push_back(rough_voxel);
      } else {
        int id = xyz_index3(query, bbx_min_array, bbx_size, voxel_size);
        occupied_mat[id] = false;
        pcl::PointXYZ rough_voxel;
        rough_voxel.x = query[0]; rough_voxel.y = query[1]; rough_voxel.z = query[2];
        obstacle_cloud->points.push_back(rough_voxel);
      }
    }
  }

  // Publish the initial ground cloud and the negative ground only cloud
  debug_publishers[0].publish(ConvertCloudToMsg(ground_cloud_free, fixed_frame_id));
  debug_publishers[1].publish(ConvertCloudToMsg(ground_cloud_prefilter, fixed_frame_id));
  debug_publishers[2].publish(ConvertCloudToMsg(ground_cloud_traversable, fixed_frame_id));
  ROS_INFO("Published points initially labeled ground before filtering.");
  // sleep(5.0);

  ROS_INFO("Normal vector filtering initial ground PointCloud of length %d", (int)ground_cloud_prefilter->points.size());

  // Filter ground by local normal vector
  pcl::PointCloud<pcl::PointXYZI>::Ptr ground_cloud_normal_filtered (new pcl::PointCloud<pcl::PointXYZI>);
  pcl::PointCloud<pcl::PointXYZI>::Ptr negative_obstacle_cloud (new pcl::PointCloud<pcl::PointXYZI>);
  pcl::NormalEstimation<pcl::PointXYZI, pcl::Normal> normal_filter;
  normal_filter.setInputCloud(ground_cloud_prefilter);
  pcl::search::KdTree<pcl::PointXYZI>::Ptr kdtree_normal (new pcl::search::KdTree<pcl::PointXYZI>());
  kdtree_normal->setInputCloud(ground_cloud_prefilter);
  normal_filter.setSearchMethod(kdtree_normal);
  pcl::PointCloud<pcl::Normal>::Ptr cloud_normals (new pcl::PointCloud<pcl::Normal>);
  normal_filter.setRadiusSearch(5.0*rough_octree->getResolution());
  normal_filter.setViewPoint(0.0, 0.0, 2.0);
  normal_filter.compute(*cloud_normals);

  for (int i=0; i<cloud_normals->points.size(); i++) {
    pcl::PointXYZI query = ground_cloud_prefilter->points[i];
    pcl::Normal query_normal = cloud_normals->points[i];
    if ((std::abs(query_normal.normal_z) >= normal_z_threshold) && (std::abs(query_normal.curvature) <= normal_curvature_threshold)) {
        ground_cloud_normal_filtered->points.push_back(query);
    } else {
      if (query.intensity <= -0.5) {
        negative_obstacle_cloud->points.push_back(query);
      }
    }
  }
  debug_publishers[3].publish(ConvertCloudToMsg(ground_cloud_normal_filtered, fixed_frame_id));
  debug_publishers[5].publish(ConvertCloudToMsg(negative_obstacle_cloud, fixed_frame_id));
  debug_publishers[6].publish(ConvertCloudToMsg(obstacle_cloud, fixed_frame_id));
  // ***** //

  ROS_INFO("Contiguity filtering normal filtered cloud of length %d...", (int)ground_cloud_normal_filtered->points.size());
  // ROS_INFO("Contiguity filtering normal filtered cloud of length %d...", (int)ground_cloud_prefilter->points.size());
  // ***** //
  // Filter ground by contiguity (is this necessary?)
  pcl::search::KdTree<pcl::PointXYZI>::Ptr kdtree(new pcl::search::KdTree<pcl::PointXYZI>);
  kdtree->setInputCloud(ground_cloud_normal_filtered);
  // kdtree->setInputCloud(ground_cloud_prefilter);

  // Initialize euclidean cluster extraction object
  std::vector<pcl::PointIndices> cluster_indices;
  pcl::EuclideanClusterExtraction<pcl::PointXYZI> euclidean_cluster_extractor;
  euclidean_cluster_extractor.setClusterTolerance(1.8*voxel_size); // Clusters must be made of contiguous sections of ground (within sqrt(2)*voxel_size of each other)
  euclidean_cluster_extractor.setMinClusterSize(min_cluster_size); // Cluster must be at least 15 voxels in size
  euclidean_cluster_extractor.setSearchMethod(kdtree);
  euclidean_cluster_extractor.setInputCloud(ground_cloud_normal_filtered);
  // euclidean_cluster_extractor.setInputCloud(ground_cloud_prefilter); // try w/o normal filtering
  euclidean_cluster_extractor.extract(cluster_indices);
  ROS_INFO("Clusters extracted.");

  // Extract a local bounding box from the ground_cloud
  pcl::PointCloud<pcl::PointXYZI>::Ptr ground_cloud_local (new pcl::PointCloud<pcl::PointXYZI>);
  pcl::CropBox<pcl::PointXYZI> box_filter;
  box_filter.setMin(bbx_min);
  box_filter.setMax(bbx_max);
  box_filter.setNegative(true);
  box_filter.setInputCloud(ground_cloud);
  box_filter.filter(*ground_cloud_local);

  // Extract local bounding box from the edt_cloud
  pcl::PointCloud<pcl::PointXYZI>::Ptr edt_cloud_local (new pcl::PointCloud<pcl::PointXYZI>);
  pcl::PointCloud<pcl::PointXYZI>::Ptr edt_cloud_bbx (new pcl::PointCloud<pcl::PointXYZI>);
  pcl::PointCloud<pcl::PointXYZI>::Ptr edt_cloud_bbx_smaller (new pcl::PointCloud<pcl::PointXYZI>);

  ROS_INFO("Copying clusters above minimum cluster size...");
  // Add the biggest (or the one with the robot in it) to the ground_cloud.
  if (cluster_indices.size() > 0) {
    for (int j=0; j< cluster_indices.size(); j++){
      if (cluster_indices[j].indices.size() >= min_cluster_size) {
        for (int i=0; i<cluster_indices[j].indices.size(); i++) {
          pcl::PointXYZI ground_point = ground_cloud_normal_filtered->points[cluster_indices[j].indices[i]];
          // pcl::PointXYZI ground_point = ground_cloud_prefilter->points[cluster_indices[j].indices[i]];
          if (ground_point.intensity <= -0.5) {
            ground_cloud_local->points.push_back(ground_point);
            pcl::PointXYZI edt_point = ground_point;
            edt_point.intensity = 0.0;
            edt_cloud_bbx->points.push_back(edt_point);
            for (int i=0; i<padding; i++) {
              edt_point.z = edt_point.z + voxel_size; // Padding
              edt_cloud_bbx->points.push_back(edt_point); // Padding
            }
          }
        }
      }
    }
  } else {
    ROS_INFO("No new cloud entries, publishing previous cloud msg");
    return;
  }

  // Add in all the "traversable" voxels from the RoughOcTree
  for (int i=0; i<ground_cloud_traversable->points.size(); i++) {
    pcl::PointXYZI ground_point = ground_cloud_traversable->points[i];
    ground_cloud_local->points.push_back(ground_point);
    pcl::PointXYZI edt_point = ground_point;
    edt_point.intensity = 0.0; // Consider passing traversability in to penalize rougher points.
    edt_cloud_bbx->points.push_back(edt_point);
    for (int i=0; i<padding; i++) {
      edt_point.z = edt_point.z + voxel_size; // Padding
      edt_cloud_bbx->points.push_back(edt_point); // Padding
    }
  }

  // Use a voxel_grid filter to remove duplicate voxel entries.
  pcl::PointCloud<pcl::PointXYZI>::Ptr edt_cloud_bbx_filtered (new pcl::PointCloud<pcl::PointXYZI>);
  pcl::VoxelGrid<pcl::PointXYZI> sor;
  sor.setInputCloud (edt_cloud_bbx);
  sor.setLeafSize (0.1*voxel_size, 0.1*voxel_size, 0.1*voxel_size);
  sor.filter (*edt_cloud_bbx_filtered);
  edt_cloud_bbx->points.clear();
  pcl::PointCloud<pcl::PointXYZI>::Ptr ground_cloud_local_filtered (new pcl::PointCloud<pcl::PointXYZI>);
  sor.setInputCloud (ground_cloud_local);
  sor.setLeafSize (0.1*voxel_size, 0.1*voxel_size, 0.1*voxel_size);
  sor.filter (*ground_cloud_local_filtered);


  // Clear ground_cloud and deep copy it from ground_cloud_local
  ground_cloud->points.clear();
  for (int i=0; i<ground_cloud_local_filtered->points.size(); i++) {
    ground_cloud->points.push_back(ground_cloud_local_filtered->points[i]);
  }

  // Calculate EDT bbx
  double bbx_min_array_edt[3];
  double bbx_max_array_edt[3];
  if (map_size == "bbx") {
    for (int i=0; i<3; i++) {
      double bbx_center = (bbx_min_array[i] + bbx_max_array[i])/2.0;
      bbx_min_array_edt[i] = bbx_center - (bbx_size[i]/4.0)*voxel_size;
      bbx_max_array_edt[i] = bbx_center + (bbx_size[i]/4.0)*voxel_size;
    }
  }
  else {
    for (int i=0; i<3; i++) {
      bbx_min_array_edt[i] = bbx_min_array[i];
      bbx_max_array_edt[i] = bbx_max_array[i];
    }
  }
  Eigen::Vector4f bbx_min_edt(bbx_min_array_edt[0], bbx_min_array_edt[1], bbx_min_array_edt[2], 0.0);
  Eigen::Vector4f bbx_max_edt(bbx_max_array_edt[0], bbx_max_array_edt[1], bbx_max_array_edt[2], 0.0);

  pcl::CropBox<pcl::PointXYZI> box_filter2;
  box_filter2.setMin(bbx_min_edt);
  box_filter2.setMax(bbx_max_edt);
  box_filter2.setNegative(false);
  box_filter2.setInputCloud(edt_cloud_bbx_filtered);
  box_filter2.filter(*edt_cloud_bbx_smaller);

  // EDT Calculation
  ROS_INFO("Calculating EDT.");
  CalculatePointCloudEDT(occupied_mat, edt_cloud_bbx_smaller, bbx_min_array, bbx_size, voxel_size, truncation_distance);
  InflateObstacles(edt_cloud_bbx_smaller, inflate_distance);
  ROS_INFO("EDT calculated.");

  // Copy to edt_cloud

  if (map_size == "bbx") {
    pcl::CropBox<pcl::PointXYZI> box_filter3;
    box_filter3.setMin(bbx_min_edt);
    box_filter3.setMax(bbx_max_edt);
    box_filter3.setNegative(true);
    box_filter3.setInputCloud(edt_cloud);
    box_filter3.filter(*edt_cloud_local);

    for (int i=0; i<edt_cloud_bbx_smaller->points.size(); i++) {
      edt_cloud_local->points.push_back(edt_cloud_bbx_smaller->points[i]);
    }

    edt_cloud->points.clear();
    for (int i=0; i<edt_cloud_local->points.size(); i++) {
      edt_cloud->points.push_back(edt_cloud_local->points[i]);
    }
  }
  else {
    edt_cloud->points.clear();
    for (int i=0; i<edt_cloud_bbx_smaller->points.size(); i++) {
      edt_cloud->points.push_back(edt_cloud_bbx_smaller->points[i]);
    }
  }
  

  GetGroundMsg();
  GetEdtMsg();
  delete[] occupied_mat;
  ROS_INFO("Publishing edt");
  return;
}


int main(int argc, char **argv)
{
  // Node declaration
  ros::init(argc, argv, "traversability_to_edt");
  ros::NodeHandle n;

  NodeManager node_manager;

  // Subscribers and Publishers
  ros::Subscriber sub_octomap = n.subscribe("octomap", 1, CallbackOctomap);
  ros::Subscriber sub_odometry = n.subscribe("odometry", 1, &NodeManager::CallbackOdometry, &node_manager);
  ros::Subscriber sub_rough = n.subscribe("rough_cloud", 1, CallbackRoughCloud);
  ros::Subscriber sub_rough_octomap = n.subscribe("rough_octomap", 1, CallbackRoughOctomap);
  ros::Publisher pub1 = n.advertise<sensor_msgs::PointCloud2>("ground", 5);
  ros::Publisher pub2 = n.advertise<sensor_msgs::PointCloud2>("edt", 5);
  ros::Publisher pub_prefilter_negative_ground = n.advertise<sensor_msgs::PointCloud2>("debug/ground_prefilter_negative", 5);
  ros::Publisher pub_prefilter_ground = n.advertise<sensor_msgs::PointCloud2>("debug/ground_prefilter", 5);
  ros::Publisher pub_traversable_ground = n.advertise<sensor_msgs::PointCloud2>("debug/ground_traversable", 5);
  ros::Publisher pub_normal_ground = n.advertise<sensor_msgs::PointCloud2>("debug/ground_normal", 5);
  ros::Publisher pub_cluster_ground = n.advertise<sensor_msgs::PointCloud2>("debug/ground_cluster", 5);
  // ros::Publisher pub_normal_cluster_ground = n.advertise<sensor_msgs::PointCloud2>("debug/ground_normal_cluster", 5);
  ros::Publisher pub_negative_obstacle = n.advertise<sensor_msgs::PointCloud2>("debug/negative_obstacle", 5);
  ros::Publisher pub_obstacle = n.advertise<sensor_msgs::PointCloud2>("debug/obstacles", 5);

  node_manager.debug_publishers.push_back(pub_prefilter_negative_ground);
  node_manager.debug_publishers.push_back(pub_prefilter_ground);
  node_manager.debug_publishers.push_back(pub_traversable_ground);
  node_manager.debug_publishers.push_back(pub_normal_ground);
  node_manager.debug_publishers.push_back(pub_cluster_ground);
  node_manager.debug_publishers.push_back(pub_negative_obstacle);
  node_manager.debug_publishers.push_back(pub_obstacle);

  ROS_INFO("Initialized subscriber and publishers.");

  // Params
  n.param("traversability_to_edt/min_cluster_size", node_manager.min_cluster_size, 100);
  n.param("traversability_to_edt/normal_z_threshold", node_manager.normal_z_threshold, (float)0.8);
  n.param("traversability_to_edt/normal_curvature_threshold", node_manager.normal_curvature_threshold, (float)50.0);
  n.param<std::string>("traversability_to_edt/robot_frame_id", node_manager.robot_frame_id, "base_link");
  n.param<std::string>("traversability_to_edt/fixed_frame_id", node_manager.fixed_frame_id, "world");
  n.param("traversability_to_edt/sensor_range", node_manager.robot.sensor_range, 5.0);
  n.param("traversability_to_edt/use_tf", node_manager.use_tf, false);
  n.param("traversability_to_edt/truncation_distance", node_manager.truncation_distance, (float)4.0);
  n.param("traversability_to_edt/inflate_distance", node_manager.inflate_distance, (float)0.0);
  n.param("traversability_to_edt/filter_holes", node_manager.filter_holes, false);
  n.param("traversability_to_edt/max_roughness", node_manager.max_roughness, (float)0.5);
  n.param("traversability_to_edt/edt_padding", node_manager.padding, (int)1);
  int full_map_ticks = 200;
  n.param("traversability_to_edt/full_map_ticks", full_map_ticks, 200);

  float update_rate;
  n.param("traversability_to_edt/update_rate", update_rate, (float)5.0);
  
  ros::Rate r(update_rate); // 5 Hz
  ROS_INFO("Finished reading params.");
  // Main Loop
  int ticks = -1;
  while (ros::ok())
  {
    r.sleep();
    ros::spinOnce();
    if (map_updated) ticks++;
    if ((ticks % full_map_ticks) == 0) {
      // node_manager.FindGroundVoxels("full");
      node_manager.FindGroundVoxels_RoughOctomap("full");
    }
    else {
      // node_manager.FindGroundVoxels("bbx");
      node_manager.FindGroundVoxels_RoughOctomap("bbx");
    }
    // ROS_INFO("ground cloud currently has %d points", (int)node_manager.ground_cloud->points.size());
    if (node_manager.ground_cloud->points.size() > 0) pub1.publish(node_manager.ground_msg);
    if (node_manager.edt_cloud->points.size() > 0) pub2.publish(node_manager.edt_msg);
  }
}