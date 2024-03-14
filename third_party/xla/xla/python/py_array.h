/* Copyright 2022 The OpenXLA Authors.

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

#ifndef XLA_PYTHON_PY_ARRAY_H_
#define XLA_PYTHON_PY_ARRAY_H_

#include <Python.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// placeholder for index annotation headers
#include "absl/types/span.h"
#include "llvm/Support/Casting.h"
#include "third_party/nanobind/include/nanobind/nanobind.h"
#include "pybind11/numpy.h"  // from @pybind11
#include "pybind11/pybind11.h"  // from @pybind11
#include "pybind11/pytypes.h"  // from @pybind11
#include "pybind11/stl.h"  // from @pybind11
#include "xla/pjrt/exceptions.h"
#include "xla/pjrt/pjrt_client.h"
#include "xla/python/ifrt/array.h"
#include "xla/python/ifrt/device.h"
#include "xla/python/ifrt/future.h"
#include "xla/python/nb_numpy.h"
#include "xla/python/pjrt_ifrt/pjrt_array.h"
#include "xla/python/py_client.h"
#include "xla/python/traceback.h"
#include "xla/shape.h"
#include "xla/status.h"
#include "xla/statusor.h"
#include "xla/util.h"
#include "tsl/concurrency/ref_count.h"

namespace xla {

// Private to PyArray, but you cannot forward declare member classes.
// Not thread safe; assumes the GIL is held.
class PyHostValue {
 public:
  PyHostValue();
  ~PyHostValue();

  PyHostValue(const PyHostValue&) = delete;
  PyHostValue(PyHostValue&&) = delete;
  PyHostValue& operator=(const PyHostValue&) = delete;
  PyHostValue& operator=(PyHostValue&&) = delete;

  Status CopyToHostAsync(std::optional<Shape>& dynamic_shape_holder,
                         ifrt::Array* ifrt_array);

  StatusOr<nanobind::object> AsNumPyArray(
      std::optional<Shape>& dynamic_shape_holder, ifrt::Array* ifrt_array);

 private:
  ifrt::Future<Status> ready_;
  nb_numpy_ndarray value_;
};

// Private to PyArray, but you cannot forward declare member classes.
struct PyArray_Storage {
  PyArray_Storage(nanobind::object aval, bool weak_type, nb_dtype dtype,
                  std::vector<int64_t> shape, nanobind::object sharding,
                  bool committed, std::shared_ptr<PyClient> py_client,
                  std::optional<nb_traceback> traceback,
                  tsl::RCReference<ifrt::Array> ifrt_array,
                  xla::PjRtFuture<absl::Status> result_status);

  // TODO(yashkatariya): remove this once the transition completes.
  struct DisableFastpath {};
  explicit PyArray_Storage(DisableFastpath);

  ~PyArray_Storage();
  nanobind::handle AsHandle();

  // TODO(yashkatariya): remove this once the transition completes.
  bool fastpath_enabled;

  nanobind::object aval;
  bool weak_type = false;
  nb_dtype dtype;
  std::vector<int64_t> shape;

  nanobind::object sharding;
  nanobind::object npy_value = nanobind::none();
  bool committed = false;

  std::shared_ptr<PyClient> py_client;
  std::optional<nb_traceback> traceback;
  tsl::RCReference<ifrt::Array> ifrt_array;

  // optional field, used only in python
  std::vector<PyArray> py_arrays;
  PyHostValue host_value;  // Protected by the GIL.
  std::optional<Shape> dynamic_shape = std::nullopt;
  // Only set if this Array was generated by a computation that has effects.
  // This is the result status of the XLA computation that generated this
  // array.
  xla::PjRtFuture<absl::Status> result_status;

  // Doubly-linked list of all PyArrays known to the client. Protected by the
  // GIL. Since multiple PyArrays may share the same PjRtBuffer, there may be
  // duplicate PjRtBuffers in this list.
  PyArray_Storage* next;
  PyArray_Storage* prev;
};

// The C++ implementation of jax.Array. A few key methods and data members are
// implemented in C++ for performance, while most of the functionalities are
// still implemented in python.
class PyArray : public nanobind::object {
 public:
  NB_OBJECT(PyArray, nanobind::object, "Array", PyArray::IsPyArray);
  PyArray() = default;

  // "__init__" methods. Only used in python
  static void PyInit(PyArray self, nanobind::object aval,
                     nanobind::object sharding,
                     absl::Span<const PyArray> py_arrays, bool committed,
                     bool skip_checks);

  // TODO(yashkatariya): remove this once the transition completes.
  struct DisableFastpath {};
  static void PyInit(nanobind::object self, DisableFastpath);

  // Only used in C++. `skip_checks` should only be set for Arrays created by
  // jax that cannot possibly have consistency issues (e.g. `sharding` devices
  // different than `ifrt_array` devices). Arrays created by users should be
  // checked.
  PyArray(nanobind::object aval, bool weak_type, nb_dtype dtype,
          std::vector<int64_t> shape, nanobind::object sharding,
          std::shared_ptr<PyClient> py_client,
          std::optional<nb_traceback> traceback,
          tsl::RCReference<ifrt::Array> ifrt_array, bool committed,
          bool skip_checks,
          xla::PjRtFuture<absl::Status> result_status =
              xla::PjRtFuture<absl::Status>());

  static PyArray MakeFromSingleDeviceArray(
      std::shared_ptr<PyClient> py_client,
      std::optional<nb_traceback> traceback,
      tsl::RCReference<ifrt::Array> ifrt_array, bool weak_type, bool committed,
      xla::PjRtFuture<absl::Status> result_status =
          xla::PjRtFuture<absl::Status>());

  static PyArray MakeFromIfrtArrayAndSharding(
      std::shared_ptr<PyClient> py_client,
      std::optional<nb_traceback> traceback,
      tsl::RCReference<ifrt::Array> ifrt_array, nanobind::object sharding,
      bool weak_type, bool committed, bool skip_checks);

  static Status RegisterTypes(nanobind::module_& m);

  using Storage = PyArray_Storage;

  const nanobind::object& aval() const { return GetStorage().aval; }
  void set_aval(nanobind::object aval) { GetStorage().aval = std::move(aval); }

  bool weak_type() const { return GetStorage().weak_type; }

  const nb_dtype& dtype() const { return GetStorage().dtype; }
  absl::Span<const int64_t> shape() const { return GetStorage().shape; }

  const nanobind::object& sharding() const { return GetStorage().sharding; }

  bool committed() const { return GetStorage().committed; }

  const nanobind::object& npy_value() const { return GetStorage().npy_value; }
  void set_npy_value(nanobind::object v) {
    GetStorage().npy_value = std::move(v);
  }

  const std::shared_ptr<PyClient>& py_client() const {
    return GetStorage().py_client;
  }

  const std::optional<nb_traceback>& traceback() const {
    return GetStorage().traceback;
  }

  // Returns xla::InvalidArgument if the buffer has been deleted.
  // See `PjRtFuture` for the semantics of `IsReady` and `IsKnownReady`.
  StatusOr<bool> IsReady() {
    ifrt::Array* ifrt_array_ptr = ifrt_array();
    if (ifrt_array_ptr->IsDeleted()) {
      return InvalidArgument("Array has been deleted.");
    }
    return ifrt_array_ptr->GetReadyFuture().IsReady();
  }

  const xla::PjRtFuture<absl::Status>& result_status() const {
    return GetStorage().result_status;
  }

  ifrt::Array* ifrt_array() const { return GetStorage().ifrt_array.get(); }

  // Short-term escape hatch to get PjRtBuffers from PyArray.
  // TODO(hyeontaek): Migrate all users of this method to be agnostic of PjRt.
  absl::Span<const std::shared_ptr<PjRtBuffer>> pjrt_buffers() const {
    ifrt::Array* ifrt_array_ptr = ifrt_array();
    if (ifrt_array_ptr == nullptr) {
      return {};
    }
    auto* arr =
        llvm::dyn_cast_or_null<ifrt::PjRtCompatibleArray>(ifrt_array_ptr);
    if (arr == nullptr) {
      throw XlaRuntimeError(
          "This operation is implemented for a PjRt-compatible backend only.");
    }
    return arr->pjrt_buffers();
  }

  int num_addressable_shards() const {
    ifrt::Array* ifrt_array_ptr = ifrt_array();
    if (ifrt_array_ptr == nullptr) {
      return 0;
    }
    auto* arr =
        llvm::dyn_cast_or_null<ifrt::PjRtCompatibleArray>(ifrt_array_ptr);
    if (arr == nullptr) {
      // TODO(hyeontaek): Add num_addressable_shards to ifrt.
      return num_shards();
    }
    return arr->pjrt_buffers().size();
  }

  std::vector<PyArray>& py_arrays() { return GetStorage().py_arrays; }
  const std::vector<PyArray>& py_arrays() const {
    return GetStorage().py_arrays;
  }
  const std::vector<PyArray>& py_arrays_cached();

  nanobind::object arrays();
  Status set_arrays(nanobind::object obj);
  StatusOr<PyArray> FullyReplicatedShard();

  int num_shards() const {
    ifrt::Array* ifrt_array_ptr = ifrt_array();
    if (ifrt_array_ptr == nullptr) {
      return 0;
    }
    return ifrt_array_ptr->sharding().devices().size();
  }

  // TODO(yashkatariya): remove this once the transition completes.
  bool fastpath_enabled() const { return GetStorage().fastpath_enabled; }

  static nanobind::handle type() {
    DCHECK(type_);
    return nanobind::handle(type_);
  }

  static bool IsPyArray(nanobind::handle arg) {
    return arg.type().is(PyArray::type());
  }

  Status BlockUntilReady() const;

  absl::Status BlockUntilResultStatusIsReady();

  StatusOr<size_t> GetOnDeviceSizeInBytes();
  StatusOr<nanobind::object> SingleDeviceArrayToNumpyArray();
  Status CopySingleDeviceArrayToHostAsync();
  nanobind::dict CudaArrayInterface();
  StatusOr<std::uintptr_t> UnsafeBufferPointer();

  Status Delete();

  bool IsDeleted() const;

  PyArray Clone() const;

  StatusOr<PyArray> CopyToDeviceWithSharding(ifrt::DeviceList devices,
                                             nanobind::object dst_sharding);

  static StatusOr<PyArray> BatchedDevicePut(
      nanobind::object aval, nanobind::object sharding,
      std::vector<nanobind::object> xs,
      std::vector<ClientAndPtr<PjRtDevice>> dst_devices, bool committed,
      bool force_copy, PjRtClient::HostBufferSemantics host_buffer_semantics,
      bool jax_enable_x64);

 private:
  StatusOr<PyArray> FetchSingleShard(std::string_view api);
  StatusOr<PyArray> AssertUnsharded(std::string_view api);

  void CheckAndRearrange();

  void SetIfrtArray(tsl::RCReference<ifrt::Array> ifrt_array);

  Storage& GetStorage();
  const Storage& GetStorage() const;

  static Status SetUpType();

  inline static PyObject* type_ = nullptr;
};

class PyArrayResultHandler {
 public:
  PyArrayResultHandler(nanobind::object aval, nanobind::object sharding,
                       bool committed, bool skip_checks);

  PyArray Call(absl::Span<const PyArray> py_arrays) const;
  PyArray Call(PyArray py_array) const;

  PyArray Call(std::shared_ptr<PyClient> py_client,
               tsl::RCReference<ifrt::Array> ifrt_array,
               xla::PjRtFuture<absl::Status> result_status =
                   xla::PjRtFuture<absl::Status>()) const;

 private:
  nanobind::object aval_;
  nanobind::object sharding_;
  bool weak_type_;
  bool committed_;
  bool skip_checks_;

  nb_dtype dtype_;
  std::vector<int64_t> shape_;
};

StatusOr<nanobind::object> CudaArrayInterfaceToBuffer(
    const nanobind::dict& cai, std::shared_ptr<PyClient> cuda_client);

}  // namespace xla

#endif  // XLA_PYTHON_PY_ARRAY_H_
