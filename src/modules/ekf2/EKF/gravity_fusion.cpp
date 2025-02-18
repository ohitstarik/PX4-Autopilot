/****************************************************************************
 *
 *   Copyright (c) 2023 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file gravity_fusion.cpp
 * Fuse observations from the gravity vector to constrain roll
 * and pitch (a la complementary filter).
 *
 * @author Daniel M. Sahu <danielmohansahu@gmail.com>
 */

#include "ekf.h"
#include <ekf_derivation/generated/compute_gravity_innov_var_and_k_and_h.h>

#include <mathlib/mathlib.h>

void Ekf::controlGravityFusion(const imuSample &imu)
{
	// get raw accelerometer reading at delayed horizon and expected measurement noise (gaussian)
	const Vector3f measurement = imu.delta_vel / imu.delta_vel_dt - _state.accel_bias;
	const float measurement_var = math::max(sq(_params.gravity_noise), sq(0.01f));

	const float accel_lpf_norm_sq = _accel_vec_filt.norm_squared();
	const float accel_norm_sq = measurement.norm_squared();
	const float upper_accel_limit = CONSTANTS_ONE_G * 1.1f;
	const float lower_accel_limit = CONSTANTS_ONE_G * 0.9f;
	const bool accel_lpf_norm_good = (accel_lpf_norm_sq > sq(lower_accel_limit)) && (accel_lpf_norm_sq < sq(upper_accel_limit));
	const bool accel_norm_good = (accel_norm_sq > sq(lower_accel_limit)) && (accel_norm_sq < sq(upper_accel_limit));

	// fuse gravity observation if our overall acceleration isn't too big
	_control_status.flags.gravity_vector = (_params.imu_ctrl & static_cast<int32_t>(ImuCtrl::GravityVector))
					       && ((accel_lpf_norm_good && accel_norm_good) || _control_status.flags.vehicle_at_rest)
					       && !isHorizontalAidingActive();

	// calculate kalman gains and innovation variances
	Vector3f innovation; // innovation of the last gravity fusion observation (m/s**2)
	Vector3f innovation_variance;
	VectorState Kx, Ky, Kz; // Kalman gain vectors
	sym::ComputeGravityInnovVarAndKAndH(
		_state.vector(), P, measurement, measurement_var, FLT_EPSILON,
		&innovation, &innovation_variance, &Kx, &Ky, &Kz);

	// fill estimator aid source status
	resetEstimatorAidStatus(_aid_src_gravity);
	_aid_src_gravity.timestamp_sample = imu.time_us;
	measurement.copyTo(_aid_src_gravity.observation);

	for (auto &var : _aid_src_gravity.observation_variance) {
		var = measurement_var;
	}

	innovation.copyTo(_aid_src_gravity.innovation);
	innovation_variance.copyTo(_aid_src_gravity.innovation_variance);

	float innovation_gate = 1.f;
	setEstimatorAidStatusTestRatio(_aid_src_gravity, innovation_gate);

	const bool accel_clipping = imu.delta_vel_clipping[0] || imu.delta_vel_clipping[1] || imu.delta_vel_clipping[2];

	if (_control_status.flags.gravity_vector && !_aid_src_gravity.innovation_rejected && !accel_clipping) {
		// perform fusion for each axis
		_aid_src_gravity.fused = measurementUpdate(Kx, innovation_variance(0), innovation(0))
					 && measurementUpdate(Ky, innovation_variance(1), innovation(1))
					 && measurementUpdate(Kz, innovation_variance(2), innovation(2));

		if (_aid_src_gravity.fused) {
			_aid_src_gravity.time_last_fuse = imu.time_us;
		}
	}
}
