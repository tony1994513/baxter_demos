#ifndef BAXTER_DEMOS_CLOUD_SEGMENTER_CPP_
#define BAXTER_DEMOS_CLOUD_SEGMENTER_CPP_

#include "CloudSegmenter.h"


bool CloudSegmenter::isPointWithinDesiredRange(const pcl::PointRGB input_pt,
                               const pcl::PointRGB desired_pt, int radius){
    pcl::PointXYZRGB input_xyz(input_pt.r, input_pt.g, input_pt.b),
                        desired_xyz(desired_pt.r, desired_pt.g, desired_pt.b);
    pcl::PointXYZHSV input_hsv, desired_hsv;
    pcl::PointXYZRGBtoXYZHSV(input_xyz, input_hsv);
    pcl::PointXYZRGBtoXYZHSV(desired_xyz, desired_hsv);
    
    if ( abs(input_hsv.h - desired_hsv.h) < radius &&
         abs(input_hsv.s - desired_hsv.s) < radius &&
         abs(input_hsv.v - desired_hsv.v) < radius){
        return true;
    }
    return false;
}

bool CloudSegmenter:: hasCloud(){
    return has_cloud;
}

bool CloudSegmenter:: hasColor(){
    return has_desired_color;
}

CloudSegmenter::CloudSegmenter() : indices( new vector<int>()) {
    map<string, int> params;
    if(!n.getParam("/object_finder_params", params)){
        throw runtime_error("Couldn't find parameters\n");
    }
    radius = params["radius"];
    filter_min = params["filter_min"];
    filter_max = params["filter_max"];
    distance_threshold = params["distance_threshold"];
    point_color_threshold = params["point_color_threshold"];
    region_color_threshold = params["region_color_threshold"];
    min_cluster_size = params["min_cluster_size"];

    has_desired_color = false;
    has_cloud = false;
    sub = n.subscribe("/camera/depth_registered/points", 500,
                                      &CloudSegmenter::callback, this);

    //TODO: add args for topic name
    pose_pub = n.advertise<geometry_msgs::PoseArray>(
                        "/object_tracker/right/goal_poses", 1000);
}

//soon to be deprecated
void CloudSegmenter:: publish_poses(){
    geometry_msgs::PoseArray msg;
    msg.poses = object_poses;
    pose_pub.publish(msg);
}

//remember to shift-click!
void CloudSegmenter:: getClickedPoint(
                const pcl::visualization::PointPickingEvent& event,
                void* args){
    int n = event.getPointIndex();
    if (n == -1){
        cout << "Got no point" << endl;
        return;
    }

    desired_color = getCloudColorAt(n);
    cout << "Desired color: " << (int) desired_color.r << ", " <<
            (int) desired_color.g << ", " << (int) desired_color.b << endl;
    has_desired_color = true;
    segmentation();
}

PointColorCloud::ConstPtr CloudSegmenter::getCloudPtr(){
    return cloud;
}

PointColorCloud::ConstPtr CloudSegmenter::getClusteredCloudPtr(){
    return colored_cloud;
}

/*void CloudSegmenter:: renderCloud(){
    cloud_viewer.showCloud(cloud);
}

void CloudSegmenter:: renderClusters(){
    cloud_viewer.showCloud(colored_cloud);
}*/

pcl::PointRGB CloudSegmenter:: getCloudColorAt(int x, int y){
    pcl::PointXYZRGB cur = cloud->at(x, y);
    return pcl::PointRGB(cur.b, cur.g, cur.r);
}
pcl::PointRGB CloudSegmenter:: getCloudColorAt(int n){
    pcl::PointXYZRGB cur = cloud->at(n);
    return pcl::PointRGB(cur.b, cur.g, cur.r);
}

