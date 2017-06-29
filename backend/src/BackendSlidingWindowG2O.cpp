#include "ygz/Feature.h"
#include "ygz/MapPoint.h"
#include "ygz/Frame.h"
#include "ygz/BackendSlidingWindowG2O.h"
#include "ygz/Tracker.h"
#include "ygz/ORBMatcher.h"
#include "ygz/Camera.h"
#include "ygz/Memory.h"

// g2o related
#include "ygz/G2OTypes.h"
#include <g2o/core/block_solver.h>
#include <g2o/core/optimization_algorithm_gauss_newton.h>
#include <g2o/core/optimization_algorithm_levenberg.h>
#include <g2o/solvers/eigen/linear_solver_eigen.h>
#include <g2o/core/robust_kernel_impl.h>

namespace ygz {

    bool BackendSlidingWindowG2O::IsBusy() {
        return false;
    }

    // 外部插新KF的接口
    int BackendSlidingWindowG2O::InsertKeyFrame(shared_ptr<Frame> newKF) {

        assert(newKF->mbIsKeyFrame);
        mpCurrent = newKF;

        AddNewKF(mpCurrent);

        // 这个关键帧还需要与周围其他关键帧搜索特征匹配
        // CreateNewMapPoints();

        if (mpKFs.size() == 1) {
            LOG(INFO) << "Backend KF: " << mpKFs.size() << ", map points: " << mpPoints.size() << endl;
            return 0;
        }

        // delete redundant frame
        //DeleteRedundantKF();

        Tracker::eTrackingState trackerState = mpTracker->GetState();
        if (trackerState == Tracker::OK || trackerState == Tracker::WEAK) {
            // 带着IMU优化
            LocalBAWithIMU();
        } else {
            // 未初始化，仅用视觉优化
            LocalBAWithoutIMU();
        }

        if (mpKFs.size() > setting::numBackendKeyframes) {
            // 超过最大数量，则删掉最早那个关键帧
            // TODO check if the current is a bad one?
            DeleteKF(0); //mpKFs.pop_front();
        }

        CleanMapPoint();

        LOG(INFO) << "Backend KF: " << mpKFs.size() << ", map points: " << mpPoints.size() << endl;

        return 0;
    }

    std::set<shared_ptr<MapPoint>> BackendSlidingWindowG2O::GetLocalMap() {
        return mpPoints;
    }

    // 内部接口
    int BackendSlidingWindowG2O::AddNewKF(shared_ptr<Frame> frameAdd) {

        assert (frameAdd->mbIsKeyFrame);
        mpKFs.push_back(frameAdd);

        // 将双目匹配出来的，且没有关联地图点的那些点，为它们创建新的地图点
        frameAdd->AssignFeaturesToGrid();

        for (size_t i = 0; i < frameAdd->mFeaturesLeft.size(); i++) {
            shared_ptr<Feature> feat = frameAdd->mFeaturesLeft[i];

            if (feat->mfInvDepth > 0 && feat->mpPoint == nullptr) {

                // 距离过近或过远都不可靠
                if (feat->mfInvDepth < setting::minNewMapPointInvD || feat->mfInvDepth > setting::maxNewMapPointInvD)
                    continue;

                // create a new map point
                shared_ptr<MapPoint> pNewMP = make_shared<MapPoint>(frameAdd, i);
                pNewMP->AddObservation(frameAdd, i);
                feat->mpPoint = pNewMP;
                pNewMP->ComputeDistinctiveDescriptor();
                pNewMP->UpdateNormalAndDepth();
                mpPoints.insert(pNewMP);

            } else if (feat->mpPoint && feat->mpPoint->isBad() == false) {
                shared_ptr<MapPoint> pMP = feat->mpPoint;
                // 局部地图中还没有，则加入局部地图
                if (mpPoints.count(pMP) == 0)
                    mpPoints.insert(pMP);
                if (pMP->GetObsFromKF(frameAdd) == -1) {
                    // 这个特征关联了地图点，则更新该地图点的信息
                    pMP->AddObservation(frameAdd, i);
                    pMP->UpdateNormalAndDepth();
                    pMP->ComputeDistinctiveDescriptor();
                }
            }
        }

//        // 根据新的frame，选出视野内的地图点
//        for (auto iter = mpPoints.begin(), iterEnd = mpPoints.end(); iter != iterEnd;) {
//            shared_ptr<MapPoint> mp = *iter;

//            // compute the projection point of this map point
//            if (frameAdd->isInFrustum(mp, 0.5, -50)) {
//                // 允许适当超出视野
//                iter++;
//            } else {
//                // 出视野
//                mp->SetMarged();
//                iter = mpPoints.erase(iter);
//            }

//        }

        //LOG(INFO) << "KFs: " << this->mpKFs.size() << ", Mps: " << this->mpPoints.size() << endl;
        return 0;
    }

    void BackendSlidingWindowG2O::DeleteKF(int idx) {
        LOG(INFO) << "Delete KF in mpKFs, index: " << idx << endl;
        if (idx < 0 || idx >= mpKFs.size() - 2) {
            LOG(ERROR) << "index error in DeleteKF" << endl;
            return;
        }
        // change reference KeyFrame
        mpKFs[idx + 1]->mpReferenceKF = mpKFs[idx]->mpReferenceKF;
        // erase KeyFrame in Map
        mpKFs.erase(mpKFs.begin() + idx);
    }

