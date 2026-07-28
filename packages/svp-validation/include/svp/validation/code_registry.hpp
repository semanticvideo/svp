#pragma once

#include "svp/validation/report.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace svp::validation {

inline constexpr std::string_view kCodeMissingManifest = "ERR_CORE_MISSING_MANIFEST";
inline constexpr std::string_view kCodeMissingSection = "ERR_CORE_MISSING_SECTION";
inline constexpr std::string_view kCodeUnknownRootSection = "ERR_CORE_UNKNOWN_ROOT_SECTION";
inline constexpr std::string_view kCodePathTraversal = "ERR_CORE_PATH_TRAVERSAL";
inline constexpr std::string_view kCodeRasterExtentMismatch =
    "ERR_CORE_RASTER_EXTENT_MISMATCH";
inline constexpr std::string_view kCodeMissingDepth = "ERR_CORE_MISSING_DEPTH";
inline constexpr std::string_view kCodeMissingMasks = "ERR_CORE_MISSING_MASKS";
inline constexpr std::string_view kCodeInvalidEntityRecord =
    "ERR_CORE_INVALID_ENTITY_RECORD";
inline constexpr std::string_view kCodeInvalidBlockHeader =
    "ERR_CORE_INVALID_BLOCK_HEADER";
inline constexpr std::string_view kCodeInvalidBlockHash =
    "ERR_CORE_INVALID_BLOCK_HASH";
inline constexpr std::string_view kCodeForbiddenBlockType =
    "ERR_CORE_FORBIDDEN_BLOCK_TYPE";
inline constexpr std::string_view kCodeIndexSchemaInvalid =
    "ERR_CORE_INDEX_SCHEMA_INVALID";
inline constexpr std::string_view kCodeIndexManifestInvalid =
    "ERR_CORE_INDEX_MANIFEST_INVALID";
inline constexpr std::string_view kCodeIndexLogicalMismatch =
    "ERR_CORE_INDEX_LOGICAL_MISMATCH";
inline constexpr std::string_view kCodeMissingTextSection = "ERR_CORE_MISSING_TEXT_SECTION";
inline constexpr std::string_view kCodeMissingColorSection = "ERR_CORE_MISSING_COLOR_SECTION";
inline constexpr std::string_view kCodeTextInvalidRegionRecord =
    "ERR_TEXT_INVALID_REGION_RECORD";
inline constexpr std::string_view kCodeTextInvalidObservationRecord =
    "ERR_TEXT_INVALID_OBSERVATION_RECORD";
inline constexpr std::string_view kCodeTextInvalidBoundingBox =
    "ERR_TEXT_INVALID_BOUNDING_BOX";
inline constexpr std::string_view kCodeTextInvalidTiming = "ERR_TEXT_INVALID_TIMING";
inline constexpr std::string_view kCodeTextInvalidReference = "ERR_TEXT_INVALID_REFERENCE";
inline constexpr std::string_view kCodeTextInvalidConfidence =
    "ERR_TEXT_INVALID_CONFIDENCE";
inline constexpr std::string_view kCodeTextInvalidNormalizedText =
    "ERR_TEXT_INVALID_NORMALIZED_TEXT";
inline constexpr std::string_view kCodeTextInvalidNumericExtraction =
    "ERR_TEXT_INVALID_NUMERIC_EXTRACTION";
inline constexpr std::string_view kCodeTextIndexMismatch = "ERR_TEXT_INDEX_MISMATCH";
inline constexpr std::string_view kCodeColorInvalidObservationRecord =
    "ERR_COLOR_INVALID_OBSERVATION_RECORD";
inline constexpr std::string_view kCodeColorInvalidBucketId = "ERR_COLOR_INVALID_BUCKET_ID";
inline constexpr std::string_view kCodeColorInvalidColorSpace =
    "ERR_COLOR_INVALID_COLOR_SPACE";
inline constexpr std::string_view kCodeColorInvalidPercentageTotal =
    "ERR_COLOR_INVALID_PERCENTAGE_TOTAL";
inline constexpr std::string_view kCodeColorInvalidPercentageBounds =
    "ERR_COLOR_INVALID_PERCENTAGE_BOUNDS";
inline constexpr std::string_view kCodeColorInvalidDominantBucket =
    "ERR_COLOR_INVALID_DOMINANT_BUCKET";
inline constexpr std::string_view kCodeColorInvalidSamplingBasis =
    "ERR_COLOR_INVALID_SAMPLING_BASIS";
inline constexpr std::string_view kCodeColorIndexMismatch = "ERR_COLOR_INDEX_MISMATCH";
inline constexpr std::string_view kCodeDiarizationFallback = "WARN_DIARIZATION_FALLBACK";
inline constexpr std::string_view kCodeDiarizationUnavailable = "ERR_DIARIZATION_UNAVAILABLE";

inline constexpr std::string_view kTempCodeInputMissing = "X_VALIDATOR_INPUT_MISSING";
inline constexpr std::string_view kTempCodeInputNotRegularFile = "X_VALIDATOR_INPUT_NOT_REGULAR_FILE";
inline constexpr std::string_view kTempCodeWrongExtension = "X_VALIDATOR_INPUT_EXTENSION";
inline constexpr std::string_view kTempCodeZipUnreadable = "X_VALIDATOR_ZIP_UNREADABLE";
inline constexpr std::string_view kTempCodeRegistryUnreadable = "X_VALIDATOR_REGISTRY_UNREADABLE";
inline constexpr std::string_view kTempCodeRegistryInvalid = "X_VALIDATOR_REGISTRY_INVALID";
inline constexpr std::string_view kTempCodeSchemaUnreadable = "X_VALIDATOR_SCHEMA_UNREADABLE";
inline constexpr std::string_view kTempCodeSchemaInvalid = "X_VALIDATOR_SCHEMA_INVALID";
inline constexpr std::string_view kTempCodeColorInvalidBucketRegistryVersion =
    "X_VALIDATOR_COLOR_INVALID_BUCKET_REGISTRY_VERSION";
