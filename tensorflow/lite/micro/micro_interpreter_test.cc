/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/lite/micro/micro_interpreter.h"

#include <cstdint>

#include "tensorflow/lite/core/api/flatbuffer_conversions.h"
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/compatibility.h"
#include "tensorflow/lite/micro/micro_arena_constants.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_profiler.h"
#include "tensorflow/lite/micro/recording_micro_allocator.h"
#include "tensorflow/lite/micro/recording_simple_memory_allocator.h"
#include "tensorflow/lite/micro/test_helpers.h"
#include "tensorflow/lite/micro/testing/micro_test.h"

namespace tflite {
namespace {

class MockProfiler : public MicroProfiler {
 public:
  MockProfiler() : event_starts_(0), event_ends_(0) {}

  uint32_t BeginEvent(const char* tag) override {
    event_starts_++;
    return 0;
  }

  void EndEvent(uint32_t event_handle) override { event_ends_++; }

  int event_starts() { return event_starts_; }
  int event_ends() { return event_ends_; }

 private:
  int event_starts_;
  int event_ends_;

  TF_LITE_REMOVE_VIRTUAL_DELETE
};

// Some targets does not support dynamic memory (i.e., no malloc or new), thus,
// the test need to place non-transitent memories in global variables. This is
// safe because tests are guarateed to run serially.
tflite::AllOpsResolver g_op_resolver;
constexpr size_t kAllocatorBufferSize = 1024 * 2;
uint8_t g_allocator_buffer[kAllocatorBufferSize];

tflite::MicroInterpreter CreateInterpreterWithSimpleMockModel() {
  const tflite::Model* model = tflite::testing::GetSimpleMockModel();
  g_op_resolver = tflite::testing::GetOpResolver();

  tflite::MicroInterpreter interpreter(model, g_op_resolver, g_allocator_buffer,
                                       kAllocatorBufferSize,
                                       tflite::GetMicroErrorReporter());
  return interpreter;
}

// Test structure for external context payload.
struct TestExternalContextPayloadData {
  // Opaque blob
  uint8_t blob_data[128];
};
}  // namespace
}  // namespace tflite

TF_LITE_MICRO_TESTS_BEGIN

// Ensures that a regular set and get pair works ok.
TF_LITE_MICRO_TEST(TestSetGetExternalContextSuccess) {
  tflite::MicroInterpreter interpreter =
      tflite::CreateInterpreterWithSimpleMockModel();

  tflite::TestExternalContextPayloadData payload;
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk,
                          interpreter.SetMicroExternalContext(&payload));

  tflite::TestExternalContextPayloadData* returned_external_context =
      reinterpret_cast<tflite::TestExternalContextPayloadData*>(
          interpreter.GetMicroExternalContext());

  // What is returned should be the same as what is set.
  TF_LITE_MICRO_EXPECT((void*)returned_external_context == (void*)(&payload));
}

TF_LITE_MICRO_TEST(TestGetExternalContextWithoutSetShouldReturnNull) {
  tflite::MicroInterpreter interpreter =
      tflite::CreateInterpreterWithSimpleMockModel();

  // Return a null if nothing is set before.
  TF_LITE_MICRO_EXPECT((void*)interpreter.GetMicroExternalContext() ==
                       (nullptr));
}

TF_LITE_MICRO_TEST(TestSetExternalContextCanOnlyBeCalledOnce) {
  tflite::MicroInterpreter interpreter =
      tflite::CreateInterpreterWithSimpleMockModel();

  tflite::TestExternalContextPayloadData payload;
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk,
                          interpreter.SetMicroExternalContext(&payload));

  // Another set should fail.
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteError,
                          interpreter.SetMicroExternalContext(&payload));
}