OrientedBoundingBox getOBBForCloud(PointColorCloud::Ptr cloud_ptr){

    pcl::MomentOfInertiaEstimation<pcl::PointXYZRGB> inertia;

    inertia.setInputCloud(cloud_ptr);

    //get the moment of inertia
    inertia.compute();

    //this centroid will be a bit off because we get only 2-3 faces of a cube
    //Get the oriented bounding box around this cluster
    pcl::PointXYZRGB min_point_OBB;
    pcl::PointXYZRGB max_point_OBB;
    pcl::PointXYZRGB position_OBB;
    Eigen::Matrix3f rotational_matrix_OBB;
    inertia.getOBB(min_point_OBB, max_point_OBB, position_OBB, rotational_matrix_OBB);
    pcl::PointXYZRGB min_point_AABB;
    pcl::PointXYZRGB max_point_AABB;
    inertia.getAABB(min_point_AABB, max_point_AABB);
    return OrientedBoundingBox(min_point_OBB, max_point_OBB, position_OBB,
                           rotational_matrix_OBB, min_point_AABB, max_point_AABB);

}

void CloudSegmenter::mergeCollidingBoxes(){
    bool collides = true;
    while(collides){
        collides = false;
        for (int i = 0; i < cloud_ptrs.size(); i++){
            for (int j = 0; j < cloud_ptrs.size(); j++){
                if (cloud_ptrs[i] == cloud_ptrs[j]){
                    continue;
                }
                if (cloud_boxes[cloud_ptrs[i]].collides_with(cloud_boxes[cloud_ptrs[j]]) ){
                    collides=true;
                    //cloud_ptrs[i] = PointColorCloud::Ptr( *cloud_ptrs[i] + *cloud_ptrs[j]));
                    PointColorCloud newcloud = *cloud_ptrs[i] + *cloud_ptrs[j];
                    
                    PointColorCloud::Ptr newptr = newcloud.makeShared();
                    //cloud_ptrs[i] = newptr;
                    //cloud_ptrs[j] = newptr;
                    cloud_boxes.erase(cloud_ptrs[j]);
                    cloud_ptrs.erase(cloud_ptrs.begin()+j);

                    cloud_boxes.erase(cloud_ptrs[i]);
                    cloud_ptrs.erase(cloud_ptrs.begin()+i);

                    //push_back new box
                    cloud_ptrs.push_back(newptr);
                    cloud_boxes[newptr] = getOBBForCloud(cloud_ptrs.back());
                    break;
                }
            }
            if(collides) break;
        }
    }
}

