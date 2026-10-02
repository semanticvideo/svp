#include "onnx_model_parameters_json.hpp"

namespace svp::vision::tasks::detail {

void add_onnx_model_fields(Json& parameters, const OnnxModelParameters& model) {
  parameters["execution_provider"] = model.execution_provider;
  parameters["model_id"] = model.model_id;
  parameters["threads"] = threads_to_json(model.threads);
}

OnnxModelParameters onnx_model_fields_from_json(const ParameterFields& fields,
                                                const Json& parameters) {
  return OnnxModelParameters{
      .model_id = fields.model_id_at(parameters, "parameters"),
      .execution_provider =
          fields.one_of(parameters, "execution_provider", "parameters", kExecutionProviders),
      .threads = fields.threads_at(parameters, "parameters"),
  };
}

}  // namespace svp::vision::tasks::detail
