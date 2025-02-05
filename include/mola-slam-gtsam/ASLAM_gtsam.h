/* -------------------------------------------------------------------------
 *   A Modular Optimization framework for Localization and mApping  (MOLA)
 * Copyright (C) 2018-2019 Jose Luis Blanco, University of Almeria
 * See LICENSE for license information.
 * ------------------------------------------------------------------------- */
/**
 * @file   ASLAM_gtsam.h
 * @brief  SLAM in absolute coordinates with GTSAM factor graphs
 * @author Jose Luis Blanco Claraco
 * @date   Jan 08, 2018
 */
#pragma once

// mrpt includes first:
#include <mola-kernel/WorkerThreadsPool.h>
#include <mola-kernel/interfaces/BackEndBase.h>
#include <mrpt/graphs/CNetworkOfPoses.h>
#include <mrpt/gui/CDisplayWindow3D.h>
#include <mrpt/poses/CPose3DInterpolator.h>
#include <mrpt/typemeta/TEnumType.h>
// gtsam next:
#include <gtsam/geometry/Cal3_S2Stereo.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/nonlinear/ISAM2.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam_unstable/slam/SmartStereoProjectionPoseFactor.h>

#include <mutex>

namespace mola
{
/** Three-ways map between IDs: feature_id, gtsam_id, MOLA_id */
template <
    typename feature_id_t, typename gtsam_id_t = std::size_t,
    typename mola_id_t = mola::fid_t>
struct TriMap
{
    // std::map<feature_id_t, gtsam_id_t> feature2gtsam;
    // std::map<feature_id_t, mola_id_t>  feature2mola;
    std::map<gtsam_id_t, mola_id_t> gtsam2mola;
    std::map<mola_id_t, gtsam_id_t> mola2gtsam;
};

/** Reference implementation of absolute-coordinates SLAM with GTSAM factor
 * graphs.
 * See docs in: \ref mola_slam_gtsam_grp
 * \ingroup mola_slam_gtsam_grp */
class ASLAM_gtsam : public BackEndBase
{
    DEFINE_MRPT_OBJECT(ASLAM_gtsam)

   public:
    ASLAM_gtsam();
    ~ASLAM_gtsam() override = default;

    // See docs in base class
    void initialize(const std::string& cfg_block) override;
    void spinOnce() override;
    void onQuit() override;

    /** Type selector for kind of KeyFrame state vector representation */
    enum class StateVectorType : int8_t
    {
        SE2 = 0,
        SE3,
        SE2Vel,
        SE3Vel,
        Undefined = -1
    };

    struct Parameters
    {
        /** See StateVectorType */
        StateVectorType state_vector{StateVectorType::Undefined};

        /** Use iSAM2 (true) or Lev-Marq. (false) */
        bool use_incremental_solver{true};

        /** iSAM2 additional update() steps. Set >0 to fasten convergence, at
         * the cost of more processing time for each timestep */
        int isam2_additional_update_steps{0};

        /** iSAM2 relinearize threshold. Refer to iSAM2 docs */
        double isam2_relinearize_threshold{0.1};

        /** iSAM2 relinearize skip. Refer to iSAM2 docs */
        int isam2_relinearize_skip{1};

        /** Saves the overall optimized trajectory at the end, in different file
         * formats, if !="" (default:"") */
        std::string save_trajectory_file_prefix{};

        /** Save map at end of a SLAM session. See
         * WorldModel::map_base_directory() to see where maps are stored by
         * default and how to change it. */
        bool save_map_at_end{true};

        /** Const. velocity model: sigma of the position equation (see paper) */
        double const_vel_model_std_pos{0.1};
        /** Const. velocity model: sigma of the velocity equation (see paper) */
        double const_vel_model_std_vel{1.0};

        double max_interval_between_kfs_for_dynamic_model{5.0};
    };

    Parameters params_;

