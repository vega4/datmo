/*
 * Copyright (c) 2014, 2015, 2016, Charles River Analytics, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimer in the documentation and/or other materials provided
 * with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "ukf.h"
#include "filter_common.h"
#include <limits>
#include <Eigen/Cholesky>
#include <vector>
#include <ros/console.h>

namespace RobotLocalization
{
  Ukf::Ukf(std::vector<double> args) :
    FilterBase(),  // Must initialize filter base!
    uncorrected_(true)
  {
    assert(args.size() == 3);

    double alpha = args[0];
    double kappa = args[1];
    double beta = args[2];
    

    size_t sigmaCount = (STATE_SIZE << 1) + 1;
    sigmaPoints_.resize(sigmaCount, Eigen::VectorXd(STATE_SIZE));
    sigmaPointsPrior_.resize(sigmaCount, Eigen::VectorXd(STATE_SIZE));

    // Prepare constants
    lambda_ = alpha * alpha * (STATE_SIZE + kappa) - STATE_SIZE;

    stateWeights_.resize(sigmaCount);
    covarWeights_.resize(sigmaCount);

    stateWeights_[0] = lambda_ / (STATE_SIZE + lambda_);
    covarWeights_[0] = stateWeights_[0] + (1 - (alpha * alpha) + beta);
    sigmaPoints_[0].setZero();
    sigmaPointsPrior_[0].setZero();
    for (size_t i = 1; i < sigmaCount; ++i)
    {
      sigmaPoints_[i].setZero();
      sigmaPointsPrior_[i].setZero();
      stateWeights_[i] =  1 / (2 * (STATE_SIZE + lambda_));
      covarWeights_[i] = stateWeights_[i];
    }
  }

  Ukf::Ukf() :
    FilterBase(),  // Must initialize filter base!
    uncorrected_(true)
  {
  }

  Ukf::~Ukf()
  {
  }

  void Ukf::correct_ctrm(const Measurement &measurement)
  {
    //ROS_WARN_STREAM("---------------------- Ukf::correct ----------------------\n" <<
       //"State is:\n" << state_ <<
       //"\nMeasurement is:\n" << measurement.measurement_ << "\n");
       //"\nMeasurement covariance is:\n" << measurement.covariance_ <<
       

    // In our implementation, it may be that after we call predict once, we call correct
    // several times in succession (multiple measurements with different time stamps). In
    // that event, the sigma points need to be updated to reflect the current state.
    // Throughout prediction and correction, we attempt to maximize efficiency in Eigen.
    if (!uncorrected_)
    {
      //ROS_INFO_STREAM("It is not uncorrected!!");
      // Take the square root of a small fraction of the estimateErrorCovariance_ using LL' decomposition
      weightedCovarSqrt_ = ((STATE_SIZE + lambda_) * estimateErrorCovariance_).llt().matrixL();

      // Compute sigma points

      // First sigma point is the current state
      sigmaPoints_[0] = state_;

      // Next STATE_SIZE sigma points are state + weightedCovarSqrt_[ith column]
      // STATE_SIZE sigma points after that are state - weightedCovarSqrt_[ith column]
      for (size_t sigmaInd = 0; sigmaInd < STATE_SIZE; ++sigmaInd)
      {
        sigmaPoints_[sigmaInd + 1] = state_ + weightedCovarSqrt_.col(sigmaInd);
        sigmaPoints_[sigmaInd + 1 + STATE_SIZE] = state_ - weightedCovarSqrt_.col(sigmaInd);
      }
    }

    // We don't want to update everything, so we need to build matrices that only update
    // the measured parts of our state vector

    // First, determine how many state vector values we're updating
    std::vector<size_t> updateIndices;
    for (size_t i = 0; i < measurement.updateVector_.size(); ++i)
    {
      if (measurement.updateVector_[i])
      {
         //Handle nan and inf values in measurements
        //if (std::isnan(measurement.measurement_(i)))
        //{
          //ROS_WARN_STREAM("Value at index " << i << " was nan. Excluding from update.\n");
        //}
        //else if (std::isinf(measurement.measurement_(i)))
        //{
          //ROS_WARN_STREAM("Value at index " << i << " was inf. Excluding from update.\n");
        //}
        //else
        //{
          //updateIndices.push_back(i);
        //}
        updateIndices.push_back(i);
      }
    }
    //ROS_INFO_STREAM("updateVector: "<<measurement.updateVector_.size()<<"indices: "<<updateIndices.size());

    //ROS_WARN_STREAM("Update indices are:\n" << updateIndices << "\n");

    size_t updateSize = updateIndices.size();

    // Now set up the relevant matrices
    Eigen::VectorXd stateSubset(updateSize);                              // x (in most literature)
    Eigen::VectorXd measurementSubset(updateSize);                        // z
    Eigen::MatrixXd measurementCovarianceSubset(updateSize, updateSize);  // R
    Eigen::MatrixXd stateToMeasurementSubset(updateSize, STATE_SIZE);     // H
    Eigen::MatrixXd kalmanGainSubset(STATE_SIZE, updateSize);             // K
    Eigen::VectorXd innovationSubset(updateSize);                         // z - Hx
    Eigen::VectorXd predictedMeasurement(updateSize);
    Eigen::VectorXd sigmaDiff(updateSize);
    Eigen::MatrixXd predictedMeasCovar(updateSize, updateSize);
    Eigen::MatrixXd crossCovar(STATE_SIZE, updateSize);

    std::vector<Eigen::VectorXd> sigmaPointMeasurements(sigmaPoints_.size(), Eigen::VectorXd(updateSize));

    stateSubset.setZero();
    measurementSubset.setZero();
    measurementCovarianceSubset.setZero();
    stateToMeasurementSubset.setZero();
    kalmanGainSubset.setZero();
    innovationSubset.setZero();
    predictedMeasurement.setZero();
    predictedMeasCovar.setZero();
    crossCovar.setZero();

    // Now build the sub-matrices from the full-sized matrices
    for (size_t i = 0; i < updateSize; ++i)
    {
      measurementSubset(i) = measurement.measurement_(updateIndices[i]);
      stateSubset(i) = state_(updateIndices[i]);

      for (size_t j = 0; j < updateSize; ++j)
      {
        measurementCovarianceSubset(i, j) = measurement.covariance_(updateIndices[i], updateIndices[j]);
      }

      // Handle negative (read: bad) covariances in the measurement. Rather
      // than exclude the measurement or make up a covariance, just take
      // the absolute value.
      //if (measurementCovarianceSubset(i, i) < 0)
      //{
        //ROS_WARN_STREAM("WARNING: Negative covariance for index " << i <<
                 //" of measurement (value is" << measurementCovarianceSubset(i, i) <<
                 //"). Using absolute value...\n");

        //measurementCovarianceSubset(i, i) = ::fabs(measurementCovarianceSubset(i, i));
      //}

      // If the measurement variance for a given variable is very
      // near 0 (as in e-50 or so) and the variance for that
      // variable in the covariance matrix is also near zero, then
      // the Kalman gain computation will blow up. Really, no
      // measurement can be completely without error, so add a small
      // amount in that case.
      //if (measurementCovarianceSubset(i, i) < 1e-9)
      //{
        //measurementCovarianceSubset(i, i) = 1e-9;

        //ROS_WARN_STREAM("WARNING: measurement had very small error covariance for index " <<
                 //updateIndices[i] <<
                 //". Adding some noise to maintain filter stability.\n");
      //}
    }

    // The state-to-measurement function, h, will now be a measurement_size x full_state_size
    // matrix, with ones in the (i, i) locations of the values to be updated
    for (size_t i = 0; i < updateSize; ++i)
    {
      stateToMeasurementSubset(i, updateIndices[i]) = 1;
    }

    //FB_DEBUG("Current state subset is:\n" << stateSubset <<
             //"\nMeasurement subset is:\n" << measurementSubset <<
             //"\nMeasurement covariance subset is:\n" << measurementCovarianceSubset <<
             //"\nState-to-measurement subset is:\n" << stateToMeasurementSubset << "\n");

    // (1) Generate sigma points, use them to generate a predicted measurement
    for (size_t sigmaInd = 0; sigmaInd < sigmaPoints_.size(); ++sigmaInd)
    {
      sigmaPointMeasurements[sigmaInd] = stateToMeasurementSubset * sigmaPoints_[sigmaInd];
      predictedMeasurement.noalias()  += stateWeights_[sigmaInd]  * sigmaPointMeasurements[sigmaInd];
    }

    // (2) Use the sigma point measurements and predicted measurement to compute a predicted
    // measurement covariance matrix P_zz and a state/measurement cross-covariance matrix P_xz.
    for (size_t sigmaInd = 0; sigmaInd < sigmaPoints_.size(); ++sigmaInd)
    {
      sigmaDiff = sigmaPointMeasurements[sigmaInd] - predictedMeasurement;
      predictedMeasCovar.noalias() += covarWeights_[sigmaInd] * (sigmaDiff * sigmaDiff.transpose());
      crossCovar.noalias() += covarWeights_[sigmaInd] * ((sigmaPoints_[sigmaInd] - state_) * sigmaDiff.transpose());
    }

    // (3) Compute the Kalman gain, making sure to use the actual measurement covariance: K = P_xz * (P_zz + R)^-1
    Eigen::MatrixXd invInnovCov = (predictedMeasCovar + measurementCovarianceSubset).inverse();
    kalmanGainSubset = crossCovar * invInnovCov;

    // (4) Apply the gain to the difference between the actual and predicted measurements: x = x + K(z - z_hat)
    innovationSubset = (measurementSubset - predictedMeasurement);

    // Wrap angles in the innovation
    for (size_t i = 0; i < updateSize; ++i)
    {
      //if (updateIndices[i] == StateMemberYaw) 
      //{
        //while (innovationSubset(i) < -PI)
        //{
          //innovationSubset(i) += TAU;
        //}

        //while (innovationSubset(i) > PI)
        //{
          //innovationSubset(i) -= TAU;
        //}
      //}
    }

    // (5) Check Mahalanobis distance of innovation
    //if (checkMahalanobisThreshold(innovationSubset, invInnovCov, measurement.mahalanobisThresh_))
    //{
      state_.noalias() += kalmanGainSubset * innovationSubset;

      // (6) Compute the new estimate error covariance P = P - (K * P_zz * K')
      estimateErrorCovariance_.noalias() -= (kalmanGainSubset * predictedMeasCovar * kalmanGainSubset.transpose());

      //wrapStateAngles();

      // Mark that we need to re-compute sigma points for successive corrections
      uncorrected_ = false;

      //FB_DEBUG("Predicated measurement covariance is:\n" << predictedMeasCovar <<
               //"\nCross covariance is:\n" << crossCovar <<
               //"\nKalman gain subset is:\n" << kalmanGainSubset <<
               //"\nInnovation:\n" << innovationSubset <<
               //"\nCorrected full state is:\n" << state_ <<
               //"\nCorrected full estimate error covariance is:\n" << estimateErrorCovariance_ <<
               //"\n\n---------------------- /Ukf::correct ----------------------\n");
    //}
  }

  void Ukf::predict_ctrm(const double delta)
  {
    //ROS_WARN_STREAM("---------------------- Ukf::predict ----------------------\n" <<
			 //"delta is " << delta <<
			 //"\nstate is " << state_ << "\n");

    // (1) Take the square root of a small fraction of the estimateErrorCovariance_ using LL' decomposition
    weightedCovarSqrt_ = ((STATE_SIZE + lambda_) * estimateErrorCovariance_).llt().matrixL();

    // (2) Compute sigma points *and* pass them through the transfer function to save the extra loop
    // First sigma point is the current state
    sigmaPoints_[0][X] =  state_[X] + (state_[Vx]/state_[Vyaw]) * sin(state_[Vyaw]*delta) - (state_[Vy]/state_[Vyaw])*(1-cos(state_[Vyaw]*delta));
    sigmaPoints_[0][Y] =  state_[Y] + (state_[Vx]/state_[Vyaw]) * (1-cos(state_[Vyaw]*delta)) + (state_[Vy]/state_[Vyaw])*sin(state_[Vyaw]*delta) ;
    sigmaPoints_[0][Vx]=  state_[Vx] * cos(state_[Vyaw]*delta) - state_[Vy] * sin(state_[Vyaw]*delta) ;
    sigmaPoints_[0][Vy]=  state_[Vx] * sin(state_[Vyaw]*delta) + state_[Vy] * cos(state_[Vyaw]*delta) ;
    sigmaPoints_[0][Vyaw]=state_[Vyaw];

    // Next STATE_SIZE sigma points are state + weightedCovarSqrt_[ith column]
    // STATE_SIZE sigma points after that are state - weightedCovarSqrt_[ith column]
    for (size_t sigmaInd = 0; sigmaInd < STATE_SIZE; ++sigmaInd)
    {
      sigmaPointsPrior_[sigmaInd + 1]              = (state_ + weightedCovarSqrt_.col(sigmaInd));
      sigmaPointsPrior_[sigmaInd + 1 + STATE_SIZE] = (state_ - weightedCovarSqrt_.col(sigmaInd));
    }

    for (size_t sigmaInd = 0; sigmaInd < STATE_SIZE; ++sigmaInd)
    {
      sigmaPoints_[sigmaInd+1][X] =  sigmaPointsPrior_[sigmaInd+1][X] + (sigmaPointsPrior_[sigmaInd+1][Vx]/sigmaPointsPrior_[sigmaInd+1][Vyaw]) * sin(sigmaPointsPrior_[sigmaInd+1][Vyaw]*delta) - (sigmaPointsPrior_[sigmaInd+1][Vy]/sigmaPointsPrior_[sigmaInd+1][Vyaw])*(1-cos(sigmaPointsPrior_[sigmaInd+1][Vyaw]*delta));
      sigmaPoints_[sigmaInd+1][Y] =  sigmaPointsPrior_[sigmaInd+1][Y] + (sigmaPointsPrior_[sigmaInd+1][Vx]/sigmaPointsPrior_[sigmaInd+1][Vyaw]) * (1-cos(sigmaPointsPrior_[sigmaInd+1][Vyaw]*delta)) + (sigmaPointsPrior_[sigmaInd+1][Vy]/sigmaPointsPrior_[sigmaInd+1][Vyaw])*sin(sigmaPointsPrior_[sigmaInd+1][Vyaw]*delta) ;
      sigmaPoints_[sigmaInd+1][Vx]=  sigmaPointsPrior_[sigmaInd+1][Vx] * cos(sigmaPointsPrior_[sigmaInd+1][Vyaw]*delta) - sigmaPointsPrior_[sigmaInd+1][Vy] * sin(sigmaPointsPrior_[sigmaInd+1][Vyaw]*delta) ;
      sigmaPoints_[sigmaInd+1][Vy]=  sigmaPointsPrior_[sigmaInd+1][Vx] * sin(sigmaPointsPrior_[sigmaInd+1][Vyaw]*delta) + sigmaPointsPrior_[sigmaInd+1][Vy] * cos(sigmaPointsPrior_[sigmaInd+1][Vyaw]*delta) ;
      sigmaPoints_[sigmaInd+1][Vyaw]=sigmaPointsPrior_[sigmaInd+1][Vyaw];

      sigmaPoints_[sigmaInd+1+STATE_SIZE][X] =  sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][X] + (sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vx]/sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vyaw]) * sin(sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vyaw]*delta) - (sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vy]/sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vyaw])*(1-cos(sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vyaw]*delta));
      sigmaPoints_[sigmaInd+1+STATE_SIZE][Y] =  sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Y] + (sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vx]/sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vyaw]) * (1-cos(sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vyaw]*delta)) + (sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vy]/sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vyaw])*sin(sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vyaw]*delta) ;
      sigmaPoints_[sigmaInd+1+STATE_SIZE][Vx]=  sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vx] * cos(sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vyaw]*delta) - sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vy] * sin(sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vyaw]*delta) ;
      sigmaPoints_[sigmaInd+1+STATE_SIZE][Vy]=  sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vx] * sin(sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vyaw]*delta) + sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vy] * cos(sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vyaw]*delta) ;
      sigmaPoints_[sigmaInd+1+STATE_SIZE][Vyaw]=sigmaPointsPrior_[sigmaInd+1+STATE_SIZE][Vyaw];

      }



    // (3) Sum the weighted sigma points to generate a new state prediction
    state_.setZero();
    for (size_t sigmaInd = 0; sigmaInd < sigmaPoints_.size(); ++sigmaInd)
    {
      state_.noalias() += stateWeights_[sigmaInd] * sigmaPoints_[sigmaInd];
    }

    // (4) Now us the sigma points and the predicted state to compute a predicted covariance
    estimateErrorCovariance_.setZero();
    Eigen::VectorXd sigmaDiff(STATE_SIZE);
    for (size_t sigmaInd = 0; sigmaInd < sigmaPoints_.size(); ++sigmaInd)
    {
      sigmaDiff = (sigmaPoints_[sigmaInd] - state_);
      estimateErrorCovariance_.noalias() += covarWeights_[sigmaInd] * (sigmaDiff * sigmaDiff.transpose());
    }

    // (5) Not strictly in the theoretical UKF formulation, but necessary here
    // to ensure that we actually incorporate the processNoiseCovariance_
    Eigen::MatrixXd *processNoiseCovariance = &processNoiseCovariance_;

    estimateErrorCovariance_.noalias() += delta * (*processNoiseCovariance);

    // Keep the angles bounded
    //wrapStateAngles();

    // Mark that we can keep these sigma points
    uncorrected_ = true;

    //ROS_WARN_STREAM("Predicted state is:\n" << state_ <<
             //"\nPredicted estimate error covariance is:\n" << estimateErrorCovariance_ <<
             //"\n\n--------------------- /Ukf::predict ----------------------\n");
  }

}  // namespace RobotLocalization
