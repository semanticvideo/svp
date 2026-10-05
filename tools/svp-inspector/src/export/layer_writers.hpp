#pragma once

#include "export_plan.hpp"
#include "layer_result.hpp"
#include "output_transaction.hpp"
#include "package_reader.hpp"
#include "reference_resolver.hpp"

namespace package_export {

// `jsonl` representation (Package_Export_v1.md Section 4.1): one line per
// record, original text kept, `svp_export` appended where rules resolve.
void export_jsonl_layer(const PackageReader& reader, const PlannedLayer& layer,
                        const ReferenceResolver& resolver,
                        OutputTransaction& output, LayerResult& result);

// `json` representation: the document's bytes, unchanged, after checking
// that they parse as JSON.
void export_json_document(const PackageReader& reader,
                          const PlannedLayer& layer, OutputTransaction& output,
                          LayerResult& result);

// `file` representation: the entry's bytes, unchanged.
void export_file_bytes(const PackageReader& reader, const PlannedLayer& layer,
                       OutputTransaction& output, LayerResult& result);

}  // namespace package_export
