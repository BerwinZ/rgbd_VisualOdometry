#include "myslam/backend.h"
#include "myslam/util.h"

namespace myslam
{

Backend::Backend(const Camera::Ptr camera): camera_(move(camera)) {
    auto solver = new g2o::OptimizationAlgorithmLevenberg(
        g2o::make_unique<BlockSolverType>(g2o::make_unique<CSparseLinearSolverType>()));
    optimizer_.setAlgorithm(solver);
    
    chi2Threshold_ = Config::get<double>("chi2_th");
    reMatchDescriptorDistance_ = Config::get<double>("re_match_descriptor_distance");

    backendRunning_ = true;
    backendThread_ = std::thread(std::bind(&Backend::BackendLoop, this));
}

void Backend::Stop() {
    backendRunning_ = false;
    backendUpdateTrigger_.notify_one();
    backendThread_.join();

    CleanUp();
}

void Backend::ProcessNewKeyframeAsync(
    const Frame::Ptr& keyframe, 
    const unordered_map<size_t, size_t>& oldMptIdKptIdxMap,
    const unordered_map<Mappoint::Ptr, size_t>& newMptKptIdxMap) {

    unique_lock<mutex> lock(backendMutex_);
    keyframeCurr_ = keyframe;
    oldMptIdKptIdxMap_.clear();
    oldMptIdKptIdxMap_.insert(oldMptIdKptIdxMap.begin(), oldMptIdKptIdxMap.end());
    newMptKptIdxMap_.clear();
    newMptKptIdxMap_.insert(newMptKptIdxMap.begin(), newMptKptIdxMap.end());

    backendUpdateTrigger_.notify_one();
}

void Backend::BackendLoop()
{
    while (backendRunning_)
    {
        unique_lock<mutex> lock(backendMutex_);
        backendUpdateTrigger_.wait(lock);

        // Need to check again because the trigger could also be triggered during deconstruction
        if (backendRunning_)
        {
            printf("\nBackend starts processing\n");
            MapManager::Instance().AddKeyframe(keyframeCurr_);
            AddObservingMappointsToNewKeyframe();
            AddNewMappointsToExistingKeyframe();
            OptimizeLocalMap();
            UpdateFrontendTrackingMap();
            CleanUp();
        }
    }
}

void Backend::AddObservingMappointsToNewKeyframe() {
    for (const auto& [mptId, kptIdx] : oldMptIdKptIdxMap_) {
        // old mpt may be replaced by previous new mpt
        auto mpt = MapManager::Instance().GetPotentialReplacedMappoint(mptId);
        keyframeCurr_->AddObservingMappoint(mpt, kptIdx);
        // old mpt has new observations, update its descriptor
        mpt->CalculateMappointDescriptor();
    }

    // add new created mpts observations
    for (const auto& [mpt, kptIdx]: newMptKptIdxMap_) {
        MapManager::Instance().AddMappoint(mpt);
        keyframeCurr_->AddObservingMappoint(mpt, kptIdx);
        // new mpt just has 1 observation at this time, no need to update descriptor
    }
}

void Backend::AddNewMappointsToExistingKeyframe() {
    auto covisibleKfIds = keyframeCurr_->GetAllCovisibleKfIds();
    unordered_set<Frame::Ptr> covisibleKfs;
    for (auto& kfId: covisibleKfIds) {
        auto kf = MapManager::Instance().GetKeyframe(kfId);
        if (kf == nullptr) {
            continue;
        }
        covisibleKfs.insert(kf);
        for (auto& neighborKfId: kf->GetAllCovisibleKfIds()) {
            auto neighborKf = MapManager::Instance().GetKeyframe(kfId);
            if (neighborKf == nullptr) {
                continue;
            }
            covisibleKfs.insert(neighborKf);
        }
    }
    if (covisibleKfs.count(keyframeCurr_)) {
        covisibleKfs.erase(keyframeCurr_);
    }

    // across different keyframes. old mpt to be replace by a new mpt
    unordered_map<size_t, pair<size_t, double>> oldMptIdToNewMptIdAndDistance;
    // for each keyframe. kpt to be matched with a new mpt
    unordered_map<size_t, pair<Mappoint::Ptr, double>> kptIdxToMptAndDistance;
    // across different keyframes. all new observations to be added
    list<tuple<Frame::Ptr, Mappoint::Ptr, size_t>> observationsToAdd;
    
    double distance;
    size_t kptIdx;
    bool mayObserveMpt;
    size_t oldMptId;

    for (auto& kf: covisibleKfs) {

        kptIdxToMptAndDistance.clear();
        for (auto& [mpt, _]: newMptKptIdxMap_) {
            if (!kf->GetMatchedKeypoint(mpt, kptIdx, distance, mayObserveMpt) || distance > reMatchDescriptorDistance_) {
                continue;
            }

            // check if the old keyframe keypoint already has matched mappoint
            if (kf->IsKeypointMatchWithMappoint(kptIdx, oldMptId)) {
                // if a previous matched keypoint could be matched with several new mappoints, find the best one for it
                if (oldMptIdToNewMptIdAndDistance.count(oldMptId) && 
                    distance >= oldMptIdToNewMptIdAndDistance[oldMptId].second) {
                    continue;
                }
                oldMptIdToNewMptIdAndDistance[oldMptId] = make_pair(mpt->GetId(), distance);
            } else {
                // if a previous empty keypoint could be matched with several new mappoints, find the best one for it
                if (kptIdxToMptAndDistance.count(kptIdx) &&
                    distance >= kptIdxToMptAndDistance[kptIdx].second) {
                    continue;
                }
                kptIdxToMptAndDistance[kptIdx] = make_pair(mpt, distance);
            }
        }
        // record observations to add for this keyframe 
        for(auto& [kptIdx, mptAndDistance]: kptIdxToMptAndDistance) {
            auto& [mpt, _] = mptAndDistance;
            observationsToAdd.push_back(make_tuple(kf, mpt, kptIdx));
        }
    }

    // add new observations
    for (auto& [kf, mpt, kptIdx]: observationsToAdd) {
        kf->AddObservingMappoint(mpt, kptIdx);
    }

    // replace the previous mpt with the new mpt
    for (auto& [oldMptId, newMptIdAndDistance]: oldMptIdToNewMptIdAndDistance) {
        auto& [newMptId, _] = newMptIdAndDistance;
        MapManager::Instance().ReplaceMappoint(oldMptId, newMptId);
    }

    // since new mpts are observed by more keyframes, update its descriptor
    for (auto& [mpt, _]: newMptKptIdxMap_) {
        mpt->CalculateMappointDescriptor();
    }

    printf("  Added new mappoint observations to old keyframes: %zu\n", observationsToAdd.size());
    printf("  Replace old mappoints with new one: %zu\n", oldMptIdToNewMptIdAndDistance.size());
}

void Backend::OptimizeLocalMap()
{
    auto covisibleKfIds = keyframeCurr_->GetActiveCovisibleKfIds();
    // Add current keyframe
    covisibleKfIds.insert(keyframeCurr_->GetId());

    int vertexIndex = 0;

    // Create pose vertices and mappoint vertices for covisible keyframes
    for (auto &kfId : covisibleKfIds)
    {
        auto kf = MapManager::Instance().GetKeyframe(kfId);

        if (kf == nullptr)
        {
            continue;
        }

        // Create camera pose vertex
        VertexPose* poseVertex = new VertexPose;
        poseVertex->setId(++vertexIndex);
        poseVertex->setEstimate(kf->GetTcw());
        poseVertex->setFixed(kf->GetId() == 0);
        optimizer_.addVertex(poseVertex);

        // Record in map
        kfIdToCovKfThenVertex_[kfId] = make_pair(kf, poseVertex);

        // Create mappoint vertices
        for (auto &mptId : kf->GetObservingMappointIds())
        {
            if (mptIdToMptThenVertex_.count(mptId))
            {
                continue;
            }

            auto mpt = MapManager::Instance().GetMappoint(mptId);
            if (mpt == nullptr || mpt->outlier_)
            {
                continue;
            }

            // Create mappoint vertex
            VertexMappoint* mptVertex = new VertexMappoint;
            mptVertex->setEstimate(mpt->GetPosition());
            mptVertex->setId(++vertexIndex);
            mptVertex->setMarginalized(true);
            optimizer_.addVertex(mptVertex);

            // Record in map
            mptIdToMptThenVertex_[mptId] = make_pair(mpt, mptVertex);
        }
    }

    const double deltaRGBD = sqrt(7.815);
    int edgeIndex = 0;

    // Create pose vertices for fixed keyframe and add all edges, also perform triangulation
    size_t triangulatedCnt = 0;
    for (auto& [mptId, mptAndVertex] : mptIdToMptThenVertex_)
    {   
        auto& [mpt, mptVertex] = mptAndVertex;

        vector<SE3> poses;
        vector<Vector3d> normalizedPos;
        // TODO: enable triangulation
        // bool needTriangulate = !mpt->outlier_ && !(mpt->triangulated_ || mpt->optimized_);
        bool needTriangulate = false;

        for (auto &[kfId, kptIdx] : mpt->GetObservedByKeyframesMap())
        {
            auto keyframe = MapManager::Instance().GetKeyframe(kfId);
            auto& kpt = keyframe->GetKeypoint(kptIdx);

            if (keyframe == nullptr)
            {
                continue;
            }
            // TODO: check is keyframe is outlier

            VertexPose* poseVertex;
            // If the keyframe is covisible keyFrame
            if (kfIdToCovKfThenVertex_.count(kfId))
            {
                poseVertex = kfIdToCovKfThenVertex_[kfId].second;
            }
            else
            {
                // else needs to create a new vertex for fixed keyFrame
                VertexPose* fixedPoseVertex = new VertexPose;
                fixedPoseVertex->setId(++vertexIndex);
                fixedPoseVertex->setEstimate(keyframe->GetTcw());
                fixedPoseVertex->setFixed(true);
                optimizer_.addVertex(fixedPoseVertex);

                // Record in map
                kfIdToFixedKfThenVertex_[kfId] = make_pair(keyframe, fixedPoseVertex);

                poseVertex = fixedPoseVertex;
            }

            // Add edge
            BinaryEdgeProjection* edge = new BinaryEdgeProjection(camera_);

            edge->setVertex(0, poseVertex);
            edge->setVertex(1, mptVertex);
            edge->setId(++edgeIndex);
            edge->setMeasurement(toVec2d(kpt.pt));
            edge->setInformation(Eigen::Matrix<double, 2, 2>::Identity());
            auto rk = new g2o::RobustKernelHuber();
            rk->setDelta(deltaRGBD);
            edge->setRobustKernel(rk);
            optimizer_.addEdge(edge);

            edgeToKfThenMpt_[edge] = make_pair(keyframe, mpt);

            if (needTriangulate) {
                poses.push_back(keyframe->GetTcw());
                normalizedPos.push_back(keyframe->camera_->Pixel2Camera(kpt.pt));
            }
        }

        if (needTriangulate) {
            Vector3d pworld = Vector3d::Zero();
            if (Triangulation(poses, normalizedPos, pworld) && pworld[2] > 0)
            {
                // if triangulate successfully
                mptVertex->setEstimate(pworld);
                mpt->triangulated_ = true;
                ++triangulatedCnt;
            }
        }
    }

    // Do first round optimization
    optimizer_.initializeOptimization(0);
    optimizer_.optimize(10);

    // Remove outlier and do second round optimization
    size_t outlierCnt = 0;
    for (auto& [edge, kfAndMpt] : edgeToKfThenMpt_)
    {
        edge->computeError();
        if (edge->chi2() > chi2Threshold_)
        {
            auto& [kf, mpt] = kfAndMpt;
            kf->RemoveObservedMappoint(mpt->GetId());
            edge->setLevel(1);
            ++outlierCnt;
        }
        edge->setRobustKernel(0);
    }

    optimizer_.initializeOptimization(0);
    optimizer_.optimize(10);

    // Set outlier again
    for (auto& [edge, kfAndMpt] : edgeToKfThenMpt_)
    {
        edge->computeError();
        if (edge->level() == 0 && edge->chi2() > chi2Threshold_)
        {
            auto& [kf, mpt] = kfAndMpt;
            kf->RemoveObservedMappoint(mpt->GetId());
            ++outlierCnt;
        }
        edgeToKfThenMpt_[edge].second->optimized_ = true;
    }

    printf("Backend results:\n");
    printf("  optimized pose count: %zu\n", kfIdToCovKfThenVertex_.size());
    printf("  fixed pose count: %zu\n", kfIdToFixedKfThenVertex_.size());
    printf("  optimized mappoint count: %zu\n", mptIdToMptThenVertex_.size());
    printf("  triangulated mappoints count: %zu\n", triangulatedCnt);
    printf("  edge count: %zu\n", edgeToKfThenMpt_.size());
    printf("  outlier edge count: %zu\n\n", outlierCnt);
}

void Backend::UpdateFrontendTrackingMap() {

    frontendMapUpdateHandler_([&](Frame::Ptr& refKeyframe, unordered_map<size_t, Mappoint::Ptr>& trackingMap){

        // Tracking map is defined by reference keyframe
        if (refKeyframe == nullptr || refKeyframe->GetId() != keyframeCurr_->GetId()) {
            refKeyframe = keyframeCurr_;
            trackingMap.clear();
            for (auto& [mptId, mptAndVertex]: mptIdToMptThenVertex_) {
                auto& [mpt, _] = mptAndVertex;
                if (mpt->outlier_) {
                    continue;
                }

                trackingMap[mptId] = mpt;
            }

            if (trackingMap.size() < 100) {
                trackingMap = MapManager::Instance().GetAllMappoints();
                cout << " Not enough active mappoints, reset tracking map to all mappoints" << endl;
            }
        }

        for (auto &[_, kfAndVertex] : kfIdToCovKfThenVertex_)
        {
            auto& [kf, kfVertex] = kfAndVertex;
            kf->SetTcw(kfVertex->estimate());
        }

        for (auto &[_, mptAndVertex] : mptIdToMptThenVertex_)
        {
            auto& [mpt, mptVertex] = mptAndVertex;
            if (!mpt->outlier_)
            {
                mpt->SetPosition(mptVertex->estimate());
            }
        }
    });
}

void Backend::CleanUp() {

    kfIdToCovKfThenVertex_.clear();
    mptIdToMptThenVertex_.clear();
    kfIdToFixedKfThenVertex_.clear();
    edgeToKfThenMpt_.clear();

    // The algorithm, vertex and edges will be deallocated by g2o
    optimizer_.clear();
}

} // namespace