TF_LITE_MICRO_TEST(TestInterpreter) {
  const tflite::Model* model = tflite::testing::GetSimpleMockModel();
  TF_LITE_MICRO_EXPECT_NE(nullptr, model);

  tflite::AllOpsResolver op_resolver = tflite::testing::GetOpResolver();

  constexpr size_t allocator_buffer_size = 2000;
  uint8_t allocator_buffer[allocator_buffer_size];

  // Create a new scope so that we can test the destructor.
  {
    tflite::MicroInterpreter interpreter(model, op_resolver, allocator_buffer,
                                         allocator_buffer_size,
                                         tflite::GetMicroErrorReporter());
    TF_LITE_MICRO_EXPECT_EQ(interpreter.AllocateTensors(), kTfLiteOk);
    TF_LITE_MICRO_EXPECT_LE(interpreter.arena_used_bytes(), 928 + 100);
    TF_LITE_MICRO_EXPECT_EQ(static_cast<size_t>(1), interpreter.inputs_size());
    TF_LITE_MICRO_EXPECT_EQ(static_cast<size_t>(2), interpreter.outputs_size());

    TfLiteTensor* input = interpreter.input(0);
    TF_LITE_MICRO_EXPECT_NE(nullptr, input);
    TF_LITE_MICRO_EXPECT_EQ(kTfLiteInt32, input->type);
    TF_LITE_MICRO_EXPECT_EQ(1, input->dims->size);
    TF_LITE_MICRO_EXPECT_EQ(1, input->dims->data[0]);
    TF_LITE_MICRO_EXPECT_EQ(static_cast<size_t>(4), input->bytes);
    TF_LITE_MICRO_EXPECT_NE(nullptr, input->data.i32);
    input->data.i32[0] = 21;

    TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, interpreter.Invoke());

    TfLiteTensor* output = interpreter.output(0);
    TF_LITE_MICRO_EXPECT_NE(nullptr, output);
    TF_LITE_MICRO_EXPECT_EQ(kTfLiteInt32, output->type);
    TF_LITE_MICRO_EXPECT_EQ(1, output->dims->size);
    TF_LITE_MICRO_EXPECT_EQ(1, output->dims->data[0]);
    TF_LITE_MICRO_EXPECT_EQ(static_cast<size_t>(4), output->bytes);
    TF_LITE_MICRO_EXPECT_NE(nullptr, output->data.i32);
    TF_LITE_MICRO_EXPECT_EQ(42, output->data.i32[0]);

    output = interpreter.output(1);
    TF_LITE_MICRO_EXPECT_NE(nullptr, output);
    TF_LITE_MICRO_EXPECT_EQ(kTfLiteInt32, output->type);
    TF_LITE_MICRO_EXPECT_EQ(1, output->dims->size);
    TF_LITE_MICRO_EXPECT_EQ(1, output->dims->data[0]);
    TF_LITE_MICRO_EXPECT_EQ(static_cast<size_t>(4), output->bytes);
    TF_LITE_MICRO_EXPECT_NE(nullptr, output->data.i32);
    TF_LITE_MICRO_EXPECT_EQ(42, output->data.i32[0]);
  }

  TF_LITE_MICRO_EXPECT_EQ(tflite::testing::MockCustom::freed_, true);
}

