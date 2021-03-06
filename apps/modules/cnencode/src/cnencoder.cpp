/*************************************************************************
 * Copyright (C) [2019] by Cambricon, Inc. All rights reserved
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *************************************************************************/

#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "cnencoder.hpp"
#include "easycodec/easy_encode.h"
#include "easycodec/vformat.h"
#include "easyinfer/mlu_context.h"

namespace cnstream {

CNEncoder::CNEncoder(const std::string &name) : Module(name) {
  param_register_.SetModuleDesc("CNEncoder is a module to encode use cnencode.");
  param_register_.Register("dst_width", "The image width of the output.");
  param_register_.Register("dst_height", "The image height of the output.");
  param_register_.Register("frame_rate", "Frame rate of the encoded video.");
  param_register_.Register("bit_rate", "The amount data encoded for a unit of time."
                           " A higher bitrate means a higher quality video.");
  param_register_.Register("gop_size", "Group of pictures is known as GOP."
                           " gop_size is the number of frames between two I-frames.");
  param_register_.Register("device_id", "Which device will be used. If there is only one device, it might be 0.");
  param_register_.Register("pre_type", "Resize and colorspace convert type.");
  hasTransmit_.store(1);  // for receive eos
}

CNEncoderContext *CNEncoder::GetCNEncoderContext(CNFrameInfoPtr data) {
  CNEncoderContext *ctx = nullptr;
  auto search = ctxs_.find(data->channel_idx);
  if (search != ctxs_.end()) {
    ctx = search->second;
  } else {
    ctx = new CNEncoderContext;
    cn_type_ = CNEncoderStream::H264;
    switch (data->frame.fmt) {
      case cnstream::CNDataFormat::CN_PIXEL_FORMAT_BGR24:
        cn_format_ = CNEncoderStream::BGR24;
        break;
      case cnstream::CNDataFormat::CN_PIXEL_FORMAT_YUV420_NV12:
        cn_format_ = CNEncoderStream::NV12;
        break;
      case cnstream::CNDataFormat::CN_PIXEL_FORMAT_YUV420_NV21:
        cn_format_ = CNEncoderStream::NV21;
        break;
      default:
        LOG(WARNING) << "[CNEncoder] unsuport pixel format.";
        break;
    }
    // build cnencoder
    ctx->stream_ = new CNEncoderStream(data->frame.width, data->frame.height, dst_width_, dst_height_,
                 frame_rate_, cn_format_, bit_rate_, gop_size_, cn_type_, data->channel_idx, device_id_, pre_type_);
    // open cnencoder
    ctx->stream_->Open();
    // add into map
    ctxs_[data->channel_idx] = ctx;
  }
  return ctx;
}

CNEncoder::~CNEncoder() { Close(); }

bool CNEncoder::Open(ModuleParamSet paramSet) {
  if (paramSet.find("frame_rate") == paramSet.end()) {
    frame_rate_ = 25;
  } else {
    frame_rate_ = std::stoi(paramSet["frame_rate"]);
  }
  if (paramSet.find("bit_rate") == paramSet.end()) {
    bit_rate_ = 0x100000;
  } else {
    bit_rate_ = std::stoi(paramSet["bit_rate"]);
    bit_rate_ *= 1024;
  }
  if (paramSet.find("gop_size") == paramSet.end()) {
    gop_size_ = 10;
  } else {
    gop_size_ = std::stoi(paramSet["gop_size"]);
  }
  if (paramSet.find("device_id") == paramSet.end()) {
    device_id_ = 0;
  } else {
    device_id_ = std::stoi(paramSet["device_id"]);
  }
  if (paramSet.find("dst_width") == paramSet.end()) {
    dst_width_ = 960;
  } else {
    dst_width_ = std::stoi(paramSet["dst_width"]);
  }
  if (paramSet.find("dst_height") == paramSet.end()) {
    dst_height_ = 540;
  } else {
    dst_height_ = std::stoi(paramSet["dst_height"]);
  }
  if (paramSet.find("pre_type") == paramSet.end()) {
    pre_type_ = "opencv";
  } else {
    pre_type_ = paramSet["pre_type"];
  }

  // cn_type_ = CNEncoderStream::H264;
  // cn_format_ = CNEncoderStream::NV21;
  edk::MluContext tx;
  tx.SetDeviceId(device_id_);
  tx.ConfigureForThisThread();
  return true;
}

void CNEncoder::Close() {
  if (ctxs_.empty()) {
    return;
  }
  for (auto &pair : ctxs_) {
    pair.second->stream_->Close();
    delete pair.second->stream_;
    delete pair.second;
  }
  ctxs_.clear();
}

int CNEncoder::Process(CNFrameInfoPtr data) {
  bool eos = data->frame.flags & CNFrameFlag::CN_FRAME_FLAG_EOS;
  CNEncoderContext *ctx = GetCNEncoderContext(data);
  if (!eos) {
    if (pre_type_ == "opencv" || pre_type_ == "ffmpeg") {
      cv::Mat image = *data->frame.ImageBGR();
      ctx->stream_->Update(image, data->frame.timestamp, data->channel_idx);
    } else if (pre_type_ == "mlu") {
      uint8_t *image_data = new uint8_t[data->frame.GetBytes()];
      uint8_t *plane_0 = reinterpret_cast<uint8_t *>(data->frame.data[0]->GetMutableCpuData());
      uint8_t *plane_1 = reinterpret_cast<uint8_t *>(data->frame.data[1]->GetMutableCpuData());
      memcpy(image_data, plane_0, data->frame.GetPlaneBytes(0));
      memcpy(image_data + data->frame.GetPlaneBytes(0), plane_1, data->frame.GetPlaneBytes(1));
      ctx->stream_->Update(image_data, data->frame.timestamp, data->channel_idx);
      delete []image_data;
      image_data = nullptr;
    } else {
      std::cout << "pre_type err !!!" << std::endl;
      return 0;
    }

  } else {
      ctx->stream_->RefreshEOS(eos);
  }
  // TransmitData(data);
  return 0;
}

bool CNEncoder::CheckParamSet(ModuleParamSet paramSet) {
  ParametersChecker checker;
  for (auto& it : paramSet) {
    if (!param_register_.IsRegisted(it.first)) {
      LOG(WARNING) << "[CNEncoder] Unknown param: " << it.first;
    }
  }

  if (paramSet.find("dst_width") == paramSet.end()
      || paramSet.find("dst_height") == paramSet.end()
      || paramSet.find("frame_rate") == paramSet.end()
      || paramSet.find("bit_rate") == paramSet.end()
      || paramSet.find("gop_size") == paramSet.end()
      || paramSet.find("device_id") == paramSet.end()
      || paramSet.find("pre_type") == paramSet.end()) {
    LOG(ERROR) << "CNEncoder must specify ";
    LOG(ERROR) << "[dst_width], [dst_height], [frame_rate], [bit_rate], [gop_size], [device_id], [pre_type].";
    return false;
  }

  std::string err_msg;
  if (!checker.IsNum({"dst_width", "dst_height, frame_rate", "bit_rate", "gop_size", "device_id", "pre_type"},
        paramSet, err_msg, true)) {
    LOG(ERROR) << "[CNEncoder] " << err_msg;
    return false;
  }
  return true;
}

}  // namespace cnstream
