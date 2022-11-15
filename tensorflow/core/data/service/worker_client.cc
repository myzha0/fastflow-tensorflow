/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/core/data/service/worker_client.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "grpcpp/client_context.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "grpcpp/support/channel_arguments.h"
#include "grpcpp/support/status.h"
#include "absl/container/flat_hash_set.h"
#include "absl/memory/memory.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "tensorflow/core/data/dataset.pb.h"
#include "tensorflow/core/data/service/credentials_factory.h"
#include "tensorflow/core/data/service/data_transfer.h"
#include "tensorflow/core/data/service/grpc_util.h"
#include "tensorflow/core/data/service/worker.grpc.pb.h"
#include "tensorflow/core/data/service/worker.pb.h"
#include "tensorflow/core/data/service/worker_impl.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/framework/variant.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/env_time.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/status.h"
#include "tensorflow/core/platform/statusor.h"
#include "tensorflow/core/platform/thread_annotations.h"
#include "tensorflow/core/platform/types.h"

namespace tensorflow {
namespace data {

void ByteBlockChecker::AddAndSleepCheck(int64_t total_bytes) {
  mutex_lock l(mu_);
  sum_total_bytes_ += total_bytes;
  if (sum_total_bytes_ >= check_block_size_) {
    int64_t expected_sleep_time = sum_total_bytes_ * 8.0 / ( max_bandwidth_bps_ / (1000.0 * 1000.0) );

    int64_t curr_time_micros = EnvTime::NowMicros();
    int64_t elapsed = curr_time_micros - prev_call_time_micros_;

    if (expected_sleep_time > elapsed) {
      Env::Default()->SleepForMicroseconds(expected_sleep_time - elapsed);
    }
    sum_total_bytes_ = 0;
    prev_call_time_micros_ = EnvTime::NowMicros();
  }
}

StatusOr<std::unique_ptr<DataServiceWorkerClient>>
CreateDataServiceWorkerClient(const std::string& address,
                              const std::string& protocol,
                              const std::string& transfer_protocol,
                              const int64_t& max_bandwidth_bps) {
  auto client = absl::make_unique<DataServiceWorkerClient>(address, protocol,
                                                           transfer_protocol,
                                                           max_bandwidth_bps);
  TF_RETURN_IF_ERROR(client->Initialize());
  return client;
}

Status DataServiceWorkerClient::GetElement(const GetElementRequest& req,
                                           GetElementResult& result) {
  TF_RETURN_IF_ERROR(EnsureInitialized());
  return client_->GetElement(req, result);
}

Status DataServiceWorkerClient::EnsureInitialized() {
  mutex_lock l(mu_);
  if (client_) {
    return Status::OK();
  }
  TF_RETURN_IF_ERROR(DataTransferClient::Build(
      GetDataTransferProtocol(), {protocol_, address_, max_bandwidth_bps_}, &client_));
  return Status::OK();
}

std::string DataServiceWorkerClient::GetDataTransferProtocol() const {
  if (transfer_protocol_ == kGrpcTransferProtocol &&
      LocalWorkers::Get(address_) != nullptr) {
    return kLocalTransferProtocol;
  }
  return transfer_protocol_;
}

void DataServiceWorkerClient::TryCancel() { client_->TryCancel(); }

class GrpcDataTransferClient : public DataTransferClient {
 public:
  GrpcDataTransferClient(std::shared_ptr<grpc::ChannelCredentials> credentials,
                         std::string address,
                         std::shared_ptr<ByteBlockChecker> byte_block_checker):
                                                     byte_block_checker_(byte_block_checker){
    VLOG(2) << "Create GrpcDataTransferClient for worker " << address << ".";
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(-1);
    auto channel = grpc::CreateCustomChannel(address, credentials, args);
    stub_ = WorkerService::NewStub(channel);
  }

  Status GetElement(const GetElementRequest& req,
                    GetElementResult& result) override {
    VLOG(3) << "GetElement for task " << req.task_id() << " from gRPC worker "
            << "server.";
    {
      mutex_lock l(mu_);
      if (cancelled_) {
        return errors::Cancelled("Client was cancelled.");
      }
    }
    grpc::ClientContext ctx;
    {
      mutex_lock l(mu_);
      active_contexts_.insert(&ctx);
    }
    GetElementResponse resp;
    grpc::Status s = stub_->GetElement(&ctx, req, &resp);

    if(byte_block_checker_ != nullptr) {
      byte_block_checker_->AddAndSleepCheck(resp.ByteSizeLong() + req.ByteSizeLong());
    }

    result.end_of_sequence = resp.end_of_sequence();
    result.skip = resp.skip_task();
    switch (resp.element_case()) {
      case GetElementResponse::kCompressed: {
        Tensor tensor(DT_VARIANT, TensorShape{});
        tensor.scalar<Variant>()() = std::move(resp.compressed());
        result.components.push_back(tensor);
        break;
      }
      case GetElementResponse::kUncompressed:
        for (const auto& component : resp.uncompressed().components()) {
          result.components.emplace_back();
          if (!result.components.back().FromProto(component)) {
            return errors::Internal("Failed to parse tensor.");
          }
        }
        break;
      case GetElementResponse::ELEMENT_NOT_SET:
        break;
    }
    {
      mutex_lock l(mu_);
      active_contexts_.erase(&ctx);
    }
    if (!s.ok()) {
      return grpc_util::WrapError("Failed to get element", s);
    }
    return Status::OK();
  }