TF_LITE_MICRO_TEST(TestMultiTenantInterpreter) {
  tflite::AllOpsResolver op_resolver = tflite::testing::GetOpResolver();
  constexpr size_t arena_size = 8192;
  uint8_t arena[arena_size];

  size_t simple_model_head_usage = 0, complex_model_head_usage = 0;

  // Get simple_model_head_usage.
  {
    tflite::RecordingMicroAllocator* allocator =
        tflite::RecordingMicroAllocator::Create(
            arena, arena_size, tflite::GetMicroErrorReporter());
    const tflite::Model* model0 = tflite::testing::GetSimpleMockModel();
    tflite::MicroInterpreter interpreter0(model0, op_resolver, allocator,
                                          tflite::GetMicroErrorReporter());
    TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, interpreter0.AllocateTensors());
    simple_model_head_usage =
        allocator->GetSimpleMemoryAllocator()->GetHeadUsedBytes();

    TfLiteTensor* input = interpreter0.input(0);
    TfLiteTensor* output = interpreter0.output(0);
    input->data.i32[0] = 21;
    TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, interpreter0.Invoke());
    TF_LITE_MICRO_EXPECT_EQ(42, output->data.i32[0]);
  }

  // Shared allocator for various models.
  tflite::RecordingMicroAllocator* allocator =
      tflite::RecordingMicroAllocator::Create(arena, arena_size,
                                              tflite::GetMicroErrorReporter());

  // Get complex_model_head_usage. No head space reuse since it's the first
  // model allocated in the `allocator`.
  const tflite::Model* model1 = tflite::testing::GetComplexMockModel();
  tflite::MicroInterpreter interpreter1(model1, op_resolver, allocator,
                                        tflite::GetMicroErrorReporter());
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, interpreter1.AllocateTensors());
  TfLiteTensor* input1 = interpreter1.input(0);
  TfLiteTensor* output1 = interpreter1.output(0);
  complex_model_head_usage =
      allocator->GetSimpleMemoryAllocator()->GetHeadUsedBytes();

  // Allocate simple model from the same `allocator`. Some head space will
  // be reused thanks to multi-tenant TFLM support. Also makes sure that
  // the output is correct.
  const tflite::Model* model2 = tflite::testing::GetSimpleMockModel();
  tflite::MicroInterpreter interpreter2(model2, op_resolver, allocator,
                                        tflite::GetMicroErrorReporter());
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, interpreter2.AllocateTensors());
  TfLiteTensor* input2 = interpreter2.input(0);
  TfLiteTensor* output2 = interpreter2.output(0);
  // Verify that 1 + 1 < 2.
  size_t multi_tenant_head_usage =
      allocator->GetSimpleMemoryAllocator()->GetHeadUsedBytes();
  TF_LITE_MICRO_EXPECT_LE(multi_tenant_head_usage,
                          complex_model_head_usage + simple_model_head_usage);

  // Now we have model1 and model2 sharing the same `allocator`.
  // Let's make sure that they can produce correct results.
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteInt32, input1->type);
  input1->data.i32[0] = 10;
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, interpreter1.Invoke());
  // Output tensor for the first model.
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteInt32, output1->type);
  TF_LITE_MICRO_EXPECT_EQ(10, output1->data.i32[0]);

  TF_LITE_MICRO_EXPECT_EQ(kTfLiteInt32, input2->type);
  input2->data.i32[0] = 21;
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, interpreter2.Invoke());
  // Output for the second model.
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteInt32, output2->type);
  TF_LITE_MICRO_EXPECT_EQ(42, output2->data.i32[0]);

  // Allocate another complex model from the `allocator` will not increase
  // head space usage.
  const tflite::Model* model3 = tflite::testing::GetComplexMockModel();
  tflite::MicroInterpreter interpreter3(model3, op_resolver, allocator,
                                        tflite::GetMicroErrorReporter());
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, interpreter3.AllocateTensors());
  TfLiteTensor* input3 = interpreter3.input(0);
  TfLiteTensor* output3 = interpreter3.output(0);
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteInt32, input3->type);
  input3->data.i32[0] = 10;
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, interpreter3.Invoke());
  // Output tensor for the third model.
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteInt32, output3->type);
  TF_LITE_MICRO_EXPECT_EQ(10, output3->data.i32[0]);
  // No increase on the head usage as we're reusing the space.
  TF_LITE_MICRO_EXPECT_EQ(
      multi_tenant_head_usage,
      allocator->GetSimpleMemoryAllocator()->GetHeadUsedBytes());
}

