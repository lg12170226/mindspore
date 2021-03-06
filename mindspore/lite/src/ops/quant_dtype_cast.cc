/**
 * Copyright 2019-2020 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/ops/quant_dtype_cast.h"

namespace mindspore {
namespace lite {
#ifdef PRIMITIVE_WRITEABLE
int QuantDTypeCast::GetSrcT() const { return this->primitive_->value.AsQuantDTypeCast()->srcT; }
int QuantDTypeCast::GetDstT() const { return this->primitive_->value.AsQuantDTypeCast()->dstT; }

void QuantDTypeCast::SetSrcT(int src_t) { this->primitive_->value.AsQuantDTypeCast()->srcT = src_t; }
void QuantDTypeCast::SetDstT(int dst_t) { this->primitive_->value.AsQuantDTypeCast()->dstT = dst_t; }

#else

int QuantDTypeCast::GetSrcT() const { return this->primitive_->value_as_QuantDTypeCast()->srcT(); }
int QuantDTypeCast::GetDstT() const { return this->primitive_->value_as_QuantDTypeCast()->dstT(); }

#endif

int QuantDTypeCast::InferShape(std::vector<tensor::Tensor *> inputs_, std::vector<tensor::Tensor *> outputs_) {
  MS_ASSERT(this->primitive_ != nullptr);
  auto input = inputs_.front();
  MS_ASSERT(input != nullptr);
  auto output = outputs_.front();
  MS_ASSERT(output != nullptr);
  MS_ASSERT(input->data_type() == param->srcT);
  output->set_data_type(static_cast<TypeId>(GetDstT()));
  output->SetFormat(input->GetFormat());
  if (!GetInferFlag()) {
    return RET_OK;
  }
  output->set_shape(input->shape());
  return RET_OK;
}
}  // namespace lite
}  // namespace mindspore
