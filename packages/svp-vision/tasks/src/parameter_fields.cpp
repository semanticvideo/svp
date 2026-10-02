#include "parameter_fields.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/models/model_id.hpp"

#include <cmath>

namespace svp::vision::tasks::detail {

double ParameterFields::double_at(const Json& object, const std::string& key,
                                  const std::string& where, double minimum,
                                  double maximum) const {
  const Json& value = object.at(key);
  const std::string name = where + "." + key;
  // Encoders always write these as floating point, so an integer is
  // non-canonical.
  if (!value.is_number_float()) reject(name + " must be a floating-point number");
  const double parsed = value.get<double>();
  if (!std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
    reject(name + " is out of range");
  }
  return parsed;
}

std::string ParameterFields::model_id_at(const Json& object, const std::string& where) const {
  const std::string id = string_at(object, "model_id", where);
  if (!svp::models::is_canonical_model_id(id)) {
    reject(where + ".model_id `" + id + "` is not a canonical model id");
  }
  return id;
}

svp::models::OrtThreadCounts ParameterFields::threads_at(const Json& object,
                                                         const std::string& where) const {
  const std::string name = where + ".threads";
  const Json& value = object.at("threads");
  require_fields(value, {"inter_op", "intra_op"}, name);
  return svp::models::OrtThreadCounts{
      .intra_op = integer_at<int>(value, "intra_op", name, 1),
      .inter_op = integer_at<int>(value, "inter_op", name, 1),
  };
}

std::string ParameterFields::ffmpeg_build_at(const Json& object,
                                             const std::string& where) const {
  std::string build = string_at(object, "ffmpeg_build", where);
  if (!svp::exec::parse_blake3_prefixed(build)) {
    reject(where + ".ffmpeg_build must be b3:<64 hex>");
  }
  return build;
}

Json threads_to_json(const svp::models::OrtThreadCounts& threads) {
  return Json{{"inter_op", threads.inter_op}, {"intra_op", threads.intra_op}};
}

}  // namespace svp::vision::tasks::detail