TF_LITE_MICRO_TEST(TestKernelMemoryPlanning) {
  const tflite::Model* model = tflite::testing::GetSimpleStatefulModel();
  TF_LITE_MICRO_EXPECT_NE(nullptr, model);

  tflite::AllOpsResolver op_resolver = tflite::testing::GetOpResolver();

  constexpr size_t allocator_buffer_size = 4096;
  uint8_t allocator_buffer[allocator_buffer_size];

  tflite::RecordingMicroAllocator* allocator =
      tflite::RecordingMicroAllocator::Create(allocator_buffer,
                                              allocator_buffer_size,
                                              tflite::GetMicroErrorReporter());

  // Make sure kernel memory planning works in multi-tenant context.
  for (int i = 0; i < 3; i++) {
    tflite::MicroInterpreter interpreter(model, op_resolver, allocator,
                                         tflite::GetMicroErrorReporter());
    TF_LITE_MICRO_EXPECT_EQ(interpreter.AllocateTensors(), kTfLiteOk);
    TF_LITE_MICRO_EXPECT_EQ(static_cast<size_t>(1), interpreter.inputs_size());
    TF_LITE_MICRO_EXPECT_EQ(static_cast<size_t>(2), interpreter.outputs_size());

    TfLiteTensor* input = interpreter.input(0);
    TF_LITE_MICRO_EXPECT_EQ(1, input->dims->size);
    TF_LITE_MICRO_EXPECT_EQ(3, input->dims->data[0]);
    input->data.uint8[0] = 2;
    input->data.uint8[1] = 3;
    input->data.uint8[2] = 1;

    uint8_t expected_median = 2;

    {
      TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, interpreter.Invoke());
      TfLiteTensor* median = interpreter.output(0);
      TF_LITE_MICRO_EXPECT_EQ(expected_median, median->data.uint8[0]);
      TfLiteTensor* invoke_count = interpreter.output(1);
      TF_LITE_MICRO_EXPECT_EQ(1, invoke_count->data.i32[0]);
    }

    {
      TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, interpreter.Invoke());
      TfLiteTensor* median = interpreter.output(0);
      TF_LITE_MICRO_EXPECT_EQ(expected_median, median->data.uint8[0]);
      TfLiteTensor* invoke_count = interpreter.output(1);
      TF_LITE_MICRO_EXPECT_EQ(2, invoke_count->data.i32[0]);
    }
  }
}

// The interpreter initialization requires multiple steps and this test case
// ensures that simply creating and destructing an interpreter object is ok.
// b/147830765 has one example of a change that caused trouble for this simple
// case.
TF_LITE_MICRO_TEST(TestIncompleteInitialization) {
  const tflite::Model* model = tflite::testing::GetComplexMockModel();
  TF_LITE_MICRO_EXPECT_NE(nullptr, model);

  tflite::AllOpsResolver op_resolver = tflite::testing::GetOpResolver();

  constexpr size_t allocator_buffer_size = 2048;
  uint8_t allocator_buffer[allocator_buffer_size];

  tflite::MicroInterpreter interpreter(model, op_resolver, allocator_buffer,
                                       allocator_buffer_size,
                                       tflite::GetMicroErrorReporter());
}

// Test that an interpreter with a supplied profiler correctly calls the
// profiler each time an operator is invoked.
TF_LITE_MICRO_TEST(InterpreterWithProfilerShouldProfileOps) {
  const tflite::Model* model = tflite::testing::GetComplexMockModel();
  TF_LITE_MICRO_EXPECT_NE(nullptr, model);

  tflite::AllOpsResolver op_resolver = tflite::testing::GetOpResolver();

  constexpr size_t allocator_buffer_size = 2048;
  uint8_t allocator_buffer[allocator_buffer_size];
  tflite::MockProfiler profiler;
  tflite::MicroInterpreter interpreter(
      model, op_resolver, allocator_buffer, allocator_buffer_size,
      tflite::GetMicroErrorReporter(), nullptr, &profiler);

  TF_LITE_MICRO_EXPECT_EQ(profiler.event_starts(), 0);
  TF_LITE_MICRO_EXPECT_EQ(profiler.event_ends(), 0);
  TF_LITE_MICRO_EXPECT_EQ(interpreter.AllocateTensors(), kTfLiteOk);
  TF_LITE_MICRO_EXPECT_EQ(interpreter.Invoke(), kTfLiteOk);
#ifndef TF_LITE_STRIP_ERROR_STRINGS
  TF_LITE_MICRO_EXPECT_EQ(profiler.event_starts(), 3);
  TF_LITE_MICRO_EXPECT_EQ(profiler.event_ends(), 3);
#else
  TF_LITE_MICRO_EXPECT_EQ(profiler.event_starts(), 0);
  TF_LITE_MICRO_EXPECT_EQ(profiler.event_ends(), 0);
#endif
}

