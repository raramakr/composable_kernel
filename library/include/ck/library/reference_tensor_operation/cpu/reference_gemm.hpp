// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023, Advanced Micro Devices, Inc. All rights reserved.

#pragma once

#include <iostream>
#include <sstream>

#include "ck/tensor_operation/gpu/element/unary_element_wise_operation.hpp"
#include "ck/tensor_operation/gpu/device/device_base.hpp"
#include "ck/library/utility/host_tensor.hpp"

namespace ck {
namespace tensor_operation {
namespace host {

template <typename ADataType,
          typename BDataType,
          typename CDataType,
          typename AccDataType,
          typename AElementwiseOperation,
          typename BElementwiseOperation,
          typename CElementwiseOperation,
          typename ComputeTypeA = ADataType,
          typename ComputeTypeB = ComputeTypeA>
struct ReferenceGemm : public device::BaseOperator
{
    // Argument
    struct Argument : public device::BaseArgument
    {
        Argument(const Tensor<ADataType>& a_m_k,
                 const Tensor<BDataType>& b_k_n,
                 Tensor<CDataType>& c_m_n,
                 AElementwiseOperation a_element_op,
                 BElementwiseOperation b_element_op,
                 CElementwiseOperation c_element_op)
            : a_m_k_{a_m_k},
              b_k_n_{b_k_n},
              c_m_n_{c_m_n},
              a_element_op_{a_element_op},
              b_element_op_{b_element_op},
              c_element_op_{c_element_op}
        {
        }

        const Tensor<ADataType>& a_m_k_;
        const Tensor<BDataType>& b_k_n_;
        Tensor<CDataType>& c_m_n_;

        AElementwiseOperation a_element_op_;
        BElementwiseOperation b_element_op_;
        CElementwiseOperation c_element_op_;
    };

    // Invoker
    struct Invoker : public device::BaseInvoker
    {
        using Argument = ReferenceGemm::Argument;

        float Run(const Argument& arg)
        {
            auto f_mk_kn_mn = [&](auto m, auto n) {
                const int K = arg.a_m_k_.mDesc.GetLengths()[1];

                AccDataType v_acc = 0;

                for(int k = 0; k < K; ++k)
                {
                    ComputeTypeA v_a;
                    ComputeTypeB v_b;

                    // use PassThrough instead of ConvertBF16RTN for reference calculation
                    if constexpr(is_same_v<AElementwiseOperation,
                                           ck::tensor_operation::element_wise::ConvertBF16RTN>)
                    {
                        ck::tensor_operation::element_wise::PassThrough{}(v_a, arg.a_m_k_(m, k));
                    }
                    else
                    {
                        arg.a_element_op_(v_a, arg.a_m_k_(m, k));
                    }
                    // same for B matrix
                    if constexpr(is_same_v<BElementwiseOperation,
                                           ck::tensor_operation::element_wise::ConvertBF16RTN>)
                    {
                        ck::tensor_operation::element_wise::PassThrough{}(v_b, arg.b_k_n_(k, n));
                    }
                    else
                    {
                        arg.b_element_op_(v_b, arg.b_k_n_(k, n));
                    }

                    v_acc +=
                        ck::type_convert<AccDataType>(v_a) * ck::type_convert<AccDataType>(v_b);
                }

                CDataType v_c;

                arg.c_element_op_(v_c, v_acc);

                arg.c_m_n_(m, n) = v_c;
            };

            make_ParallelTensorFunctor(
                f_mk_kn_mn, arg.c_m_n_.mDesc.GetLengths()[0], arg.c_m_n_.mDesc.GetLengths()[1])(
                std::thread::hardware_concurrency());

            return 0;
        }

        float Run(const device::BaseArgument* p_arg,
                  const StreamConfig& /* stream_config */ = StreamConfig{}) override
        {
            return Run(*dynamic_cast<const Argument*>(p_arg));
        }
    };

    static constexpr bool IsValidCompilationParameter()
    {
        // TODO: properly implement this check
        return true;
    }

    bool IsSupportedArgument(const device::BaseArgument*) override { return true; }

    static auto MakeArgument(const Tensor<ADataType>& a_m_k,
                             const Tensor<BDataType>& b_k_n,
                             Tensor<CDataType>& c_m_n,
                             AElementwiseOperation a_element_op,
                             BElementwiseOperation b_element_op,
                             CElementwiseOperation c_element_op)
    {
        return Argument{a_m_k, b_k_n, c_m_n, a_element_op, b_element_op, c_element_op};
    }

    static auto MakeInvoker() { return Invoker{}; }

    virtual std::unique_ptr<device::BaseInvoker> MakeInvokerPointer()
    {
        return std::make_unique<Invoker>(Invoker{});
    }

    std::string GetTypeString() const override
    {
        auto str = std::stringstream();

        // clang-format off
        str << "ReferenceGemm"
            << std::endl;
        // clang-format on

        return str.str();
    }
};

} // namespace host
} // namespace tensor_operation
} // namespace ck
