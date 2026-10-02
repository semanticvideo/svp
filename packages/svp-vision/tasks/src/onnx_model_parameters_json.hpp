#pragma once

#include "parameter_fields.hpp"
#include "svp/vision/tasks/onnx_model_parameters.hpp"

namespace svp::vision::tasks::detail {

// Adds the three model fields to `parameters`.
void add_onnx_model_fields(Json& parameters, const OnnxModelParameters& model);

// Reads them; the caller has checked the object's field set.
[[nodiscard]] OnnxModelParameters onnx_model_fields_from_json(const ParameterFields& fields,
                                                              const Json& parameters);

}  // namespace svp::vision::tasks::detail