  void TryCancel() override {
    VLOG(2) << "Cancel GrpcDataTransferClient.";
    mutex_lock l(mu_);
    cancelled_ = true;
    for (const auto& ctx : active_contexts_) {
      ctx->TryCancel();
    }
  }

 private:
  mutex mu_;
  std::unique_ptr<WorkerService::Stub> stub_;
  // Set of all currently active clients contexts. Used to support
  // cancellation.
  absl::flat_hash_set<::grpc::ClientContext*> active_contexts_
      TF_GUARDED_BY(mu_);
  // Estimate Network Bandwidth
  std::shared_ptr<ByteBlockChecker> byte_block_checker_;
  // Indicates that the client has been cancelled, so no further requests should
  // be accepted.
  bool cancelled_ TF_GUARDED_BY(mu_) = false;
};

static std::shared_ptr<ByteBlockChecker> g_byte_block_checker;
class GrpcTransferClientRegistrar {
 public:
  GrpcTransferClientRegistrar() {
    DataTransferClient::Register(
        kGrpcTransferProtocol, [](DataTransferClient::Config config,
                                  std::unique_ptr<DataTransferClient>* out) {
          std::shared_ptr<grpc::ChannelCredentials> credentials;
          TF_RETURN_IF_ERROR(CredentialsFactory::CreateClientCredentials(
              config.protocol, &credentials));
          if (g_byte_block_checker == nullptr && config.max_bandwidth_bps > 0) {
            g_byte_block_checker = std::make_shared<ByteBlockChecker>(config.max_bandwidth_bps);
          }
          *out = std::make_unique<GrpcDataTransferClient>(credentials,
                                                          config.address,
                                                          g_byte_block_checker);
          return Status::OK();
        });
  }
};
static GrpcTransferClientRegistrar gprc_client_registrar;

class LocalDataTransferClient : public DataTransferClient {
 public:
  explicit LocalDataTransferClient(absl::string_view worker_address)
      : worker_address_(worker_address) {
    VLOG(2) << "Create LocalDataTransferClient for worker " << worker_address_
            << ".";
  }

  Status GetElement(const GetElementRequest& req,
                    GetElementResult& result) override {
    VLOG(3) << "GetElement for task " << req.task_id() << " from local worker.";
    TF_RETURN_IF_ERROR(VerifyClientIsNotCancelled());
    TF_ASSIGN_OR_RETURN(std::shared_ptr<DataServiceWorkerImpl> worker,
                        GetWorker(req));
    return worker->GetElementResult(&req, &result);
  }

  void TryCancel() override {
    VLOG(2) << "Cancel LocalDataTransferClient for worker " << worker_address_
            << ".";
    // Cancels incoming requests. Currently local reads assume the requests are
    // first-come-first-served. If we need to support coordinated reads, we need
    // to cancel in-flight requests since they may wait infinitely.
    mutex_lock l(mu_);
    cancelled_ = true;
  }

 private:
  Status VerifyClientIsNotCancelled() TF_LOCKS_EXCLUDED(mu_) {
    mutex_lock l(mu_);
    if (cancelled_) {
      return errors::Cancelled(absl::Substitute(
          "Client for worker $0 has been cancelled.", worker_address_));
    }
    return Status::OK();
  }

  StatusOr<std::shared_ptr<DataServiceWorkerImpl>> GetWorker(
      const GetElementRequest& req) const {
    std::shared_ptr<DataServiceWorkerImpl> worker =
        LocalWorkers::Get(worker_address_);
    if (!worker) {
      return errors::Cancelled(absl::Substitute(
          "Local worker at address $0 is no longer available; cancel request "
          "for task $1.",
          worker_address_, req.task_id()));
    }
    return worker;
  }

  const std::string worker_address_;

  mutex mu_;
  bool cancelled_ TF_GUARDED_BY(mu_) = false;
};

class LocalTransferClientRegistrar {
 public:
  LocalTransferClientRegistrar() {
    DataTransferClient::Register(
        kLocalTransferProtocol, [](DataTransferClient::Config config,
                                   std::unique_ptr<DataTransferClient>* out) {
          *out = absl::make_unique<LocalDataTransferClient>(config.address);
          return Status::OK();
        });
  }
};
static LocalTransferClientRegistrar local_client_registrar;

}  // namespace data
}  // namespace tensorflow