    bool BackendSlidingWindowG2O::DeleteRedundantKF() {

        //LOG(INFO) << "Call delete redundant KF" << endl;
        int N = mpKFs.size();
        // we ignore the newest 2 frames and the oldest 1 frame, so need N>3, here we choose N>5
        if (N <= setting::numBackendKeyframes || N <= 5)
            return false;
        const int thObs = 3;
        bool ret = false;
        // Ignore the newest 2 Frames
        for (int i = N - 3; i >= 1; i--) {
            // check the feature redundancy
            shared_ptr<Frame> pKF = mpKFs[i];
            //shared_ptr<Frame> pKFpre = mpKFs[i-1];
            //shared_ptr<Frame> pKFnext = mpKFs[i+1];
            //
            int nMPs = 0;
            int nRedundantObs = 0;
            for (shared_ptr<Feature> feat : pKF->mFeaturesLeft) {
                if (!feat)
                    continue;
                shared_ptr<MapPoint> mp = feat->mpPoint;
                if (!mp)
                    continue;
                if (mp->isBad())
                    continue;
                // if the mappoint is only observed by this KF, then do not count it
                if (mp->Observations() < 2)
                    continue;
                // valid MapPoint counter
                nMPs++;
                if (mp->Observations() > thObs) {
                    const int &scaleLevel = feat->mLevel;
                    int nObs = 0;
                    for (MapPoint::ObsMap::const_iterator mit = mp->GetObservations().begin(),
                                 mend = mp->GetObservations().end(); mit != mend; mit++) {

                        shared_ptr<Frame> pKFi = mit->first.lock();

                        if (pKFi.get() == pKF.get())
                            continue;

                        shared_ptr<Feature> feati = pKFi->mFeaturesLeft[mit->second];
                        if (!feati) {
                            LOG(WARNING) << "pKFi->mFeaturesLeft[mit->second] in DeleteRedundant" << endl;
                            continue;
                        }
                        //feati->mLevel;
                        const int scaleLeveli = feati->mLevel;
                        if (scaleLeveli <= scaleLevel + 1) {
                            nObs++;
                            if (nObs >= thObs)
                                break;
                        }
                    }
                    //
                    if (nObs >= thObs) {
                        nRedundantObs++;
                    }
                }
            }


            //LOG(INFO) << pKF->mnId << " 'th feature redundancy: " << (0.0 + nRedundantObs) / nMPs << ", "
            //          << nRedundantObs << " / " << nMPs << endl;
            if (nRedundantObs > 0.7 * nMPs) {
                // delete this frame!!
                // only need to erase this frame in mpKFs
                DeleteKF(i);    //mpKFs.erase(mpKFs.begin() + i);
                ret = true;
                break;
            }
        }

        return ret;
    }

    // 找到当前关键帧在图中邻接的一些关键帧
    // 对于每一个邻接的关键帧，根据当前关键帧和该邻接关键帧的姿态，算出两幅图像的基本矩阵
    // 搜索当前关键帧和邻接关键帧之间未成为3d点的特征点匹配
    //         在搜索特征点匹配时，先获分别获得这两个帧在数据库中所有二级节点对应的特征点集
    //         获取两个帧中所有相等的二级节点对，在每个二级节点对
    //         能够得到一对特征点集合，分别对应当前关键帧和相邻关键帧
    //         在两个集合中利用极线约束(基本矩阵)和描述子距离来寻求特征点匹配对
    //         可以在满足极线约束的条件下搜索描述子距离最小并且不超过一定阈值的匹配对
    // 获得未成为3d点的特征点匹配集合后，通过三角化获得3d坐标
    // 在通过平行、重投影误差、尺度一致性等检查后，则建立一个对应的3d点MapPoint对象
    // 需要标记新的3d点与两个关键帧之间的观测关系，还需要计算3d点的平均观测方向以及最佳的描述子
    // 最后将新产生的3d点放到检测队列中等待检验
    int BackendSlidingWindowG2O::CreateNewMapPoints() {

        //LOG(INFO) << "Creating map points" << endl;
        ORBMatcher matcher(0.6, false);

        Matrix3d Rcw1 = mpCurrent->mRcw;
        Matrix3d Rwc1 = mpCurrent->mRwc;
        Vector3d tcw1 = mpCurrent->mtcw;
        Eigen::Matrix<double, 3, 4> Tcw1;
        Tcw1.block<3, 3>(0, 0) = Rcw1;
        Tcw1.block<3, 1>(0, 3) = tcw1;
        Vector3d Ow1 = mpCurrent->mOw;

        shared_ptr<CameraParam> cam = mpCurrent->mpCam;
        const float &fx1 = cam->fx;
        const float &fy1 = cam->fy;
        const float &cx1 = cam->cx;
        const float &cy1 = cam->cy;
        const float &invfx1 = cam->fxinv;
        const float &invfy1 = cam->fyinv;

        int nnew = 0;   // 新增地图点

        // Search matches with epipolar restriction and triangulate
        // 遍历所有找到权重前nn的相邻关键帧
        for (auto frame : mpKFs) {
            if (frame == mpCurrent)
                continue;

            //LOG(INFO) << "Testing " << mpCurrent->mnKFId << " with " << frame->mnKFId << endl;

            // 比较 mpCurrent 和 frame
            Vector3d Ow2 = frame->mOw;

            // 基线，两个关键帧的位移
            Vector3d vBaseline = Ow2 - Ow1;

            // 基线长度
            const double baseline = vBaseline.norm();

            if (baseline < cam->b) {
                //LOG(INFO) << "base line is too small: " << baseline << endl;
                continue;
            } else {
                // 邻接关键帧的场景深度中值
                const float medianDepthKF2 = mpCurrent->ComputeSceneMedianDepth(2);
                // 景深与距离的比例
                const float ratioBaselineDepth = baseline / medianDepthKF2;
                // 如果特别远(比例特别小)，那么不考虑当前邻接的关键帧
                if (ratioBaselineDepth < 0.01) {
                    //LOG(INFO) << "ration base line / depth is small: " << ratioBaselineDepth << endl;
                    continue;
                }
            }

            // Compute Fundamental Matrix
            Matrix3d F12 = ComputeF12(mpCurrent, frame);

            // Search matches that fullfil epipolar constraint
            vector<Match> vMatchedIndices;
            //LOG(INFO) << "Search for triangulation returns: "
            //          << matcher.SearchForTriangulation(mpCurrent, frame, F12, vMatchedIndices) << endl;


            Matrix3d Rcw2 = frame->mRcw;
            Matrix3d Rwc2 = frame->mRwc;
            Vector3d tcw2 = frame->mtcw;

            Matrix<double, 3, 4> Tcw2;
            Tcw2.block<3, 3>(0, 0) = Rcw2;
            Tcw2.block<3, 1>(0, 3) = tcw2;

            // Triangulate each match
            const int nmatches = vMatchedIndices.size();

            for (int ikp = 0; ikp < nmatches; ikp++) {
                // 当前匹配对在当前关键帧中的索引
                const int &idx1 = vMatchedIndices[ikp].index1;

                // 当前匹配对在邻接关键帧中的索引
                const int &idx2 = vMatchedIndices[ikp].index2;

                // 当前匹配在当前关键帧中的特征点
                auto feat1 = mpCurrent->mFeaturesLeft[idx1];

                // 当前匹配在邻接关键帧中的特征点
                auto feat2 = frame->mFeaturesLeft[idx2];

                // Check parallax between rays
                Vector3d xn1((feat1->mPixel[0] - cx1) * invfx1, (feat1->mPixel[1] - cy1) * invfy1, 1.0);
                Vector3d xn2((feat2->mPixel[0] - cx1) * invfx1, (feat2->mPixel[1] - cy1) * invfy1, 1.0);

                Vector3d ray1 = Rwc1 * xn1;
                Vector3d ray2 = Rwc2 * xn2;
                const float cosParallaxRays = ray1.dot(ray2) / (ray1.norm() * ray2.norm());

                float cosParallaxStereo = cosParallaxRays + 1;

                Vector3d x3D;
                if (cosParallaxRays < cosParallaxStereo && cosParallaxRays > 0 &&
                    cosParallaxRays < 0.9998) {
                    // Linear Triangulation Method
                    Matrix4d A;
                    A.row(0) = xn1[0] * Tcw1.row(2) - Tcw1.row(0);
                    A.row(1) = xn1[1] * Tcw1.row(2) - Tcw1.row(1);
                    A.row(2) = xn2[0] * Tcw2.row(2) - Tcw2.row(0);
                    A.row(3) = xn2[1] * Tcw2.row(2) - Tcw2.row(1);

                    Eigen::JacobiSVD<Matrix4d> SVD(A, Eigen::ComputeFullU | Eigen::ComputeFullV);
                    Matrix4d V = SVD.matrixV();
                    if (V(3, 3) == 0)
                        continue;
                    x3D = V.block<3, 1>(0, 3) / V(3, 3);
                } else
                    continue; //No stereo and very low parallax

                Vector3d x3Dt = x3D;

                //Check triangulation in front of cameras
                float z1 = Rcw1.row(2).dot(x3Dt) + tcw1[2];
                if (z1 <= 0)
                    continue;

                float z2 = Rcw2.row(2).dot(x3Dt) + tcw2[2];
                if (z2 <= 0)
                    continue;

                //Check reprojection error in first keyframe
                const float &sigmaSquare1 = setting::levelSigma2[feat1->mLevel];
                const float x1 = Rcw1.row(0).dot(x3Dt) + tcw1[0];
                const float y1 = Rcw1.row(1).dot(x3Dt) + tcw1[1];
                const float invz1 = 1.0 / z1;

                float u1 = fx1 * x1 * invz1 + cx1;
                float v1 = fy1 * y1 * invz1 + cy1;
                float errX1 = u1 - feat1->mPixel[0];
                float errY1 = v1 - feat2->mPixel[1];
                if ((errX1 * errX1 + errY1 * errY1) > 5.991 * sigmaSquare1)
                    continue;

                // Check reprojection error in second keyframe
                const float sigmaSquare2 = setting::levelSigma2[feat2->mLevel];
                const float x2 = Rcw2.row(0).dot(x3Dt) + tcw2[0];
                const float y2 = Rcw2.row(1).dot(x3Dt) + tcw2[1];
                const float invz2 = 1.0 / z2;
                float u2 = fx1 * x2 * invz2 + cx1;
                float v2 = fy1 * y2 * invz2 + cy1;
                float errX2 = u2 - feat2->mPixel[0];
                float errY2 = v2 - feat2->mPixel[1];
                if ((errX2 * errX2 + errY2 * errY2) > 5.991 * sigmaSquare2)
                    continue;

                //Check scale consistency
                Vector3d normal1 = x3D - Ow1;
                float dist1 = normal1.norm();

                Vector3d normal2 = x3D - Ow2;
                float dist2 = normal2.norm();

                if (dist1 == 0 || dist2 == 0)
                    continue;

                const float ratioDist = dist2 / dist1;

                // Triangulation is succesfull
                // 终于可以新建地图点了
                shared_ptr<MapPoint> pMP = make_shared<MapPoint>(mpCurrent, idx1);

                // Set feature initial inverse depth, for both 2 Frames
                shared_ptr<Feature> pFeat = mpCurrent->mFeaturesLeft[idx1];
                pFeat->mfInvDepth = 1. / z1;
                shared_ptr<Feature> pFeat2 = frame->mFeaturesLeft[idx2];
                pFeat2->mfInvDepth = 1. / z2;

                pMP->AddObservation(mpCurrent, idx1);
                pMP->AddObservation(frame, idx2);

                feat1->mpPoint = pMP;
                feat2->mpPoint = pMP;

                pMP->ComputeDistinctiveDescriptor();
                pMP->UpdateNormalAndDepth();

                // 将新增的点放地局部地图
                mpPoints.insert(pMP);

                nnew++;
            }
        }
        //LOG(INFO) << "create " << nnew << " new map points for keyframe " << mpCurrent->mnId << endl;
        return nnew;
    }

