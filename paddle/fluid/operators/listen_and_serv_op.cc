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

#include <stdint.h>
#include <sys/stat.h>
#include <ostream>
#include <thread>

#include <unistd.h>

#include "paddle/fluid/framework/executor.h"
#include "paddle/fluid/framework/framework.pb.h"
#include "paddle/fluid/framework/lod_tensor.h"
#include "paddle/fluid/framework/op_registry.h"
#include "paddle/fluid/framework/proto_desc.h"
#include "paddle/fluid/operators/detail/grpc_server.h"
#include "paddle/fluid/operators/detail/sendrecvop_utils.h"
#include "paddle/fluid/operators/detail/simple_block_queue.h"
#include "paddle/fluid/string/printf.h"

namespace paddle {
namespace operators {

constexpr char kOptimizeBlock[] = "OptimizeBlock";

void RunServer(std::shared_ptr<detail::AsyncGRPCServer> service) {
  service->RunSyncUpdate();
  VLOG(4) << "RunServer thread end";
}

static void CreateTensorFromMessageType(framework::Variable *var,
                                        sendrecv::VarType var_type) {
  if (var_type == sendrecv::VarType::LOD_TENSOR) {
    var->GetMutable<framework::LoDTensor>();
  } else if (var_type == sendrecv::VarType::SELECTED_ROWS) {
    var->GetMutable<framework::SelectedRows>();
  } else {
    PADDLE_THROW(
        "VariableMessage type %d is not in "
        "[LoDTensor, SelectedRows]",
        var_type);
  }
}

class ListenAndServOp : public framework::OperatorBase {
 public:
  ListenAndServOp(const std::string &type,
                  const framework::VariableNameMap &inputs,
                  const framework::VariableNameMap &outputs,
                  const framework::AttributeMap &attrs)
      : OperatorBase(type, inputs, outputs, attrs) {
    if (!rpc_service_) {
      std::string endpoint = Attr<std::string>("endpoint");
      rpc_service_.reset(new detail::AsyncGRPCServer(endpoint));
      server_thread_.reset(new std::thread(RunServer, rpc_service_));
    }
  }

  void Stop() override {
    detail::MessageWithName term_msg;
    term_msg.first = LISTEN_TERMINATE_MESSAGE;
    rpc_service_->Push(term_msg);
    rpc_service_->ShutDown();
    server_thread_->join();
  }

  void Run(const framework::Scope &scope,
           const platform::Place &dev_place) const override {
    platform::DeviceContextPool &pool = platform::DeviceContextPool::Instance();
    auto &dev_ctx = *pool.Get(dev_place);
    framework::Scope &recv_scope = scope.NewScope();

    // FIXME(Yancey1989): initialize rpc server with lazy mode.
    rpc_service_->SetScope(&recv_scope);
    rpc_service_->SetDevCtx(&dev_ctx);
    auto ins = Inputs("X");
    auto fan_in = Attr<int>("Fanin");

    auto *block = Attr<framework::BlockDesc *>(kOptimizeBlock);
    auto *program = block->Program();
    framework::Executor executor(dev_place);

    // TODO(typhoonzero): change this to a while_op for every cluster-batch.
    bool exit_flag = false;
    while (!exit_flag) {
      // Get from multiple trainers, we don't care about the order in which
      // the gradients arrives, just add suffix 0~n and merge the gradient.
      rpc_service_->SetCond(0);
      size_t recv_var_cnt = 0;
      size_t update_param_cnt = 0;
      int batch_barrier = 0;
      while (batch_barrier != fan_in) {
        const detail::MessageWithName &v = rpc_service_->Get();
        auto recv_var_name = v.first;
        if (recv_var_name == LISTEN_TERMINATE_MESSAGE) {
          LOG(INFO) << "received terminate message and exit";
          exit_flag = true;
          break;
        } else if (recv_var_name == BATCH_BARRIER_MESSAGE) {
          VLOG(3) << "recv batch barrier message";
          batch_barrier++;
          continue;
        } else {
          VLOG(3) << "received grad: " << recv_var_name;
          recv_var_cnt++;
          auto *var = recv_scope.FindVar(recv_var_name);
          if (var == nullptr) {
            LOG(ERROR) << "Can not find server side var: " << recv_var_name;
            PADDLE_THROW("Can not find server side var");
          }
          detail::DeserializeFromMessage(v.second, dev_ctx, var);
        }
      }
      VLOG(3) << "recv " << recv_var_cnt << " parmeters for one barrier.";
      if (exit_flag) {
        rpc_service_->ShutDown();
      }
      VLOG(3) << "run optimize graph...";
      try {
        executor.Run(*program, &recv_scope, block->ID(), /*global_block*/
                     false /*create_local_scope*/, false /*create_vars*/);
      } catch (std::exception &e) {
        LOG(ERROR) << "run sub program error " << e.what();
      }
      rpc_service_->SetCond(1);
      rpc_service_->WaitClientGet(update_param_cnt);
      grads_counter_.clear();
    }  // while(true)
  }

 protected:
  std::shared_ptr<detail::AsyncGRPCServer> rpc_service_;
  std::shared_ptr<std::thread> server_thread_;
  mutable std::unordered_map<std::string, int> grads_counter_;
};

class ListenAndServOpMaker : public framework::OpProtoAndCheckerMaker {
 public:
  ListenAndServOpMaker(OpProto *proto, OpAttrChecker *op_checker)
      : OpProtoAndCheckerMaker(proto, op_checker) {
    AddInput("X", "(Tensor) Variables that server recv.").AsDuplicable();
    AddComment(R"DOC(
ListenAndServ operator

This operator will start a RPC server which can receive variables
from send_op and send back variables to recv_op.
)DOC");
    AddAttr<std::string>("endpoint",
                         "(string, default 127.0.0.1:6164)"
                         "IP address to listen on.")
        .SetDefault("127.0.0.1:6164")
        .AddCustomChecker([](const std::string &ip) { return !ip.empty(); });
    AddAttr<framework::BlockDesc *>(kOptimizeBlock,
                                    "BlockID to run on server side.");
    AddAttr<int>("Fanin", "How many clients send to this server.")
        .SetDefault(1);
  }
};

}  // namespace operators
}  // namespace paddle

namespace ops = paddle::operators;

REGISTER_OPERATOR(listen_and_serv, ops::ListenAndServOp,
                  ops::ListenAndServOpMaker);
