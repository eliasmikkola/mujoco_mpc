// Copyright 2022 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mjpc/tasks/humanoid/skateboard/pushing.h"

#include <mujoco/mujoco.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <random>
#include <string>
#include <tuple>

#include "mjpc/utilities.h"

namespace {
int jiiri = 0;

void move_goal(const mjModel *model, mjData *d,
               const std::vector<double, std::allocator<double>> parameters,
               int mode) {
  // Set new goal position in `data->mocap_pos` if we've reached the goal.
  const int goal_body_id = mj_name2id(model, mjOBJ_XBODY, "goal");
  if (goal_body_id < 0) mju_error("body 'goal' not found");
  const int goal_mocapid = model->body_mocapid[goal_body_id];
  if (goal_mocapid < 0) mju_error("mocap 'goal' not found");
  double goal_position[3];
  mju_copy3(goal_position, d->mocap_pos + 3 * goal_mocapid);

  double skateboard_position[3];
  int skateboard_body_id_ = mj_name2id(model, mjOBJ_XBODY, "skateboard");

  // move mpos to x,y position of skateboard
  mju_copy(skateboard_position, d->xpos + 3 * skateboard_body_id_, 3);

  double skateboard_goal_error[3];
  mju_sub3(skateboard_goal_error, goal_position, skateboard_position);
  double skateboard_goal_distance = mju_norm(skateboard_goal_error, 2);

  double goal_switch_threshold_m = 0.5;
  if (skateboard_goal_distance < goal_switch_threshold_m) {
    std::random_device rd;              // Obtain a random number from hardware
    std::mt19937 eng(rd());             // Seed the generator
    std::bernoulli_distribution distr;  // Define the distribution

    // Move goal to a new position. We choose a random position that is
    // `goal_offset_x` ahead and `goal_offset_y` to either left or right,
    // direction chosen uniformly. The "zero"-direction is the skateboard's
    // heading direction at the time when we reach the goal.

    // get skateboard heading.
    double skateboard_xmat[9];
    mju_copy(skateboard_xmat, d->xmat + 9 * skateboard_body_id_, 9);
    // double skateboard_heading = atan2(skateboard_xmat[3],
    // skateboard_xmat[0]);
    double skateboard_heading[2] = {skateboard_xmat[0], skateboard_xmat[3]};

    double goal_offset_xy[2];
    mju_copy(goal_offset_xy, skateboard_heading, 2);
    mju_normalize(goal_offset_xy, 2);

    double goal_move_distance_forward = 8.0;
    double goal_move_distance_side = 2.0;

    bool left_or_right = distr(eng);  // Generate a random boolean

    // // compute offset vector in front of the board.
    double goal_offset_forward[2] = {
        goal_offset_xy[0] * goal_move_distance_forward,
        goal_offset_xy[1] * goal_move_distance_forward,
    };

    // compute offset vector to the side of the board.
    double goal_offset_perpendicular[2];

    if (left_or_right) {
      goal_offset_perpendicular[0] =
          -goal_offset_xy[1] * goal_move_distance_side;
      goal_offset_perpendicular[1] =
          +goal_offset_xy[0] * goal_move_distance_side;
    } else {
      goal_offset_perpendicular[0] =
          +goal_offset_xy[1] * goal_move_distance_side;
      goal_offset_perpendicular[1] =
          -goal_offset_xy[0] * goal_move_distance_side;
    }

    double goal_offset[3] = {
        goal_offset_forward[0] + goal_offset_perpendicular[0],
        goal_offset_forward[1] + goal_offset_perpendicular[1],
        0.0,
    };

    double new_goal_position[3] = {
        skateboard_position[0] + goal_offset[0],
        skateboard_position[1] + goal_offset[1],
        goal_position[2],
    };
    mju_copy3(d->mocap_pos + 3 * goal_mocapid, new_goal_position);
  }
}

// compute interpolation between mocap frames
std::tuple<int, int, double, double> ComputeInterpolationValues(double index,
                                                                int max_index) {
  int index_0 = std::floor(std::clamp(index, 0.0, (double)max_index));
  int index_1 = std::min(index_0 + 1, max_index);

  double weight_1 = std::clamp(index, 0.0, (double)max_index) - index_0;
  double weight_0 = 1.0 - weight_1;

  return {index_0, index_1, weight_0, weight_1};
}

// Hardcoded constant matching keyframes from CMU mocap dataset.
constexpr double kFps = 30.0;

constexpr int kMotionLengths[] = {
    1,  // pushing
};

// return length of motion trajectory
int MotionLength(int id) { return kMotionLengths[id]; }

// return starting keyframe index for motion
int MotionStartIndex(int id) {
  int start = 0;
  for (int i = 0; i < id; i++) {
    start += MotionLength(i);
  }
  return start;
}

// names for humanoid bodies
const std::array<std::string, 16> body_names = {
    "pelvis",    "head",      "ltoe",  "rtoe",  "lheel",  "rheel",
    "lknee",     "rknee",     "lhand", "rhand", "lelbow", "relbow",
    "lshoulder", "rshoulder", "lhip",  "rhip",
};
const std::array<std::string, 11> track_body_names = {
    "pelvis", "ltoe",      "rtoe",      "lheel", "rheel", "lhand",
    "rhand",  "lshoulder", "rshoulder", "lhip",  "rhip"};
// compute mocap translations and rotations
void move_mocap_poses(mjtNum *result, const mjModel *model, const mjData *data,
                      std::__1::vector<double> parameters, int mode) {
  // todo move residual here

  std::vector<mjtNum> modified_mocap_pos(3 * (model->nmocap - 1));

  // Compute interpolated frame.
  mju_scl(modified_mocap_pos.data(), model->key_mpos + 3 * model->nmocap * mode,
          1, 3 * (model->nmocap - 1));
  double skateboard_center[3];
  int skateboard_body_id_ = mj_name2id(model, mjOBJ_XBODY, "skateboard");

  // move mpos to x,y position of skateboard
  mju_copy(skateboard_center, data->xpos + 3 * skateboard_body_id_, 3);

  // print average center of mpos
  double average_mpos[2] = {0};

  // get average center of mpos
  for (int i = 0; i < model->nmocap - 1; i++) {
    average_mpos[0] += modified_mocap_pos[3 * i + 0];
    average_mpos[1] += modified_mocap_pos[3 * i + 1];

    modified_mocap_pos[3 * i + 0] += skateboard_center[0];
    modified_mocap_pos[3 * i + 1] += skateboard_center[1];
    modified_mocap_pos[3 * i + 2] += skateboard_center[2] - 0.1;
  }
  average_mpos[0] /= model->nmocap - 1;
  average_mpos[1] /= model->nmocap - 1;

  // subtract the difference between average_mpos and skateboard_center
  for (int i = 0; i < model->nmocap - 1; i++) {
    modified_mocap_pos[3 * i + 0] -= average_mpos[0];
    modified_mocap_pos[3 * i + 1] -= average_mpos[1];
  }

  for (int i = 0; i < model->nmocap - 1; i++) {
    modified_mocap_pos[3 * i + 0] -= 0.1;
    // mocap_pos_0[3 * i + 1] -= 0.+;
  }
  double amplitude_z = parameters[mjpc::ParameterIndex(model, "Amplitude_z")];
  double amplitude_y = parameters[mjpc::ParameterIndex(model, "Amplitude_y")];
  double frequency_z = parameters[mjpc::ParameterIndex(model, "Frequency_z")];
  double frequency_y = parameters[mjpc::ParameterIndex(model, "Frequency_y")];
  double phase_z = parameters[mjpc::ParameterIndex(model, "Phase_z")];
  double phase_y = parameters[mjpc::ParameterIndex(model, "Phase_y")];
  double offset_z = parameters[mjpc::ParameterIndex(model, "Offset_z")];
  double offset_y = parameters[mjpc::ParameterIndex(model, "Offset_y")];

  double time = data->time;

  int mocap_body_id_ltoe = mj_name2id(model, mjOBJ_BODY, "mocap[ltoe]");
  int mocap_body_id_lheel = mj_name2id(model, mjOBJ_BODY, "mocap[lheel]");
  int body_mocapid_ltoe = model->body_mocapid[mocap_body_id_ltoe];
  int body_mocapid_lheel = model->body_mocapid[mocap_body_id_lheel];

  double left_toe_pos[3] = {0.0, 0.0, 0.0};
  double left_heel_pos[3] = {0.0, 0.0, 0.0};
  mju_copy(left_toe_pos, model->key_mpos + 3 * body_mocapid_ltoe, 3);
  mju_copy(left_heel_pos, model->key_mpos + 3 * body_mocapid_lheel, 3);

  double left_foot_z_ref =
      amplitude_z * sin(2 * M_PI * frequency_z * time + phase_z) - offset_z;
  double left_foot_y_toe_ref =
      amplitude_y * sin(2 * M_PI * frequency_y * time + phase_y) +
      left_toe_pos[1] + offset_y;
  // double left_foot_y_heel_ref = amplitude_y * sin(2 * M_PI * frequency_y *
  // time + phase_y) + left_heel_pos[1] + offset_y;

  double left_toe_new_pos = left_foot_y_toe_ref;
  // double left_heel_new_pos = left_foot_y_heel_ref;

  modified_mocap_pos[3 * body_mocapid_ltoe + 1] += left_toe_new_pos;
  modified_mocap_pos[3 * body_mocapid_lheel + 1] =
      modified_mocap_pos[3 * body_mocapid_ltoe + 1] - 0.2;
  modified_mocap_pos[3 * body_mocapid_ltoe + 2] =
      left_foot_z_ref + left_toe_pos[2];
  modified_mocap_pos[3 * body_mocapid_lheel + 2] =
      left_foot_z_ref + left_heel_pos[2];

  // // helper function

  // same for pelvis, hip, sholder, head
  const std::array<std::string, 7> upper_body_names = {
      "mocap[pelvis]", "mocap[lhip]",      "mocap[rhip]",     "mocap[lknee]",
      "mocap[head]",   "mocap[lshoulder]", "mocap[rshoulder]"};
  // scale for each
  const std::array<double, 7> upper_body_scale_y = {-0.25, -0.5, -0.5, -1,
                                                    1.3,   1.3,  1.3};
  const std::array<double, 7> upper_body_scale_z = {-0.05, 0.05,  0.05, -0.1,
                                                    -0.15, -0.15, -0.15};
  for (int i = 0; i < upper_body_names.size(); i++) {
    int mocap_body_id =
        mj_name2id(model, mjOBJ_BODY, upper_body_names[i].c_str());
    int body_mocapid = model->body_mocapid[mocap_body_id];
    double body_pos[3] = {0.0, 0.0, 0.0};
    mju_copy(body_pos, model->key_mpos + 3 * body_mocapid, 3);
    double body_y_ref = -upper_body_scale_y[i] * amplitude_y * 0.5 *
                            sin(2 * M_PI * frequency_y * time + phase_y) +
                        body_pos[1] + offset_y + 0.2;
    double body_z_ref =
        -upper_body_scale_z[i] * sin(2 * M_PI * frequency_y * time + phase_y);
    modified_mocap_pos[3 * body_mocapid + 1] += body_y_ref;
    modified_mocap_pos[3 * body_mocapid + 2] += body_z_ref;
  }

  double skateboard_heading = 0.0;
  double skateboard_xmat[9];
  mju_copy(skateboard_xmat, data->xmat + 9 * skateboard_body_id_, 9);
  skateboard_heading = atan2(skateboard_xmat[3], skateboard_xmat[0]);
  skateboard_heading -= M_PI / 2.0;

  int goal_id = mj_name2id(model, mjOBJ_XBODY, "goal");
  if (goal_id < 0) mju_error("body 'goal' not found");

  int goal_mocap_id_ = model->body_mocapid[goal_id];
  if (goal_mocap_id_ < 0) mju_error("body 'goal' is not mocap");

  // get goal position
  double *goal_pos = data->mocap_pos + 3 * goal_mocap_id_;

  // Get goal heading from board position
  double goal_heading = atan2(goal_pos[1] - skateboard_center[1],
                              goal_pos[0] - skateboard_center[0]) -
                        M_PI / 2.0;
  ;

  // Calculate heading error using sine function
  double heading_error = sin(goal_heading - skateboard_heading) / 3;

  // Rotate the pixels in 3D space around the Z-axis (board_center)
  double mocap_tilt = parameters[mjpc::ParameterIndex(model, "Tilt ratio")];
  // # TODO(eliasmikkola): fix ParameterIndex not working (from utilities.h)
  // tilt angle max is PI/3
  double tilt_angle =
      (mju_min(0.5, mju_max(-0.5, heading_error)) * M_PI / 2.0) * mocap_tilt;
  // tilt_angle = 0.0;
  for (int i = 0; i < model->nmocap - 1; i++) {
    // Get the pixel position relative to the board_center
    double rel_x = modified_mocap_pos[3 * i + 0] - skateboard_center[0];
    double rel_y = modified_mocap_pos[3 * i + 1] - skateboard_center[1];

    // perform rotation of tilt_angle around the Y-axis
    double rotated_z = rel_x * sin(tilt_angle) +
                       modified_mocap_pos[3 * i + 2] * cos(tilt_angle);
    rel_x = rel_x * cos(tilt_angle) -
            modified_mocap_pos[3 * i + 2] * sin(tilt_angle);

    // Perform rotation around the Z-axis
    double rotated_x =
        cos(skateboard_heading) * rel_x - sin(skateboard_heading) * rel_y;
    double rotated_y =
        sin(skateboard_heading) * rel_x + cos(skateboard_heading) * rel_y;

    // Update the rotated pixel positions in modified_mocap_pos
    modified_mocap_pos[3 * i + 0] = skateboard_center[0] + rotated_x;
    modified_mocap_pos[3 * i + 1] = skateboard_center[1] + rotated_y;
    modified_mocap_pos[3 * i + 2] = rotated_z;

    rotated_x =
        cos(skateboard_heading) * rel_x - sin(skateboard_heading) * rel_y;
    rotated_y =
        sin(skateboard_heading) * rel_x + cos(skateboard_heading) * rel_y;
  }
  mju_copy(result, modified_mocap_pos.data(), 3 * (model->nmocap - 1));
}

}  // namespace

