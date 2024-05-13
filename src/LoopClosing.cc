/**
 * This file is part of ORB-SLAM3
 *
 * Copyright (C) 2017-2021 Carlos Campos, Richard Elvira, Juan J. Gómez
 * Rodríguez, José M.M. Montiel and Juan D. Tardós, University of Zaragoza.
 * Copyright (C) 2014-2016 Raúl Mur-Artal, José M.M. Montiel and Juan D. Tardós,
 * University of Zaragoza.
 *
 * ORB-SLAM3 is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * ORB-SLAM3 is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ORB-SLAM3. If not, see <http://www.gnu.org/licenses/>.
 */

#include "MORB_SLAM/LoopClosing.h"

#include <mutex>
#include <thread>

#include "MORB_SLAM/ImprovedTypes.hpp"
#include "MORB_SLAM/Converter.h"
#include "MORB_SLAM/G2oTypes.h"
#include "MORB_SLAM/ORBmatcher.h"
#include "MORB_SLAM/Optimizer.h"
#include "MORB_SLAM/Sim3Solver.h"

namespace MORB_SLAM {

LoopClosing::LoopClosing(const Atlas_ptr &pAtlas, std::shared_ptr<KeyFrameDatabase> pDB, std::shared_ptr<ORBVocabulary> pVoc, const bool bFixScale, const bool bActiveLC, bool bInertial)
    : 
#ifdef REGISTER_TIMES
      nMerges(0),
      nLoop(0),
      nFGBA_exec(0),
      nFGBA_abort(0),
#endif    
      hasMergedLocalMap(false),
      mbResetRequested(false),
      mbResetActiveMapRequested(false),
      mbFinishRequested(false),
      mbFinished(true),
      mpAtlas(pAtlas),
      mpKeyFrameDB(pDB),
      mpORBVocabulary(pVoc),
      mnCovisibilityConsistencyTh(3),
      mpLastCurrentKF(nullptr),
      mpMatchedKF(nullptr),
      mbLoopDetected(false),
      mnLoopNumCoincidences(0),
      mnLoopNumNotFound(0),
      mbMergeDetected(false),
      mnMergeNumCoincidences(0),
      mnMergeNumNotFound(0), 
      mbRunningGBA(false),
      mbFinishedGBA(true),
      mbStopGBA(false),
      mbFixScale(bFixScale),
      mnFullBAIdx(0),
      mstrFolderSubTraj("SubTrajectories/"),
      mnNumCorrection(0),
      mnCorrectionGBA(0),
      mbActiveLC(bActiveLC),
      mbInertial(bInertial) {

}

void LoopClosing::SetTracker(Tracking_ptr pTracker) { mpTracker = pTracker; }

void LoopClosing::SetLocalMapper(std::shared_ptr<LocalMapping> pLocalMapper) { mpLocalMapper = pLocalMapper; }

void LoopClosing::Run() {
  mbFinished = false;

  while (1) {

    // NEW LOOP AND MERGE DETECTION ALGORITHM
    //----------------------------

    if (CheckNewKeyFrames()) {
      if (mpLastCurrentKF) {
        mpLastCurrentKF->mvpLoopCandKFs.clear();
        mpLastCurrentKF->mvpMergeCandKFs.clear();
      }
#ifdef REGISTER_TIMES
      std::chrono::steady_clock::time_point time_StartPR = std::chrono::steady_clock::now();
#endif

      bool bFindedRegion = NewDetectCommonRegions();

#ifdef REGISTER_TIMES
      std::chrono::steady_clock::time_point time_EndPR = std::chrono::steady_clock::now();
      double timePRTotal = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(time_EndPR - time_StartPR).count();
      vdPRTotal_ms.push_back(timePRTotal);
#endif
      if (bFindedRegion) {
        if (mbMergeDetected) {
          if (mbInertial && (!mpCurrentKF->GetMap()->isImuInitialized())) {
            std::cout << "IMU is not initilized, merge is aborted" << std::endl;
          } else {
            Sophus::SE3d mTmw = mpMergeMatchedKF->GetPose().cast<double>();
            g2o::Sim3 gSmw2(mTmw.unit_quaternion(), mTmw.translation(), 1.0);
            Sophus::SE3d mTcw = mpCurrentKF->GetPose().cast<double>();
            g2o::Sim3 gScw1(mTcw.unit_quaternion(), mTcw.translation(), 1.0);
            g2o::Sim3 gSw2c = mg2oMergeSlw.inverse();
            // g2o::Sim3 gSw1m = mg2oMergeSlw; // UNUSED

            mSold_new = (gSw2c * gScw1);

            if (mbInertial) {
              std::cout << "Merge check transformation with IMU" << std::endl;
              if (mSold_new.scale() < 0.90 || mSold_new.scale() > 1.1) {
                mpMergeLastCurrentKF->SetErase();
                mpMergeMatchedKF->SetErase();
                mnMergeNumCoincidences = 0;
                mvpMergeMatchedMPs.clear();
                mvpMergeMPs.clear();
                mvpMergeConnectedKFs.clear();
                mnMergeNumNotFound = 0;
                mbMergeDetected = false;
                Verbose::PrintMess("scale bad estimated. Abort merging", Verbose::VERBOSITY_NORMAL);
                continue;
              }
              // If inertial, force only yaw
              if ((mpTracker->mSensor == CameraType::IMU_MONOCULAR || mpTracker->mSensor == CameraType::IMU_STEREO || mpTracker->mSensor == CameraType::IMU_RGBD) && mpCurrentKF->GetMap()->GetIniertialBA1()) {
                Eigen::Vector3d phi = LogSO3(mSold_new.rotation().toRotationMatrix());
                phi(0) = 0;
                phi(1) = 0;
                mSold_new = g2o::Sim3(ExpSO3(phi), mSold_new.translation(), 1.0);
              }
            }

            mg2oMergeSmw = gSmw2 * gSw2c * gScw1;

            mg2oMergeScw = mg2oMergeSlw;

            // mpTracker->SetStepByStep(true);

            Verbose::PrintMess("*Merge detected", Verbose::VERBOSITY_QUIET);

#ifdef REGISTER_TIMES
            std::chrono::steady_clock::time_point time_StartMerge = std::chrono::steady_clock::now();
            nMerges += 1;
#endif
            mpLocalMapper->setIsDoneVIBA(false);
            mpTracker->mLockPreTeleportTranslation = true;
            // TODO UNCOMMENT
            if (mpTracker->mSensor == CameraType::IMU_MONOCULAR || mpTracker->mSensor == CameraType::IMU_STEREO || mpTracker->mSensor == CameraType::IMU_RGBD)
              MergeLocal2();
            else
              MergeLocal();

#ifdef REGISTER_TIMES
            std::chrono::steady_clock::time_point time_EndMerge = std::chrono::steady_clock::now();
            double timeMergeTotal = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(time_EndMerge - time_StartMerge).count();
            vdMergeTotal_ms.push_back(timeMergeTotal);
#endif
            mpTracker->mTeleported = true;
            Verbose::PrintMess("Merge finished!", Verbose::VERBOSITY_QUIET);
          }

          vdPR_CurrentTime.push_back(mpCurrentKF->mTimeStamp);
          vdPR_MatchedTime.push_back(mpMergeMatchedKF->mTimeStamp);
          vnPR_TypeRecogn.push_back(1);

          // Reset all variables
          mpMergeLastCurrentKF->SetErase();
          mpMergeMatchedKF->SetErase();
          mnMergeNumCoincidences = 0;
          mvpMergeMatchedMPs.clear();
          mvpMergeMPs.clear();
          mvpMergeConnectedKFs.clear();
          mnMergeNumNotFound = 0;
          mbMergeDetected = false;

          if (mbLoopDetected) {
            // Reset Loop variables
            mpLoopLastCurrentKF->SetErase();
            mpLoopMatchedKF->SetErase();
            mnLoopNumCoincidences = 0;
            mvpLoopMatchedMPs.clear();
            mvpLoopMPs.clear();
            mnLoopNumNotFound = 0;
            mbLoopDetected = false;
          }
        }

        if (mbLoopDetected) {
          bool bGoodLoop = true;
          vdPR_CurrentTime.push_back(mpCurrentKF->mTimeStamp);
          vdPR_MatchedTime.push_back(mpLoopMatchedKF->mTimeStamp);
          vnPR_TypeRecogn.push_back(0);

          Verbose::PrintMess("*Loop detected", Verbose::VERBOSITY_QUIET);

          mg2oLoopScw = mg2oLoopSlw;  //*mvg2oSim3LoopTcw[nCurrentIndex];
          if (mbInertial) {
            Sophus::SE3d Twc = mpCurrentKF->GetPoseInverse().cast<double>();
            g2o::Sim3 g2oTwc(Twc.unit_quaternion(), Twc.translation(), 1.0);
            g2o::Sim3 g2oSww_new = g2oTwc * mg2oLoopScw;

            Eigen::Vector3d phi = LogSO3(g2oSww_new.rotation().toRotationMatrix());
            std::cout << "phi = " << phi.transpose() << std::endl;
            if (fabs(phi(0)) < 0.008f && fabs(phi(1)) < 0.008f && fabs(phi(2)) < 0.349f) {
                // If inertial, force only yaw
              if (mpCurrentKF->GetMap()->GetIniertialBA2()) {
                phi(0) = 0;
                phi(1) = 0;
                g2oSww_new = g2o::Sim3(ExpSO3(phi), g2oSww_new.translation(), 1.0);
                mg2oLoopScw = g2oTwc.inverse() * g2oSww_new;
              }

            } else {
              std::cout << "BAD LOOP!!!" << std::endl;
              bGoodLoop = false;
            }
          }

          if (bGoodLoop) {
            mvpLoopMapPoints = mvpLoopMPs;
            mpLocalMapper->setIsDoneVIBA(false);
            mpTracker->mLockPreTeleportTranslation = true;
#ifdef REGISTER_TIMES
            std::chrono::steady_clock::time_point time_StartLoop = std::chrono::steady_clock::now();
            nLoop += 1;
#endif
            CorrectLoop();
#ifdef REGISTER_TIMES
            std::chrono::steady_clock::time_point time_EndLoop = std::chrono::steady_clock::now();
            double timeLoopTotal = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(time_EndLoop - time_StartLoop).count();
            vdLoopTotal_ms.push_back(timeLoopTotal);
#endif
            mnNumCorrection += 1;
            mpTracker->mTeleported = true;
            std::cout << "Loop Closed Successfully" << std::endl;
          }

          // Reset all variables
          mpLoopLastCurrentKF->SetErase();
          mpLoopMatchedKF->SetErase();
          mnLoopNumCoincidences = 0;
          mvpLoopMatchedMPs.clear();
          mvpLoopMPs.clear();
          mnLoopNumNotFound = 0;
          mbLoopDetected = false;
        }
      }
      mpLastCurrentKF = mpCurrentKF;
    }

    ResetIfRequested();

    if (CheckFinish()) {
      break;
    }

    usleep(5000);
  }

  SetFinish();
}

void LoopClosing::InsertKeyFrame(KeyFrame* pKF) {
  std::unique_lock<std::mutex> lock(mMutexLoopQueue);
  if (pKF->mnId != 0) mlpLoopKeyFrameQueue.push_back(pKF);
}

bool LoopClosing::CheckNewKeyFrames() {
  std::unique_lock<std::mutex> lock(mMutexLoopQueue);
  return (!mlpLoopKeyFrameQueue.empty());
}

bool LoopClosing::NewDetectCommonRegions() {
  // To deactivate placerecognition. No loopclosing nor merging will be performed
  if (!mbActiveLC) return false;

  {
    std::unique_lock<std::mutex> lock(mMutexLoopQueue);
    mpCurrentKF = mlpLoopKeyFrameQueue.front();
    mlpLoopKeyFrameQueue.pop_front();
    // Avoid that a keyframe can be erased while it is being process by this
    // thread
    mpCurrentKF->SetNotErase();

    mpLastMap = mpCurrentKF->GetMap();
  }

  if (mbInertial && !mpLastMap->GetIniertialBA2()) {
    mpKeyFrameDB->add(mpCurrentKF);
    mpCurrentKF->SetErase();
    return false;
  }

  if (mpTracker->mSensor == CameraType::STEREO && mpLastMap->GetAllKeyFrames().size() < 5) { // 12
    // std::cout << "LoopClousure: Stereo KF inserted without check: " << mpCurrentKF->mnId << std::endl;
    mpKeyFrameDB->add(mpCurrentKF);
    mpCurrentKF->SetErase();
    return false;
  }

  if (mpLastMap->GetAllKeyFrames().size() < 12) {
    // std::cout << "LoopClousure: Stereo KF inserted without check, map is small: " << mpCurrentKF->mnId << std::endl;
    mpKeyFrameDB->add(mpCurrentKF);
    mpCurrentKF->SetErase();
    return false;
  }

  // std::cout << "LoopClousure: Checking KF: " << mpCurrentKF->mnId << std::endl;

  // Check the last candidates with geometric validation
  // Loop candidates
  bool bLoopDetectedInKF = false;
  // bool bCheckSpatial = false; // UNUSED

#ifdef REGISTER_TIMES
  std::chrono::steady_clock::time_point time_StartEstSim3_1 = std::chrono::steady_clock::now();
#endif
  if (mnLoopNumCoincidences > 0) {
    // bCheckSpatial = true; // UNUSED
    // Find from the last KF candidates
    Sophus::SE3d mTcl = (mpCurrentKF->GetPose() * mpLoopLastCurrentKF->GetPoseInverse()).cast<double>();
    g2o::Sim3 gScl(mTcl.unit_quaternion(), mTcl.translation(), 1.0);
    g2o::Sim3 gScw = gScl * mg2oLoopSlw;
    int numProjMatches = 0;
    std::vector<MapPoint*> vpMatchedMPs;
    bool bCommonRegion = DetectAndReffineSim3FromLastKF(mpCurrentKF, mpLoopMatchedKF, gScw, numProjMatches, mvpLoopMPs, vpMatchedMPs);
    if (bCommonRegion) {
      bLoopDetectedInKF = true;

      mnLoopNumCoincidences++;
      mpLoopLastCurrentKF->SetErase();
      mpLoopLastCurrentKF = mpCurrentKF;
      mg2oLoopSlw = gScw;
      mvpLoopMatchedMPs = vpMatchedMPs;

      mbLoopDetected = mnLoopNumCoincidences >= 3;
      mnLoopNumNotFound = 0;

      if (!mbLoopDetected) {
        std::cout << "PR: Loop detected with Reffine Sim3" << std::endl;
      }
    } else {
      bLoopDetectedInKF = false;

      mnLoopNumNotFound++;
      if (mnLoopNumNotFound >= 2) {
        mpLoopLastCurrentKF->SetErase();
        mpLoopMatchedKF->SetErase();
        mnLoopNumCoincidences = 0;
        mvpLoopMatchedMPs.clear();
        mvpLoopMPs.clear();
        mnLoopNumNotFound = 0;
      }
    }
  }

  // Merge candidates
  bool bMergeDetectedInKF = false;
  if (mnMergeNumCoincidences > 0) {
    // Find from the last KF candidates
    Sophus::SE3d mTcl = (mpCurrentKF->GetPose() * mpMergeLastCurrentKF->GetPoseInverse()).cast<double>();

    g2o::Sim3 gScl(mTcl.unit_quaternion(), mTcl.translation(), 1.0);
    g2o::Sim3 gScw = gScl * mg2oMergeSlw;
    int numProjMatches = 0;
    std::vector<MapPoint*> vpMatchedMPs;
    bool bCommonRegion = DetectAndReffineSim3FromLastKF(mpCurrentKF, mpMergeMatchedKF, gScw, numProjMatches, mvpMergeMPs, vpMatchedMPs);
    if (bCommonRegion) {
      bMergeDetectedInKF = true;

      mnMergeNumCoincidences++;
      mpMergeLastCurrentKF->SetErase();
      mpMergeLastCurrentKF = mpCurrentKF;
      mg2oMergeSlw = gScw;
      mvpMergeMatchedMPs = vpMatchedMPs;

      mbMergeDetected = mnMergeNumCoincidences >= 3;
    } else {
      mbMergeDetected = false;
      bMergeDetectedInKF = false;

      mnMergeNumNotFound++;
      if (mnMergeNumNotFound >= 2) {
        mpMergeLastCurrentKF->SetErase();
        mpMergeMatchedKF->SetErase();
        mnMergeNumCoincidences = 0;
        mvpMergeMatchedMPs.clear();
        mvpMergeMPs.clear();
        mvpMergeConnectedKFs.clear();
        mnMergeNumNotFound = 0;
      }
    }
  }
#ifdef REGISTER_TIMES
  std::chrono::steady_clock::time_point time_EndEstSim3_1 = std::chrono::steady_clock::now();
  double timeEstSim3 = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(time_EndEstSim3_1 - time_StartEstSim3_1).count();
#endif

  if (mbMergeDetected || mbLoopDetected) {
#ifdef REGISTER_TIMES
    vdEstSim3_ms.push_back(timeEstSim3);
#endif
    mpKeyFrameDB->add(mpCurrentKF);
    return true;
  }

  // TODO: This is only necessary if we use a minimun score for pick the best
  // candidates
  // UNUSED
  // const std::vector<KeyFrame*> vpConnectedKeyFrames = mpCurrentKF->GetVectorCovisibleKeyFrames();

  // Extract candidates from the bag of words
  std::vector<KeyFrame*> vpMergeBowCand, vpLoopBowCand;
  if (!bMergeDetectedInKF || !bLoopDetectedInKF) {
    // Search in BoW
#ifdef REGISTER_TIMES
    std::chrono::steady_clock::time_point time_StartQuery = std::chrono::steady_clock::now();
#endif
    mpKeyFrameDB->DetectNBestCandidates(mpCurrentKF, vpLoopBowCand, vpMergeBowCand, 3);
#ifdef REGISTER_TIMES
    std::chrono::steady_clock::time_point time_EndQuery = std::chrono::steady_clock::now();
    double timeDataQuery = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(time_EndQuery - time_StartQuery).count();
    vdDataQuery_ms.push_back(timeDataQuery);
#endif
  }

#ifdef REGISTER_TIMES
  std::chrono::steady_clock::time_point time_StartEstSim3_2 = std::chrono::steady_clock::now();
#endif
  // Check the BoW candidates if the geometric candidate list is empty
  // Loop candidates
  if (!bLoopDetectedInKF && !vpLoopBowCand.empty()) {
    mbLoopDetected = DetectCommonRegionsFromBoW(vpLoopBowCand, mpLoopMatchedKF, mpLoopLastCurrentKF, mg2oLoopSlw, mnLoopNumCoincidences, mvpLoopMPs, mvpLoopMatchedMPs);
  }
  // Merge candidates
  if (!bMergeDetectedInKF && !vpMergeBowCand.empty()) {
    mbMergeDetected = DetectCommonRegionsFromBoW(vpMergeBowCand, mpMergeMatchedKF, mpMergeLastCurrentKF, mg2oMergeSlw, mnMergeNumCoincidences, mvpMergeMPs, mvpMergeMatchedMPs);
  }

#ifdef REGISTER_TIMES
  std::chrono::steady_clock::time_point time_EndEstSim3_2 = std::chrono::steady_clock::now();
  timeEstSim3 += std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(time_EndEstSim3_2 - time_StartEstSim3_2).count();
  vdEstSim3_ms.push_back(timeEstSim3);
#endif

  mpKeyFrameDB->add(mpCurrentKF);

  if (mbMergeDetected || mbLoopDetected) {
    return true;
  }

  mpCurrentKF->SetErase();

  return false;
}

bool LoopClosing::DetectAndReffineSim3FromLastKF(KeyFrame* pCurrentKF, KeyFrame* pMatchedKF, g2o::Sim3& gScw, int& nNumProjMatches,
                                                std::vector<MapPoint*>& vpMPs, std::vector<MapPoint*>& vpMatchedMPs) {
  std::set<MapPoint*> spAlreadyMatchedMPs;
  nNumProjMatches = FindMatchesByProjection(pCurrentKF, pMatchedKF, gScw, spAlreadyMatchedMPs, vpMPs, vpMatchedMPs);

  int nProjMatches = 30;
  int nProjOptMatches = 50;
  int nProjMatchesRep = 100;

  if (nNumProjMatches >= nProjMatches) {
    // Verbose::PrintMess("Sim3 reffine: There are " + to_string(nNumProjMatches) + " initial matches ", Verbose::VERBOSITY_DEBUG);
    Sophus::SE3d mTwm = pMatchedKF->GetPoseInverse().cast<double>();
    g2o::Sim3 gSwm(mTwm.unit_quaternion(), mTwm.translation(), 1.0);
    g2o::Sim3 gScm = gScw * gSwm;
    Eigen::Matrix<double, 7, 7> mHessian7x7;

    bool bFixedScale = mbFixScale;  // TODO CHECK; Solo para el monocular inertial
    if (mpTracker->mSensor == CameraType::IMU_MONOCULAR && !pCurrentKF->GetMap()->GetIniertialBA2())
      bFixedScale = false;
    int numOptMatches = Optimizer::OptimizeSim3(mpCurrentKF, pMatchedKF, vpMatchedMPs, gScm, 10, bFixedScale, mHessian7x7, true);

    // Verbose::PrintMess("Sim3 reffine: There are " + to_string(numOptMatches) + " matches after of the optimization ", Verbose::VERBOSITY_DEBUG);

    if (numOptMatches > nProjOptMatches) {
      g2o::Sim3 gScw_estimation(gScw.rotation(), gScw.translation(), 1.0);

      std::vector<MapPoint*> vpMatchedMP;
      vpMatchedMP.resize(mpCurrentKF->GetMapPointMatches().size(), nullptr);

      nNumProjMatches = FindMatchesByProjection(pCurrentKF, pMatchedKF, gScw_estimation, spAlreadyMatchedMPs, vpMPs, vpMatchedMPs);
      if (nNumProjMatches >= nProjMatchesRep) {
        gScw = gScw_estimation;
        return true;
      }
    }
  }
  return false;
}

bool LoopClosing::DetectCommonRegionsFromBoW(std::vector<KeyFrame*>& vpBowCand, KeyFrame*& pMatchedKF2, KeyFrame*& pLastCurrentKF, g2o::Sim3& g2oScw,
                                            int& nNumCoincidences, std::vector<MapPoint*>& vpMPs, std::vector<MapPoint*>& vpMatchedMPs) {
  int nBoWMatches = 20;  // lower this and try again
  int nBoWInliers = 15;
  int nSim3Inliers = 20;
  int nProjMatches = 50;
  int nProjOptMatches = 100;//80;

  std::set<KeyFrame*> spConnectedKeyFrames = mpCurrentKF->GetConnectedKeyFrames();

  int nNumCovisibles = 10;

  ORBmatcher matcherBoW(0.8, true);
  ORBmatcher matcher(0.75, true);

  // Varibles to select the best numbe
  KeyFrame* pBestMatchedKF;
  int nBestMatchesReproj = 0;
  int nBestNumCoindicendes = 0;
  g2o::Sim3 g2oBestScw;
  std::vector<MapPoint*> vpBestMapPoints;
  std::vector<MapPoint*> vpBestMatchedMapPoints;

  int numCandidates = vpBowCand.size();
  std::vector<int> vnStage(numCandidates, 0);
  std::vector<int> vnMatchesStage(numCandidates, 0);

  int index = 0;
  // Verbose::PrintMess("BoW candidates: There are " + to_string(vpBowCand.size()) + " possible candidates ", Verbose::VERBOSITY_DEBUG);
  for (KeyFrame* pKFi : vpBowCand) {
    if (!pKFi || pKFi->isBad()) continue;

    // std::cout << "KF candidate: " << pKFi->mnId << std::endl;
    // Current KF against KF with covisibles version
    std::vector<KeyFrame*> vpCovKFi = pKFi->GetBestCovisibilityKeyFrames(nNumCovisibles);
    if (vpCovKFi.empty()) {
      vpCovKFi.push_back(pKFi);
    } else {
      vpCovKFi.push_back(vpCovKFi[0]);
      vpCovKFi[0] = pKFi;
    }

    bool bAbortByNearKF = false;
    for (size_t j = 0; j < vpCovKFi.size(); ++j) {
      if (spConnectedKeyFrames.find(vpCovKFi[j]) != spConnectedKeyFrames.end()) {
        bAbortByNearKF = true;
        break;
      }
    }
    if (bAbortByNearKF) {
      // std::cout << "Check BoW aborted because is close to the matched one " << std::endl;
      continue;
    }
    // std::cout << "Check BoW continue because is far to the matched one " << std::endl;

    std::vector<std::vector<MapPoint*>> vvpMatchedMPs;
    vvpMatchedMPs.resize(vpCovKFi.size());
    std::set<MapPoint*> spMatchedMPi;
    int numBoWMatches = 0;

    KeyFrame* pMostBoWMatchesKF = pKFi;
    // int nMostBoWNumMatches = 0; // UNUSED

    std::vector<MapPoint*> vpMatchedPoints = std::vector<MapPoint*>(mpCurrentKF->GetMapPointMatches().size(), nullptr);
    std::vector<KeyFrame*> vpKeyFrameMatchedMP = std::vector<KeyFrame*>(mpCurrentKF->GetMapPointMatches().size(), nullptr);

    // int nIndexMostBoWMatchesKF=0; // UNUSED
    for (size_t j = 0; j < vpCovKFi.size(); ++j) {
      if (!vpCovKFi[j] || vpCovKFi[j]->isBad()) continue;

      //int num =  // UNUSED
      matcherBoW.SearchByBoW(mpCurrentKF, vpCovKFi[j], vvpMatchedMPs[j]); 

      // UNUSED
      // if (num > nMostBoWNumMatches) {
      //   nMostBoWNumMatches = num;
      //   nIndexMostBoWMatchesKF = j;
      // }
    }

    for (size_t j = 0; j < vpCovKFi.size(); ++j) {
      for (size_t k = 0; k < vvpMatchedMPs[j].size(); ++k) {
        MapPoint* pMPi_j = vvpMatchedMPs[j][k];
        if (!pMPi_j || pMPi_j->isBad()) continue;

        if (spMatchedMPi.find(pMPi_j) == spMatchedMPi.end()) {
          spMatchedMPi.insert(pMPi_j);
          numBoWMatches++;

          vpMatchedPoints[k] = pMPi_j;
          vpKeyFrameMatchedMP[k] = vpCovKFi[j];
        }
      }
    }

    // pMostBoWMatchesKF = vpCovKFi[pMostBoWMatchesKF];

    if (numBoWMatches >= nBoWMatches) { // TODO pick a good threshold
      // Geometric validation

      // Scale is not fixed if the cam is IMU_MONO and the IMU's unititialized
      bool bFixedScale = mbFixScale && !(mpTracker->mSensor == CameraType::IMU_MONOCULAR && !mpCurrentKF->GetMap()->GetIniertialBA2());

      Sim3Solver solver = Sim3Solver(mpCurrentKF, pMostBoWMatchesKF, vpMatchedPoints, bFixedScale, vpKeyFrameMatchedMP);
      solver.SetRansacParameters(0.99, nBoWInliers, 300);  // at least 15 inliers

      bool bNoMore = false;
      std::vector<bool> vbInliers;
      int nInliers;
      bool bConverge = false;
      Eigen::Matrix4f mTcm;
      while (!bConverge && !bNoMore) {
        mTcm = solver.iterate(20, bNoMore, vbInliers, nInliers, bConverge);
        // Verbose::PrintMess("BoW guess: Solver achieve " + to_string(nInliers) + " geometrical inliers among " + to_string(nBoWInliers) + " BoW matches", Verbose::VERBOSITY_DEBUG);
      }

      if (bConverge) {
        // std::cout << "Check BoW: SolverSim3 converged" << std::endl;

        // Verbose::PrintMess("BoW guess: Convergende with " + to_string(nInliers) + " geometrical inliers among " + to_string(nBoWInliers) + " BoW matches", Verbose::VERBOSITY_DEBUG);
        // Match by reprojection
        vpCovKFi.clear();
        vpCovKFi = pMostBoWMatchesKF->GetBestCovisibilityKeyFrames(nNumCovisibles);
        vpCovKFi.push_back(pMostBoWMatchesKF);
        std::set<KeyFrame*> spCheckKFs(vpCovKFi.begin(), vpCovKFi.end());

        // std::cout << "There are " << vpCovKFi.size() <<" near KFs" << std::endl;

        std::set<MapPoint*> spMapPoints;
        std::vector<MapPoint*> vpMapPoints;
        std::vector<KeyFrame*> vpKeyFrames;
        for (KeyFrame* pCovKFi : vpCovKFi) {
          for (MapPoint* pCovMPij : pCovKFi->GetMapPointMatches()) {
            if (!pCovMPij || pCovMPij->isBad()) continue;

            if (spMapPoints.find(pCovMPij) == spMapPoints.end()) {
              spMapPoints.insert(pCovMPij);
              vpMapPoints.push_back(pCovMPij);
              vpKeyFrames.push_back(pCovKFi);
            }
          }
        }

        // std::cout << "There are " << vpKeyFrames.size() <<" KFs which view
        // all the mappoints" << std::endl;

        g2o::Sim3 gScm(solver.GetEstimatedRotation().cast<double>(), solver.GetEstimatedTranslation().cast<double>(), (double)solver.GetEstimatedScale());
        g2o::Sim3 gSmw(pMostBoWMatchesKF->GetRotation().cast<double>(), pMostBoWMatchesKF->GetTranslation().cast<double>(), 1.0);
        g2o::Sim3 gScw = gScm * gSmw;  // Similarity matrix of current from the world position
        Sophus::Sim3f mScw = Converter::toSophus(gScw);

        std::vector<MapPoint*> vpMatchedMP;
        vpMatchedMP.resize(mpCurrentKF->GetMapPointMatches().size(), nullptr);
        std::vector<KeyFrame*> vpMatchedKF;
        vpMatchedKF.resize(mpCurrentKF->GetMapPointMatches().size(), nullptr);
        int numProjMatches = matcher.SearchByProjection(mpCurrentKF, mScw, vpMapPoints, vpKeyFrames, vpMatchedMP, vpMatchedKF, 8, 1.5);
        // std::cout <<"BoW: " << numProjMatches << " matches between " << vpMapPoints.size() << " points with coarse Sim3" << std::endl;

        if (numProjMatches >= nProjMatches) {
          // Optimize Sim3 transformation with every matches
          Eigen::Matrix<double, 7, 7> mHessian7x7;

          /* The bool below ('bFixedScale') is UNUSED and since all function calls below do not change state. this is not needed.
          bool bFixedScale = mbFixScale; 
          if(mpTracker->mSensor==CameraType::IMU_MONOCULAR && !mpCurrentKF->GetMap()->GetIniertialBA2()) bFixedScale=false;
          */

          int numOptMatches = Optimizer::OptimizeSim3(mpCurrentKF, pKFi, vpMatchedMP, gScm, 10, mbFixScale, mHessian7x7, true);

          if (numOptMatches >= nSim3Inliers) {
            g2o::Sim3 gSmw(pMostBoWMatchesKF->GetRotation().cast<double>(), pMostBoWMatchesKF->GetTranslation().cast<double>(), 1.0);
            g2o::Sim3 gScw = gScm * gSmw;  // Similarity matrix of current from the world position
            Sophus::Sim3f mScw = Converter::toSophus(gScw);

            std::vector<MapPoint*> vpMatchedMP;
            vpMatchedMP.resize(mpCurrentKF->GetMapPointMatches().size(), nullptr);
            int numProjOptMatches = matcher.SearchByProjection(mpCurrentKF, mScw, vpMapPoints, vpMatchedMP, 5, 1.0);

            if (numProjOptMatches >= nProjOptMatches) {
              int max_x = -1, min_x = 1000000;
              int max_y = -1, min_y = 1000000;
              for (MapPoint* pMPi : vpMatchedMP) {
                if (!pMPi || pMPi->isBad()) {
                  continue;
                }

                std::tuple<size_t, size_t> indexes = pMPi->GetIndexInKeyFrame(pKFi);
                int index = std::get<0>(indexes);
                if (index >= 0) {
                  int coord_x = pKFi->mvKeysUn[index].pt.x;
                  if (coord_x < min_x) {
                    min_x = coord_x;
                  }
                  if (coord_x > max_x) {
                    max_x = coord_x;
                  }
                  int coord_y = pKFi->mvKeysUn[index].pt.y;
                  if (coord_y < min_y) {
                    min_y = coord_y;
                  }
                  if (coord_y > max_y) {
                    max_y = coord_y;
                  }
                }
              }

              int nNumKFs = 0;
              // vpMatchedMPs = vpMatchedMP;
              // vpMPs = vpMapPoints;
              // Check the Sim3 transformation with the current KeyFrame covisibles
              std::vector<KeyFrame*> vpCurrentCovKFs = mpCurrentKF->GetBestCovisibilityKeyFrames(nNumCovisibles);

              for (size_t j = 0; nNumKFs < 3 && j < vpCurrentCovKFs.size(); ++j) {
                KeyFrame* pKFj = vpCurrentCovKFs[j];
                Sophus::SE3d mTjc = (pKFj->GetPose() * mpCurrentKF->GetPoseInverse()).cast<double>();
                g2o::Sim3 gSjc(mTjc.unit_quaternion(), mTjc.translation(), 1.0);
                g2o::Sim3 gSjw = gSjc * gScw;
                int numProjMatches_j = 0;
                std::vector<MapPoint*> vpMatchedMPs_j;
                bool bValid = DetectCommonRegionsFromLastKF(pKFj, pMostBoWMatchesKF, gSjw, numProjMatches_j, vpMapPoints, vpMatchedMPs_j);

                if (bValid) {
                  // Sophus::SE3f Tc_w = mpCurrentKF->GetPose(); // UNUSED
                  // Sophus::SE3f Tw_cj = pKFj->GetPoseInverse(); // UNUSED
                  // Sophus::SE3f Tc_cj = Tc_w * Tw_cj; // UNUSED
                  // Eigen::Vector3f vector_dist = Tc_cj.translation(); // UNUSED
                  nNumKFs++;
                }
              }

              if (nNumKFs < 3) {
                vnStage[index] = 8;
                vnMatchesStage[index] = nNumKFs;
              }

              if (nBestMatchesReproj < numProjOptMatches) {
                nBestMatchesReproj = numProjOptMatches;
                nBestNumCoindicendes = nNumKFs;
                pBestMatchedKF = pMostBoWMatchesKF;
                g2oBestScw = gScw;
                vpBestMapPoints = vpMapPoints;
                vpBestMatchedMapPoints = vpMatchedMP;
              }
            }
          }
        }
      }
    }
    index++;
  }

  if (nBestMatchesReproj > 0) {
    pLastCurrentKF = mpCurrentKF;
    nNumCoincidences = nBestNumCoindicendes;
    pMatchedKF2 = pBestMatchedKF;
    pMatchedKF2->SetNotErase();
    g2oScw = g2oBestScw;
    vpMPs = vpBestMapPoints;
    vpMatchedMPs = vpBestMatchedMapPoints;
    if(nNumCoincidences >= 3){
      std::cout << "Number of matches: " << nBestMatchesReproj << std::endl;
    }

    return nNumCoincidences >= 3;
  /* Everything down here does not change any state and is UNUSED
  } else {
    int maxStage = -1;
    int maxMatched;
    for (size_t i = 0; i < vnStage.size(); ++i) {
      if (vnStage[i] > maxStage) {
        maxStage = vnStage[i];
        maxMatched = vnMatchesStage[i];
      }
    }
  }
  */
  }
  return false;
}

bool LoopClosing::DetectCommonRegionsFromLastKF(KeyFrame* pCurrentKF, KeyFrame* pMatchedKF, g2o::Sim3& gScw, int& nNumProjMatches, std::vector<MapPoint*>& vpMPs, std::vector<MapPoint*>& vpMatchedMPs) {
  std::set<MapPoint*> spAlreadyMatchedMPs(vpMatchedMPs.begin(), vpMatchedMPs.end());
  nNumProjMatches = FindMatchesByProjection(pCurrentKF, pMatchedKF, gScw, spAlreadyMatchedMPs, vpMPs, vpMatchedMPs);

  return (nNumProjMatches >= 30);
}

int LoopClosing::FindMatchesByProjection(KeyFrame* pCurrentKF, KeyFrame* pMatchedKFw, g2o::Sim3& g2oScw,
    std::set<MapPoint*>& spMatchedMPinOrigin, std::vector<MapPoint*>& vpMapPoints, std::vector<MapPoint*>& vpMatchedMapPoints) {
  int nNumCovisibles = 10;
  std::vector<KeyFrame*> vpCovKFm = pMatchedKFw->GetBestCovisibilityKeyFrames(nNumCovisibles);
  int nInitialCov = vpCovKFm.size();
  vpCovKFm.push_back(pMatchedKFw);
  std::set<KeyFrame*> spCheckKFs(vpCovKFm.begin(), vpCovKFm.end());
  std::set<KeyFrame*> spCurrentCovisbles = pCurrentKF->GetConnectedKeyFrames();
  if (nInitialCov < nNumCovisibles) {
    for (int i = 0; i < nInitialCov; ++i) {
      std::vector<KeyFrame*> vpKFs = vpCovKFm[i]->GetBestCovisibilityKeyFrames(nNumCovisibles);
      int nInserted = 0;
      for (size_t j = 0; j < vpKFs.size() && nInserted < nNumCovisibles; ++j) {
        if (spCheckKFs.find(vpKFs[j]) == spCheckKFs.end() && spCurrentCovisbles.find(vpKFs[j]) == spCurrentCovisbles.end()) {
          spCheckKFs.insert(vpKFs[j]);
          ++nInserted;
        }
      }
      vpCovKFm.insert(vpCovKFm.end(), vpKFs.begin(), vpKFs.end());
    }
  }
  std::set<MapPoint*> spMapPoints;
  vpMapPoints.clear();
  vpMatchedMapPoints.clear();
  for (KeyFrame* pKFi : vpCovKFm) {
    for (MapPoint* pMPij : pKFi->GetMapPointMatches()) {
      if (!pMPij || pMPij->isBad()) continue;

      if (spMapPoints.find(pMPij) == spMapPoints.end()) {
        spMapPoints.insert(pMPij);
        vpMapPoints.push_back(pMPij);
      }
    }
  }

  Sophus::Sim3f mScw = Converter::toSophus(g2oScw);
  ORBmatcher matcher(0.9, true);

  vpMatchedMapPoints.resize(pCurrentKF->GetMapPointMatches().size(), nullptr);
  int num_matches = matcher.SearchByProjection(pCurrentKF, mScw, vpMapPoints, vpMatchedMapPoints, 3, 1.5);

  return num_matches;
}

void LoopClosing::CorrectLoop() {
  // std::cout << "Loop detected!" << std::endl;

  // Send a stop signal to Local Mapping
  // Avoid new keyframes are inserted while correcting the loop
  mpLocalMapper->RequestStop();
  mpLocalMapper->EmptyQueue();  // Proccess keyframes in the queue

  // If a Global Bundle Adjustment is running, abort it
  if (isRunningGBA()) {
    std::cout << "Stoping Global Bundle Adjustment...";
    std::unique_lock<std::mutex> lock(mMutexGBA);
    mbStopGBA = true;

    mnFullBAIdx++;

    // if (mpThreadGBA) {
    //   mpThreadGBA->detach();
    //   delete mpThreadGBA;
    // }
    std::cout << "  Done!!" << std::endl;
  }

  // Wait until Local Mapping has effectively stopped
  while (!mpLocalMapper->isStopped()) {
    usleep(1000);
  }

  // Ensure current keyframe is updated
  // std::cout << "Start updating connections" << std::endl;
  // assert(mpCurrentKF->GetMap()->CheckEssentialGraph());
  mpCurrentKF->UpdateConnections();
  // assert(mpCurrentKF->GetMap()->CheckEssentialGraph());

  // Retrive keyframes connected to the current keyframe and compute corrected
  // Sim3 pose by propagation
  mvpCurrentConnectedKFs = mpCurrentKF->GetVectorCovisibleKeyFrames();
  mvpCurrentConnectedKFs.push_back(mpCurrentKF);

  // std::cout << "Loop: number of connected KFs -> " + to_string(mvpCurrentConnectedKFs.size()) << std::endl;

  KeyFrameAndPose CorrectedSim3, NonCorrectedSim3;
  CorrectedSim3[mpCurrentKF] = mg2oLoopScw;
  Sophus::SE3f Twc = mpCurrentKF->GetPoseInverse();
  Sophus::SE3f Tcw = mpCurrentKF->GetPose();
  g2o::Sim3 g2oScw(Tcw.unit_quaternion().cast<double>(), Tcw.translation().cast<double>(), 1.0);
  NonCorrectedSim3[mpCurrentKF] = g2oScw;

  // Update keyframe pose with corrected Sim3. First transform Sim3 to SE3
  // (scale translation)
  Sophus::SE3d correctedTcw(mg2oLoopScw.rotation(), mg2oLoopScw.translation() / mg2oLoopScw.scale());
  mpCurrentKF->SetPose(correctedTcw.cast<float>());

  std::shared_ptr<Map> pLoopMap = mpCurrentKF->GetMap();

#ifdef REGISTER_TIMES
  /*KeyFrame* pKF = mpCurrentKF;
  int numKFinLoop = 0;
  while(pKF && pKF->mnId > mpLoopMatchedKF->mnId) {
      pKF = pKF->GetParent();
      numKFinLoop += 1;
  }
  vnLoopKFs.push_back(numKFinLoop);*/
  std::chrono::steady_clock::time_point time_StartFusion = std::chrono::steady_clock::now();
#endif

  {
    // Get Map Mutex
    std::unique_lock<std::mutex> lock(pLoopMap->mMutexMapUpdate);

    const bool bImuInit = pLoopMap->isImuInitialized();

    for (std::vector<KeyFrame*>::iterator vit = mvpCurrentConnectedKFs.begin(), vend = mvpCurrentConnectedKFs.end(); vit != vend; vit++) {
      KeyFrame* pKFi = *vit;

      if (pKFi != mpCurrentKF) {
        Sophus::SE3f Tiw = pKFi->GetPose();
        Sophus::SE3d Tic = (Tiw * Twc).cast<double>();
        g2o::Sim3 g2oSic(Tic.unit_quaternion(), Tic.translation(), 1.0);
        g2o::Sim3 g2oCorrectedSiw = g2oSic * mg2oLoopScw;
        // Pose corrected with the Sim3 of the loop closure
        CorrectedSim3[pKFi] = g2oCorrectedSiw;

        // Update keyframe pose with corrected Sim3. First transform Sim3 to SE3 (scale translation)
        Sophus::SE3d correctedTiw(g2oCorrectedSiw.rotation(), g2oCorrectedSiw.translation() / g2oCorrectedSiw.scale());
        pKFi->SetPose(correctedTiw.cast<float>());

        // Pose without correction
        g2o::Sim3 g2oSiw(Tiw.unit_quaternion().cast<double>(), Tiw.translation().cast<double>(), 1.0);
        NonCorrectedSim3[pKFi] = g2oSiw;
      }
    }

    // Correct all MapPoints obsrved by current keyframe and neighbors, so that they align with the other side of the loop
    for (KeyFrameAndPose::iterator mit = CorrectedSim3.begin(), mend = CorrectedSim3.end(); mit != mend; mit++) {
      KeyFrame* pKFi = mit->first;
      g2o::Sim3 g2oCorrectedSiw = mit->second;
      g2o::Sim3 g2oCorrectedSwi = g2oCorrectedSiw.inverse();

      g2o::Sim3 g2oSiw = NonCorrectedSim3[pKFi];

      // Update keyframe pose with corrected Sim3. First transform Sim3 to SE3 (scale translation)
      /*Sophus::SE3d correctedTiw(g2oCorrectedSiw.rotation(),g2oCorrectedSiw.translation() / g2oCorrectedSiw.scale());
      pKFi->SetPose(correctedTiw.cast<float>());*/

      std::vector<MapPoint*> vpMPsi = pKFi->GetMapPointMatches();
      for (size_t iMP = 0, endMPi = vpMPsi.size(); iMP < endMPi; iMP++) {
        MapPoint* pMPi = vpMPsi[iMP];
        if (!pMPi) continue;
        if (pMPi->isBad()) continue;
        if (pMPi->mnCorrectedByKF == mpCurrentKF->mnId) continue;

        // Project with non-corrected pose and project back with corrected pose
        Eigen::Vector3d P3Dw = pMPi->GetWorldPos().cast<double>();
        Eigen::Vector3d eigCorrectedP3Dw = g2oCorrectedSwi.map(g2oSiw.map(P3Dw));

        pMPi->SetWorldPos(eigCorrectedP3Dw.cast<float>());
        pMPi->mnCorrectedByKF = mpCurrentKF->mnId;
        pMPi->mnCorrectedReference = pKFi->mnId;
        pMPi->UpdateNormalAndDepth();
      }

      // Correct velocity according to orientation correction
      if (bImuInit) {
        Eigen::Quaternionf Rcor = (g2oCorrectedSiw.rotation().inverse() * g2oSiw.rotation()).cast<float>();
        pKFi->SetVelocity(Rcor * pKFi->GetVelocity());
      }

      // Make sure connections are updated
      pKFi->UpdateConnections();
    }
    // TODO Check this index increasement
    mpAtlas->GetCurrentMap()->IncreaseChangeIndex();

    // Start Loop Fusion
    // Update matched map points and replace if duplicated
    for (size_t i = 0; i < mvpLoopMatchedMPs.size(); i++) {
      if (mvpLoopMatchedMPs[i]) {
        MapPoint* pLoopMP = mvpLoopMatchedMPs[i];
        MapPoint* pCurMP = mpCurrentKF->GetMapPoint(i);
        if (pCurMP)
          pCurMP->Replace(pLoopMP);
        else {
          mpCurrentKF->AddMapPoint(pLoopMP, i);
          pLoopMP->AddObservation(mpCurrentKF, i);
          pLoopMP->ComputeDistinctiveDescriptors();
        }
      }
    }
    // std::cout << "LC: end replacing duplicated" << std::endl;
  }

  // Project MapPoints observed in the neighborhood of the loop keyframe into the current keyframe and neighbors using corrected poses.
  // Fuse duplications.
  SearchAndFuse(CorrectedSim3, mvpLoopMapPoints);

  // After the MapPoint fusion, new links in the covisibility graph will appear attaching both sides of the loop
  std::map<KeyFrame*, std::set<KeyFrame*>> LoopConnections;

  for (std::vector<KeyFrame*>::iterator vit = mvpCurrentConnectedKFs.begin(), vend = mvpCurrentConnectedKFs.end(); vit != vend; vit++) {
    KeyFrame* pKFi = *vit;
    std::vector<KeyFrame*> vpPreviousNeighbors = pKFi->GetVectorCovisibleKeyFrames();

    // Update connections. Detect new links.
    pKFi->UpdateConnections();
    LoopConnections[pKFi] = pKFi->GetConnectedKeyFrames();
    for (std::vector<KeyFrame*>::iterator vit_prev = vpPreviousNeighbors.begin(), vend_prev = vpPreviousNeighbors.end(); vit_prev != vend_prev; vit_prev++) {
      LoopConnections[pKFi].erase(*vit_prev);
    }
    for (std::vector<KeyFrame*>::iterator vit2 = mvpCurrentConnectedKFs.begin(), vend2 = mvpCurrentConnectedKFs.end(); vit2 != vend2; vit2++) {
      LoopConnections[pKFi].erase(*vit2);
    }
  }

  // Optimize graph
  bool bFixedScale = mbFixScale && !(mpTracker->mSensor == CameraType::IMU_MONOCULAR && !mpCurrentKF->GetMap()->GetIniertialBA2());
  // TODO CHECK; Solo para el monocular inertial


#ifdef REGISTER_TIMES
  std::chrono::steady_clock::time_point time_EndFusion = std::chrono::steady_clock::now();
  double timeFusion = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(time_EndFusion - time_StartFusion).count();
  vdLoopFusion_ms.push_back(timeFusion);
#endif
  // std::cout << "Optimize essential graph" << std::endl;
  if (mbInertial && pLoopMap->isImuInitialized()) {
    Optimizer::OptimizeEssentialGraph4DoF(pLoopMap, mpLoopMatchedKF, mpCurrentKF, NonCorrectedSim3, CorrectedSim3, LoopConnections);
  } else {
    // std::cout << "Loop -> Scale correction: " << mg2oLoopScw.scale() << std::endl;
    Optimizer::OptimizeEssentialGraph(pLoopMap, mpLoopMatchedKF, mpCurrentKF, NonCorrectedSim3, CorrectedSim3, LoopConnections, bFixedScale);
  }
#ifdef REGISTER_TIMES
  std::chrono::steady_clock::time_point time_EndOpt = std::chrono::steady_clock::now();
  double timeOptEss = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(time_EndOpt - time_EndFusion).count();
  vdLoopOptEss_ms.push_back(timeOptEss);
#endif

  mpAtlas->InformNewBigChange();

  // Add loop edge
  mpLoopMatchedKF->AddLoopEdge(mpCurrentKF);
  mpCurrentKF->AddLoopEdge(mpLoopMatchedKF);

  // Launch a new thread to perform Global Bundle Adjustment (Only if few keyframes, if not it would take too much time)
  if (!pLoopMap->isImuInitialized() || (pLoopMap->KeyFramesInMap() < 200 && mpAtlas->CountMaps() == 1)) {
    mbRunningGBA = true;
    mbFinishedGBA = false;
    mbStopGBA = false;
    mnCorrectionGBA = mnNumCorrection;
    std::cout << "Creating Thread in LoopClosing::CorrectLoop()" << std::endl;
    mpThreadGBA = std::jthread(&LoopClosing::RunGlobalBundleAdjustment, this, pLoopMap, mpCurrentKF->mnId);
  }

  // Loop closed. Release Local Mapping.
  mpLocalMapper->Release();

}

void LoopClosing::MergeLocal() {
  std::cout << "MERGE LOCAL MAP" << std::endl;
  int numTemporalKFs = 25;  // Temporal KFs in the local window if the map is inertial.

  // Relationship to rebuild the essential graph, it is used two times, first in the local window and later in the rest of the map
  KeyFrame* pNewChild;
  KeyFrame* pNewParent;

  std::vector<KeyFrame*> vpLocalCurrentWindowKFs;
  std::vector<KeyFrame*> vpMergeConnectedKFs;

  // Flag that is true only when we stopped a running BA, in this case we need relaunch at the end of the merge
  bool bRelaunchBA = false;

  // Verbose::PrintMess("MERGE-VISUAL: Check Full Bundle Adjustment", Verbose::VERBOSITY_DEBUG);
  // If a Global Bundle Adjustment is running, abort it
  if (isRunningGBA()) {
    std::unique_lock<std::mutex> lock(mMutexGBA);
    mbStopGBA = true;

    mnFullBAIdx++;

    // if (mpThreadGBA) {
    //   mpThreadGBA->detach();
    //   delete mpThreadGBA;
    // }
    bRelaunchBA = true;
  }

  // Verbose::PrintMess("MERGE-VISUAL: Request Stop Local Mapping", Verbose::VERBOSITY_DEBUG);
  // std::cout << "Request Stop Local Mapping" << std::endl;
  mpLocalMapper->RequestStop();
  // Wait until Local Mapping has effectively stopped
  while (!mpLocalMapper->isStopped()) {
    usleep(1000);
  }
  // std::cout << "Local Map stopped" << std::endl;

  mpLocalMapper->EmptyQueue();

  // Merge map will become in the new active map with the local window of KFs and MPs from the current map. Later, the elements of the current map will be transform to the new active map reference, in order to keep real time tracking
  // Translation (I think?): The Merge Map becomes the new active map, and all of the MapPoints, KeyFrames, etc.  from the Current Map will be transormed to fit the Merge Map
  std::shared_ptr<Map> pCurrentMap = mpCurrentKF->GetMap();
  std::shared_ptr<Map> pMergeMap = mpMergeMatchedKF->GetMap();

  // std::cout << "Merge local, Active map: " << pCurrentMap->GetId() << std::endl;
  // std::cout << "Merge local, Non-Active map: " << pMergeMap->GetId() << std::endl;

#ifdef REGISTER_TIMES
  std::chrono::steady_clock::time_point time_StartMerge = std::chrono::steady_clock::now();
#endif

  // Ensure current keyframe is updated
  mpCurrentKF->UpdateConnections();

  // Get the current KF and its neighbors(visual->covisibles;
  // inertial->temporal+covisibles)
  std::set<KeyFrame*> spLocalWindowKFs;
  // Get MPs in the welding area from the current map
  std::set<MapPoint*> spLocalWindowMPs;
  if (mbInertial){  // TODO Check the correct initialization
    
    KeyFrame* pKFi = mpCurrentKF;
    int nInserted = 0;
    while (pKFi && nInserted < numTemporalKFs) {
      spLocalWindowKFs.insert(pKFi);
      pKFi = mpCurrentKF->mPrevKF;
      nInserted++;

      std::set<MapPoint*> spMPi = pKFi->GetMapPoints();
      spLocalWindowMPs.insert(spMPi.begin(), spMPi.end());
    }

    pKFi = mpCurrentKF->mNextKF;
    while (pKFi) {
      spLocalWindowKFs.insert(pKFi);

      std::set<MapPoint*> spMPi = pKFi->GetMapPoints();
      spLocalWindowMPs.insert(spMPi.begin(), spMPi.end());

      pKFi = mpCurrentKF->mNextKF;
    }
  } else {
    spLocalWindowKFs.insert(mpCurrentKF);
  }

  std::vector<KeyFrame*> vpCovisibleKFs = mpCurrentKF->GetBestCovisibilityKeyFrames(numTemporalKFs);
  spLocalWindowKFs.insert(vpCovisibleKFs.begin(), vpCovisibleKFs.end());
  spLocalWindowKFs.insert(mpCurrentKF);
  const int nMaxTries = 5;
  for (size_t nNumTries = 0; static_cast<int>(spLocalWindowKFs.size()) < numTemporalKFs && nNumTries < nMaxTries; ++nNumTries) {
    std::vector<KeyFrame*> vpNewCovKFs;
    for (KeyFrame* pKFi : spLocalWindowKFs) {
      std::vector<KeyFrame*> vpKFiCov = pKFi->GetBestCovisibilityKeyFrames(numTemporalKFs / 2);
      for (KeyFrame* pKFcov : vpKFiCov) {
        if (pKFcov && !pKFcov->isBad() && spLocalWindowKFs.find(pKFcov) == spLocalWindowKFs.end()) {
          vpNewCovKFs.push_back(pKFcov);
        }
      }
    }

    spLocalWindowKFs.insert(vpNewCovKFs.begin(), vpNewCovKFs.end());
  }

  for (KeyFrame* pKFi : spLocalWindowKFs) {
    if (!pKFi || pKFi->isBad()) continue;

    std::set<MapPoint*> spMPs = pKFi->GetMapPoints();
    spLocalWindowMPs.insert(spMPs.begin(), spMPs.end());
  }

  // std::cout << "[Merge]: Ma = " << to_string(pCurrentMap->GetId()) << ";#KFs = " << to_string(spLocalWindowKFs.size()) << "; #MPs = " << to_string(spLocalWindowMPs.size()) << std::endl;

  std::set<KeyFrame*> spMergeConnectedKFs;
  if (mbInertial) {  // TODO Check the correct initialization
    KeyFrame* pKFi = mpMergeMatchedKF;
    int nInserted = 0;
    while (pKFi && nInserted < numTemporalKFs / 2) {
      spMergeConnectedKFs.insert(pKFi);
      pKFi = mpCurrentKF->mPrevKF;
      nInserted++;
    }

    pKFi = mpMergeMatchedKF->mNextKF;
    while (pKFi && nInserted < numTemporalKFs) {
      spMergeConnectedKFs.insert(pKFi);
      pKFi = mpCurrentKF->mNextKF;
    }
  } else {
    spMergeConnectedKFs.insert(mpMergeMatchedKF);
  }
  vpCovisibleKFs = mpMergeMatchedKF->GetBestCovisibilityKeyFrames(numTemporalKFs);
  spMergeConnectedKFs.insert(vpCovisibleKFs.begin(), vpCovisibleKFs.end());
  spMergeConnectedKFs.insert(mpMergeMatchedKF);
  for (size_t nNumTries = 0; static_cast<int>(spMergeConnectedKFs.size()) < numTemporalKFs && nNumTries < nMaxTries; ++nNumTries) {
    std::vector<KeyFrame*> vpNewCovKFs;
    for (KeyFrame* pKFi : spMergeConnectedKFs) {
      std::vector<KeyFrame*> vpKFiCov = pKFi->GetBestCovisibilityKeyFrames(numTemporalKFs / 2);
      for (KeyFrame* pKFcov : vpKFiCov) {
        if (pKFcov && !pKFcov->isBad() && spMergeConnectedKFs.find(pKFcov) == spMergeConnectedKFs.end()) {
          vpNewCovKFs.push_back(pKFcov);
        }
      }
    }

    spMergeConnectedKFs.insert(vpNewCovKFs.begin(), vpNewCovKFs.end());
  }

  std::set<MapPoint*> spMapPointMerge;
  for (KeyFrame* pKFi : spMergeConnectedKFs) {
    std::set<MapPoint*> vpMPs = pKFi->GetMapPoints();
    spMapPointMerge.insert(vpMPs.begin(), vpMPs.end());
  }

  std::vector<MapPoint*> vpCheckFuseMapPoint;
  vpCheckFuseMapPoint.reserve(spMapPointMerge.size());
  std::copy(spMapPointMerge.begin(), spMapPointMerge.end(), std::back_inserter(vpCheckFuseMapPoint));

  // std::cout << "[Merge]: Mm = " << to_string(pMergeMap->GetId()) << "; #KFs = " << to_string(spMergeConnectedKFs.size()) << "; #MPs = " << to_string(spMapPointMerge.size()) << std::endl;

  Sophus::SE3d Twc = mpCurrentKF->GetPoseInverse().cast<double>();
  g2o::Sim3 g2oNonCorrectedSwc(Twc.unit_quaternion(), Twc.translation(), 1.0);
  g2o::Sim3 g2oNonCorrectedScw = g2oNonCorrectedSwc.inverse();
  g2o::Sim3 g2oCorrectedScw = mg2oMergeScw;  // TODO Check the transformation

  KeyFrameAndPose vCorrectedSim3, vNonCorrectedSim3;
  vCorrectedSim3[mpCurrentKF] = g2oCorrectedScw;
  vNonCorrectedSim3[mpCurrentKF] = g2oNonCorrectedScw;

#ifdef REGISTER_TIMES
  vnMergeKFs.push_back(spLocalWindowKFs.size() + spMergeConnectedKFs.size());
  vnMergeMPs.push_back(spLocalWindowMPs.size() + spMapPointMerge.size());
#endif
  for (KeyFrame* pKFi : spLocalWindowKFs) {
    if (!pKFi || pKFi->isBad()) {
      Verbose::PrintMess("Bad KF in correction", Verbose::VERBOSITY_DEBUG);
      continue;
    }

    if (pKFi->GetMap() != pCurrentMap)
      Verbose::PrintMess("Other map KF, this should't happen", Verbose::VERBOSITY_DEBUG);

    g2o::Sim3 g2oCorrectedSiw;

    if (pKFi != mpCurrentKF) {
      Sophus::SE3d Tiw = (pKFi->GetPose()).cast<double>();
      g2o::Sim3 g2oSiw(Tiw.unit_quaternion(), Tiw.translation(), 1.0);
      // Pose without correction
      vNonCorrectedSim3[pKFi] = g2oSiw;

      Sophus::SE3d Tic = Tiw * Twc;
      g2o::Sim3 g2oSic(Tic.unit_quaternion(), Tic.translation(), 1.0);
      g2oCorrectedSiw = g2oSic * mg2oMergeScw;
      vCorrectedSim3[pKFi] = g2oCorrectedSiw;
    } else {
      g2oCorrectedSiw = g2oCorrectedScw;
    }
    pKFi->mTcwMerge = pKFi->GetPose();

    // Update keyframe pose with corrected Sim3. First transform Sim3 to SE3 (scale translation)
    double s = g2oCorrectedSiw.scale();
    pKFi->mfScale = s;
    Sophus::SE3d correctedTiw(g2oCorrectedSiw.rotation(), g2oCorrectedSiw.translation() / s);

    pKFi->mTcwMerge = correctedTiw.cast<float>();

    if (pCurrentMap->isImuInitialized()) {
      Eigen::Quaternionf Rcor = (g2oCorrectedSiw.rotation().inverse() * vNonCorrectedSim3[pKFi].rotation()).cast<float>();
      pKFi->mVwbMerge = Rcor * pKFi->GetVelocity();
    }

    // TODO DEBUG to know which are the KFs that had been moved to the other map
  }

  int numPointsWithCorrection = 0;

  // for(MapPoint* pMPi : spLocalWindowMPs)
  std::set<MapPoint*>::iterator itMP = spLocalWindowMPs.begin();
  while (itMP != spLocalWindowMPs.end()) {
    MapPoint* pMPi = *itMP;
    if (!pMPi || pMPi->isBad()) {
      itMP = spLocalWindowMPs.erase(itMP);
      continue;
    }

    KeyFrame* pKFref = pMPi->GetReferenceKeyFrame();
    if (vCorrectedSim3.find(pKFref) == vCorrectedSim3.end()) {
      itMP = spLocalWindowMPs.erase(itMP);
      numPointsWithCorrection++;
      continue;
    }
    g2o::Sim3 g2oCorrectedSwi = vCorrectedSim3[pKFref].inverse();
    g2o::Sim3 g2oNonCorrectedSiw = vNonCorrectedSim3[pKFref];

    // Project with non-corrected pose and project back with corrected pose
    Eigen::Vector3d P3Dw = pMPi->GetWorldPos().cast<double>();
    Eigen::Vector3d eigCorrectedP3Dw = g2oCorrectedSwi.map(g2oNonCorrectedSiw.map(P3Dw));
    Eigen::Quaterniond Rcor = g2oCorrectedSwi.rotation() * g2oNonCorrectedSiw.rotation();

    pMPi->mPosMerge = eigCorrectedP3Dw.cast<float>();
    pMPi->mNormalVectorMerge = Rcor.cast<float>() * pMPi->GetNormal();

    itMP++;
  }
  /*if(numPointsWithCorrection>0) {
    std::cout << "[Merge]: " << std::to_string(numPointsWithCorrection) << " points removed from Ma due to its reference KF is not in welding area" << std::endl;
    std::cout << "[Merge]: Ma has " << std::to_string(spLocalWindowMPs.size()) << " points" << std::endl;
  }*/

  {
    std::unique_lock<std::mutex> currentLock(pCurrentMap->mMutexMapUpdate);  // We update the current map with the merge information
    std::unique_lock<std::mutex> mergeLock(pMergeMap->mMutexMapUpdate);  // We remove the Kfs and MPs in the merged area from the old map

    // std::cout << "Merge local window: " << spLocalWindowKFs.size() << std::endl;
    // std::cout << "[Merge]: init merging maps " << std::endl;
    for (KeyFrame* pKFi : spLocalWindowKFs) {
      if (!pKFi || pKFi->isBad()) {
        // std::cout << "Bad KF in correction" << std::endl;
        continue;
      }

      // std::cout << "KF id: " << pKFi->mnId << std::endl;

      pKFi->mTcwBefMerge = pKFi->GetPose();
      pKFi->mTwcBefMerge = pKFi->GetPoseInverse();
      pKFi->SetPose(pKFi->mTcwMerge);

      // Make sure connections are updated
      pKFi->UpdateMap(pMergeMap);
      pKFi->mnMergeCorrectedForKF = mpCurrentKF->mnId;
      pMergeMap->AddKeyFrame(pKFi);
      pCurrentMap->EraseKeyFrame(pKFi);

      if (pCurrentMap->isImuInitialized()) {
        pKFi->SetVelocity(pKFi->mVwbMerge);
      }
    }

    for (MapPoint* pMPi : spLocalWindowMPs) {
      if (!pMPi || pMPi->isBad()) continue;

      pMPi->SetWorldPos(pMPi->mPosMerge);
      pMPi->SetNormalVector(pMPi->mNormalVectorMerge);
      pMPi->UpdateMap(pMergeMap);
      pMergeMap->AddMapPoint(pMPi);
      pCurrentMap->EraseMapPoint(pMPi);
    }

    mpAtlas->ChangeMap(pMergeMap);
    mpAtlas->SetMapBad(pCurrentMap);
    pMergeMap->IncreaseChangeIndex();
    // TODO for debug
    pMergeMap->ChangeId(pCurrentMap->GetId());

    // std::cout << "[Merge]: merging maps finished" << std::endl;
  }

  // Rebuild the essential graph in the local window
  pCurrentMap->GetOriginKF()->SetFirstConnection(false);
  // Old parent, it will be the new child of this KF
  pNewChild = mpCurrentKF->GetParent();     
  // Old child, now it will be the parent of its own parent(we need eliminate this KF from children list in its old parent)
  pNewParent = mpCurrentKF;
  mpCurrentKF->ChangeParent(mpMergeMatchedKF);
  while (pNewChild) {
    // We remove the relation between the old parent and the new for avoid loop
    pNewChild->EraseChild(pNewParent);  
    KeyFrame* pOldParent = pNewChild->GetParent();

    pNewChild->ChangeParent(pNewParent);

    pNewParent = pNewChild;
    pNewChild = pOldParent;
  }

  // Update the connections between the local window
  mpMergeMatchedKF->UpdateConnections();

  vpMergeConnectedKFs = mpMergeMatchedKF->GetVectorCovisibleKeyFrames();
  vpMergeConnectedKFs.push_back(mpMergeMatchedKF);
  // vpCheckFuseMapPoint.reserve(spMapPointMerge.size());
  // std::copy(spMapPointMerge.begin(), spMapPointMerge.end(),
  // std::back_inserter(vpCheckFuseMapPoint));

  // Project MapPoints observed in the neighborhood of the merge keyframe into the current keyframe and neighbors using corrected poses.
  // Fuse duplications.
  // std::cout << "[Merge]: start fuse points" << std::endl;
  SearchAndFuse(vCorrectedSim3, vpCheckFuseMapPoint);
  // std::cout << "[Merge]: fuse points finished" << std::endl;

  // Update connectivity
  for (KeyFrame* pKFi : spLocalWindowKFs) {
    if (!pKFi || pKFi->isBad()) continue;

    pKFi->UpdateConnections();
  }
  for (KeyFrame* pKFi : spMergeConnectedKFs) {
    if (!pKFi || pKFi->isBad()) continue;

    pKFi->UpdateConnections();
  }

  // std::cout << "[Merge]: Start welding bundle adjustment" << std::endl;

#ifdef REGISTER_TIMES
  std::chrono::steady_clock::time_point time_StartWeldingBA = std::chrono::steady_clock::now();
  double timeMergeMaps = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(time_StartWeldingBA - time_StartMerge).count();
  vdMergeMaps_ms.push_back(timeMergeMaps);
#endif

  bool bStop = false;
  vpLocalCurrentWindowKFs.clear();
  vpMergeConnectedKFs.clear();
  std::copy(spLocalWindowKFs.begin(), spLocalWindowKFs.end(), std::back_inserter(vpLocalCurrentWindowKFs));
  std::copy(spMergeConnectedKFs.begin(), spMergeConnectedKFs.end(), std::back_inserter(vpMergeConnectedKFs));
  if (mpTracker->mSensor == CameraType::IMU_MONOCULAR || mpTracker->mSensor == CameraType::IMU_STEREO || mpTracker->mSensor == CameraType::IMU_RGBD) {
    Optimizer::MergeInertialBA(mpCurrentKF, mpMergeMatchedKF, &bStop, pCurrentMap, vCorrectedSim3);
  } else {
    Optimizer::LocalBundleAdjustment(mpCurrentKF, vpLocalCurrentWindowKFs, vpMergeConnectedKFs, &bStop);
  }

#ifdef REGISTER_TIMES
  std::chrono::steady_clock::time_point time_EndWeldingBA = std::chrono::steady_clock::now();
  double timeWeldingBA = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(time_EndWeldingBA - time_StartWeldingBA).count();
  vdWeldingBA_ms.push_back(timeWeldingBA);
#endif
  // std::cout << "[Merge]: Welding bundle adjustment finished" << std::endl;

  // Loop closed. Release Local Mapping.
  mpLocalMapper->Release();

  // Update the non critical area from the current map to the merged map
  std::vector<KeyFrame*> vpCurrentMapKFs = pCurrentMap->GetAllKeyFrames();
  std::vector<MapPoint*> vpCurrentMapMPs = pCurrentMap->GetAllMapPoints();

  if (vpCurrentMapKFs.size() != 0) {
    if (mpTracker->mSensor == CameraType::MONOCULAR) {
      // We update the current map with the merge information
      std::unique_lock<std::mutex> currentLock(pCurrentMap->mMutexMapUpdate);

      for (KeyFrame* pKFi : vpCurrentMapKFs) {
        if (!pKFi || pKFi->isBad() || pKFi->GetMap() != pCurrentMap) {
          continue;
        }

        g2o::Sim3 g2oCorrectedSiw;

        Sophus::SE3d Tiw = (pKFi->GetPose()).cast<double>();
        g2o::Sim3 g2oSiw(Tiw.unit_quaternion(), Tiw.translation(), 1.0);
        // Pose without correction
        vNonCorrectedSim3[pKFi] = g2oSiw;

        Sophus::SE3d Tic = Tiw * Twc;
        g2o::Sim3 g2oSim(Tic.unit_quaternion(), Tic.translation(), 1.0);
        g2oCorrectedSiw = g2oSim * mg2oMergeScw;
        vCorrectedSim3[pKFi] = g2oCorrectedSiw;

        // Update keyframe pose with corrected Sim3. First transform Sim3 to SE3 (scale translation)
        double s = g2oCorrectedSiw.scale();

        pKFi->mfScale = s;

        Sophus::SE3d correctedTiw(g2oCorrectedSiw.rotation(), g2oCorrectedSiw.translation() / s);

        pKFi->mTcwBefMerge = pKFi->GetPose();
        pKFi->mTwcBefMerge = pKFi->GetPoseInverse();

        pKFi->SetPose(correctedTiw.cast<float>());

        if (pCurrentMap->isImuInitialized()) {
          Eigen::Quaternionf Rcor = (g2oCorrectedSiw.rotation().inverse() * vNonCorrectedSim3[pKFi].rotation()).cast<float>();
          pKFi->SetVelocity(Rcor * pKFi->GetVelocity());  // TODO: should add here scale s
        }
      }
      for (MapPoint* pMPi : vpCurrentMapMPs) {
        if (!pMPi || pMPi->isBad() || pMPi->GetMap() != pCurrentMap) continue;

        KeyFrame* pKFref = pMPi->GetReferenceKeyFrame();
        g2o::Sim3 g2oCorrectedSwi = vCorrectedSim3[pKFref].inverse();
        g2o::Sim3 g2oNonCorrectedSiw = vNonCorrectedSim3[pKFref];

        // Project with non-corrected pose and project back with corrected pose
        Eigen::Vector3d P3Dw = pMPi->GetWorldPos().cast<double>();
        Eigen::Vector3d eigCorrectedP3Dw = g2oCorrectedSwi.map(g2oNonCorrectedSiw.map(P3Dw));
        pMPi->SetWorldPos(eigCorrectedP3Dw.cast<float>());

        pMPi->UpdateNormalAndDepth();
      }
    }

    mpLocalMapper->RequestStop();
    // Wait until Local Mapping has effectively stopped
    while (!mpLocalMapper->isStopped()) {
      usleep(1000);
    }

    // Optimize graph (and update the loop position for each element from the beginning to the end)
    if (mpTracker->mSensor != CameraType::MONOCULAR) {
      Optimizer::OptimizeEssentialGraph(mpCurrentKF, vpMergeConnectedKFs, vpLocalCurrentWindowKFs, vpCurrentMapKFs, vpCurrentMapMPs);
    }

    {
      // Get Merge Map std::mutex
      std::unique_lock<std::mutex> currentLock(pCurrentMap->mMutexMapUpdate);  // We update the current map with the Merge information
      std::unique_lock<std::mutex> mergeLock(pMergeMap->mMutexMapUpdate);  // We remove the Kfs and MPs in the merged area from the old map

      // std::cout << "Merge outside KFs: " << vpCurrentMapKFs.size() << std::endl;
      for (KeyFrame* pKFi : vpCurrentMapKFs) {
        if (!pKFi || pKFi->isBad() || pKFi->GetMap() != pCurrentMap) continue;
        // std::cout << "KF id: " << pKFi->mnId << std::endl;

        // Make sure connections are updated
        pKFi->UpdateMap(pMergeMap);
        pMergeMap->AddKeyFrame(pKFi);
        pCurrentMap->EraseKeyFrame(pKFi);
      }

      for (MapPoint* pMPi : vpCurrentMapMPs) {
        if (!pMPi || pMPi->isBad()) continue;

        pMPi->UpdateMap(pMergeMap);
        pMergeMap->AddMapPoint(pMPi);
        pCurrentMap->EraseMapPoint(pMPi);
      }
    }
  }

#ifdef REGISTER_TIMES
  std::chrono::steady_clock::time_point time_EndOptEss = std::chrono::steady_clock::now();
  double timeOptEss = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(time_EndOptEss - time_EndWeldingBA).count();
  vdMergeOptEss_ms.push_back(timeOptEss);
#endif

  mpLocalMapper->Release();

  if (bRelaunchBA && (!pCurrentMap->isImuInitialized() || (pCurrentMap->KeyFramesInMap() < 200 && mpAtlas->CountMaps() == 1))) {
    // Launch a new thread to perform Global Bundle Adjustment
    mbRunningGBA = true;
    mbFinishedGBA = false;
    mbStopGBA = false;
    std::cout << "Creating Thread in LoopClosing::MergeLocal()" << std::endl;
    mpThreadGBA = std::jthread(&LoopClosing::RunGlobalBundleAdjustment, this, pMergeMap, mpCurrentKF->mnId);
  }

  mpMergeMatchedKF->AddMergeEdge(mpCurrentKF);
  mpCurrentKF->AddMergeEdge(mpMergeMatchedKF);

  pCurrentMap->IncreaseChangeIndex();
  pMergeMap->IncreaseChangeIndex();

  mpAtlas->RemoveBadMaps();
}

void LoopClosing::MergeLocal2() {
  std::cout << "MERGE LOCAL MAP 2" << std::endl;
  loopClosed = true;
  Verbose::PrintMess("Merge detected!!!", Verbose::VERBOSITY_NORMAL);

  // int numTemporalKFs = 11; UNUSED due to todo // TODO (set by parameter): Temporal KFs in the local window if the map is inertial.

  // Relationship to rebuild the essential graph, it is used two times, first in the local window and later in the rest of the map
  KeyFrame* pNewChild;
  KeyFrame* pNewParent;

  std::vector<KeyFrame*> vpLocalCurrentWindowKFs;
  std::vector<KeyFrame*> vpMergeConnectedKFs;

  KeyFrameAndPose CorrectedSim3, NonCorrectedSim3;
  // NonCorrectedSim3[mpCurrentKF]=mg2oLoopScw;

  // Flag that is true only when we stopped a running BA, in this case we need relaunch at the end of the merge
  // bool bRelaunchBA = false; // UNUSED

  // std::cout << "Check Full Bundle Adjustment" << std::endl;
  // If a Global Bundle Adjustment is running, abort it
  if (isRunningGBA()) {
    std::unique_lock<std::mutex> lock(mMutexGBA);
    mbStopGBA = true;

    mnFullBAIdx++;

    // if (mpThreadGBA) {
    //   mpThreadGBA->detach();
    //   delete mpThreadGBA;
    // }
    // bRelaunchBA = true; // UNUSED
  }

  // std::cout << "Request Stop Local Mapping" << std::endl;
  mpLocalMapper->RequestStop();
  // Wait until Local Mapping has effectively stopped
  while (!mpLocalMapper->isStopped()) {
    usleep(1000);
  }
  // std::cout << "Local Map stopped" << std::endl;

  std::shared_ptr<Map> pCurrentMap = mpCurrentKF->GetMap();
  std::shared_ptr<Map> pMergeMap = mpMergeMatchedKF->GetMap();

  {
    float s_on = mSold_new.scale();
    Sophus::SE3f T_on(mSold_new.rotation().cast<float>(), mSold_new.translation().cast<float>());

    std::unique_lock<std::mutex> lock(mpAtlas->GetCurrentMap()->mMutexMapUpdate);

    // std::cout << "KFs before empty: " << mpAtlas->GetCurrentMap()->KeyFramesInMap() << std::endl;
    mpLocalMapper->EmptyQueue();
    // std::cout << "KFs after empty: " << mpAtlas->GetCurrentMap()->KeyFramesInMap() << std::endl;

    // std::chrono::steady_clock::time_point t2 = std::chrono::steady_clock::now(); // UNUSED
    // std::cout << "updating active map to merge reference" << std::endl;
    // std::cout << "curr merge KF id: " << mpCurrentKF->mnId << std::endl;
    // std::cout << "curr tracking KF id: " << mpTracker->GetLastKeyFrame()->mnId << std::endl;
    bool bScaleVel = false;
    if (s_on != 1) bScaleVel = true;
    mpAtlas->GetCurrentMap()->ApplyScaledRotation(T_on, s_on, bScaleVel);
    mpTracker->UpdateFrameIMU(s_on, mpCurrentKF->GetImuBias(), mpTracker->GetLastKeyFrame());

    // std::chrono::steady_clock::time_point t3 = std::chrono::steady_clock::now(); // UNUSED
  }

  const int numKFnew = pCurrentMap->KeyFramesInMap();

  if (mpTracker->mSensor.isInertial() && !pCurrentMap->GetIniertialBA2()) {
    // Map is not completly initialized
    Eigen::Vector3d bg, ba;
    bg << 0., 0., 0.;
    ba << 0., 0., 0.;
    Optimizer::InertialOptimization(pCurrentMap, bg, ba);
    IMU::Bias b(ba[0], ba[1], ba[2], bg[0], bg[1], bg[2]);
    std::unique_lock<std::mutex> lock(mpAtlas->GetCurrentMap()->mMutexMapUpdate);
    mpTracker->UpdateFrameIMU(1.0f, b, mpTracker->GetLastKeyFrame());

    // Set map initialized
    pCurrentMap->SetIniertialBA2();
    pCurrentMap->SetIniertialBA1();
    pCurrentMap->SetImuInitialized();
  }

  // std::cout << "MergeMap init ID: " << pMergeMap->GetInitKFid() << " CurrMap init ID: " << pCurrentMap->GetInitKFid() << std::endl;

  // Load KFs and MPs from merge map
  // std::cout << "updating current map" << std::endl;
  {
    // Get Merge Map Mutex (This section stops tracking!!)
    std::unique_lock<std::mutex> currentLock(pCurrentMap->mMutexMapUpdate);  // We update the current std::map with the merge information
    std::unique_lock<std::mutex> mergeLock(pMergeMap->mMutexMapUpdate);  // We remove the Kfs and MPs in the merged area from the old std::map

    std::vector<KeyFrame*> vpMergeMapKFs = pMergeMap->GetAllKeyFrames();
    std::vector<MapPoint*> vpMergeMapMPs = pMergeMap->GetAllMapPoints();

    for (KeyFrame* pKFi : vpMergeMapKFs) {
      if (!pKFi || pKFi->isBad() || pKFi->GetMap() != pMergeMap) {
        continue;
      }

      // Make sure connections are updated
      pKFi->UpdateMap(pCurrentMap);
      pCurrentMap->AddKeyFrame(pKFi);
      pMergeMap->EraseKeyFrame(pKFi);
    }

    for (MapPoint* pMPi : vpMergeMapMPs) {
      if (!pMPi || pMPi->isBad() || pMPi->GetMap() != pMergeMap) continue;

      pMPi->UpdateMap(pCurrentMap);
      pCurrentMap->AddMapPoint(pMPi);
      pMergeMap->EraseMapPoint(pMPi);
    }

    // Save non corrected poses (already merged maps)
    std::vector<KeyFrame*> vpKFs = pCurrentMap->GetAllKeyFrames();
    for (KeyFrame* pKFi : vpKFs) {
      Sophus::SE3d Tiw = (pKFi->GetPose()).cast<double>();
      g2o::Sim3 g2oSiw(Tiw.unit_quaternion(), Tiw.translation(), 1.0);
      NonCorrectedSim3[pKFi] = g2oSiw;
    }
  }

  // std::cout << "MergeMap init ID: " << pMergeMap->GetInitKFid() << " CurrMap init ID: " << pCurrentMap->GetInitKFid() << std::endl;
  // std::cout << "end updating current std::map" << std::endl;

  // Critical zone
  // bool good = pCurrentMap->CheckEssentialGraph();
  /*if(!good)
    std::cout << "BAD ESSENTIAL GRAPH!!" << std::endl;*/

  // std::cout << "Update essential graph" << std::endl;
  // mpCurrentKF->UpdateConnections(); // to put at false mbFirstConnection
  pMergeMap->GetOriginKF()->SetFirstConnection(false);
  pNewChild = mpMergeMatchedKF->GetParent();  // Old parent, it will be the new child of this KF
  pNewParent = mpMergeMatchedKF;  // Old child, now it will be the parent of its own parent(we need eliminate this KF from children std::list in its old parent)
  mpMergeMatchedKF->ChangeParent(mpCurrentKF);
  while (pNewChild) {
    pNewChild->EraseChild(pNewParent);  // We remove the relation between the old parent and the new for avoid loop
    KeyFrame* pOldParent = pNewChild->GetParent();
    pNewChild->ChangeParent(pNewParent);
    pNewParent = pNewChild;
    pNewChild = pOldParent;
  }

  // std::cout << "MergeMap init ID: " << pMergeMap->GetInitKFid() << " CurrMap init ID: " << pCurrentMap->GetInitKFid() << std::endl;

  // std::cout << "end update essential graph" << std::endl;

  /*good = pCurrentMap->CheckEssentialGraph();
  if(!good)
    std::cout << "BAD ESSENTIAL GRAPH 1!!" << std::endl;*/

  // std::cout << "Update relationship between KFs" << std::endl;
  std::vector<MapPoint*> vpCheckFuseMapPoint;  // MapPoint vector from current map to allow to fuse duplicated points with the old map (merge)
  std::vector<KeyFrame*> vpCurrentConnectedKFs;

  mvpMergeConnectedKFs.push_back(mpMergeMatchedKF);
  std::vector<KeyFrame*> aux = mpMergeMatchedKF->GetVectorCovisibleKeyFrames();
  mvpMergeConnectedKFs.insert(mvpMergeConnectedKFs.end(), aux.begin(), aux.end());
  if(mvpMergeConnectedKFs.size() > 6)
    mvpMergeConnectedKFs.erase(mvpMergeConnectedKFs.begin() + 6, mvpMergeConnectedKFs.end());
  /*mvpMergeConnectedKFs = mpMergeMatchedKF->GetVectorCovisibleKeyFrames();
  mvpMergeConnectedKFs.push_back(mpMergeMatchedKF);*/

  mpCurrentKF->UpdateConnections();
  vpCurrentConnectedKFs.push_back(mpCurrentKF);
  /*vpCurrentConnectedKFs = mpCurrentKF->GetVectorCovisibleKeyFrames();
  vpCurrentConnectedKFs.push_back(mpCurrentKF);*/
  aux = mpCurrentKF->GetVectorCovisibleKeyFrames();
  vpCurrentConnectedKFs.insert(vpCurrentConnectedKFs.end(), aux.begin(), aux.end());
  if (vpCurrentConnectedKFs.size() > 6)
    vpCurrentConnectedKFs.erase(vpCurrentConnectedKFs.begin() + 6, vpCurrentConnectedKFs.end());

  std::set<MapPoint*> spMapPointMerge;
  for (KeyFrame* pKFi : mvpMergeConnectedKFs) {
    std::set<MapPoint*> vpMPs = pKFi->GetMapPoints();
    spMapPointMerge.insert(vpMPs.begin(), vpMPs.end());
    if (spMapPointMerge.size() > 1000) break;
  }

  /*std::cout << "vpCurrentConnectedKFs.size() " << vpCurrentConnectedKFs.size() << std::endl; 
  std::cout << "mvpMergeConnectedKFs.size() " << mvpMergeConnectedKFs.size() << std::endl;
  std::cout << "spMapPointMerge.size() " << spMapPointMerge.size() << std::endl;*/

  vpCheckFuseMapPoint.reserve(spMapPointMerge.size());
  std::copy(spMapPointMerge.begin(), spMapPointMerge.end(), std::back_inserter(vpCheckFuseMapPoint));
  // std::cout << "Finished to update relationship between KFs" << std::endl;

  // std::cout << "MergeMap init ID: " << pMergeMap->GetInitKFid() << ", CurrMap init ID: " << pCurrentMap->GetInitKFid() << std::endl;

  /*good = pCurrentMap->CheckEssentialGraph();
  if(!good)
    std::cout << "BAD ESSENTIAL GRAPH 2!!" << std::endl;*/

  // std::cout << "start SearchAndFuse" << std::endl;
  SearchAndFuse(vpCurrentConnectedKFs, vpCheckFuseMapPoint);
  // std::cout << "end SearchAndFuse" << std::endl;

  // std::cout << "MergeMap init ID: " << pMergeMap->GetInitKFid() << ", CurrMap init ID: " << pCurrentMap->GetInitKFid() << std::endl;

  /*good = pCurrentMap->CheckEssentialGraph();
  if(!good)
    std::cout << "BAD ESSENTIAL GRAPH 3!!" << std::endl;

  std::cout << "Init to update connections" << std::endl;*/

  for (KeyFrame* pKFi : vpCurrentConnectedKFs) {
    if (!pKFi || pKFi->isBad()) continue;

    pKFi->UpdateConnections();
  }
  for (KeyFrame* pKFi : mvpMergeConnectedKFs) {
    if (!pKFi || pKFi->isBad()) continue;

    pKFi->UpdateConnections();
  }
  // std::cout << "end update connections" << std::endl;

  // std::cout << "MergeMap init ID: " << pMergeMap->GetInitKFid() << ", CurrMap init ID: " << pCurrentMap->GetInitKFid() << std::endl;

  /*good = pCurrentMap->CheckEssentialGraph();
  if(!good)
    std::cout << "BAD ESSENTIAL GRAPH 4!!" << std::endl;*/

  // TODO Check: If new map is too small, we suppose that not informaiton can be
  // propagated from new to old map
  if (numKFnew < 10) {
    mpLocalMapper->Release();
    return;
  }

  /*good = pCurrentMap->CheckEssentialGraph();
  if(!good)
    std::cout << "BAD ESSENTIAL GRAPH 5!!" << std::endl;*/

  // Perform BA
  bool bStopFlag = false;
  KeyFrame* pCurrKF = mpTracker->GetLastKeyFrame();
  // std::cout << "start MergeInertialBA" << std::endl;
  if (pCurrKF == nullptr) {
    std::cerr << "\033[22;34mcurrent KF is nullptr" << std::endl;
    mpLocalMapper->Release();
    return;
  }
  Optimizer::MergeInertialBA(pCurrKF, mpMergeMatchedKF, &bStopFlag, pCurrentMap, CorrectedSim3);
  // std::cout << "end MergeInertialBA" << std::endl;

  /*good = pCurrentMap->CheckEssentialGraph();
  if(!good)
    std::cout << "BAD ESSENTIAL GRAPH 6!!" << std::endl;*/

  // Release Local Mapping.
  mpLocalMapper->Release();
  hasMergedLocalMap = true;

  return;
}

void LoopClosing::CheckObservations(std::set<KeyFrame*>& spKFsMap1, std::set<KeyFrame*>& spKFsMap2) {
  std::cout << "----------------------" << std::endl;
  for (KeyFrame* pKFi1 : spKFsMap1) {
    std::map<KeyFrame*, int> mMatchedMP;
    std::set<MapPoint*> spMPs = pKFi1->GetMapPoints();

    for (MapPoint* pMPij : spMPs) {
      if (!pMPij || pMPij->isBad()) {
        continue;
      }

      std::map<KeyFrame*, std::tuple<int, int>> mMPijObs = pMPij->GetObservations();
      for (KeyFrame* pKFi2 : spKFsMap2) {
        if (mMPijObs.find(pKFi2) != mMPijObs.end()) {
          if (mMatchedMP.find(pKFi2) != mMatchedMP.end()) {
            mMatchedMP[pKFi2] = mMatchedMP[pKFi2] + 1;
          } else {
            mMatchedMP[pKFi2] = 1;
          }
        }
      }
    }

    if (mMatchedMP.size() == 0) {
      std::cout << "CHECK-OBS: KF " << pKFi1->mnId << " has not any matched MP with the other map" << std::endl;
    } else {
      std::cout << "CHECK-OBS: KF " << pKFi1->mnId << " has matched MP with " << mMatchedMP.size() << " KF from the other map" << std::endl;
      for (std::pair<KeyFrame*, int> matchedKF : mMatchedMP) {
        std::cout << "   -KF: " << matchedKF.first->mnId << ", Number of matches: " << matchedKF.second << std::endl;
      }
    }
  }
  std::cout << "----------------------" << std::endl;
}

void LoopClosing::SearchAndFuse(const KeyFrameAndPose& CorrectedPosesMap, std::vector<MapPoint*>& vpMapPoints) {
  ORBmatcher matcher(0.8);

  int total_replaces = 0;

  // std::cout << "[FUSE]: Initially there are " << vpMapPoints.size() << " MPs" << std::endl; std::cout << "FUSE: Intially there are " << CorrectedPosesMap.size() << " KFs" << std::endl;
  for (KeyFrameAndPose::const_iterator mit = CorrectedPosesMap.begin(), mend = CorrectedPosesMap.end(); mit != mend; mit++) {
    int num_replaces = 0;
    KeyFrame* pKFi = mit->first;
    std::shared_ptr<Map> pMap = pKFi->GetMap();

    g2o::Sim3 g2oScw = mit->second;
    Sophus::Sim3f Scw = Converter::toSophus(g2oScw);

    std::vector<MapPoint*> vpReplacePoints(vpMapPoints.size(), nullptr);
    /*int numFused = */matcher.Fuse(pKFi, Scw, vpMapPoints, 4, vpReplacePoints);

    // Get Map Mutex
    std::unique_lock<std::mutex> lock(pMap->mMutexMapUpdate);
    const int nLP = vpMapPoints.size();
    for (int i = 0; i < nLP; i++) {
      MapPoint* pRep = vpReplacePoints[i];
      if (pRep) {
        num_replaces += 1;
        pRep->Replace(vpMapPoints[i]);
      }
    }

    total_replaces += num_replaces;
  }
  // std::cout << "[FUSE]: " << total_replaces << " MPs had been fused" << std::endl;
}

void LoopClosing::SearchAndFuse(const std::vector<KeyFrame*>& vConectedKFs, std::vector<MapPoint*>& vpMapPoints) {
  ORBmatcher matcher(0.8);

  // int total_replaces = 0; // UNUSED

  // std::cout << "FUSE-POSE: Initially there are " << vpMapPoints.size() << " MPs" << std::endl; std::cout << "FUSE-POSE: Intially there are " << vConectedKFs.size() << " KFs" << std::endl;
  for (auto mit = vConectedKFs.begin(), mend = vConectedKFs.end(); mit != mend; mit++) {
    int num_replaces = 0;
    KeyFrame* pKF = (*mit);
    std::shared_ptr<Map> pMap = pKF->GetMap();
    Sophus::SE3f Tcw = pKF->GetPose();
    Sophus::Sim3f Scw(Tcw.unit_quaternion(), Tcw.translation());
    Scw.setScale(1.f);
    // std::cout << "These should be zeros: " << Scw.rotationMatrix() - Tcw.rotationMatrix() << std::endl << Scw.translation() - Tcw.translation() << std::endl << Scw.scale() - 1.f << std::endl;
    std::vector<MapPoint*> vpReplacePoints(vpMapPoints.size(), nullptr);
    matcher.Fuse(pKF, Scw, vpMapPoints, 4, vpReplacePoints);

    // Get Map Mutex
    std::unique_lock<std::mutex> lock(pMap->mMutexMapUpdate);
    const int nLP = vpMapPoints.size();
    for (int i = 0; i < nLP; i++) {
      MapPoint* pRep = vpReplacePoints[i];
      if (pRep) {
        num_replaces += 1;
        pRep->Replace(vpMapPoints[i]);
      }
    }
    // std::cout << "FUSE-POSE: KF " << pKF->mnId << " ->" << num_replaces << " MPs fused" << std::endl; total_replaces += num_replaces;
  }
  // std::cout << "FUSE-POSE: " << total_replaces << " MPs had been fused" << std::endl;
}

void LoopClosing::RequestReset() {
  {
    std::unique_lock<std::mutex> lock(mMutexReset);
    mbResetRequested = true;
  }

  while (1) {
    {
      std::unique_lock<std::mutex> lock2(mMutexReset);
      if (!mbResetRequested) break;
    }
    usleep(5000);
  }
}

void LoopClosing::RequestResetActiveMap(std::shared_ptr<Map> pMap) {
  {
    std::unique_lock<std::mutex> lock(mMutexReset);
    mbResetActiveMapRequested = true;
    mpMapToReset = pMap;
  }

  while (1) {
    {
      std::unique_lock<std::mutex> lock2(mMutexReset);
      if (!mbResetActiveMapRequested) break;
    }
    usleep(3000);
  }
}

void LoopClosing::ResetIfRequested() {
  std::unique_lock<std::mutex> lock(mMutexReset);
  if (mbResetRequested) {
    std::cout << "Loop closer reset requested..." << std::endl;
    mlpLoopKeyFrameQueue.clear();
    mbResetRequested = false;
    mbResetActiveMapRequested = false;
  } else if (mbResetActiveMapRequested) {
    for (std::list<KeyFrame*>::const_iterator it = mlpLoopKeyFrameQueue.begin(); it != mlpLoopKeyFrameQueue.end();) {
      KeyFrame* pKFi = *it;
      if (pKFi->GetMap() == mpMapToReset) {
        it = mlpLoopKeyFrameQueue.erase(it);
      } else
        ++it;
    }

    mbResetActiveMapRequested = false;
  }
}

void LoopClosing::RunGlobalBundleAdjustment(std::shared_ptr<Map> pActiveMap, unsigned long nLoopKF) {
  Verbose::PrintMess("Starting Global Bundle Adjustment", Verbose::VERBOSITY_NORMAL);

#ifdef REGISTER_TIMES
  std::chrono::steady_clock::time_point time_StartFGBA = std::chrono::steady_clock::now();
  nFGBA_exec += 1;
  vnGBAKFs.push_back(pActiveMap->GetAllKeyFrames().size());
  vnGBAMPs.push_back(pActiveMap->GetAllMapPoints().size());
#endif

  const bool bImuInit = pActiveMap->isImuInitialized();

  if (!bImuInit)
    Optimizer::GlobalBundleAdjustemnt(pActiveMap, 10, &mbStopGBA, nLoopKF, false);
  else
    Optimizer::FullInertialBA(pActiveMap, 7, false, nLoopKF, &mbStopGBA);

#ifdef REGISTER_TIMES
  std::chrono::steady_clock::time_point time_EndGBA = std::chrono::steady_clock::now();
  double timeGBA = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(time_EndGBA - time_StartFGBA).count();
  vdGBA_ms.push_back(timeGBA);
  if (mbStopGBA) {
    nFGBA_abort += 1;
  }
#endif

  int idx = mnFullBAIdx;
  // Optimizer::GlobalBundleAdjustemnt(mpMap,10,&mbStopGBA,nLoopKF,false);

  // Update all MapPoints and KeyFrames
  // Local Mapping was active during BA, that means that there might be new keyframes not included in the Global BA and they are not consistent with
  // the updated map. We need to propagate the correction through the spanning tree
  {
    std::unique_lock<std::mutex> lock(mMutexGBA);
    if (idx != mnFullBAIdx) return;

    if (!bImuInit && pActiveMap->isImuInitialized()) return;

    if (!mbStopGBA) {
      Verbose::PrintMess("Global Bundle Adjustment finished", Verbose::VERBOSITY_NORMAL);
      Verbose::PrintMess("Updating map ...", Verbose::VERBOSITY_NORMAL);

      mpLocalMapper->RequestStop();
      // Wait until Local Mapping has effectively stopped

      while (!mpLocalMapper->isStopped() && !mpLocalMapper->isFinished()) {
        usleep(1000);
      }

      // Get Map Mutex
      std::unique_lock<std::mutex> lock(pActiveMap->mMutexMapUpdate);
      // std::cout << "LC: Update Map mutex adquired" << std::endl;

      // pActiveMap->PrintEssentialGraph();
      // Correct keyframes starting at map first keyframe
      std::list<KeyFrame*> lpKFtoCheck(pActiveMap->mvpKeyFrameOrigins.begin(), pActiveMap->mvpKeyFrameOrigins.end());

      while (!lpKFtoCheck.empty()) {
        KeyFrame* pKF = lpKFtoCheck.front();
        const std::set<KeyFrame*> sChilds = pKF->GetChilds();
        // std::cout << "---Updating KF " << pKF->mnId << " with " << sChilds.size() << " childs" << std::endl; std::cout << " KF mnBAGlobalForKF: " << pKF->mnBAGlobalForKF << std::endl;
        Sophus::SE3f Twc = pKF->GetPoseInverse();
        // std::cout << "Twc: " << Twc << std::endl;
        // std::cout << "GBA: Correct KeyFrames" << std::endl;
        for (std::set<KeyFrame*>::const_iterator sit = sChilds.begin(); sit != sChilds.end(); sit++) {
          KeyFrame* pChild = *sit;
          if (!pChild || pChild->isBad()) continue;

          if (pChild->mnBAGlobalForKF != nLoopKF) {
            // std::cout << "++++New child with flag " << pChild->mnBAGlobalForKF << "; LoopKF: " << nLoopKF << std::endl; std::cout << " child id: " << pChild->mnId << std::endl;
            Sophus::SE3f Tchildc = pChild->GetPose() * Twc;
            // std::cout << "Child pose: " << Tchildc << std::endl;
            // std::cout << "pKF->mTcwGBA: " << pKF->mTcwGBA << std::endl;
            pChild->mTcwGBA = Tchildc * pKF->mTcwGBA;  //*Tcorc*pKF->mTcwGBA;

            Sophus::SO3f Rcor = pChild->mTcwGBA.so3().inverse() * pChild->GetPose().so3();
            if (pChild->isVelocitySet()) {
              pChild->mVwbGBA = Rcor * pChild->GetVelocity();
            } else
              Verbose::PrintMess("Child velocity empty!! ", Verbose::VERBOSITY_NORMAL);

            // std::cout << "Child bias: " << pChild->GetImuBias() << std::endl;
            pChild->mBiasGBA = pChild->GetImuBias();

            pChild->mnBAGlobalForKF = nLoopKF;
          }
          lpKFtoCheck.push_back(pChild);
        }

        // std::cout << "-------Update pose" << std::endl;
        pKF->mTcwBefGBA = pKF->GetPose();
        // std::cout << "pKF->mTcwBefGBA: " << pKF->mTcwBefGBA << std::endl;
        pKF->SetPose(pKF->mTcwGBA);
        /*cv::Mat Tco_cn = pKF->mTcwBefGBA * pKF->mTcwGBA.inv();
        cv::Vec3d trasl = Tco_cn.rowRange(0,3).col(3);
        double dist = cv::norm(trasl);
        std::cout << "GBA: KF " << pKF->mnId << " had been moved " << dist << " meters" << std::endl; double desvX = 0; double desvY = 0; double desvZ = 0;
        if(pKF->mbHasHessian) {
            cv::Mat hessianInv = pKF->mHessianPose.inv();

            double covX = hessianInv.at<double>(3,3);
            desvX = std::sqrt(covX);
            double covY = hessianInv.at<double>(4,4);
            desvY = std::sqrt(covY);
            double covZ = hessianInv.at<double>(5,5);
            desvZ = std::sqrt(covZ);
            pKF->mbHasHessian = false;
        }
        if(dist > 1) {
          std::cout << "--To much distance correction: It has " << pKF->GetConnectedKeyFrames().size() << " connected KFs" << std::endl;
          std::cout << "--It has " << pKF->GetCovisiblesByWeight(80).size() << " connected KF with 80 common matches or more" << std::endl;
          std::cout << "--It has " << pKF->GetCovisiblesByWeight(50).size() << " connected KF with 50 common matches or more" << std::endl;
          std::cout << "--It has " << pKF->GetCovisiblesByWeight(20).size() << " connected KF with 20 common matches or more" << std::endl;
          std::cout << "--STD in meters(x, y, z): " << desvX << ", " << desvY << ", " << desvZ << std::endl;


          std::string strNameFile = pKF->mNameFile;
          cv::Mat imLeft = cv::imread(strNameFile, CV_LOAD_IMAGE_UNCHANGED);

          cv::cvtColor(imLeft, imLeft, CV_GRAY2BGR);

          std::vector<MapPoint*> vpMapPointsKF = pKF->GetMapPointMatches();
          int num_MPs = 0;
          for(int i=0; i<vpMapPointsKF.size(); ++i) {
              if(!vpMapPointsKF[i] || vpMapPointsKF[i]->isBad()) {
                  continue;
              }
              num_MPs += 1;
              std::string strNumOBs = to_string(vpMapPointsKF[i]->Observations());
              cv::circle(imLeft, pKF->mvKeys[i].pt, 2, cv::Scalar(0, 255, 0));
              cv::putText(imLeft, strNumOBs, pKF->mvKeys[i].pt, CV_FONT_HERSHEY_DUPLEX, 1, cv::Scalar(255, 0, 0));
          }
          std::cout << "--It has " << num_MPs << " MPs matched in the map" << std::endl;

          std::string namefile = "./test_GBA/GBA_" + to_string(nLoopKF) + "_KF" + to_string(pKF->mnId) +"_D" + to_string(dist) +".png";
          cv::imwrite(namefile, imLeft);
        }*/

        if (pKF->bImu) {
          // std::cout << "-------Update inertial values" << std::endl;
          pKF->mVwbBefGBA = pKF->GetVelocity();
          // if (pKF->mVwbGBA.empty())
          //    Verbose::PrintMess("pKF->mVwbGBA is empty",
          //    Verbose::VERBOSITY_NORMAL);

          // assert(!pKF->mVwbGBA.empty());
          pKF->SetVelocity(pKF->mVwbGBA);
          pKF->SetNewBias(pKF->mBiasGBA);
        }

        lpKFtoCheck.pop_front();
      }

      // std::cout << "GBA: Correct MapPoints" << std::endl;
      // Correct MapPoints
      const std::vector<MapPoint*> vpMPs = pActiveMap->GetAllMapPoints();

      for (size_t i = 0; i < vpMPs.size(); i++) {
        MapPoint* pMP = vpMPs[i];

        if (pMP->isBad()) continue;

        if (pMP->mnBAGlobalForKF == nLoopKF) {
          // If optimized by Global BA, just update
          pMP->SetWorldPos(pMP->mPosGBA);
        } else {
          // Update according to the correction of its reference keyframe
          KeyFrame* pRefKF = pMP->GetReferenceKeyFrame();

          if (pRefKF->mnBAGlobalForKF != nLoopKF) continue;

          /*if(pRefKF->mTcwBefGBA.empty())
              continue;*/

          // Map to non-corrected camera
          // cv::Mat Rcw = pRefKF->mTcwBefGBA.rowRange(0,3).colRange(0,3);
          // cv::Mat tcw = pRefKF->mTcwBefGBA.rowRange(0,3).col(3);
          Eigen::Vector3f Xc = pRefKF->mTcwBefGBA * pMP->GetWorldPos();

          // Backproject using corrected camera
          pMP->SetWorldPos(pRefKF->GetPoseInverse() * Xc);
        }
      }

      pActiveMap->InformNewBigChange();
      pActiveMap->IncreaseChangeIndex();

      // TODO Check this update
      // mpTracker->UpdateFrameIMU(1.0f,
      // mpTracker->GetLastKeyFrame()->GetImuBias(),
      // mpTracker->GetLastKeyFrame());

      mpLocalMapper->Release();

#ifdef REGISTER_TIMES
      std::chrono::steady_clock::time_point time_EndUpdateMap = std::chrono::steady_clock::now();
      double timeUpdateMap = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(time_EndUpdateMap - time_EndGBA).count();
      vdUpdateMap_ms.push_back(timeUpdateMap);
      double timeFGBA = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(time_EndUpdateMap - time_StartFGBA).count();
      vdFGBATotal_ms.push_back(timeFGBA);
#endif
      Verbose::PrintMess("Map updated!", Verbose::VERBOSITY_NORMAL);
    }

    mbFinishedGBA = true;
    mbRunningGBA = false;
  }
}

void LoopClosing::RequestFinish() {
  std::unique_lock<std::mutex> lock(mMutexFinish);
  // std::cout << "LC: Finish requested" << std::endl;
  mbFinishRequested = true;
}

bool LoopClosing::CheckFinish() {
  std::unique_lock<std::mutex> lock(mMutexFinish);
  return mbFinishRequested;
}

void LoopClosing::SetFinish() {
  std::unique_lock<std::mutex> lock(mMutexFinish);
  mbFinished = true;
}

bool LoopClosing::isFinished() {
  std::unique_lock<std::mutex> lock(mMutexFinish);
  return mbFinished;
}

}  // namespace MORB_SLAM