void CloudSegmenter:: segmentation(){
    if (!has_desired_color){
        return;
    }

    /* Segmentation code from:
       http://pointclouds.org/documentation/tutorials/region_growing_rgb_segmentation.php*/
    pcl::search::Search <pcl::PointXYZRGB>::Ptr tree =
                        boost::shared_ptr<pcl::search::Search
                        <pcl::PointXYZRGB> >
                        (new pcl::search::KdTree<pcl::PointXYZRGB>);

    reg.setInputCloud (cloud);
    reg.setIndices (indices);
    reg.setSearchMethod (tree);
    reg.setDistanceThreshold (distance_threshold);
    reg.setPointColorThreshold (point_color_threshold);
    reg.setRegionColorThreshold (region_color_threshold);
    reg.setMinClusterSize (min_cluster_size);
 
    vector <pcl::PointIndices> clusters;
    reg.extract (clusters);
 
    colored_cloud = reg.getColoredCloud();

    cloud_ptrs.clear();

    cloud_boxes.clear();
    // Select the correct color clouds from the segmentation
    for (int i = 0; i < clusters.size(); i++){
        pcl::PointIndices cluster = clusters[i];

        // Get a representative color in the cluster
        int n = cluster.indices.size();
        float r = 0; float g = 0; float b = 0;
        // TODO: this is a major bottleneck. improve performance
        for (int j = 0; j < n; j++){
            pcl::PointRGB color = getCloudColorAt(cluster.indices[j]);
            r += (float) color.r; //float typecasts suck
            g += (float) color.g;
            b += (float) color.b;
        }
        r /= n; g /= n; b /= n;
        cout << "Average color: " << r << ", " << g << ", " << b << endl;
        pcl::PointRGB avg((unsigned char) b, (unsigned char) g, (unsigned char) r);
        /*int n = cluster.indices.size();
        pcl::CentroidPoint<pcl::PointXYZRGB> rgb_centroid;
        for (int j = 0; j < n; j++){
            rgb_centroid.add(cloud->at(cluster.indices[j]));
            
        }
        pcl::PointXYZRGB avg_xyz;
        rgb_centroid.get(avg_xyz);
        //TODO: test that colors get averaged
        pcl::PointRGB avg(avg_xyz.b, avg_xyz.g, avg_xyz.r);

        cout << "Average color: " << avg.r << ", " << avg.g << ", " << avg.b << endl;
        */

        // Check if avg is within the clicked color
        if (isPointWithinDesiredRange(avg, desired_color, radius)){
            //blob_clusters.push_back(clusters[i]);
            PointColorCloud cloud_subset = PointColorCloud(*cloud, cluster.indices);
            PointColorCloud::Ptr cloud_ptr = cloud_subset.makeShared();

            cloud_ptrs.push_back(cloud_ptr);
            
        }
    }

    cout << "Clusters found: " << cloud_ptrs.size() << endl;
    object_poses = vector<geometry_msgs::Pose>();
    vector<OrientedBoundingBox> OBBs;
    for (int i = 0; i < cloud_ptrs.size(); i++){
        OBBs.push_back(getOBBForCloud(cloud_ptrs[i]));
        cout << "OBB position: " << OBBs.back().get_position() << endl;
        OBBs.back().set_sides( object_side, object_side, object_side);
        cloud_boxes.insert(CloudPtrBoxPair(cloud_ptrs[i], OBBs.back()));
    }

    //Combine poses with intersecting bounding boxes
    mergeCollidingBoxes();

    //For each OBB, extract the pose and make a ROS msg

    for(CloudPtrBoxMap::iterator it = cloud_boxes.begin(); it != cloud_boxes.end(); it++){
    //for(int i = 0; i < cloud_ptrs.size(); i++ ){
    
        OrientedBoundingBox box = (OrientedBoundingBox) it->second;
        //OrientedBoundingBox box = cloud_boxes[cloud_ptrs[i]];
        Eigen::Vector3f position_OBB = box.get_position();
        Eigen::Matrix3f rotational_matrix_OBB = box.get_rotational_matrix();
        
        geometry_msgs::Point position;
        position.x = position_OBB[0];
        position.y = position_OBB[1];
        position.z = position_OBB[2];

        //Now dat orientation
        Eigen::Quaternionf q(rotational_matrix_OBB);

        geometry_msgs::Quaternion orientation;
        orientation.w = q.w();
        orientation.x = q.x();
        orientation.y = q.y();
        orientation.z = q.z();

        geometry_msgs::PoseStamped pose_in;
        pose_in.pose.position = position; pose_in.pose.orientation = orientation;
        cout << "Pose in: " << pose_in.pose << endl;
        pose_in.header.frame_id = frame_id;
        geometry_msgs::PoseStamped pose_out;
        pose_in.header.stamp = ros::Time::now();
        tf_listener.transformPose("/base", pose_in, pose_out);
        cout << "Pose out: " << pose_out.pose << endl;
        object_poses.push_back(pose_out.pose);
    }
}

void CloudSegmenter:: callback(const sensor_msgs::PointCloud2::ConstPtr& msg){
    frame_id = msg->header.frame_id;
    // Members: float x, y, z; uint32_t rgba
    pcl::PointCloud <pcl::PointXYZRGB>::Ptr new_cloud
                                (new pcl::PointCloud <pcl::PointXYZRGB>);

    pcl::PCLPointCloud2 pcl_pc;
    pcl_conversions::toPCL(*msg, pcl_pc);
    pcl::fromPCLPointCloud2(pcl_pc, *new_cloud);
    indices = pcl::IndicesPtr( new vector<int>() );
    pcl::removeNaNFromPointCloud(*new_cloud, *new_cloud, *indices);

    pcl::PassThrough<pcl::PointXYZRGB> pass;
    pass.setInputCloud (new_cloud);
    pass.setFilterFieldName ("z");
    pass.setFilterLimits (filter_min, filter_max);
    pass.filter (*indices);

    cloud = new_cloud;
    has_cloud = true;
}

#endif