    int BackendSlidingWindowG2O::CleanMapPoint() {

        for (auto iter = mpPoints.begin(); iter != mpPoints.end();) {
            shared_ptr<MapPoint> mp = *iter;
            if (mp->mState == MapPoint::BAD) {
                // 如果被标记成坏点，就清理它
                iter = mpPoints.erase(iter);
            } else {
                if (mp->mpRefKF.expired()) {
                    // erase the reference KeyFrame in the observation
                    mp->mObservations.erase(mp->mpRefKF);
                    // change the reference KeyFrame
                    if (mp->SetAnotherRef() == false) {
                        iter = mpPoints.erase(iter);    // ref失效且没法找到新的ref
                        continue;
                    }
                }
                iter++;
            }
        }
        return 0;
    }

    void BackendSlidingWindowG2O::LocalBAWithoutIMU(bool verbose) {
        // Bundle adjustment without IMU
        // TODO 考虑优化对于地图点的影响，包括把哪些setbad，对margin掉的点怎么做之类

        // Setup optimizer
        g2o::SparseOptimizer optimizer;
        g2o::BlockSolverX::LinearSolverType *linearSolver;
        linearSolver = new g2o::LinearSolverEigen<g2o::BlockSolverX::PoseMatrixType>();
        g2o::BlockSolverX *solver_ptr = new g2o::BlockSolverX(linearSolver);
        // g2o::OptimizationAlgorithmGaussNewton *solver = new g2o::OptimizationAlgorithmGaussNewton(solver_ptr);
        g2o::OptimizationAlgorithmLevenberg *solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);

        optimizer.setAlgorithm(solver);
        optimizer.setVerbose(verbose);

