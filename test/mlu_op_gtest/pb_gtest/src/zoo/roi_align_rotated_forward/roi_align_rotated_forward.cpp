/*************************************************************************
 * Copyright (C) [2022] by Cambricon, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *************************************************************************/
#include "roi_align_rotated_forward.h"

#include <algorithm>
#include <string>

namespace mluoptest {

void RoiAlignRotatedForwardExecutor::preCalcForBilinearInterpolate(
    const int height, const int width, const int channel,
    const int pooled_height, const int pooled_width, const int roi_bin_grid_h,
    const int roi_bin_grid_w, const float roi_start_x, const float roi_start_y,
    const float bin_size_h, const float bin_size_w, const float roi_center_x,
    const float roi_center_y, const float cos_theta, const float sin_theta,
    std::vector<PreCalc> &pre_calc) {
  int pre_calc_idx = 0;
  for (int ph = 0; ph < pooled_height; ++ph) {
    for (int pw = 0; pw < pooled_width; ++pw) {
      for (int iy = 0; iy < roi_bin_grid_h; ++iy) {
        const float yy = roi_start_y + ph * bin_size_h +
                         static_cast<float>(iy + 0.5) * bin_size_h /
                             static_cast<float>(roi_bin_grid_h);
        theory_ops_ += 8;  // cur block
        for (int ix = 0; ix < roi_bin_grid_w; ++ix) {
          const float xx = roi_start_x + pw * bin_size_w +
                           static_cast<float>(ix + 0.5) * bin_size_w /
                               static_cast<float>(roi_bin_grid_w);
          float y = yy * cos_theta - xx * sin_theta + roi_center_y;
          float x = yy * sin_theta + xx * cos_theta + roi_center_x;
          theory_ops_ += 16;  // cur block

          if (y < -1.0 || y > height || x < -1.0 || x > width) {
            PreCalc pc{0, 0, 0, 0, 0, 0, 0, 0};
            pre_calc[pre_calc_idx] = pc;
            ++pre_calc_idx;
            continue;
          }

          if (y < 0) y = 0;
          if (x < 0) x = 0;

          int y_low = (int)y;
          int x_low = (int)x;
          int y_high, x_high;
          theory_ops_ += 2;  // cur block

          if (y_low >= height - 1) {
            y_high = y_low = height - 1;
            y = (float)y_low;
            theory_ops_ += 2;  // cur block
          } else {
            y_high = y_low + 1;
            theory_ops_ += 1;  // cur block
          }
          if (x_low >= width - 1) {
            x_high = x_low = width - 1;
            x = (float)x_low;
            theory_ops_ += 2;  // cur block
          } else {
            x_high = x_low + 1;
            theory_ops_ += 1;  // cur block
          }

          float ly = y - y_low;
          float lx = x - x_low;
          float hy = 1. - ly, hx = 1. - lx;
          float w1 = hy * hx, w2 = hy * lx, w3 = ly * hx, w4 = ly * lx;

          PreCalc pc;
          pc.pos1 = (y_low * width + x_low) * channel;
          pc.pos2 = (y_low * width + x_high) * channel;
          pc.pos3 = (y_high * width + x_low) * channel;
          pc.pos4 = (y_high * width + x_high) * channel;
          pc.w1 = w1;
          pc.w2 = w2;
          pc.w3 = w3;
          pc.w4 = w4;
          pre_calc[pre_calc_idx] = pc;
          // next stmt not count theory_ops_
          ++pre_calc_idx;
          theory_ops_ += 20;  // cur block
        }
      }
    }
  }
}

void RoiAlignRotatedForwardExecutor::paramCheck() {
  if (!parser_->getProtoNode()->has_roi_align_rotated_forward_param()) {
    LOG(ERROR) << "mluOpRoiAlignRotatedForward: lose roi_align_rotated_param. ";
    throw std::invalid_argument(std::string(__FILE__) + " +" +
                                std::to_string(__LINE__));
  }
  if (parser_->getInputNum() != 2) {
    LOG(ERROR) << "mluOpRoiAlignRotatedForward: tensor input number is wrong.";
    throw std::invalid_argument(std::string(__FILE__) + " +" +
                                std::to_string(__LINE__));
  }
  if (parser_->getOutputNum() != 1) {
    LOG(ERROR) << "output number is" << parser_->getOutputNum();
    LOG(ERROR) << "mluOpRoiAlignRotatedForward: tensor output number is wrong.";
    throw std::invalid_argument(std::string(__FILE__) + " +" +
                                std::to_string(__LINE__));
  }
}

void RoiAlignRotatedForwardExecutor::compute() {
  VLOG(4) << "RoiAlignRotatedForwardExecutor compute.";
  const int pooled_height = parser_->getProtoNode()
                                ->roi_align_rotated_forward_param()
                                .pooled_height();
  const int pooled_width =
      parser_->getProtoNode()->roi_align_rotated_forward_param().pooled_width();
  const int sample_ratio =
      parser_->getProtoNode()->roi_align_rotated_forward_param().sample_ratio();
  const float spatial_scale = parser_->getProtoNode()
                                  ->roi_align_rotated_forward_param()
                                  .spatial_scale();
  const bool aligned =
      parser_->getProtoNode()->roi_align_rotated_forward_param().aligned();
  const bool clockwise =
      parser_->getProtoNode()->roi_align_rotated_forward_param().clockwise();

  auto features_desc = parser_->getMetaTensor(0).tensor;
  auto rois_desc = parser_->getMetaTensor(1).tensor;
  auto output_desc = parser_->getMetaTensor(2).tensor;

  void *features_ptr = data_vector_[0].device_ptr;
  void *rois_ptr = data_vector_[1].device_ptr;
  void *output_ptr = data_vector_[2].device_ptr;
  interface_timer_.start();
  MLUOP_CHECK(mluOpRoiAlignRotatedForward(
      handle_, features_desc, features_ptr, rois_desc, rois_ptr, pooled_height,
      pooled_width, sample_ratio, spatial_scale, aligned, clockwise,
      output_desc, output_ptr));
  interface_timer_.stop();
}

void RoiAlignRotatedForwardExecutor::cpuCompute() {
  VLOG(4) << "RoiAlignRotatedForwardExecutor cpu compute ";
  // not count theory_ops_ begin
  const int pooled_height = parser_->getProtoNode()
                                ->roi_align_rotated_forward_param()
                                .pooled_height();
  const int pooled_width =
      parser_->getProtoNode()->roi_align_rotated_forward_param().pooled_width();
  const int sample_ratio =
      parser_->getProtoNode()->roi_align_rotated_forward_param().sample_ratio();
  const float spatial_scale = parser_->getProtoNode()
                                  ->roi_align_rotated_forward_param()
                                  .spatial_scale();
  const bool aligned =
      parser_->getProtoNode()->roi_align_rotated_forward_param().aligned();
  const bool clockwise =
      parser_->getProtoNode()->roi_align_rotated_forward_param().clockwise();
  // not count theory_ops_ end

  auto features_desc = parser_->getMetaTensor(0).tensor;
  auto rois_desc = parser_->getMetaTensor(1).tensor;
  auto output_desc = parser_->getMetaTensor(2).tensor;

  float *features = cpu_fp32_input_[0];
  float *rois = cpu_fp32_input_[1];  // (n, 6) [batch_id, x, y, w, h, Θ]
  float *output = cpu_fp32_output_[0];

  const int channel = features_desc->getDimIndex(3);
  const int width = features_desc->getDimIndex(2);
  const int height = features_desc->getDimIndex(1);
  const int batch = features_desc->getDimIndex(0);
  const int rois_nums = rois_desc->getDimIndex(0);

  if (mluOpGetTensorElementNum(features_desc) == 0) {
    return;
  }

  for (int n_idx = 0; n_idx < rois_nums; ++n_idx) {
    // not count theory_ops_ begin
    const int output_nidx = n_idx * pooled_height * pooled_width * channel;
    const float *current_roi = rois + n_idx * ROI_OFFSET;
    // not count theory_ops_ end

    const int roi_batch_idx = (int)current_roi[0];
    const float offset = aligned ? 0.5 : 0.0;
    const float roi_center_x = current_roi[1] * spatial_scale - offset;
    const float roi_center_y = current_roi[2] * spatial_scale - offset;
    float roi_width = current_roi[3] * spatial_scale;
    float roi_height = current_roi[4] * spatial_scale;
    float theta = current_roi[5];
    theory_ops_ += 7;  // cur block
    if (clockwise) {
      theta = -theta;
      theory_ops_ += 1;  // cur block
    }
    const float cos_theta = cos(theta);
    const float sin_theta = sin(theta);
    theory_ops_ += 2;  // cur block

    if (aligned) {
      if (roi_width < 0 || roi_height < 0) {
        VLOG(4) << "ROIs do not have non-negative value.";
        throw std::invalid_argument(std::string(__FILE__) + " +" +
                                    std::to_string(__LINE__));
      }
    } else {
      roi_width = std::max(roi_width, (float)1.0);
      roi_height = std::max(roi_height, (float)1.0);
      theory_ops_ += 4;  // cur block
    }
    const float bin_size_h = roi_height / static_cast<float>(pooled_height);
    const float bin_size_w = roi_width / static_cast<float>(pooled_width);
    int roi_bin_grid_h =
        (sample_ratio > 0) ? sample_ratio : ceilf(roi_height / pooled_height);
    int roi_bin_grid_w =
        (sample_ratio > 0) ? sample_ratio : ceilf(roi_width / pooled_width);
    const float count = std::max(roi_bin_grid_h * roi_bin_grid_w, 1);
    std::vector<PreCalc> pre_calc(pooled_height * pooled_width * count);
    const float roi_start_x = -roi_width / 2.0;
    const float roi_start_y = -roi_height / 2.0;

    preCalcForBilinearInterpolate(
        height, width, channel, pooled_height, pooled_width, roi_bin_grid_h,
        roi_bin_grid_w, roi_start_x, roi_start_y, bin_size_h, bin_size_w,
        roi_center_x, roi_center_y, cos_theta, sin_theta, pre_calc);
    theory_ops_ += 16;  // cur block

    for (int c_idx = 0; c_idx < channel; ++c_idx) {
      // next stmt not count theory_ops_
      const float *offset_features =
          features + roi_batch_idx * height * width * channel;
      int pre_calc_idx = 0;

      for (int ph = 0; ph < pooled_height; ++ph) {
        for (int pw = 0; pw < pooled_width; ++pw) {
          // next stmt not count theory_ops_
          int output_idx =
              output_nidx + (ph * pooled_width + pw) * channel + c_idx;

          float output_val = 0.;
          for (int iy = 0; iy < roi_bin_grid_h; ++iy) {
            for (int ix = 0; ix < roi_bin_grid_w; ++ix) {
              auto pc = pre_calc[pre_calc_idx];
              if (!(pc.w1 == 0 && pc.w2 == 0 && pc.w3 == 0 && pc.w4 == 0)) {
                output_val += pc.w1 * offset_features[pc.pos1 + c_idx] +
                              pc.w2 * offset_features[pc.pos2 + c_idx] +
                              pc.w3 * offset_features[pc.pos3 + c_idx] +
                              pc.w4 * offset_features[pc.pos4 + c_idx];
                theory_ops_ += 9;  // cur block
              }
              // next stmt not count theory_ops_
              ++pre_calc_idx;
            }
          }
          output_val /= count;
          output[output_idx] = output_val;
          theory_ops_ += 2;  // cur block
        }
      }
    }
  }
}

int64_t RoiAlignRotatedForwardExecutor::getTheoryOps() {
#if 0
  if (parser_->device() != CPU) {
    for (int i = 0; i < parser_->getInputNum(); ++i) {
      auto *ts = parser_->input(i);
      if (unlikely(ts->empty())) {
            return 0;
      }
      if (ts->getDtype() == MLUOP_DTYPE_FLOAT) {
        ts->cpu_ptr =
            (float *)cpu_runtime_.allocate(ts->shape_count * ts->sizeof_dtype);
        parser_->getInputTensorValue(i, (void *)ts->cpu_ptr, ts->shape_count);
      } else {
        void *temp = cpu_runtime_.allocate(ts->shape_count * ts->sizeof_dtype);
        parser_->getInputTensorValue(i, temp, ts->shape_count);
        ts->cpu_ptr =
            (float *)cpu_runtime_.allocate(ts->shape_count * sizeof(float));
        castDataOut(temp, ts->getDtype(), ts->cpu_ptr, MLUOP_DTYPE_FLOAT,
                        ts->shape_count, NO_QUANT);
      }
      cpu_fp32_input_.push_back(ts->cpu_ptr);
    }
    for (int i = 0; i < parser_->getOutputNum(); ++i) {
      auto *ts = parser_->output(i);
      if (unlikely(ts->empty())) {
            return 0;
      }
      if (ts->getDtype() == MLUOP_DTYPE_FLOAT) {
        ts->cpu_ptr =
            (float *)cpu_runtime_.allocate(ts->shape_count * ts->sizeof_dtype);
        parser_->getOutputTensorValue(i, (void *)ts->cpu_ptr, ts->shape_count);
      } else {
        void *temp = cpu_runtime_.allocate(ts->shape_count * ts->sizeof_dtype);
        parser_->getOutputTensorValue(i, temp, ts->shape_count);
        ts->cpu_ptr =
            (float *)cpu_runtime_.allocate(ts->shape_count * sizeof(float));
        castDataOut(temp, ts->getDtype(), ts->cpu_ptr, MLUOP_DTYPE_FLOAT,
                        ts->shape_count, NO_QUANT);
      }
      cpu_fp32_output_.push_back(ts->cpu_ptr);
    }
    cpuCompute();
  }
#endif

  VLOG(4) << "getTheoryOps: " << theory_ops_ << " ops";
  return theory_ops_;
}
}  // namespace mluoptest
