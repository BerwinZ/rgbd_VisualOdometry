/*
 * Reprensents a camera frame 
 */

#ifndef FRAME_H
#define FRAME_H

#include "myslam/common_include.h"
#include "myslam/camera.h"

namespace myslam 
{

class Frame
{
public:
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    typedef shared_ptr<Frame> Ptr;
    typedef unordered_map<size_t, int> CovisibleKeyframeIdToWeight;

    double              timestamp_;     // when it is recorded
    Camera::Ptr         camera_;        // Pinhole RGBD Camera model 
    Mat                 color_, depth_; // color and depth image 

    // factory function
    static Frame::Ptr CreateFrame(
        const double timestamp, 
        const Camera::Ptr camera, 
        const Mat color, 
        const Mat depth
    ); 

    size_t GetId() const { 
        return id_; 
    }

    SE3 GetPose() {
        unique_lock<mutex> lck(poseMutex_);
        return T_c_w_;
    }

    SE3 SetPose(const SE3 pose) {
        unique_lock<mutex> lck(poseMutex_);
        T_c_w_ = move(pose);
    }
    
    // find the depth in depth map
    double GetDepth( const KeyPoint& kp );
    
    // Get Camera Center
    Vector3d GetCamCenter() const {
        return T_c_w_.inverse().translation();
    }
    
    // check if a point is in the view of this frame 
    bool IsInFrame( const Vector3d& pt_world );


    void AddObservedMappoint(const size_t id) {
        unique_lock<mutex> lck(observationMutex_);
        observedMappointIds_.insert(id);
    }

    void RemoveObservedMappoint(const size_t id);

    unordered_set<size_t> GetObservedMappointIds() {
        unique_lock<mutex> lck(observationMutex_);
        return observedMappointIds_;
    }

    bool IsObservedMappoint(const size_t id) {
        unique_lock<mutex> lck(observationMutex_);
        return observedMappointIds_.count(id);
    }

    // Update the covisible keyframes when this frame is a keyframe 
    void ComputeCovisibleKeyframes();

    // Add the connection of another frame with weight to current frame
    void AddCovisibleKeyframe(const size_t id, const int weight) {
        unique_lock<mutex> lck(observationMutex_);
        covisibleKeyframeIdToWeight_[id] = weight;
    }

    // Decrease the weight of covosible keyframe by 1
    void DecreaseCovisibleKeyframeWeightByOne(const size_t id);


    CovisibleKeyframeIdToWeight GetCovisibleKeyframes() {
        unique_lock<mutex> lck(observationMutex_);
        return covisibleKeyframeIdToWeight_;
    }

private: 
    static size_t           factoryId_;
    size_t                  id_;         // id of this frame

    mutex                   poseMutex_;
    SE3                     T_c_w_;      // transform from world to camera

    mutex                   observationMutex_;
    unordered_set<size_t>   observedMappointIds_;
    CovisibleKeyframeIdToWeight covisibleKeyframeIdToWeight_;  // Covisible keyframes (has same observed mappoints >= 15) and the number of covisible mappoints


    Frame(  const size_t id, 
            const double timestamp, 
            const Camera::Ptr camera, 
            const Mat color, 
            const Mat depth );

    void DecreaseCovisibleKeyFrameWeightByOneWithoutMutex(const size_t id);
};

}

#endif // FRAME_H