TF_LITE_MICRO_TEST(TestIncompleteInitializationAllocationsWithSmallArena) {
  const tflite::Model* model = tflite::testing::GetComplexMockModel();
  TF_LITE_MICRO_EXPECT_NE(nullptr, model);

  tflite::AllOpsResolver op_resolver = tflite::testing::GetOpResolver();

  // This test is designed to create the following classes/buffers successfully
  // on the arena:
  //
  // From tail: RecordingSimpleMemoryAllocator, RecordingMicroAllocator,
  //        RecordingMicroAllocator.
  //
  // From head:ScratchBufferRequest buffer.
  //
  // Since sizes of the above classes vary between architecture, we use sizeof
  // for whatever is visible from this test file. For those that are not visible
  // from this test file, we use the upper bound for x86 architecture since it
  // is not ideal to expose definitions for test only.
  constexpr size_t max_scratch_buffer_request_size = 192;
  constexpr size_t max_micro_builtin_data_allocator_size = 16;
  constexpr size_t allocator_buffer_size =
      sizeof(tflite::RecordingSimpleMemoryAllocator) +
      sizeof(tflite::RecordingMicroAllocator) +
      max_micro_builtin_data_allocator_size + max_scratch_buffer_request_size;
  uint8_t allocator_buffer[allocator_buffer_size];

  tflite::RecordingMicroAllocator* allocator =
      tflite::RecordingMicroAllocator::Create(allocator_buffer,
                                              allocator_buffer_size,
                                              tflite::GetMicroErrorReporter());
  TF_LITE_MICRO_EXPECT_NE(nullptr, allocator);

  tflite::MicroInterpreter interpreter(model, op_resolver, allocator,
                                       tflite::GetMicroErrorReporter());

  // Interpreter fails because arena is too small:
  TF_LITE_MICRO_EXPECT_EQ(interpreter.Invoke(), kTfLiteError);

  // The head buffer use cannot exceed the upper bound from x86.
  TF_LITE_MICRO_EXPECT_LE(
      allocator->GetSimpleMemoryAllocator()->GetHeadUsedBytes(),
      max_scratch_buffer_request_size);

  // Ensure allocations are zero (ignore tail since some internal structs are
  // initialized with this space):
  TF_LITE_MICRO_EXPECT_EQ(
      static_cast<size_t>(0),
      allocator
          ->GetRecordedAllocation(
              tflite::RecordedAllocationType::kTfLiteEvalTensorData)
          .used_bytes);
  TF_LITE_MICRO_EXPECT_EQ(
      static_cast<size_t>(0),
      allocator
          ->GetRecordedAllocation(
              tflite::RecordedAllocationType::kTfLiteTensorVariableBufferData)
          .used_bytes);
  TF_LITE_MICRO_EXPECT_EQ(
      static_cast<size_t>(0),
      allocator->GetRecordedAllocation(tflite::RecordedAllocationType::kOpData)
          .used_bytes);
}

TF_LITE_MICRO_TEST(TestInterpreterDoesNotAllocateUntilInvoke) {
  const tflite::Model* model = tflite::testing::GetComplexMockModel();
  TF_LITE_MICRO_EXPECT_NE(nullptr, model);

  tflite::AllOpsResolver op_resolver = tflite::testing::GetOpResolver();

  constexpr size_t allocator_buffer_size = 1024 * 10;
  uint8_t allocator_buffer[allocator_buffer_size];

  tflite::RecordingMicroAllocator* allocator =
      tflite::RecordingMicroAllocator::Create(allocator_buffer,
                                              allocator_buffer_size,
                                              tflite::GetMicroErrorReporter());
  TF_LITE_MICRO_EXPECT_NE(nullptr, allocator);

  tflite::MicroInterpreter interpreter(model, op_resolver, allocator,
                                       tflite::GetMicroErrorReporter());

  // Ensure allocations are zero (ignore tail since some internal structs are
  // initialized with this space):
  TF_LITE_MICRO_EXPECT_EQ(
      static_cast<size_t>(0),
      allocator->GetSimpleMemoryAllocator()->GetHeadUsedBytes());
  TF_LITE_MICRO_EXPECT_EQ(
      static_cast<size_t>(0),
      allocator
          ->GetRecordedAllocation(
              tflite::RecordedAllocationType::kTfLiteTensorVariableBufferData)
          .used_bytes);
  TF_LITE_MICRO_EXPECT_EQ(
      static_cast<size_t>(0),
      allocator
          ->GetRecordedAllocation(
              tflite::RecordedAllocationType::kTfLiteEvalTensorData)
          .used_bytes);
  TF_LITE_MICRO_EXPECT_EQ(
      static_cast<size_t>(0),
      allocator->GetRecordedAllocation(tflite::RecordedAllocationType::kOpData)
          .used_bytes);

  TF_LITE_MICRO_EXPECT_EQ(interpreter.Invoke(), kTfLiteOk);
  allocator->PrintAllocations();

  // Allocation sizes vary based on platform - check that allocations are now
  // non-zero:
  TF_LITE_MICRO_EXPECT_GT(
      allocator->GetSimpleMemoryAllocator()->GetHeadUsedBytes(),
      static_cast<size_t>(0));
  TF_LITE_MICRO_EXPECT_GT(
      allocator
          ->GetRecordedAllocation(
              tflite::RecordedAllocationType::kTfLiteEvalTensorData)
          .used_bytes,
      0);

  TF_LITE_MICRO_EXPECT_GT(
      allocator
          ->GetRecordedAllocation(
              tflite::RecordedAllocationType::kTfLiteTensorVariableBufferData)
          .used_bytes,
      static_cast<size_t>(0));

  // TODO(b/160160549): This check is mostly meaningless right now because the
  // operator creation in our mock models is inconsistent.  Revisit what
  // this check should be once the mock models are properly created.
  TF_LITE_MICRO_EXPECT_EQ(
      allocator->GetRecordedAllocation(tflite::RecordedAllocationType::kOpData)
          .used_bytes,
      static_cast<size_t>(0));
}

