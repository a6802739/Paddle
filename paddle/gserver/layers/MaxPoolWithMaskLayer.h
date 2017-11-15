/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserve.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#include <vector>
#include "PoolLayer.h"
#include "paddle/math/Matrix.h"

namespace paddle {
/**
 * @brief Basic parent layer of different kinds of pooling
 */
class MaxPoolWithMaskLayer : public PoolLayer {
protected:
  Argument mask_;

public:
  explicit MaxPoolWithMaskLayer(const LayerConfig& config)
      : PoolLayer(config) {}

  size_t getSize();

  void forward(PassType passType) override;
  void backward(const UpdateCallback& callback = nullptr) override;
  bool init(const LayerMap& layerMap,
            const ParameterMap& parameterMap) override;
};
}  // namespace paddle