        // 关键帧顶点
        int maxKFid = 0;
        for (shared_ptr<Frame> pKFi: mpKFs) {

            // 每个KF只有P+R
            int idKF = pKFi->mnKFId;
            if (idKF > maxKFid) {
                maxKFid = idKF;
            }

            // P+R
            VertexPR *vPR = new VertexPR();
            vPR->setEstimate(pKFi->PR());
            vPR->setId(idKF);
            optimizer.addVertex(vPR);

            // fix the first one and the last one.
            if (pKFi == mpKFs.front() /*|| pKFi == mpKFs.back() */ ) {
                vPR->setFixed(true);
            }
        }

        // Tcb 顶点
        VertexPR *vTcb = new VertexPR();
        Vector6d Tcb;
        Tcb.head<3>() = setting::TBC.inverse().translation();
        Tcb.tail<3>() = setting::TBC.inverse().so3().log();
        vTcb->setEstimate(Tcb);
        int vTcbId = Frame::nNextKFId + MapPoint::nNextId + 1000;
        vTcb->setId(vTcbId);
        vTcb->setFixed(true);
        optimizer.addVertex(vTcb);

        int maxMPid = maxKFid;

        // Set MapPoint vertices
        vector<EdgePRIDP *> vpEdgePoints;
        vpEdgePoints.reserve(mpPoints.size() * 2);

        vector<shared_ptr<Frame> > vpFrames;
        vector<shared_ptr<MapPoint> > vpMappoints;
        vpFrames.reserve(mpPoints.size());
        vpMappoints.reserve(mpPoints.size());

        vector<EdgeIDPPrior *> vpEdgeIDPPrior;
        vpEdgeIDPPrior.reserve(mpPoints.size());
        vector<shared_ptr<MapPoint>> vpMPprior;
        vpMPprior.reserve(mpPoints.size());

        const float thHuber = sqrt(5.991);  // 0.95 卡方检验
        const float thHuber2 = 5.991;  // 0.95 卡方检验
        const float Huber1dsqr = 3.84;  // 0.95~3.84, 0.99~6.635 for 1-Dimension
        const float thHuber1D = sqrt(Huber1dsqr);

        for (shared_ptr<MapPoint> mp: mpPoints) {
            if (mp == nullptr)
                continue;
            if (mp->isBad())
                continue;

            if (mp->mState == MapPoint::GOOD && mp->Observations() > 1) {

                if (mp->mpRefKF.expired()) {
                    continue;
                }

                // 好的点，加入优化
                VertexPointInvDepth *vIDP = new VertexPointInvDepth();
                int idMP = mp->mnId + maxKFid + 1;
                if (idMP > maxMPid) {
                    maxMPid = idMP;
                    if (maxMPid >= vTcbId) { LOG(ERROR) << "maxMPid >= vTcbId. Note." << endl; }
                }

                vIDP->setId(idMP);
                auto feat = mp->mpRefKF.lock()->mFeaturesLeft[mp->mObservations[mp->mpRefKF]]; // 对应的特征点

                if (feat->mfInvDepth <= 0) {
                    const Vector3d &Pw = mp->GetWorldPos();
                    const Matrix3d &Rcw = mp->mpRefKF.lock()->mRcw;
                    const Vector3d &tcw = mp->mpRefKF.lock()->mtcw;
                    const Vector3d Pc = Rcw * Pw + tcw;
                    feat->mfInvDepth = Pc[2];
                    if (feat->mfInvDepth <= 0)
                        LOG(ERROR) << "computed negative feat mfInvDepth = " << feat->mfInvDepth << endl;

                }

                vIDP->setEstimate(feat->mfInvDepth);
                vIDP->setMarginalized(true);

                // 测试这个点除了reference之外，还有没有被别的关键帧看到
                int mpUseCnt = 0;

                // host 帧看到的xy
                Vector3d xyOrig = mp->mpRefKF.lock()->mpCam->Img2Cam(feat->mPixel);
                // add edges in observation
                for (auto &obs: mp->mObservations) {
                    auto f = obs.first;
                    if (f.expired() || f.lock() == mp->mpRefKF.lock())
                        continue;

                    shared_ptr<Frame> kf = f.lock();

                    // if the MapPoint links to more than one KeyFrame, then add this MapPoint vertex
                    if (mpUseCnt == 0) {
                        optimizer.addVertex(vIDP);
                    }

                    mpUseCnt++;

                    shared_ptr<Feature> featObs = kf->mFeaturesLeft[obs.second];

                    // 增加一条边
                    EdgePRIDP *eProj = new EdgePRIDP(xyOrig[0], xyOrig[1], mp->mpRefKF.lock()->mpCam.get());

                    eProj->setVertex(0, (g2o::OptimizableGraph::Vertex *) vIDP);
                    eProj->setVertex(1, optimizer.vertex(mp->mpRefKF.lock()->mnKFId));
                    eProj->setVertex(2, optimizer.vertex(kf->mnKFId));
                    eProj->setVertex(3, (g2o::OptimizableGraph::Vertex *) vTcb);
                    eProj->setMeasurement(featObs->mPixel.cast<double>());

                    const float &invSigma2 = setting::invLevelSigma2[feat->mLevel];
                    eProj->setInformation(Eigen::Matrix2d::Identity() * invSigma2);

                    g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                    eProj->setRobustKernel(rk);
                    rk->setDelta(thHuber);

                    optimizer.addEdge(eProj);
                    vpEdgePoints.push_back(eProj);
                    vpFrames.push_back(kf);
                    vpMappoints.push_back(mp);
                }

                // if the MapPoint is used, add inverse depth prior according to stereo-triangulation
                if (mpUseCnt > 0) {
                    if (feat->mfInvDepth <= 0)
                        LOG(ERROR) << "feat->mfInvDepth mpUseCnt>0 in LocalBAWithIMU" << endl;
                    if (feat->mfInvDepth > 0) {
                        EdgeIDPPrior *eIdpPrior = new EdgeIDPPrior();
                        eIdpPrior->setVertex(0, (g2o::OptimizableGraph::Vertex *) vIDP);
                        eIdpPrior->setMeasurement(feat->mfInvDepth);
                        const double &invSigma2 = setting::invLevelSigma2[feat->mLevel];
                        const double infidp =
                                invSigma2 * 0.5 * mp->mpRefKF.lock()->mpCam->bf * mp->mpRefKF.lock()->mpCam->bf;

                        Eigen::Matrix<double, 1, 1> infIDPMat;
                        infIDPMat(0) = infidp;
                        eIdpPrior->setInformation(infIDPMat);

                        g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                        rk->setDelta(thHuber1D);
                        eIdpPrior->setRobustKernel(rk);

                        optimizer.addEdge(eIdpPrior);
                        vpEdgeIDPPrior.push_back(eIdpPrior);
                        vpMPprior.push_back(mp);
                    }
                } else {
                    delete vIDP;
                }

            } else if (mp->mState == MapPoint::MARGED) {
                LOG(ERROR) << "should not marged" << endl;
            }
        }