inline constexpr std::string_view kTempCodeBlockEntryNotStored =
    "X_VALIDATOR_BLOCK_ENTRY_NOT_STORED";
inline constexpr std::string_view kTempCodeBlockPayloadDecodeFailed =
    "X_VALIDATOR_BLOCK_PAYLOAD_DECODE_FAILED";
inline constexpr std::string_view kTempCodeMissingEmbeddingsEntry =
    "X_VALIDATOR_MISSING_EMBEDDINGS_ENTRY";

inline constexpr std::string_view kCodeSvpiWrongMimetype = "ERR_SVPI_WRONG_MIMETYPE";
inline constexpr std::string_view kCodeSvpiMissingMediaBinding = "ERR_SVPI_MISSING_MEDIA_BINDING";
inline constexpr std::string_view kCodeSvpiForbiddenPrimaryMedia = "ERR_SVPI_FORBIDDEN_PRIMARY_MEDIA";
inline constexpr std::string_view kCodeSvpiForbiddenReplayableMediaDerivative = "ERR_SVPI_FORBIDDEN_REPLAYABLE_MEDIA_DERIVATIVE";
inline constexpr std::string_view kCodeSvpiWrongManifestFormat = "ERR_SVPI_WRONG_MANIFEST_FORMAT";
inline constexpr std::string_view kCodeSvpiMissingProvenance = "ERR_SVPI_MISSING_PROVENANCE";
inline constexpr std::string_view kCodeSvpiMissingIndex = "ERR_SVPI_MISSING_INDEX";
inline constexpr std::string_view kCodeSvpiWrongBindingContract = "ERR_SVPI_WRONG_BINDING_CONTRACT";
inline constexpr std::string_view kCodeSvpiLegacyManifestName = "ERR_SVPI_LEGACY_MANIFEST_NAME";
inline constexpr std::string_view kCodeIsoBmffBoxStructureInvalid = "ERR_ISOBMFF_BOX_STRUCTURE_INVALID";
inline constexpr std::string_view kCodeIsoBmffUnsupportedContainer =
    "ERR_ISOBMFF_UNSUPPORTED_CONTAINER";
inline constexpr std::string_view kCodeIsoBmffUuidBoxTruncated = "ERR_ISOBMFF_UUID_BOX_TRUNCATED";
inline constexpr std::string_view kCodeIsoBmffSvpiProfileUnsupported = "ERR_ISOBMFF_SVPI_PROFILE_UNSUPPORTED";
inline constexpr std::string_view kCodeIsoBmffSvpiEnvelopeInvalid = "ERR_ISOBMFF_SVPI_ENVELOPE_INVALID";
inline constexpr std::string_view kCodeIsoBmffSvpiPayloadBounds = "ERR_ISOBMFF_SVPI_PAYLOAD_BOUNDS";
inline constexpr std::string_view kCodeIsoBmffSvpiPayloadLengthMismatch = "ERR_ISOBMFF_SVPI_PAYLOAD_LENGTH_MISMATCH";
inline constexpr std::string_view kCodeIsoBmffSvpiPayloadHashMismatch = "ERR_ISOBMFF_SVPI_PAYLOAD_HASH_MISMATCH";
inline constexpr std::string_view kCodeIsoBmffSvpiDuplicate = "ERR_ISOBMFF_SVPI_DUPLICATE";
inline constexpr std::string_view kCodeIsoBmffSvpiNotFound = "ERR_ISOBMFF_SVPI_NOT_FOUND";
inline constexpr std::string_view kCodeIsoBmffEmbeddedSvpiInvalid = "ERR_ISOBMFF_EMBEDDED_SVPI_INVALID";
inline constexpr std::string_view kCodeIsoBmffUnsafeTailLayout = "ERR_ISOBMFF_UNSAFE_TAIL_LAYOUT";
inline constexpr std::string_view kCodeIsoBmffZeroSizedBox = "ERR_ISOBMFF_ZERO_SIZED_TOP_LEVEL_BOX";
inline constexpr std::string_view kCodeIsoBmffSvpiMediaBindingMismatch = "ERR_ISOBMFF_SVPI_MEDIA_BINDING_MISMATCH";

struct ValidationCode {
  std::string code;
  FindingSeverity severity = FindingSeverity::error;
  std::string report_bucket;
  bool affects_core_status = true;
  std::string status_effect;
  std::string section;
  std::string description;
};

class ValidationCodeRegistry {
 public:
  void add(ValidationCode code);

  [[nodiscard]] const ValidationCode* find(std::string_view code) const noexcept;
  [[nodiscard]] bool contains(std::string_view code) const noexcept;

 private:
  std::unordered_map<std::string, ValidationCode> codes_;
};

[[nodiscard]] ValidationCodeRegistry load_validation_code_registry(
    const std::filesystem::path& path);

[[nodiscard]] ValidationFinding make_finding(const ValidationCodeRegistry& registry,
                                             std::string_view code,
                                             std::string path,
                                             std::string message);

[[nodiscard]] ValidationFinding make_runtime_finding(std::string_view code,
                                                     std::string path,
                                                     std::string message);

}  // namespace svp::validation
