/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include <fstream>
#include <sstream>
#include <vector>

#include "tensorflow/core/common_runtime/device_factory.h"
#include "tensorflow/core/common_runtime/device_mgr.h"
#include "tensorflow/core/common_runtime/device_set.h"
#include "tensorflow/core/common_runtime/dma_helper.h"
#include "tensorflow/core/common_runtime/local_device.h"
#include "tensorflow/core/framework/allocator.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/default/logging.h"
#include "tensorflow/core/public/session_options.h"

#include "ngraph_utils.h"

namespace ngraph_bridge {
extern const char* const DEVICE_NGRAPH_CPU = "NGRAPH_CPU";
}

namespace tensorflow {

class NGraphDeviceContext : public tf::DeviceContext {
 public:
  perftools::gputools::Stream* stream() const override {
    tf::errors::Internal("NGraphDeviceContext::stream() called");
    return nullptr;
  }
  void MaintainLifetimeOnStream(
      const Tensor* t, perftools::gputools::Stream* stream) const override {
    tf::errors::Internal(
        "NGraphDeviceContext::MaintainLifetimeOnStream() called");
  }

  // "cpu_tensor" is a tensor on a CPU. Copies "cpu_tensor" into
  // "device_tensor" which is on a GPU device "device". "device_tensor"
  // must be allocated to be of the same size as "cpu_tensor".
  void CopyCPUTensorToDevice(const Tensor* cpu_tensor, Device* device,
                             Tensor* device_tensor,
                             StatusCallback done) const override {
    if (cpu_tensor->NumElements() > 0) {
      VLOG(0) << "CopyCPUTensorToDevice "
              << reinterpret_cast<const void*>(cpu_tensor->tensor_data().data())
              << " " << reinterpret_cast<const void*>(
                            device_tensor->tensor_data().data())
              << " " << cpu_tensor->NumElements();

      void* src_ptr = const_cast<void*>(DMAHelper::base(cpu_tensor));
      const int64 total_bytes = cpu_tensor->TotalBytes();
      void* dst_ptr = DMAHelper::base(device_tensor);
      memcpy(dst_ptr, src_ptr, total_bytes);

      VLOG(0) << "CPU Tensor: " << cpu_tensor->DebugString();
      // done(errors::Internal("Unrecognized device type in CPU-to-device
      // Copy"));

      done(tf::Status::OK());
      return;
    }

    VLOG(0) << "CopyCPUTensorToDevice empty tensor";
    VLOG(0) << cpu_tensor->DebugString();

    // Call the done callback
    done(tf::Status::OK());
  }

  // "device_tensor" is a tensor on a non-CPU device.  Copies
  // device_tensor into "cpu_tensor".  "cpu_tensor" must be allocated
  // to be of the same size as "device_tensor".
  void CopyDeviceTensorToCPU(const Tensor* device_tensor,
                             StringPiece tensor_name, Device* device,
                             Tensor* cpu_tensor, StatusCallback done) override {
    if (device_tensor->NumElements() > 0) {
      VLOG(2) << "CopyDeviceTensorToCPU "
              << reinterpret_cast<const void*>(
                     device_tensor->tensor_data().data())
              << " "
              << reinterpret_cast<const void*>(cpu_tensor->tensor_data().data())
              << device_tensor->NumElements();
      VLOG(0) << device_tensor->DebugString();
      // done(errors::Internal("Unrecognized device type in device-to-CPU
      // Copy"));

      void* src_ptr = const_cast<void*>(DMAHelper::base(device_tensor));
      const int64 total_bytes = device_tensor->TotalBytes();
      void* dst_ptr = DMAHelper::base(cpu_tensor);
      memcpy(dst_ptr, src_ptr, total_bytes);

      done(tf::Status::OK());
      return;
    }
    VLOG(0) << "CopyDeviceTensorToCPU empty tensor";
    VLOG(0) << device_tensor->DebugString();
    done(tf::Status::OK());
  }
};  // namespace tensorflow

// Return a fake device with the specified type and name.
class NGraphDevice : public Device {
 public:
  explicit NGraphDevice(const DeviceAttributes& attr) : Device(nullptr, attr) {
    m_allocator = cpu_allocator();
    m_device_context = new NGraphDeviceContext();
    m_device_context->Ref();
  }
  ~NGraphDevice() { m_device_context->Unref(); }

  Status Sync() override { return Status::OK(); }

  Allocator* GetAllocator(AllocatorAttributes attrs) override {
    std::cout << "NGraphDevice::GetAllocator called. OnHost: "
              << attrs.on_host()
              << " GPU Compatible: " << attrs.gpu_compatible() << std::endl;
    return m_allocator;
  }

  tf::Status FillContextMap(const Graph* graph,
                            DeviceContextMap* device_context_map) override {
    VLOG(0) << "NGraphDevice::FillContextMap";
    device_context_map->resize(graph->num_node_ids());

    for (Node* n : graph->nodes()) {
      // VLOG(0) << n->id() << " : " << n->type_string() << " : " << n->name();
      m_device_context->Ref();
      (*device_context_map)[n->id()] = m_device_context;
    }
    return tf::Status::OK();
  }

  // Overwrite MaybeRewriteGraph
  Status MaybeRewriteGraph(std::unique_ptr<Graph>* graph) override {
    VLOG(0) << "NGraphDevice::MaybeRewriteGraph() called";
    return Status::OK();
  }

 private:
  tf::Allocator* m_allocator;
  NGraphDeviceContext* m_device_context;  // not owned
};

class NGraphDeviceFactory : public DeviceFactory {
 public:
  Status CreateDevices(const SessionOptions& options, const string& name_prefix,
                       std::vector<Device*>* devices) override {
    std::cout << "NGraphDeviceFactory::CreateDevices() called: Name: "
              << name_prefix << std::endl;
    DeviceAttributes attr;
    attr.set_name(strings::StrCat(name_prefix, "/device:NGRAPH_CPU:0"));
    attr.set_device_type(ngraph_bridge::DEVICE_NGRAPH_CPU);

    devices->push_back(new NGraphDevice(attr));
    return Status::OK();
  }
};

// Assumes the default priority is '50'.
REGISTER_LOCAL_DEVICE_FACTORY(ngraph_bridge::DEVICE_NGRAPH_CPU,
                              NGraphDeviceFactory, 50);

static bool InitModule() {
  std::cout << "InitModule called" << std::endl;

  return true;
}

volatile bool not_used = InitModule();

}  // namespace tensorflow