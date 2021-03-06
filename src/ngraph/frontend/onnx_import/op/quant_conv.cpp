//*****************************************************************************
// Copyright 2017-2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#include <cstddef>
#include <memory>
#include <vector>

#include "ngraph/builder/quantization/quantized_linear_convolution.hpp"
#include "ngraph/coordinate_diff.hpp"
#include "ngraph/frontend/onnx_import/exceptions.hpp"
#include "ngraph/frontend/onnx_import/op/conv.hpp"
#include "ngraph/frontend/onnx_import/utils/convpool.hpp"
#include "ngraph/op/add.hpp"
#include "ngraph/op/broadcast.hpp"
#include "ngraph/op/concat.hpp"
#include "ngraph/op/divide.hpp"
#include "ngraph/op/experimental/quantized_conv.hpp"
#include "ngraph/op/multiply.hpp"
#include "ngraph/op/slice.hpp"
#include "ngraph/op/util/broadcasting.hpp"
#include "ngraph/strides.hpp"
#include "quant_conv.hpp"

namespace ngraph
{
    namespace onnx_import
    {
        namespace op
        {
            namespace set_1
            {
                namespace
                {
                    struct OpScale
                    {
                        std::shared_ptr<ngraph::Node> data_scale;
                        std::shared_ptr<ngraph::Node> filter_scale;
                        std::shared_ptr<ngraph::Node> output_scale;
                    };

                    std::shared_ptr<ngraph::Node>
                        make_ng_quant_conv(const std::shared_ptr<ngraph::Node>& data,
                                           const std::shared_ptr<ngraph::Node>& filters,
                                           const Strides& strides,
                                           const Strides& filter_dilations,
                                           const CoordinateDiff& padding_below,
                                           const CoordinateDiff& padding_above,
                                           const Strides& data_dilations,
                                           int groups,
                                           const OpScale& op_scale,
                                           const std::shared_ptr<ngraph::Node>& bias = nullptr)
                    {
                        if (groups > 1)
                        {
                            // Split one convolution op to N ops where N is the number of groups
                            // and concat results after computation.
                            // reference: https://github.com/NervanaSystems/ngraph-mxnet/blob/fdd692/src/ngraph/ngraph_emitter.cc#L822-L856
                            std::size_t n_data_channels{data->get_shape().at(1)};
                            std::size_t n_filters_channels{filters->get_shape().at(0)};

                            std::size_t data_group_size{n_data_channels / groups};
                            std::size_t filters_group_size{n_filters_channels / groups};
                            NodeVector convolution_nodes;

                            // initial bounds for splice
                            std::vector<std::size_t> data_lower_bounds(data->get_shape().size());
                            std::vector<std::size_t> data_upper_bounds{data->get_shape()};
                            std::vector<std::size_t> filters_lower_bounds(
                                filters->get_shape().size());
                            std::vector<std::size_t> filters_upper_bounds{filters->get_shape()};

                            for (std::size_t group{0}; group < groups; ++group)
                            {
                                // slice data
                                data_lower_bounds[1] = group * data_group_size;
                                data_upper_bounds[1] = (group + 1) * data_group_size;
                                auto sliced_data = std::make_shared<ngraph::op::Slice>(
                                    data, data_lower_bounds, data_upper_bounds);
                                // slice filters
                                filters_lower_bounds[0] = group * filters_group_size;
                                filters_upper_bounds[0] = (group + 1) * filters_group_size;
                                auto sliced_filters = std::make_shared<ngraph::op::Slice>(
                                    filters, filters_lower_bounds, filters_upper_bounds);

                                if (bias)
                                {
                                    throw error::NotSupported(
                                        "Groups != 1 not supported for Quantized Convolution with "
                                        "bias.");
                                }
                                else
                                {
                                    convolution_nodes.push_back(
                                        ngraph::builder::quantization::QuantizedLinearConvolution(
                                            sliced_data,
                                            sliced_filters,
                                            strides,
                                            filter_dilations,
                                            padding_below,
                                            padding_above,
                                            data_dilations,
                                            op_scale.data_scale,
                                            op_scale.filter_scale,
                                            op_scale.output_scale));
                                }
                            }
                            std::size_t concatenation_axis = 1;
                            return std::make_shared<ngraph::op::Concat>(convolution_nodes,
                                                                        concatenation_axis);
                        }
                        else
                        {
                            if (bias)
                            {
                                return ngraph::builder::quantization::
                                    QuantizedLinearConvolutionBias(data,
                                                                   filters,
                                                                   bias,
                                                                   strides,
                                                                   filter_dilations,
                                                                   padding_below,
                                                                   padding_above,
                                                                   data_dilations,
                                                                   op_scale.data_scale,
                                                                   op_scale.filter_scale,
                                                                   op_scale.output_scale);
                            }
                            else
                            {
                                return ngraph::builder::quantization::QuantizedLinearConvolution(
                                    data,
                                    filters,
                                    strides,
                                    filter_dilations,
                                    padding_below,
                                    padding_above,
                                    data_dilations,
                                    op_scale.data_scale,
                                    op_scale.filter_scale,
                                    op_scale.output_scale);
                            }
                        }
                    }

                } // namespace

