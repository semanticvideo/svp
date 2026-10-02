#include "svp/vision/tasks/embed_text_batch_parameters.hpp"

#include "onnx_model_parameters_json.hpp"
#include "parameter_fields.hpp"

#include <stdexcept>

namespace svp::vision::tasks {
namespace {

using detail::Json;

const detail::ParameterFields& fields() {
  static const detail::ParameterFields kFields(kEmbedTextBatchTaskType);
  return kFields;
}

Json item_to_json(const OrderedTextEmbeddingItem& item) {
  return Json{{"id", item.item.id}, {"ordinal", item.ordinal}, {"text", item.item.text}};
}

}  // namespace

std::uint64_t embed_text_item_parameter_bytes(const OrderedTextEmbeddingItem& item) {
  return item_to_json(item).dump().size();
}

Json embed_text_batch_parameters_to_json(const EmbedTextBatchParameters& parameters) {
  Json items = Json::array();
  for (const OrderedTextEmbeddingItem& item : parameters.items) {
    items.push_back(item_to_json(item));
  }
  Json value{{"embedding_dim", parameters.embedding_dim}, {"items", std::move(items)}};
  detail::add_onnx_model_fields(value, parameters.model);
  (void)embed_text_batch_parameters_from_json(value);
  return value;
}

EmbedTextBatchParameters embed_text_batch_parameters_from_json(const Json& value) {
  fields().require_fields(
      value, {"embedding_dim", "execution_provider", "items", "model_id", "threads"},
      "parameters");
  EmbedTextBatchParameters parameters;
  parameters.embedding_dim =
      fields().integer_at<std::uint32_t>(value, "embedding_dim", "parameters", 1);
  parameters.model = detail::onnx_model_fields_from_json(fields(), value);
  const Json& items = fields().array_at(value, "items", "parameters");
  if (items.empty()) fields().reject("items must not be empty");
  for (std::size_t index = 0; index < items.size(); ++index) {
    const std::string where = "items[" + std::to_string(index) + "]";
    fields().require_fields(items[index], {"id", "ordinal", "text"}, where);
    OrderedTextEmbeddingItem item{
        .ordinal = fields().integer_at<std::uint64_t>(items[index], "ordinal", where, 0),
        .item = {.id = fields().string_at(items[index], "id", where),
                 .text = fields().string_at(items[index], "text", where)}};
    if (!parameters.items.empty() && item.ordinal <= parameters.items.back().ordinal) {
      fields().reject(where + " is not strictly after the item before it");
    }
    parameters.items.push_back(std::move(item));
  }
  return parameters;
}

std::optional<std::string> validate_embed_text_batch_parameters(const Json& value) {
  try {
    (void)embed_text_batch_parameters_from_json(value);
    return std::nullopt;
  } catch (const std::invalid_argument& error) {
    return std::string(error.what());
  } catch (const Json::exception& error) {
    return std::string(kEmbedTextBatchTaskType) + " parameters: " + error.what();
  }
}

}  // namespace svp::vision::tasks
