#   Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserve.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
import unittest

import paddle.v2.fluid as fluid
import numpy as np
import decorators


class TestRegistry(unittest.TestCase):
    @decorators.prog_scope()
    def test_registry_layer(self):
        x = fluid.layers.data(name='X', shape=[10, 10], dtype='float32')
        output = fluid.layers.mean(x=x)

        place = fluid.CPUPlace()
        exe = fluid.Executor(place)
        X = np.random.random((10, 10)).astype("float32")
        mean_out = exe.run(feed={"X": X}, fetch_list=[output])
        self.assertAlmostEqual(np.mean(X), mean_out[0])