        optimizer.initializeOptimization();
        optimizer.optimize(20);

        // Check inliers and optimize again without outliers
        int cntInliers = 0;
        for (EdgePRIDP *e: vpEdgePoints) {
            if (e->chi2() > thHuber2 ||
                static_cast<ygz::VertexPointInvDepth *>(e->vertex(0))->estimate() < 1e-5 ||
                static_cast<ygz::VertexPointInvDepth *>(e->vertex(0))->estimate() > 1e2) {
                e->setLevel(1);
            } else {
                cntInliers++;
                e->setLevel(0);
            }
            e->setRobustKernel(nullptr);
        }

        int cntPriorOutliers = 0;
        for (EdgeIDPPrior *e : vpEdgeIDPPrior) {
            if (e->chi2() > Huber1dsqr) {
                e->setLevel(1);
                cntPriorOutliers++;
            }
            e->setRobustKernel(nullptr);
        }

        LOG(INFO) << "total: " << vpEdgeIDPPrior.size() << ", cntPriorOutliers = " << cntPriorOutliers << endl;

        // do it again
        optimizer.initializeOptimization();
        optimizer.optimize(10);

        // recover the pose and points estimation
        for (shared_ptr<Frame> frame: mpKFs) {
            VertexPR *vPR = (VertexPR *) optimizer.vertex(frame->mnKFId);
            frame->SetPose(SE3d(vPR->R(), vPR->t()));
        }

        // and the points
        for (shared_ptr<MapPoint> mp: mpPoints) {
            if (mp && mp->isBad() == false && mp->mState == MapPoint::GOOD && mp->Observations() > 1) {
                VertexPointInvDepth *v = (VertexPointInvDepth *) optimizer.vertex(mp->mnId + maxKFid + 1);
                if (!v) {
                    LOG(ERROR) << "v is nullptr" << endl;
                    continue;
                }

                shared_ptr<Feature> feat = mp->mpRefKF.lock()->mFeaturesLeft[mp->mObservations[mp->mpRefKF]];
                double invD = v->estimate();

                feat->mfInvDepth = invD;

                // TODO 消灭谜阈值从我做起
                if (invD < 1e-5 || invD > 1e2) {
                    mp->SetBadFlag();
                    LOG(INFO) << "Estimated invD = " << invD << ", original value: " << feat->mfInvDepth << endl;
                }
            }
        }

        // update all the points because we have updated the key-frames and use the inverse depth parameterization
        for ( shared_ptr<MapPoint> mp: mpPoints ) {
            if ( mp->isBad() == false ) {
                mp->UpdateWorldPos();
            }
        }

        // TODO 处理观测中的inlier和outlier，对于outlier需要删掉相应的观测
        //LOG(INFO) << "dealing with outlier" << endl;
        for (size_t i = 0, iend = vpEdgeIDPPrior.size(); i < iend; i++) {
            EdgeIDPPrior *e = vpEdgeIDPPrior[i];
            shared_ptr<MapPoint> mp = vpMPprior[i];
            if (e->chi2() > Huber1dsqr) {
                mp->SetBadFlag();
            }
        }