    // Impl. if virtual methods. See base class docs:
    ProposeKF_Output doAddKeyFrame(const ProposeKF_Input& i) override;
    AddFactor_Output doAddFactor(Factor& newF) override;
    void             doAdvertiseUpdatedLocalization(
                    AdvertiseUpdatedLocalization_Input l) override;
    void onSmartFactorChanged(
        mola::fid_t id, const mola::FactorBase* f) override;

    void lock_slam() override;
    void unlock_slam() override;

    mola::id_t temp_createStereoCamera(
        const mrpt::img::TCamera& left, const mrpt::img::TCamera& right,
        const double baseline) override;

    mola::id_t temp_createLandmark(
        const mrpt::math::TPoint3D& init_value) override;

   private:
    /** Indices for accessing the KF_gtsam_keys array */
    enum kf_key_index_t
    {
        KF_KEY_POSE = 0,
        KF_KEY_VEL,
        //-- end of list --
        KF_KEY_COUNT
    };

    using KF_gtsam_keys = std::array<gtsam::Key, KF_KEY_COUNT>;

    struct SLAM_state
    {
        /** Incremental estimator */
        std::unique_ptr<gtsam::ISAM2> isam2;

        /** Pending new elements to add to the map */
        gtsam::NonlinearFactorGraph                       newfactors;
        gtsam::Values                                     newvalues;
        gtsam::FastMap<gtsam::FactorIndex, gtsam::KeySet> changedSmartFactors;
        /** Map: new factor index in newfactors ==> MOLA factor ID */
        std::map<std::size_t, mola::fid_t> newFactor2molaid;

        std::set<mola::id_t> kf_has_value;
        gtsam::Values        last_values;

        template <class T>
        T at_new_or_last_values(const gtsam::Key& k) const
        {
            if (last_values.exists(k)) return last_values.at<T>(k);
            if (newvalues.exists(k)) return newvalues.at<T>(k);
            throw gtsam::ValuesKeyDoesNotExist("at_new_or_last_values", k);
        }

        /** History of vehicle poses over time (stored if
         * params_.save_trajectory_file_prefix!="").
         * Note that this stores relative poses for all frames, keyframes and
         * non-keyframes. We keep them relative so we can reconstruct the
         * optimal poses at any moment, composing the poses of the base,
         * optimized, KF of reference for each entry. */
        std::map<mrpt::Clock::time_point, AdvertiseUpdatedLocalization_Input>
            trajectory{};

        // locked by last_kf_estimates_lock_ as well:
        mrpt::graphs::CNetworkOfPoses3D            vizmap;
        std::map<mola::id_t, mrpt::math::TTwist3D> vizmap_dyn;

        /** Absolute coordinates single reference frame (WorldModel index) */
        id_t root_kf_id{mola::INVALID_ID};

        id_t last_created_kf_id{mola::INVALID_ID};
        id_t former_last_created_kf_id{mola::INVALID_ID};

        void updateLastCreatedKF(id_t id)
        {
            former_last_created_kf_id = last_created_kf_id;
            last_created_kf_id        = id;
        }

        mrpt::Clock::time_point last_created_kf_id_tim{INVALID_TIMESTAMP};

        /** Map between mola WorldModel KF indices and the corresponding gtsam
         * Key(s) value(s). When in SE2/SE3 mode, only the pose Key is used.
         * When in SE2Vel/SE3Vel mode, the extra key for the velocity variable
         * is stored a well */
        std::map<mola::id_t, KF_gtsam_keys> mola2gtsam;
        /** Inverse map for `mola2gtsam` (indexed by gtsam *pose* ID) */
        std::array<std::map<gtsam::Key, mola::id_t>, KF_KEY_COUNT> gtsam2mola;

        struct StereoSmartFactorState
        {
            gtsam::Cal3_S2Stereo::shared_ptr camera_K;

            std::map<
                mola::fid_t, gtsam::SmartStereoProjectionPoseFactor::shared_ptr>
                factors;

