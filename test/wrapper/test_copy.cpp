// SPDX-License-Identifier: MIT
// Copyright (c) 2023-2024, Advanced Micro Devices, Inc. All rights reserved.

#include <numeric>
#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <vector>
#include <gtest/gtest.h>

#include "ck/host_utility/kernel_launch.hpp"
#include "ck/library/utility/device_memory.hpp"
#include "ck/library/utility/check_err.hpp"
#include "ck/utility/common_header.hpp"
#include "ck/wrapper/layout.hpp"
#include "ck/wrapper/tensor.hpp"
#include "ck/wrapper/operations/copy.hpp"

// Test copy from Global to Global through LDS and VGPR
template <typename InputTensor,
          typename OutputTensor,
          typename BlockShape,
          typename ThreadLayoutShape,
          typename LocalTileSteps,
          typename LocalPartitionSteps>
__global__ void TestCopyDevice(const InputTensor input_tensor,
                               OutputTensor output_tensor,
                               const BlockShape tile_shape,
                               const ThreadLayoutShape thread_layout,
                               const LocalTileSteps block_steps,
                               const LocalPartitionSteps thread_steps)
{
    __shared__ ck::index_t p_shared[ck::wrapper::size(tile_shape)];
    auto tensor_lds = ck::wrapper::make_tensor<ck::wrapper::MemoryTypeEnum::Lds>(
        p_shared, ck::wrapper::make_layout(tile_shape));

    const auto block_idxs = ck::make_tuple(ck::make_tuple(0, 0), blockIdx.x);

    // Get local tiles for global memory
    const auto input_local_tile =
        ck::wrapper::make_local_tile(input_tensor, tile_shape, block_idxs, block_steps);
    const auto output_local_tile =
        ck::wrapper::make_local_tile(output_tensor, tile_shape, block_idxs, block_steps);

    // Get partition per thread
    const auto input_local_partition = ck::wrapper::make_local_partition(
        input_local_tile, thread_layout, threadIdx.x, thread_steps);
    auto lds_local_partition =
        ck::wrapper::make_local_partition(tensor_lds, thread_layout, threadIdx.x, thread_steps);
    auto output_local_partition = ck::wrapper::make_local_partition(
        output_local_tile, thread_layout, threadIdx.x, thread_steps);

    // Allocate VGPR
    constexpr ck::index_t scalar_per_vector = 1;
    constexpr ck::index_t vgpr_size         = ck::wrapper::size(lds_local_partition);
    auto tensor_vgpr = ck::wrapper::make_register_tensor<ck::wrapper::MemoryTypeEnum::Vgpr,
                                                         vgpr_size,
                                                         scalar_per_vector,
                                                         ck::index_t>();

    // Perform copy
    ck::wrapper::copy(input_local_partition, lds_local_partition);
    ck::wrapper::copy(lds_local_partition, tensor_vgpr);
    ck::wrapper::copy(tensor_vgpr, output_local_partition);
}

void PerformCopyGlobalToGlobalViaLDS()
{
    const auto shape =
        ck::make_tuple(ck::make_tuple(ck::Number<2>{}, ck::Number<2>{}), ck::Number<256>{});
    const auto strides =
        ck::make_tuple(ck::make_tuple(ck::Number<1>{}, ck::Number<2>{}), ck::Number<4>{});
    const auto layout = ck::wrapper::make_layout(shape, strides);

    // 0, 1, 2, ..., size(shape) - 1
    std::vector<ck::index_t> input_data(ck::wrapper::size(shape));
    std::iota(input_data.begin(), input_data.end(), 0);

    // Global memory buffers
    DeviceMem in_buf(ck::wrapper::size(layout) * sizeof(ck::index_t));
    DeviceMem out_buf(ck::wrapper::size(layout) * sizeof(ck::index_t));

    in_buf.ToDevice(input_data.data());
    out_buf.SetZero();

    // Create tensors for global memory
    const auto input_tensor_global = ck::wrapper::make_tensor<ck::wrapper::MemoryTypeEnum::Global>(
        static_cast<const ck::index_t*>(in_buf.GetDeviceBuffer()), layout);
    auto output_tensor_global = ck::wrapper::make_tensor<ck::wrapper::MemoryTypeEnum::Global>(
        static_cast<ck::index_t*>(out_buf.GetDeviceBuffer()), layout);

    const auto thread_layout =
        ck::make_tuple(ck::make_tuple(ck::Number<1>{}, ck::Number<1>{}), ck::Number<32>{});
    const auto tile_shape =
        ck::make_tuple(ck::make_tuple(ck::Number<2>{}, ck::Number<2>{}), ck::Number<64>{});

    const auto thread_steps =
        ck::make_tuple(ck::make_tuple(ck::Number<1>{}, ck::Number<1>{}), ck::Number<2>{});
    const auto block_steps =
        ck::make_tuple(ck::make_tuple(ck::Number<1>{}, ck::Number<1>{}), ck::Number<64>{});

    const ck::index_t grid_size = ck::math::integer_divide_ceil(
        ck::wrapper::size(input_tensor_global), ck::wrapper::size(tile_shape));

    const auto kernel = TestCopyDevice<decltype(input_tensor_global),
                                       decltype(output_tensor_global),
                                       decltype(tile_shape),
                                       decltype(thread_layout),
                                       decltype(block_steps),
                                       decltype(thread_steps)>;
    launch_and_time_kernel(StreamConfig{},
                           kernel,
                           dim3(grid_size),
                           dim3(ck::wrapper::size(thread_layout)),
                           0,
                           input_tensor_global,
                           output_tensor_global,
                           tile_shape,
                           thread_layout,
                           block_steps,
                           thread_steps);

    // Verify results
    std::vector<ck::index_t> output_data(ck::wrapper::size(shape));
    out_buf.FromDevice(output_data.data());
    EXPECT_TRUE(ck::utils::check_err(output_data, input_data));
}

TEST(TestCopy, CopyGlobalToGlobalViaLDS) { PerformCopyGlobalToGlobalViaLDS(); }