TF_LITE_MICRO_TEST(TestInterpreterMultipleInputs) {
  const tflite::Model* model = tflite::testing::GetSimpleMultipleInputsModel();
  TF_LITE_MICRO_EXPECT_NE(nullptr, model);

  tflite::AllOpsResolver op_resolver = tflite::testing::GetOpResolver();

  constexpr size_t allocator_buffer_size = 2000;
  uint8_t allocator_buffer[allocator_buffer_size];

  // Create a new scope so that we can test the destructor.
  {
    tflite::MicroInterpreter interpreter(model, op_resolver, allocator_buffer,
                                         allocator_buffer_size,
                                         tflite::GetMicroErrorReporter());

    TF_LITE_MICRO_EXPECT_EQ(interpreter.AllocateTensors(), kTfLiteOk);
    TF_LITE_MICRO_EXPECT_LE(interpreter.arena_used_bytes(), 928 + 100);

    TF_LITE_MICRO_EXPECT_EQ(static_cast<size_t>(3), interpreter.inputs_size());
    TF_LITE_MICRO_EXPECT_EQ(static_cast<size_t>(1), interpreter.outputs_size());

    TfLiteTensor* input = interpreter.input(0);
    TF_LITE_MICRO_EXPECT_NE(nullptr, input);
    TF_LITE_MICRO_EXPECT_EQ(kTfLiteInt32, input->type);
    TF_LITE_MICRO_EXPECT_EQ(1, input->dims->size);
    TF_LITE_MICRO_EXPECT_EQ(1, input->dims->data[0]);
    TF_LITE_MICRO_EXPECT_EQ(static_cast<size_t>(4), input->bytes);
    TF_LITE_MICRO_EXPECT_NE(nullptr, input->data.i32);
    input->data.i32[0] = 21;

    TfLiteTensor* input1 = interpreter.input(1);
    TF_LITE_MICRO_EXPECT_NE(nullptr, input1);
    TF_LITE_MICRO_EXPECT_EQ(kTfLiteInt8, input1->type);
    TF_LITE_MICRO_EXPECT_EQ(1, input1->dims->size);
    TF_LITE_MICRO_EXPECT_EQ(1, input1->dims->data[0]);
    TF_LITE_MICRO_EXPECT_EQ(static_cast<size_t>(1), input1->bytes);
    TF_LITE_MICRO_EXPECT_NE(nullptr, input1->data.i32);
    input1->data.i32[0] = 21;

    TfLiteTensor* input2 = interpreter.input(2);
    TF_LITE_MICRO_EXPECT_NE(nullptr, input2);
    TF_LITE_MICRO_EXPECT_EQ(kTfLiteInt32, input2->type);
    TF_LITE_MICRO_EXPECT_EQ(1, input2->dims->size);
    TF_LITE_MICRO_EXPECT_EQ(1, input2->dims->data[0]);
    TF_LITE_MICRO_EXPECT_EQ(static_cast<size_t>(4), input2->bytes);
    TF_LITE_MICRO_EXPECT_NE(nullptr, input2->data.i32);
    input2->data.i32[0] = 24;

    TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, interpreter.Invoke());

    TfLiteTensor* output = interpreter.output(0);
    TF_LITE_MICRO_EXPECT_NE(nullptr, output);
    TF_LITE_MICRO_EXPECT_EQ(kTfLiteInt32, output->type);
    TF_LITE_MICRO_EXPECT_EQ(1, output->dims->size);
    TF_LITE_MICRO_EXPECT_EQ(1, output->dims->data[0]);
    TF_LITE_MICRO_EXPECT_EQ(static_cast<size_t>(4), output->bytes);
    TF_LITE_MICRO_EXPECT_NE(nullptr, output->data.i32);
    TF_LITE_MICRO_EXPECT_EQ(66, output->data.i32[0]);
  }

  TF_LITE_MICRO_EXPECT_EQ(tflite::testing::MultipleInputs::freed_, true);
}