            /** Relationship between ID numbers in the different systems */
            TriMap<std::size_t> ids;
        };
        StereoSmartFactorState stereo_factors;

        std::map<mrpt::Clock::time_point, mola::id_t> time2kf;

        std::vector<mola::SmartFactorIMU*> active_imu_factors;
    };

    SLAM_state state_;
    /** mutex for: gtsam solver (isam2), newfactors, newvalues & kf_has_value */
    std::recursive_timed_mutex isam2_lock_;
    std::recursive_timed_mutex vizmap_lock_;
    std::recursive_timed_mutex keys_map_lock_;  //!< locks mola2gtsam/gtsam2mola

    fid_t addFactor(const FactorRelativePose3& f);
    fid_t addFactor(const FactorDynamicsConstVel& f);
    fid_t addFactor(const FactorStereoProjectionPose& f);
    fid_t addFactor(const SmartFactorStereoProjectionPose& f);
    fid_t addFactor(const SmartFactorIMU& f);

    mola::id_t internal_addKeyFrame_Root(const ProposeKF_Input& i);
    mola::id_t internal_addKeyFrame_Regular(const ProposeKF_Input& i);

    void mola2gtsam_register_new_kf(const mola::id_t kf_id);

    void internal_add_gtsam_prior_vel(const mola::id_t kf_id);

    struct whole_path_t
    {
        mrpt::poses::CPose3DInterpolator                        poses;
        std::map<mrpt::Clock::time_point, mrpt::math::TTwist3D> twists;
        mola::fast_map<mola::id_t, mrpt::Clock::time_point>     id2time;
        mola::fast_map<mrpt::Clock::time_point, mola::id_t>     time2id;
    };

    /** Returns a list with all keyframes and, if
     * save_trajectory_file_prefix!="", all non keyframes. */
    whole_path_t reconstruct_whole_path() const;

    /** Latest localization */
    std::mutex                         latest_localization_data_mtx_;
    AdvertiseUpdatedLocalization_Input latest_localization_data_;

    /** Returns the closest KF in time, or invalid_id if none. */
    mola::id_t find_closest_KF_in_time(const mrpt::Clock::time_point& t) const;

    // TODO: Temporary code, should be moved to a new module "MapViz":
    // --------------
    mola::WorkerThreadsPool gui_updater_pool_{
        1, mola::WorkerThreadsPool::POLICY_DROP_OLD};

    struct DisplayInfo
    {
        mrpt::Clock::time_point         current_tim{};
        mrpt::graphs::CNetworkOfPoses3D vizmap;
    };
    /** This will be run in a dedicated thread inside gui_updater_pool_ */
    void doUpdateDisplay(std::shared_ptr<DisplayInfo> di);

    struct DisplayState
    {
        std::set<mola::id_t> kf_checked_decorations;
        /** List of render decorations for each KF. Stored here
         * so we can update their pose  */
        std::map<mola::id_t, mrpt::opengl::CRenderizable::Ptr> kf_decorations;
        mrpt::opengl::CSetOfObjects::Ptr                       slam_graph_gl;
    };

    DisplayState display_state_;

    mrpt::gui::CDisplayWindow3D::Ptr display_;
    // ----------------------------
};

}  // namespace mola

MRPT_ENUM_TYPE_BEGIN(mola::ASLAM_gtsam::StateVectorType)
MRPT_FILL_ENUM_MEMBER(mola::ASLAM_gtsam::StateVectorType, SE2);
MRPT_FILL_ENUM_MEMBER(mola::ASLAM_gtsam::StateVectorType, SE3);
MRPT_FILL_ENUM_MEMBER(mola::ASLAM_gtsam::StateVectorType, SE2Vel);
MRPT_FILL_ENUM_MEMBER(mola::ASLAM_gtsam::StateVectorType, SE3Vel);
MRPT_ENUM_TYPE_END()