        for (size_t i = 0, iend = vpEdgePoints.size(); i < iend; i++) {
            EdgePRIDP *e = vpEdgePoints[i];
            shared_ptr<MapPoint> mp = vpMappoints[i];
            shared_ptr<Frame> kf = vpFrames[i];
            if (e->chi2() > thHuber2 ||
                static_cast<ygz::VertexPointInvDepth *>(e->vertex(0))->estimate() < 1e-5 ||
                static_cast<ygz::VertexPointInvDepth *>(e->vertex(0))->estimate() > 1e2) {
                // erase observation
                if (mp->mObservations.count(kf)) {
                    //LOG(INFO) << "Erase an obs, chi2 = " << e->chi2() << ", mp.isBad: " << mp->isBad() << endl;

                    // 1. delete feature in KeyFrame
                    weak_ptr<Frame> kfweak(kf);
                    shared_ptr<Feature> feat = kf->mFeaturesLeft[mp->mObservations[kfweak]];
                    feat->mpPoint = nullptr;

                    // 2. delete observation in MapPoint
                    mp->mObservations.erase(kf);
                    if (kf == mp->mpRefKF.lock()) { // change reference KeyFrame
                        mp->mpRefKF = mp->mObservations.begin()->first;
                    }
                    if (mp->mObservations.size() < 2) { // if less than 2
                        mp->SetBadFlag();
                    }
                }
            }
        }
        //LOG(INFO) << "optimization without imu done." << endl;
    }

    void BackendSlidingWindowG2O::LocalBAWithIMU(bool verbose) {
        // Bundle adjustment with IMU

        // Gravity vector in world frame
        Vector3d GravityVec = mpTracker->g();

        // Setup optimizer
        g2o::SparseOptimizer optimizer;
        g2o::BlockSolverX::LinearSolverType *linearSolver;
        linearSolver = new g2o::LinearSolverEigen<g2o::BlockSolverX::PoseMatrixType>();
        g2o::BlockSolverX *solver_ptr = new g2o::BlockSolverX(linearSolver);
        g2o::OptimizationAlgorithmGaussNewton *solver = new g2o::OptimizationAlgorithmGaussNewton(solver_ptr);
        //g2o::OptimizationAlgorithmLevenberg *solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
        optimizer.setAlgorithm(solver);

        optimizer.setVerbose(verbose);

        int maxKFid = 0;
        // 关键帧顶点
        for (shared_ptr<Frame> pKFi: mpKFs) {
            assert(pKFi && pKFi->mbIsKeyFrame);

            // 每个KF有四个顶点
            int idKF = pKFi->mnKFId * 4;
            if (idKF + 3 > maxKFid) {
                maxKFid = idKF + 3;
            }

            // P+R
            VertexPR *vPR = new VertexPR();
            vPR->setEstimate(pKFi->PR());
            vPR->setId(idKF);
            optimizer.addVertex(vPR);

            // speed
            VertexSpeed *vSpeed = new VertexSpeed();
            vSpeed->setEstimate(pKFi->Speed());
            vSpeed->setId(idKF + 1);
            optimizer.addVertex(vSpeed);

            // Ba and Bg
            VertexGyrBias *vBg = new VertexGyrBias();
            vBg->setId(idKF + 2);
            vBg->setEstimate(pKFi->BiasG());
            optimizer.addVertex(vBg);

            VertexAcceBias *vBa = new VertexAcceBias();
            vBa->setId(idKF + 3);
            vBa->setEstimate(pKFi->BiasA());
            optimizer.addVertex(vBa);

            // fix the first one
            if (pKFi == mpKFs.front()) {
                vPR->setFixed(true);
                vSpeed->setFixed(true);
                vBg->setFixed(true);
                vBa->setFixed(true);
            }
        }

        // Tcb
        VertexPR *vTcb = new VertexPR();
        Vector6d Tcb;
        Tcb.head<3>() = setting::TBC.inverse().translation();
        Tcb.tail<3>() = setting::TBC.inverse().so3().log();
        vTcb->setEstimate(Tcb);
        int vTcbId = Frame::nNextKFId * 4 + MapPoint::nNextId + 1000;
        vTcb->setId(vTcbId);
        vTcb->setFixed(true);   // fix this will give better result?
        optimizer.addVertex(vTcb);

        // 关键帧之间的边
        vector<EdgePRV *> vpEdgePRV;
        vector<EdgeBiasG *> vpEdgeBg;
        vector<EdgeBiasA *> vpEdgeBa;

        // 谜之阈值，用在Huber里
        // Use chi2inv() in MATLAB to compute the value corresponding to 0.95/0.99 prob. w.r.t 15DOF: 24.9958/30.5779
        // 12.592/16.812 for 0.95/0.99 6DoF
        // 16.919/21.666 for 0.95/0.99 9DoF
        //const float thHuberNavState = sqrt(30.5779);
        const float thHuberPRV = sqrt(100 * 21.666);
        const float thHuberBias = sqrt(100 * 16.812);

        // Inverse covariance of bias random walk
        Matrix3d infoBg = Matrix3d::Identity() / setting::gyrBiasRw2;
        Matrix3d infoBa = Matrix3d::Identity() / setting::accBiasRw2;

        for (shared_ptr<Frame> pKF1: mpKFs) {
            if (pKF1->mpReferenceKF.expired())
                continue;

            shared_ptr<Frame> pKF0 = pKF1->mpReferenceKF.lock();   // Previous KF

            // PR0, PR1, V0, V1, Bg0, Ba0
            EdgePRV *ePRV = new EdgePRV(GravityVec);
            ePRV->setVertex(0, optimizer.vertex(pKF0->mnKFId * 4));
            ePRV->setVertex(1, optimizer.vertex(pKF1->mnKFId * 4));
            ePRV->setVertex(2, optimizer.vertex(pKF0->mnKFId * 4 + 1));
            ePRV->setVertex(3, optimizer.vertex(pKF1->mnKFId * 4 + 1));
            ePRV->setVertex(4, optimizer.vertex(pKF0->mnKFId * 4 + 2));
            ePRV->setVertex(5, optimizer.vertex(pKF0->mnKFId * 4 + 3));
            ePRV->setMeasurement(pKF1->GetIMUPreInt());

            ePRV->computeError();

            // set Covariance
            Matrix9d CovPRV = pKF1->GetIMUPreInt().getCovPVPhi();
            // 但是Edge里用是P,R,V，所以交换顺序
            CovPRV.col(3).swap(CovPRV.col(6));
            CovPRV.col(4).swap(CovPRV.col(7));
            CovPRV.col(5).swap(CovPRV.col(8));
            CovPRV.row(3).swap(CovPRV.row(6));
            CovPRV.row(4).swap(CovPRV.row(7));
            CovPRV.row(5).swap(CovPRV.row(8));
            // LOG(INFO)<<"Info PRV = \n"<<CovPRV.inverse()<<endl;

            ePRV->setInformation(CovPRV.inverse());

            g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
            ePRV->setRobustKernel(rk);
            rk->setDelta(thHuberPRV);
            optimizer.addEdge(ePRV);

            vpEdgePRV.push_back(ePRV);

            // bias 的随机游走，用两条边来约束
            double dt = pKF1->GetIMUPreInt().getDeltaTime();
            EdgeBiasG *eBG = new EdgeBiasG();
            eBG->setVertex(0, optimizer.vertex(pKF0->mnKFId * 4 + 2));
            eBG->setVertex(1, optimizer.vertex(pKF1->mnKFId * 4 + 2));
            eBG->setMeasurement(Vector3d::Zero());   // bias受白噪声影响
            eBG->setInformation(infoBg / dt);
            g2o::RobustKernelHuber *rkb = new g2o::RobustKernelHuber;
            eBG->setRobustKernel(rkb);
            rkb->setDelta(thHuberBias);
            optimizer.addEdge(eBG);
            vpEdgeBg.push_back(eBG);

            EdgeBiasA *eBA = new EdgeBiasA();
            eBA->setVertex(0, optimizer.vertex(pKF0->mnKFId * 4 + 3));
            eBA->setVertex(1, optimizer.vertex(pKF1->mnKFId * 4 + 3));
            eBA->setMeasurement(Vector3d::Zero());   // bias受白噪声影响
            eBA->setInformation(infoBa / dt);
            g2o::RobustKernelHuber *rkba = new g2o::RobustKernelHuber;
            eBA->setRobustKernel(rkba);
            rkba->setDelta(thHuberBias);
            optimizer.addEdge(eBA);
            vpEdgeBa.push_back(eBA);
        }

        // Set MapPoint vertices
        vector<EdgePRIDP *> vpEdgePoints;
        //vector<VertexPointInvDepth *> vIDPs;
        vpEdgePoints.reserve(mpPoints.size());
        vector<shared_ptr<Frame> > vpFrames;
        vector<shared_ptr<MapPoint> > vpMappoints;
        vpFrames.reserve(mpPoints.size());
        vpMappoints.reserve(mpPoints.size());

        vector<EdgeIDPPrior *> vpEdgeIDPPrior;
        vpEdgeIDPPrior.reserve(mpPoints.size());
        vector<shared_ptr<MapPoint> > vpMPprior;
        vpMPprior.reserve(mpPoints.size());

        const float thHuber = sqrt(5.991);  // 0.95 卡方检验
        const float thHuber2 = 5.991;  // 0.95 卡方检验
        const float Huber1dsqr = 3.84;  // 0.95~3.84, 0.99~6.635 for 1-Dimension
        const float thHuber1D = sqrt(Huber1dsqr);

        //LOG(INFO) << "Vertices: " << optimizer.vertices().size() << endl;

        int maxMPid = maxKFid;
        for (shared_ptr<MapPoint> mp: mpPoints) {
            if (mp == nullptr)
                continue;
            if (mp->isBad())
                continue;

            if (mp->mState == MapPoint::GOOD && mp->Observations() > 1) {

                if (mp->mpRefKF.expired()) {
                    continue;
                }

                // 好的点，加入优化
                VertexPointInvDepth *vIDP = new VertexPointInvDepth();
                int idmp = mp->mnId + maxKFid + 1;
                if (idmp > maxMPid) {
                    maxMPid = idmp;
                    if (maxMPid >= vTcbId) { LOG(ERROR) << "maxMPid >= vTcbId 2. Note." << endl; }
                }

                vIDP->setId(idmp);
                shared_ptr<Feature> feat = mp->mpRefKF.lock()->mFeaturesLeft[mp->mObservations[mp->mpRefKF]]; // 对应的特征点
                assert(feat);
                assert(feat->mfInvDepth > 0);

                if (feat->mfInvDepth <= 0) {
                    const Vector3d &Pw = mp->GetWorldPos();
                    const Matrix3d &Rcw = mp->mpRefKF.lock()->mRcw;
                    const Vector3d &tcw = mp->mpRefKF.lock()->mtcw;
                    const Vector3d Pc = Rcw * Pw + tcw;
                    feat->mfInvDepth = Pc[2];
                    if (feat->mfInvDepth <= 0)
                        LOG(ERROR) << "computed negative feat mfInvDepth = " << feat->mfInvDepth << endl;

                }
                vIDP->setEstimate(feat->mfInvDepth);
                vIDP->setMarginalized(true);
                //vIDPs.push_back(vIDP);
                //optimizer.addVertex(vIDP);

                // 测试这个点除了reference之外，还有没有被别的关键帧看到
                int mpUseCnt = 0;

                // host 帧看到的xy
                Vector3d xyOrig = mp->mpRefKF.lock()->mpCam->Img2Cam(feat->mPixel);
                // add edges in observation
                for (auto &obs: mp->mObservations) {
                    if (obs.first.expired())
                        continue;

                    shared_ptr<Frame> kf = obs.first.lock();
                    if (kf == mp->mpRefKF.lock())    // 同一个
                        continue;

                    // if the MapPoint links to more than one KeyFrame, then add this MapPoint vertex
                    if (mpUseCnt == 0) {
                        optimizer.addVertex(vIDP);
                    }
                    mpUseCnt++;

                    shared_ptr<Feature> featObs = kf->mFeaturesLeft[obs.second];

                    assert (featObs);

                    // 增加一条边
                    EdgePRIDP *eProj = new EdgePRIDP(xyOrig[0], xyOrig[1], mp->mpRefKF.lock()->mpCam.get());

                    eProj->setVertex(0, (g2o::OptimizableGraph::Vertex *) vIDP);
                    eProj->setVertex(1, optimizer.vertex(mp->mpRefKF.lock()->mnKFId * 4)); // P+R
                    eProj->setVertex(2, optimizer.vertex(kf->mnKFId * 4));  // P+R
                    eProj->setVertex(3, optimizer.vertex(vTcbId));    // Tcb
                    eProj->setMeasurement(featObs->mPixel.cast<double>());

                    const float &invSigma2 = setting::invLevelSigma2[feat->mLevel];
                    eProj->setInformation(Eigen::Matrix2d::Identity() * invSigma2);
                    g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                    eProj->setRobustKernel(rk);
                    rk->setDelta(thHuber);

                    optimizer.addEdge(eProj);
                    vpEdgePoints.push_back(eProj);
                    vpFrames.push_back(kf);
                    vpMappoints.push_back(mp);
                }

                // if the MapPoint is used, add inverse depth prior according to stereo-triangulation
                if (mpUseCnt > 0) {
                    if (feat->mfInvDepth <= 0)
                        LOG(ERROR) << "feat->mfInvDepth mpUseCnt>0 in LocalBAWithIMU" << endl;
                    if (feat->mfInvDepth > 0) {
                        EdgeIDPPrior *eIdpPrior = new EdgeIDPPrior();
                        eIdpPrior->setVertex(0, (g2o::OptimizableGraph::Vertex *) vIDP);
                        eIdpPrior->setMeasurement(feat->mfInvDepth);
                        const double &invSigma2 = setting::invLevelSigma2[feat->mLevel];
                        const double infidp =
                                invSigma2 * 0.5 * mp->mpRefKF.lock()->mpCam->bf * mp->mpRefKF.lock()->mpCam->bf;

                        Eigen::Matrix<double, 1, 1> infIDPMat;
                        infIDPMat(0) = infidp;
                        eIdpPrior->setInformation(infIDPMat);

                        g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                        rk->setDelta(thHuber1D);
                        eIdpPrior->setRobustKernel(rk);

                        optimizer.addEdge(eIdpPrior);
                        vpEdgeIDPPrior.push_back(eIdpPrior);
                        vpMPprior.push_back(mp);
                    }
                } else {
                    delete vIDP;
                }

            } else if (mp->mState == MapPoint::MARGED) {
                // 已经出视野，仅用于优化pose
                // TODO 加入处理视野外点的做法
                LOG(ERROR) << "should not marg" << endl;
            }
        }

        if (verbose) {
            LOG(INFO) << "Vertices: " << optimizer.vertices().size() << endl;
            LOG(INFO) << vpEdgeBg.size() << ", " << vpEdgeBa.size() << endl;
        }

        optimizer.initializeOptimization();
        optimizer.optimize(5);

        // Check inliers and optimize again without outliers
        int cntInliers = 0;
        for (EdgePRIDP *e: vpEdgePoints) {
            if (e->chi2() > thHuber2 ||
                static_cast<ygz::VertexPointInvDepth *>(e->vertex(0))->estimate() < 1e-5 ||
                static_cast<ygz::VertexPointInvDepth *>(e->vertex(0))->estimate() > 1e2) {
                e->setLevel(1);
            } else {
                cntInliers++;
                e->setLevel(0);
            }
            e->setRobustKernel(nullptr);
        }
        int cntPriorOutliers = 0;
        for (EdgeIDPPrior *e : vpEdgeIDPPrior) {
            if (e->chi2() > Huber1dsqr) {
                e->setLevel(1);
                cntPriorOutliers++;
                //LOG(INFO) << "chi2 = " << e->chi2() << ", error: " << e->error() << endl;
            }
            e->setRobustKernel(nullptr);
        }
        if (verbose) {
            LOG(INFO) << "total: " << vpEdgeIDPPrior.size() << ", cntPriorOutliers = " << cntPriorOutliers << endl;
            LOG(INFO) << "Inliers/total: " << cntInliers / double(vpEdgePoints.size()) << endl;
        }

        // do it again
        optimizer.initializeOptimization();
        optimizer.optimize(10);

        // recover the pose and points estimation
        for (shared_ptr<Frame> frame: mpKFs) {
            VertexPR *vPR = (VertexPR *) optimizer.vertex(frame->mnKFId * 4);
            VertexSpeed *vSpeed = (VertexSpeed *) optimizer.vertex(frame->mnKFId * 4 + 1);
            VertexGyrBias *vBg = (VertexGyrBias *) optimizer.vertex(frame->mnKFId * 4 + 2);
            VertexAcceBias *vBa = (VertexAcceBias *) optimizer.vertex(frame->mnKFId * 4 + 3);

            frame->SetPose(SE3d(vPR->R(), vPR->t()));
            frame->SetSpeedBias(vSpeed->estimate(), vBg->estimate(), vBa->estimate());

            if (verbose) {
                LOG(INFO) << "Frame " << frame->mnKFId << ", pose = \n" << frame->GetPose().matrix() << endl;
                LOG(INFO) << "Speed and bias = " << frame->mSpeedAndBias.transpose() << endl;
            }
        }

        // and the points
        for (shared_ptr<MapPoint> mp: mpPoints) {
            if (mp && mp->isBad() == false && mp->mState == MapPoint::GOOD && mp->Observations() > 1) {
                VertexPointInvDepth *v = (VertexPointInvDepth *) optimizer.vertex(mp->mnId + maxKFid + 1);
                assert(v);
                if (!v)
                    continue;
                shared_ptr<Feature> feat = mp->mpRefKF.lock()->mFeaturesLeft[mp->mObservations[mp->mpRefKF]];

                double invD = v->estimate();
                feat->mfInvDepth = invD;

                mp->UpdateWorldPos();
                mp->UpdateNormalAndDepth();

                // TODO 消灭谜阈值从我做起
                if (invD < 1e-5 || invD > 1e2) {
                    mp->SetBadFlag();
                    LOG(INFO) << "Estimated invD = " << invD << ", original value: " << feat->mfInvDepth << endl;
                }
            }
        }

        // TODO 处理观测中的inlier和outlier，对于outlier需要删掉相应的观测
        //LOG(INFO) << "dealing with outlier" << endl;
        for (size_t i = 0, iend = vpEdgeIDPPrior.size(); i < iend; i++) {
            EdgeIDPPrior *e = vpEdgeIDPPrior[i];
            shared_ptr<MapPoint> mp = vpMPprior[i];
            if (e->chi2() > Huber1dsqr) {
                LOG(INFO) << "stereo map point bad, depth: "
                          << 1. / mp->mpRefKF.lock()->mFeaturesLeft[mp->mObservations[mp->mpRefKF]]->mfInvDepth
                          << ", isBad: " << mp->isBad() << endl;
                mp->SetBadFlag();
            }
        }
        for (size_t i = 0, iend = vpEdgePoints.size(); i < iend; i++) {
            EdgePRIDP *e = vpEdgePoints[i];
            shared_ptr<MapPoint> mp = vpMappoints[i];
            shared_ptr<Frame> kf = vpFrames[i];
            if (e->chi2() > thHuber2 ||
                static_cast<ygz::VertexPointInvDepth *>(e->vertex(0))->estimate() < 1e-5 ||
                static_cast<ygz::VertexPointInvDepth *>(e->vertex(0))->estimate() > 1e2) {
                // erase observation
                if (mp->mObservations.count(kf)) {
                    //LOG(INFO) << "Erase an obs, chi2 = " << e->chi2() << ", mp.isBad: " << mp->isBad() << endl;
                    // 1. delete feature in KeyFrame
                    kf->EraseMapPointMatch(mp);
                    // 2. delete observation in MapPoint
                    mp->mObservations.erase(kf);
                    if (kf.get() == mp->mpRefKF.lock().get()) { // change reference KeyFrame
                        mp->mpRefKF = mp->mObservations.begin()->first;
                    }
                    if (mp->mObservations.size() < 2) { // if less than 2
                        mp->SetBadFlag();
                    }
                }
            }
        }
        //LOG(INFO) << "optimization with imu done." << endl;

    }

    void BackendSlidingWindowG2O::UpdateWholeMap(const SE3d &Twb) {

    }

    void BackendSlidingWindowG2O::Shutdown() {

    }

    Matrix3d BackendSlidingWindowG2O::ComputeF12(shared_ptr<Frame> f1, shared_ptr<Frame> f2) {

        Matrix3d R1w = f1->mRcw;
        Vector3d t1w = f1->mtcw;
        Matrix3d R2w = f2->mRcw;
        Vector3d t2w = f2->mtcw;

        Matrix3d R12 = R1w * R2w.transpose();
        Vector3d t12 = -1 * R1w * R2w.transpose() * t2w + t1w;

        Matrix3d t12x = SO3d::hat(t12);

        const Matrix3d &K1 = f1->mpCam->Kinv.cast<double>();
        const Matrix3d &K2 = f2->mpCam->K.cast<double>();

        return K1.transpose() * t12x * R12 * K2.inverse();
    }

    void BackendSlidingWindowG2O::PrintKFInfo() {
        for (shared_ptr<Frame> frame: mpKFs) {
            if (frame->mpReferenceKF.expired() == false)
                LOG(INFO) << "Keyframe " << frame->mnKFId << ", ref = " << frame->mpReferenceKF.lock()->mnKFId << endl;
            else
                LOG(INFO) << "Keyframe " << frame->mnKFId << ", ref = null";

            LOG(INFO) << "Pose = \n" << frame->GetPose().matrix() << endl;
        }
    }

    void BackendSlidingWindowG2O::Reset() {
        LOG(INFO) << "Backend is reset" << endl;
        mpCurrent = nullptr;
        mpKFs.clear();
        mpPoints.clear();
    }

}