namespace mjpc::humanoid {

std::string Pushing::XmlPath() const {
  return GetModelPath("humanoid/skateboard/pushing-task.xml");
}
std::string Pushing::Name() const { return "Humanoid Skateboard Push"; }

/**
 * Humanoid tracking residual.
 *
 * The residual computes the difference between the target positions and the
 * current positions of the humanoid body parts. The target positions are
 * obtained from the mocap data.
 */
std::vector<double> Pushing::ResidualFn::ComputeTrackingResidual(
    const mjModel *model, const mjData *data) const {
  std::vector<mjtNum> mocap_translated(3 * (model->nmocap - 1));

  move_mocap_poses(mocap_translated.data(), model, data, parameters_,
                   current_mode_);

  // ----- get mocap frames ----- //
  // get motion start index
  int start = MotionStartIndex(current_mode_);
  // get motion trajectory length
  int length = MotionLength(current_mode_);
  double current_index = (data->time - reference_time_) * kFps + start;
  int last_key_index = start + length - 1;

  // create a vector to store the residuals
  std::vector<double> residual_to_return;
  // Positions:
  // We interpolate linearly between two consecutive key frames in order to
  // provide smoother signal for pushing.
  int key_index_0, key_index_1;
  double weight_0, weight_1;
  std::tie(key_index_0, key_index_1, weight_0, weight_1) =
      ComputeInterpolationValues(current_index, last_key_index);

  // ----- position ----- //
  // Compute interpolated frame.
  auto get_body_mpos = [&](const std::string &body_name, double result[3]) {
    std::string mocap_body_name = "mocap[" + body_name + "]";
    int mocap_body_id = mj_name2id(model, mjOBJ_BODY, mocap_body_name.c_str());
    assert(0 <= mocap_body_id);
    int body_mocapid = model->body_mocapid[mocap_body_id];
    assert(0 <= body_mocapid);

    // current frame
    mju_scl3(result,
             mocap_translated.data() + 3 * (model->nmocap - 1) * key_index_0 +
                 3 * body_mocapid,
             weight_0);

    // next frame
    mju_addToScl3(result,
                  mocap_translated.data() +
                      3 * (model->nmocap - 1) * key_index_1 + 3 * body_mocapid,
                  weight_1);
  };

  auto get_body_sensor_pos = [&](const std::string &body_name,
                                 double result[3]) {
    std::string pos_sensor_name = "tracking_pos[" + body_name + "]";
    double *sensor_pos =
        mjpc::SensorByName(model, data, pos_sensor_name.c_str());
    mju_copy3(result, sensor_pos);
  };

  // compute marker and sensor averages
  double avg_mpos[3] = {0};
  double avg_sensor_pos[3] = {0};
  int num_body = 0;
  for (const auto &body_name : body_names) {
    double body_mpos[3];
    double body_sensor_pos[3];
    get_body_mpos(body_name, body_mpos);
    mju_addTo3(avg_mpos, body_mpos);
    get_body_sensor_pos(body_name, body_sensor_pos);
    mju_addTo3(avg_sensor_pos, body_sensor_pos);
    num_body++;
  }
  mju_scl3(avg_mpos, avg_mpos, 1.0 / num_body);
  mju_scl3(avg_sensor_pos, avg_sensor_pos, 1.0 / num_body);

  // residual_to_return for averages
  residual_to_return.push_back(avg_mpos[0] - avg_sensor_pos[0]);
  residual_to_return.push_back(avg_mpos[1] - avg_sensor_pos[1]);
  residual_to_return.push_back(avg_mpos[2] - avg_sensor_pos[2]);

  for (const auto &body_name : track_body_names) {
    double body_mpos[3];
    get_body_mpos(body_name, body_mpos);

    // current position
    double body_sensor_pos[3];
    get_body_sensor_pos(body_name, body_sensor_pos);

    mju_subFrom3(body_mpos, avg_mpos);
    mju_subFrom3(body_sensor_pos, avg_sensor_pos);

    residual_to_return.push_back(body_mpos[0] - body_sensor_pos[0]);
    residual_to_return.push_back(body_mpos[1] - body_sensor_pos[1]);
    residual_to_return.push_back(body_mpos[2] - body_sensor_pos[2]);
  }

  // ----- velocity ----- //
  for (const auto &body_name : track_body_names) {
    std::string mocap_body_name = "mocap[" + body_name + "]";
    std::string linvel_sensor_name = "tracking_linvel[" + body_name + "]";
    int mocap_body_id = mj_name2id(model, mjOBJ_BODY, mocap_body_name.c_str());
    assert(0 <= mocap_body_id);
    int body_mocapid = model->body_mocapid[mocap_body_id];
    assert(0 <= body_mocapid);

    // Compute finite-difference velocity for x, y, z components
    double fd_velocity[3];  // Finite-difference velocity
    for (int i = 0; i < 3; ++i) {
      fd_velocity[i] = (model->key_mpos[3 * model->nmocap * key_index_1 +
                                        3 * body_mocapid + i] -
                        model->key_mpos[3 * model->nmocap * key_index_0 +
                                        3 * body_mocapid + i]) *
                       kFps;
    }

    // Get current velocity from sensor
    double *sensor_linvel =
        mjpc::SensorByName(model, data, linvel_sensor_name.c_str());

    for (int i = 0; i < 3; ++i) {
      double velocity_residual = fd_velocity[i] - sensor_linvel[i];
      residual_to_return.push_back(velocity_residual);
    }
  }
  return residual_to_return;
}
// ----- COM xy velocity should be compared to linear_velocity_global ----- //

std::array<double, 2> Pushing::ResidualFn::ComputeComVelXyResidual(
    const mjModel *model, const mjData *data) const {
  double *linear_velocity_global =
      mjpc::SensorByName(model, data, "skateboard_framelinvel");
  double *com_velocity = mjpc::SensorByName(model, data, "torso_subtreelinvel");
  double *com_difference = new double[2];
  com_difference[0] = (linear_velocity_global[0] - com_velocity[0]);
  com_difference[1] = (linear_velocity_global[1] - com_velocity[1]);

  return {com_difference[0], com_difference[1]};
}

// compute left foot contact force
std::array<double, 1> Pushing::ResidualFn::ComputeFootContactForceResidual(
    const mjModel *model, const mjData *data) const {
  // left foot contact forces, 6d force
  int left_heel_id_ = mj_name2id(model, mjOBJ_GEOM, "foot1_left");
  int left_toe_id_ = mj_name2id(model, mjOBJ_GEOM, "foot2_left");
  int floor_id = mj_name2id(model, mjOBJ_GEOM, "floor");

  int left_heel_contact_id = -1;  // Initialize to an invalid value
  int left_toe_contact_id = -1;   // Initialize to an invalid value
  int found = 0;

  int board_nose_id = mj_name2id(model, mjOBJ_GEOM, "board-nose");
  int board_tail_id = mj_name2id(model, mjOBJ_GEOM, "board-tail");

  int left_foot_touch_board = 0;
  int right_foot_ground_id = -1;

  for (int i = 0; i < data->ncon; i++) {
    std::string name = mj_id2name(model, mjOBJ_XBODY, data->contact[i].geom1);
    std::string name2 = mj_id2name(model, mjOBJ_XBODY, data->contact[i].geom2);
    // print contact id to name
    if ((data->contact[i].geom1 == left_heel_id_ &&
         data->contact[i].geom2 == floor_id) ||
        (data->contact[i].geom2 == left_heel_id_ &&
         data->contact[i].geom1 == floor_id)) {
      left_toe_contact_id = i;
      // print geom names
      // printf("left_heel_contact_id: %s , %s\n", name.c_str(), name2.c_str());
      found++;
    } else if ((data->contact[i].geom1 == left_toe_id_ &&
                data->contact[i].geom2 == floor_id) ||
               (data->contact[i].geom2 == left_toe_id_ &&
                data->contact[i].geom1 == floor_id)) {
      left_heel_contact_id = i;
      // printf("left_heel_contact_id: %s , %s\n", name.c_str(), name2.c_str());
      found++;
    }

    if (found == 2) {
      break;
    }
  }

  double left_toe_contact_force[6];
  double left_heel_contact_force[6];

  mj_contactForce(model, data, left_toe_contact_id, left_toe_contact_force);
  mj_contactForce(model, data, left_heel_contact_id, left_heel_contact_force);

  // printf("left_foot_contact_force: %f , %f , %f , %f , %f , %f\n",
  // left_foot_contact_force[0], left_foot_contact_force[1],
  // left_foot_contact_force[2], left_foot_contact_force[3],
  // left_foot_contact_force[4], left_foot_contact_force[5]); sum all the forces
  double left_foot_contact_force[6];
  mju_add3(left_foot_contact_force, left_toe_contact_force,
           left_heel_contact_force);

  double force_abs_sum =
      mju_abs(left_foot_contact_force[0]) +
      mju_abs(left_foot_contact_force[1]) +
      mju_abs(
          left_foot_contact_force[2]);  //+ mju_abs(left_foot_contact_force[2]);
  // check if mocap_pos_0 of left foot is near z = 0
  int mocap_body_id_ltoe = mj_name2id(model, mjOBJ_BODY, "mocap[ltoe]");
  int body_mocapid_ltoe = model->body_mocapid[mocap_body_id_ltoe];
  double should_add_force =
      (data->mocap_pos[3 * body_mocapid_ltoe + 2] <= 0.05);
  // Applying a sigmoid function to map input to a value between 0 and 1

  double left_foot_contact_force_error =
      (1.0 / (1.0 + std::exp((force_abs_sum - 500.0) / 80.0))) *
      should_add_force;
  return {left_foot_contact_force_error};
}

/**
 * Humanoid foot positions residual.
 *
 * The board includes two sensors for tracking the position of the right and
 * left foot. The residual is computed as the difference between these target
 * positions and the current positions of the feet.
 */
std::array<double, 6> Pushing::ResidualFn::ComputeFootPositionsResidual(
    const mjModel *model, const mjData *data) const {
  // Right foot on the front plate
  double *humanoid_rtoe_pos_sensor =
      mjpc::SensorByName(model, data, "tracking_pos[rtoe]");
  double *board_front_plate_pos =
      mjpc::SensorByName(model, data, "track-front-plate");

  // Left foot on the tail.
  double *humanoid_ltoe_pos_sensor =
      mjpc::SensorByName(model, data, "tracking_pos[ltoe]");
  double *board_tail_pos = mjpc::SensorByName(model, data, "track-tail");

  return {
      humanoid_rtoe_pos_sensor[0] - board_front_plate_pos[0],
      humanoid_rtoe_pos_sensor[1] - board_front_plate_pos[1],
      humanoid_rtoe_pos_sensor[2] - board_front_plate_pos[2],
      humanoid_ltoe_pos_sensor[0] - board_tail_pos[0],
      humanoid_ltoe_pos_sensor[1] - board_tail_pos[1],
      humanoid_ltoe_pos_sensor[2] - board_tail_pos[2],
  };
}

/**
 * Skateboard heading residual.
 *
 * The skateboard heading residual is computed as the difference between the
 * target heading and the current heading of the skateboard. The target heading
 * always points from the board to the goal.
 */
std::array<double, 2> Pushing::ResidualFn::ComputeBoardHeadingResidual(
    const mjModel *model, const mjData *data) const {
  std::array<double, 2> skateboard_heading = {
      data->xmat[9 * skateboard_body_id_ + 0],
      data->xmat[9 * skateboard_body_id_ + 3],
  };

  std::array<mjtNum, 2> goal_position = {
      data->mocap_pos[3 * goal_body_mocap_id_ + 0],
      data->mocap_pos[3 * goal_body_mocap_id_ + 1],
  };

  std::array<mjtNum, 2> skateboard_position = {
      data->xpos[3 * skateboard_body_id_ + 0],
      data->xpos[3 * skateboard_body_id_ + 1],
  };

  std::array<mjtNum, 2> board_to_goal = {
      goal_position[0] - skateboard_position[0],
      goal_position[1] - skateboard_position[1],
  };

  mju_normalize(skateboard_heading.data(), 2);
  mju_normalize(board_to_goal.data(), 2);

  mjtNum skateboard_yaw = atan2(skateboard_heading[1], skateboard_heading[0]);
  mjtNum target_yaw = atan2(board_to_goal[1], board_to_goal[0]);

  std::array<double, 2> result = {
      skateboard_heading[0] - board_to_goal[0],
      skateboard_heading[1] - board_to_goal[1],
  };

  return result;
}

/**
 * Skateboard velocity residual.
 *
 * The skateboard velocity residual is computed as the difference between the
 * target velocity and the current velocity of the skateboard. The target
 * velocity for the longitudinal axis is given by the `Velocity` parameter,
 * while the target velocity for the lateral and vertical axes is zero.
 */
std::array<mjtNum, 3> Pushing::ResidualFn::ComputeBoardVelocityResidual(
    const mjModel *model, const mjData *data) const {
  std::array<mjtNum, 3> skateboard_linear_velocity_target = {
      parameters_[ParameterIndex(model, "Velocity")],
      0.0,
      0.0,
  };
  double *skateboard_framelinvel =
      SensorByName(model, data, "skateboard_framelinvel");
  std::array<mjtNum, 3> skateboard_linear_velocity_global = {
      skateboard_framelinvel[0],
      skateboard_framelinvel[1],
      skateboard_framelinvel[2],
  };

  std::array<mjtNum, 9> skateboard_xmat;
  mju_copy(skateboard_xmat.data(), data->xmat + 9 * skateboard_body_id_, 9);

  // Transform the global velocity to local velocity
  std::array<mjtNum, 3> skateboard_linear_velocity_local;
  mju_rotVecMatT(skateboard_linear_velocity_local.data(),
                 skateboard_linear_velocity_global.data(),
                 skateboard_xmat.data());

  // NOTE: we add small tolerance to the longitudinal residual here.
  std::array<mjtNum, 3> result = {
      skateboard_linear_velocity_target[0] -
          skateboard_linear_velocity_local[0] - 0.03,
      skateboard_linear_velocity_target[1] -
          skateboard_linear_velocity_local[1],
      skateboard_linear_velocity_target[2] -
          skateboard_linear_velocity_global[2],
  };

  return result;
}

void Pushing::ModifyScene(const mjModel *model, const mjData *data,
                          mjvScene *scene) const {}

void Pushing::ResetLocked(const mjModel *model) {
  residual_.skateboard_xbody_id_ = mj_name2id(model, mjOBJ_XBODY, "skateboard");
  if (residual_.skateboard_xbody_id_ < 0)
    mju_error("xbody 'skateboard' not found");

  residual_.skateboard_body_id_ = mj_name2id(model, mjOBJ_BODY, "skateboard");
  if (residual_.skateboard_body_id_ < 0)
    mju_error("body 'skateboard' not found");

  // TODO(hartikainen): `mjOBJ_XBODY` or `mjOBJ_BODY`? Does it matter?
  residual_.goal_body_id_ = mj_name2id(model, mjOBJ_XBODY, "goal");
  if (residual_.goal_body_id_ < 0) mju_error("body 'goal' not found");
  residual_.goal_body_mocap_id_ = model->body_mocapid[residual_.goal_body_id_];
  if (residual_.goal_body_mocap_id_ < 0) mju_error("body 'goal' is not mocap");
}

void Pushing::ResidualFn::Residual(const mjModel *model, const mjData *data,
                                   double *residual) const {
  int counter = 0;

  int n_humanoid_joints = model->nv - 6 - 6 - 7;
  mju_copy(residual + counter, data->qvel + 6, n_humanoid_joints);

  counter += n_humanoid_joints;

  mju_copy(&residual[counter], data->ctrl, model->nu);
  counter += model->nu;

  auto tracking_residual = ComputeTrackingResidual(model, data);
  mju_copy(residual + counter, tracking_residual.data(),
           tracking_residual.size());
  counter += tracking_residual.size();

  auto foot_positions_residual = ComputeFootPositionsResidual(model, data);
  mju_copy(residual + counter, foot_positions_residual.data(),
           foot_positions_residual.size());
  counter += foot_positions_residual.size();

  auto board_heading_residual = ComputeBoardHeadingResidual(model, data);
  mju_copy(residual + counter, board_heading_residual.data(),
           board_heading_residual.size());
  counter += board_heading_residual.size();

  auto board_velocity_residual = ComputeBoardVelocityResidual(model, data);
  mju_copy(residual + counter, board_velocity_residual.data(),
           board_velocity_residual.size());
  counter += board_velocity_residual.size();

  auto foot_contact_force_residual =
      ComputeFootContactForceResidual(model, data);
  mju_copy(residual + counter, foot_contact_force_residual.data(),
           foot_contact_force_residual.size());
  counter += foot_contact_force_residual.size();

  auto com_vel_xy_residual = ComputeComVelXyResidual(model, data);
  mju_copy(residual + counter, com_vel_xy_residual.data(),
           com_vel_xy_residual.size());
  counter += com_vel_xy_residual.size();
  // TODO(eliasmikkola): fill missing skateboard residuals

  CheckSensorDim(model, counter);
}

void Pushing::TransitionLocked(mjModel *model, mjData *d) {
  assert(residual_.skateboard_body_id_ >= 0);
  int start = MotionStartIndex(mode);
  int length = MotionLength(mode);

  // check for motion switch
  if (residual_.current_mode_ != mode || d->time == 0.0) {
    residual_.current_mode_ = mode;       // set motion id
    residual_.reference_time_ = d->time;  // set reference time

    // set initial state
    mju_copy(d->qpos, model->key_qpos + model->nq * start, model->nq);
    mju_copy(d->qvel, model->key_qvel + model->nv * start, model->nv);
  }

  // indices
  double current_index = (d->time - residual_.reference_time_) * kFps + start;
  int last_key_index = start + length - 1;

  // Positions:
  // We interpolate linearly between two consecutive key frames in order to
  // provide smoother signal for pushing.
  int key_index_0, key_index_1;
  double weight_0, weight_1;
  std::tie(key_index_0, key_index_1, weight_0, weight_1) =
      ComputeInterpolationValues(current_index, last_key_index);

  mj_markStack(d);

  mjtNum *mocap_pos_0 = mj_stackAllocNum(d, 3 * (model->nmocap - 1));
  mjtNum *mocap_pos_1 = mj_stackAllocNum(d, 3 * (model->nmocap - 1));

  // Compute interpolated frame.
  mju_scl(mocap_pos_0, model->key_mpos + 3 * model->nmocap * key_index_0,
          weight_0, 3 * (model->nmocap - 1));

  mju_scl(mocap_pos_1, model->key_mpos + 3 * model->nmocap * key_index_1,
          weight_1, 3 * (model->nmocap - 1));

  mju_copy(d->mocap_pos, mocap_pos_0, 3 * (model->nmocap - 1));
  mju_addTo(d->mocap_pos, mocap_pos_1, 3 * (model->nmocap - 1));

  mjtNum *mocap_pos_result = mj_stackAllocNum(d, 3 * (model->nmocap - 1));
  move_mocap_poses(mocap_pos_result, model, d, parameters, mode);
  mju_copy(d->mocap_pos, mocap_pos_result, 3 * (model->nmocap - 1));
  move_goal(model, d, parameters, mode);
  mj_freeStack(d);
}

}  // namespace mjpc::humanoid