TF_LITE_MICRO_TEST(TestInterpreterNullInputsAndOutputs) {
  const tflite::Model* model =
      tflite::testing::GetSimpleModelWithNullInputsAndOutputs();
  TF_LITE_MICRO_EXPECT_NE(nullptr, model);

  tflite::AllOpsResolver op_resolver = tflite::testing::GetOpResolver();

  constexpr size_t allocator_buffer_size = 2000;
  uint8_t allocator_buffer[allocator_buffer_size];

  tflite::MicroInterpreter interpreter(model, op_resolver, allocator_buffer,
                                       allocator_buffer_size,
                                       tflite::GetMicroErrorReporter());

  TF_LITE_MICRO_EXPECT_EQ(interpreter.AllocateTensors(), kTfLiteOk);

  TF_LITE_MICRO_EXPECT_EQ(static_cast<size_t>(1), interpreter.inputs_size());
  TF_LITE_MICRO_EXPECT_EQ(static_cast<size_t>(1), interpreter.outputs_size());

  TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, interpreter.Invoke());
}

// This test is disabled from Bluepill platform because it requires more SRAM
// than what our Bluepill simulation platform specifies.
TF_LITE_MICRO_TEST(TestArenaUsedBytes) {
  const tflite::Model* model = tflite::testing::GetModelWith256x256Tensor();
  TF_LITE_MICRO_EXPECT_NE(nullptr, model);

  tflite::AllOpsResolver op_resolver = tflite::testing::GetOpResolver();

  constexpr size_t arena_buffer_size = 256 * 1024;
  uint8_t arena_buffer[arena_buffer_size];

  tflite::MicroInterpreter interpreter(model, op_resolver, arena_buffer,
                                       arena_buffer_size,
                                       tflite::GetMicroErrorReporter());

  TF_LITE_MICRO_EXPECT_EQ(interpreter.AllocateTensors(), kTfLiteOk);

  // Store the required arena size before Invoke() because this is what this
  // api might be used.
  size_t used_arena_size = interpreter.arena_used_bytes();

  TF_LITE_MICRO_EXPECT_EQ(interpreter.Invoke(), kTfLiteOk);

  // The reported used_arena_size plus alignment padding is sufficient for this
  // model to run. Plus alignment padding is because SimpleMemoryAllocator is
  // given the arena after the alignment.
  size_t required_arena_size =
      used_arena_size + tflite::MicroArenaBufferAlignment();

  tflite::MicroInterpreter interpreter2(model, op_resolver, arena_buffer,
                                        required_arena_size,
                                        tflite::GetMicroErrorReporter());

  TF_LITE_MICRO_EXPECT_EQ(interpreter2.AllocateTensors(), kTfLiteOk);

  TF_LITE_MICRO_EXPECT_EQ(interpreter2.Invoke(), kTfLiteOk);
}

TF_LITE_MICRO_TESTS_END