                NodeVector quant_conv(const Node& node)
                {
                    const NodeVector& inputs = node.get_ng_inputs();
                    auto data = inputs.at(0);
                    auto filters = inputs.at(3);

                    int64_t groups{node.get_attribute_value<int64_t>("group", 1)};

                    auto data_scale = inputs.at(1);
                    auto filters_scale = inputs.at(4);
                    auto output_scale = inputs.at(6);

                    ASSERT_VALID_ARGUMENT(node,
                                          ((groups >= 0) && (groups <= data->get_shape().at(1)) &&
                                           (groups <= filters->get_shape().at(0))))
                        << "incorrect value of 'group' attribute: " << groups;

                    std::size_t n_data_channels{data->get_shape().at(1)};
                    std::size_t n_filters_channels{filters->get_shape().at(0)};

                    ASSERT_VALID_ARGUMENT(node, n_data_channels % groups == 0)
                        << "provided group attribute value must be a multiple of data channels "
                           "count.";
                    ASSERT_VALID_ARGUMENT(node, n_filters_channels % groups == 0)
                        << "provided group attribute value must be a multiple of filter channels "
                           "count.";

                    Strides strides = convpool::get_strides(node);
                    Strides filter_dilations = convpool::get_dilations(node);
                    Strides data_dilations = Strides(convpool::get_kernel_shape(node).size(), 1UL);
                    auto paddings = convpool::get_pads(node);
                    const CoordinateDiff& padding_below = paddings.first;
                    const CoordinateDiff& padding_above = paddings.second;

                    std::shared_ptr<ngraph::Node> conv_node = nullptr;

                    // no bias param
                    if (inputs.size() == 9 && !inputs.at(8)->is_null())
                    {
                        auto bias = inputs.at(8);
                        conv_node =
                            make_ng_quant_conv(data,
                                               filters,
                                               strides,
                                               filter_dilations,
                                               padding_below,
                                               padding_above,
                                               data_dilations,
                                               groups,
                                               OpScale{data_scale, filters_scale, output_scale},
                                               bias);
                    }
                    else
                    {
                        if (filters->get_element_type() == ngraph::element::u8 && groups == 1)
                        {
                            conv_node = ngraph::builder::quantization::QuantizedLinearConvolution(
                                data,
                                filters,
                                strides,
                                filter_dilations,
                                padding_below,
                                padding_above,
                                data_dilations,
                                data_scale,
                                inputs.at(2),
                                filters_scale,
                                inputs.at(5),
                                output_scale,
                                inputs.at(7));
                        }
                        else
                        {
                            conv_node = make_ng_quant_conv(
                                data,
                                filters,
                                strides,
                                filter_dilations,
                                padding_below,
                                padding_above,
                                data_dilations,
                                groups,
                                OpScale{data_scale, filters_scale, output_scale});
                        }
                    }

                    return {conv_node};
                }

            } // namespace set_1

        } //namespace op

    } // namespace onnx_import

} // namespace ngraph
