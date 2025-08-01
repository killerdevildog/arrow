// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "parquet/arrow/reader_internal.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "arrow/array.h"
#include "arrow/compute/api.h"
#include "arrow/datum.h"
#include "arrow/io/memory.h"
#include "arrow/ipc/reader.h"
#include "arrow/ipc/writer.h"
#include "arrow/scalar.h"
#include "arrow/status.h"
#include "arrow/table.h"
#include "arrow/type.h"
#include "arrow/type_traits.h"
#include "arrow/util/base64.h"
#include "arrow/util/bit_util.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/endian.h"
#include "arrow/util/float16.h"
#include "arrow/util/int_util_overflow.h"
#include "arrow/util/logging_internal.h"
#include "arrow/util/ubsan.h"

#include "parquet/arrow/reader.h"
#include "parquet/arrow/schema.h"
#include "parquet/arrow/schema_internal.h"
#include "parquet/column_reader.h"
#include "parquet/platform.h"
#include "parquet/properties.h"
#include "parquet/schema.h"
#include "parquet/statistics.h"
#include "parquet/types.h"
#include "parquet/windows_fixup.h"  // for OPTIONAL

using arrow::Array;
using arrow::BooleanArray;
using arrow::ChunkedArray;
using arrow::DataType;
using arrow::Datum;
using arrow::Decimal128;
using arrow::Decimal128Array;
using arrow::Decimal128Type;
using arrow::Decimal256;
using arrow::Decimal256Array;
using arrow::Decimal256Type;
using arrow::Field;
using arrow::Int32Array;
using arrow::ListArray;
using arrow::MemoryPool;
using arrow::ResizableBuffer;
using arrow::Status;
using arrow::StructArray;
using arrow::Table;
using arrow::TimestampArray;

using ::arrow::bit_util::FromBigEndian;
using ::arrow::internal::checked_cast;
using ::arrow::internal::checked_pointer_cast;
using ::arrow::internal::SafeLeftShift;
using ::arrow::util::Float16;
using ::arrow::util::SafeLoadAs;

using parquet::internal::BinaryRecordReader;
using parquet::internal::DictionaryRecordReader;
using parquet::internal::RecordReader;
using parquet::schema::GroupNode;
using parquet::schema::Node;
using parquet::schema::PrimitiveNode;
using ParquetType = parquet::Type;

namespace bit_util = arrow::bit_util;

namespace parquet::arrow {
namespace {

template <typename ArrowType>
using ArrayType = typename ::arrow::TypeTraits<ArrowType>::ArrayType;

template <typename CType, typename StatisticsType>
Status MakeMinMaxScalar(const StatisticsType& statistics,
                        std::shared_ptr<::arrow::Scalar>* min,
                        std::shared_ptr<::arrow::Scalar>* max) {
  *min = ::arrow::MakeScalar(static_cast<CType>(statistics.min()));
  *max = ::arrow::MakeScalar(static_cast<CType>(statistics.max()));
  return Status::OK();
}

template <typename CType, typename StatisticsType>
Status MakeMinMaxTypedScalar(const StatisticsType& statistics,
                             std::shared_ptr<DataType> type,
                             std::shared_ptr<::arrow::Scalar>* min,
                             std::shared_ptr<::arrow::Scalar>* max) {
  ARROW_ASSIGN_OR_RAISE(*min, ::arrow::MakeScalar(type, statistics.min()));
  ARROW_ASSIGN_OR_RAISE(*max, ::arrow::MakeScalar(type, statistics.max()));
  return Status::OK();
}

template <typename StatisticsType>
Status MakeMinMaxIntegralScalar(const StatisticsType& statistics,
                                const ::arrow::DataType& arrow_type,
                                std::shared_ptr<::arrow::Scalar>* min,
                                std::shared_ptr<::arrow::Scalar>* max) {
  const auto column_desc = statistics.descr();
  const auto& logical_type = column_desc->logical_type();
  const auto& integer = checked_pointer_cast<const IntLogicalType>(logical_type);
  const bool is_signed = integer->is_signed();

  switch (integer->bit_width()) {
    case 8:
      return is_signed ? MakeMinMaxScalar<int8_t>(statistics, min, max)
                       : MakeMinMaxScalar<uint8_t>(statistics, min, max);
    case 16:
      return is_signed ? MakeMinMaxScalar<int16_t>(statistics, min, max)
                       : MakeMinMaxScalar<uint16_t>(statistics, min, max);
    case 32:
      return is_signed ? MakeMinMaxScalar<int32_t>(statistics, min, max)
                       : MakeMinMaxScalar<uint32_t>(statistics, min, max);
    case 64:
      return is_signed ? MakeMinMaxScalar<int64_t>(statistics, min, max)
                       : MakeMinMaxScalar<uint64_t>(statistics, min, max);
  }

  return Status::OK();
}

static Status FromInt32Statistics(const Int32Statistics& statistics,
                                  const LogicalType& logical_type,
                                  std::shared_ptr<::arrow::Scalar>* min,
                                  std::shared_ptr<::arrow::Scalar>* max) {
  ARROW_ASSIGN_OR_RAISE(auto type, FromInt32(logical_type));

  switch (logical_type.type()) {
    case LogicalType::Type::INT:
      return MakeMinMaxIntegralScalar(statistics, *type, min, max);
      break;
    case LogicalType::Type::DATE:
    case LogicalType::Type::TIME:
    case LogicalType::Type::NONE:
      return MakeMinMaxTypedScalar<int32_t>(statistics, type, min, max);
      break;
    default:
      break;
  }

  return Status::NotImplemented("Cannot extract statistics for type ");
}

static Status FromInt64Statistics(const Int64Statistics& statistics,
                                  const LogicalType& logical_type,
                                  std::shared_ptr<::arrow::Scalar>* min,
                                  std::shared_ptr<::arrow::Scalar>* max) {
  ARROW_ASSIGN_OR_RAISE(auto type, FromInt64(logical_type));

  switch (logical_type.type()) {
    case LogicalType::Type::INT:
      return MakeMinMaxIntegralScalar(statistics, *type, min, max);
      break;
    case LogicalType::Type::TIME:
    case LogicalType::Type::TIMESTAMP:
    case LogicalType::Type::NONE:
      return MakeMinMaxTypedScalar<int64_t>(statistics, type, min, max);
      break;
    default:
      break;
  }

  return Status::NotImplemented("Cannot extract statistics for type ");
}

template <typename DecimalType>
Result<std::shared_ptr<::arrow::Scalar>> FromBigEndianString(
    const std::string& data, std::shared_ptr<DataType> arrow_type) {
  ARROW_ASSIGN_OR_RAISE(
      DecimalType decimal,
      DecimalType::FromBigEndian(reinterpret_cast<const uint8_t*>(data.data()),
                                 static_cast<int32_t>(data.size())));
  return ::arrow::MakeScalar(std::move(arrow_type), decimal);
}

// Extracts Min and Max scalar from bytes like types (i.e. types where
// decimal is encoded as little endian.
Status ExtractDecimalMinMaxFromBytesType(const Statistics& statistics,
                                         const LogicalType& logical_type,
                                         std::shared_ptr<::arrow::Scalar>* min,
                                         std::shared_ptr<::arrow::Scalar>* max) {
  const DecimalLogicalType& decimal_type =
      checked_cast<const DecimalLogicalType&>(logical_type);

  Result<std::shared_ptr<DataType>> maybe_type =
      Decimal128Type::Make(decimal_type.precision(), decimal_type.scale());
  std::shared_ptr<DataType> arrow_type;
  if (maybe_type.ok()) {
    arrow_type = maybe_type.ValueOrDie();
    ARROW_ASSIGN_OR_RAISE(
        *min, FromBigEndianString<Decimal128>(statistics.EncodeMin(), arrow_type));
    ARROW_ASSIGN_OR_RAISE(*max, FromBigEndianString<Decimal128>(statistics.EncodeMax(),
                                                                std::move(arrow_type)));
    return Status::OK();
  }
  // Fallback to see if Decimal256 can represent the type.
  ARROW_ASSIGN_OR_RAISE(
      arrow_type, Decimal256Type::Make(decimal_type.precision(), decimal_type.scale()));
  ARROW_ASSIGN_OR_RAISE(
      *min, FromBigEndianString<Decimal256>(statistics.EncodeMin(), arrow_type));
  ARROW_ASSIGN_OR_RAISE(*max, FromBigEndianString<Decimal256>(statistics.EncodeMax(),
                                                              std::move(arrow_type)));

  return Status::OK();
}

Status ByteArrayStatisticsAsScalars(const Statistics& statistics,
                                    std::shared_ptr<::arrow::Scalar>* min,
                                    std::shared_ptr<::arrow::Scalar>* max) {
  auto logical_type = statistics.descr()->logical_type();
  if (logical_type->type() == LogicalType::Type::DECIMAL) {
    return ExtractDecimalMinMaxFromBytesType(statistics, *logical_type, min, max);
  }
  std::shared_ptr<::arrow::DataType> type;
  if (statistics.descr()->physical_type() == Type::FIXED_LEN_BYTE_ARRAY) {
    type = ::arrow::fixed_size_binary(statistics.descr()->type_length());
  } else {
    type = logical_type->type() == LogicalType::Type::STRING ? ::arrow::utf8()
                                                             : ::arrow::binary();
  }
  ARROW_ASSIGN_OR_RAISE(
      *min, ::arrow::MakeScalar(type, Buffer::FromString(statistics.EncodeMin())));
  ARROW_ASSIGN_OR_RAISE(
      *max, ::arrow::MakeScalar(type, Buffer::FromString(statistics.EncodeMax())));

  return Status::OK();
}

Result<std::shared_ptr<ChunkedArray>> ViewOrCastChunkedArray(
    const std::shared_ptr<ChunkedArray>& array, MemoryPool* pool,
    const std::shared_ptr<DataType>& logical_value_type) {
  auto view_result = array->View(logical_value_type);
  if (view_result.ok()) {
    return view_result.ValueOrDie();
  } else {
    ::arrow::compute::ExecContext exec_context(pool);
    ARROW_ASSIGN_OR_RAISE(
        auto casted_datum,
        ::arrow::compute::Cast(Datum(array), logical_value_type,
                               ::arrow::compute::CastOptions(), &exec_context));
    return casted_datum.chunked_array();
  }
}

}  // namespace

Status StatisticsAsScalars(const Statistics& statistics,
                           std::shared_ptr<::arrow::Scalar>* min,
                           std::shared_ptr<::arrow::Scalar>* max) {
  if (!statistics.HasMinMax()) {
    return Status::Invalid("Statistics has no min max.");
  }

  auto column_desc = statistics.descr();
  if (column_desc == nullptr) {
    return Status::Invalid("Statistics carries no descriptor, can't infer arrow type.");
  }

  auto physical_type = column_desc->physical_type();
  auto logical_type = column_desc->logical_type();
  switch (physical_type) {
    case Type::BOOLEAN:
      return MakeMinMaxScalar<bool, BoolStatistics>(
          checked_cast<const BoolStatistics&>(statistics), min, max);
    case Type::FLOAT:
      return MakeMinMaxScalar<float, FloatStatistics>(
          checked_cast<const FloatStatistics&>(statistics), min, max);
    case Type::DOUBLE:
      return MakeMinMaxScalar<double, DoubleStatistics>(
          checked_cast<const DoubleStatistics&>(statistics), min, max);
    case Type::INT32:
      return FromInt32Statistics(checked_cast<const Int32Statistics&>(statistics),
                                 *logical_type, min, max);
    case Type::INT64:
      return FromInt64Statistics(checked_cast<const Int64Statistics&>(statistics),
                                 *logical_type, min, max);
    case Type::BYTE_ARRAY:
    case Type::FIXED_LEN_BYTE_ARRAY:
      return ByteArrayStatisticsAsScalars(statistics, min, max);
    default:
      return Status::NotImplemented("Extract statistics unsupported for physical_type ",
                                    physical_type, " unsupported.");
  }

  return Status::OK();
}

// ----------------------------------------------------------------------
// Primitive types

namespace {

/// Drop the validity buffer from each chunk.
///
/// Used when reading a non-nullable field.
void ReconstructChunksWithoutNulls(::arrow::ArrayVector* chunks) {
  for (size_t i = 0; i < chunks->size(); i++) {
    if ((*chunks)[i]->data()->buffers[0]) {
      std::shared_ptr<::arrow::ArrayData> data = (*chunks)[i]->data();
      data->null_count = 0;
      data->buffers[0] = nullptr;
      (*chunks)[i] = MakeArray(data);
    }
  }
}

template <typename ArrowType, typename ParquetType>
void AttachStatistics(::arrow::ArrayData* data,
                      std::unique_ptr<::parquet::ColumnChunkMetaData> metadata,
                      const ReaderContext* ctx) {
  if (!metadata) {
    return;
  }

  using ArrowCType = typename ArrowType::c_type;

  auto statistics = metadata->statistics();
  if (data->null_count == ::arrow::kUnknownNullCount && !statistics) {
    return;
  }

  auto array_statistics = std::make_shared<::arrow::ArrayStatistics>();
  if (data->null_count != ::arrow::kUnknownNullCount) {
    array_statistics->null_count = data->null_count;
  }
  if (statistics) {
    if (statistics->HasDistinctCount()) {
      array_statistics->distinct_count = statistics->distinct_count();
    }
    if (statistics->HasMinMax()) {
      const auto* typed_statistics =
          checked_cast<const ::parquet::TypedStatistics<ParquetType>*>(statistics.get());
      const ArrowCType min = typed_statistics->min();
      const ArrowCType max = typed_statistics->max();
      if constexpr (std::is_same_v<ArrowCType, bool>) {
        array_statistics->min = static_cast<bool>(min);
        array_statistics->max = static_cast<bool>(max);
      } else if constexpr (std::is_floating_point_v<ArrowCType>) {
        array_statistics->min = static_cast<double>(min);
        array_statistics->max = static_cast<double>(max);
      } else if constexpr (std::is_signed_v<ArrowCType>) {
        array_statistics->min = static_cast<int64_t>(min);
        array_statistics->max = static_cast<int64_t>(max);
      } else {
        array_statistics->min = static_cast<uint64_t>(min);
        array_statistics->max = static_cast<uint64_t>(max);
      }
      // We can assume that integer/floating point number/boolean
      // based min/max are always exact if they exist. Apache
      // Parquet's "Statistics" has "is_min_value_exact" and
      // "is_max_value_exact" but we can ignore them for integer/
      // floating point number/boolean based min/max.
      //
      // See also the discussion at dev@parquet.apache.org:
      // https://lists.apache.org/thread/zfnmg5p51b7oylft5w5k4670wgkd4zv4
      array_statistics->is_min_exact = true;
      array_statistics->is_max_exact = true;
    }
  }

  data->statistics = std::move(array_statistics);
}

template <typename ArrowType, typename ParquetType>
Status TransferInt(RecordReader* reader,
                   std::unique_ptr<::parquet::ColumnChunkMetaData> metadata,
                   const ReaderContext* ctx, const std::shared_ptr<Field>& field,
                   Datum* out) {
  using ArrowCType = typename ArrowType::c_type;
  using ParquetCType = typename ParquetType::c_type;
  int64_t length = reader->values_written();
  ARROW_ASSIGN_OR_RAISE(auto data,
                        ::arrow::AllocateBuffer(length * sizeof(ArrowCType), ctx->pool));

  auto values = reinterpret_cast<const ParquetCType*>(reader->values());
  auto out_ptr = reinterpret_cast<ArrowCType*>(data->mutable_data());
  std::copy(values, values + length, out_ptr);
  int64_t null_count = 0;
  std::vector<std::shared_ptr<Buffer>> buffers = {nullptr, std::move(data)};
  if (field->nullable()) {
    null_count = reader->null_count();
    buffers[0] = reader->ReleaseIsValid();
  }
  auto array_data =
      ::arrow::ArrayData::Make(field->type(), length, std::move(buffers), null_count);
  AttachStatistics<ArrowType, ParquetType>(array_data.get(), std::move(metadata), ctx);
  *out = std::make_shared<ArrayType<ArrowType>>(std::move(array_data));
  return Status::OK();
}

template <typename ArrowType, typename ParquetType>
std::shared_ptr<Array> TransferZeroCopy(
    RecordReader* reader, std::unique_ptr<::parquet::ColumnChunkMetaData> metadata,
    const ReaderContext* ctx, const std::shared_ptr<Field>& field) {
  std::shared_ptr<::arrow::ArrayData> data;
  if (field->nullable()) {
    std::vector<std::shared_ptr<Buffer>> buffers = {reader->ReleaseIsValid(),
                                                    reader->ReleaseValues()};
    data = std::make_shared<::arrow::ArrayData>(field->type(), reader->values_written(),
                                                std::move(buffers), reader->null_count());
  } else {
    std::vector<std::shared_ptr<Buffer>> buffers = {nullptr, reader->ReleaseValues()};
    data = std::make_shared<::arrow::ArrayData>(field->type(), reader->values_written(),
                                                std::move(buffers), /*null_count=*/0);
  }
  AttachStatistics<ArrowType, ParquetType>(data.get(), std::move(metadata), ctx);
  return ::arrow::MakeArray(std::move(data));
}

Status TransferBool(RecordReader* reader,
                    std::unique_ptr<::parquet::ColumnChunkMetaData> metadata,
                    const ReaderContext* ctx, bool nullable, Datum* out) {
  int64_t length = reader->values_written();

  const int64_t buffer_size = bit_util::BytesForBits(length);
  ARROW_ASSIGN_OR_RAISE(auto data, ::arrow::AllocateBuffer(buffer_size, ctx->pool));

  // Transfer boolean values to packed bitmap
  auto values = reinterpret_cast<const bool*>(reader->values());
  uint8_t* data_ptr = data->mutable_data();
  memset(data_ptr, 0, static_cast<size_t>(buffer_size));

  for (int64_t i = 0; i < length; i++) {
    if (values[i]) {
      ::arrow::bit_util::SetBit(data_ptr, i);
    }
  }

  std::shared_ptr<::arrow::ArrayData> array_data;
  if (nullable) {
    array_data = ::arrow::ArrayData::Make(::arrow::boolean(), length,
                                          {reader->ReleaseIsValid(), std::move(data)},
                                          reader->null_count());
  } else {
    array_data = ::arrow::ArrayData::Make(::arrow::boolean(), length,
                                          {/*null_bitmap=*/nullptr, std::move(data)},
                                          /*null_count=*/0);
  }
  AttachStatistics<::arrow::BooleanType, BooleanType>(array_data.get(),
                                                      std::move(metadata), ctx);
  *out = std::make_shared<BooleanArray>(std::move(array_data));
  return Status::OK();
}

Status TransferInt96(RecordReader* reader, MemoryPool* pool,
                     const std::shared_ptr<Field>& field, Datum* out,
                     const ::arrow::TimeUnit::type int96_arrow_time_unit) {
  int64_t length = reader->values_written();
  auto values = reinterpret_cast<const Int96*>(reader->values());
  ARROW_ASSIGN_OR_RAISE(auto data,
                        ::arrow::AllocateBuffer(length * sizeof(int64_t), pool));
  auto data_ptr = reinterpret_cast<int64_t*>(data->mutable_data());
  for (int64_t i = 0; i < length; i++) {
    if (values[i].value[2] == 0) {
      // Happens for null entries: avoid triggering UBSAN as that Int96 timestamp
      // isn't representable as a 64-bit Unix timestamp.
      *data_ptr++ = 0;
    } else {
      switch (int96_arrow_time_unit) {
        case ::arrow::TimeUnit::NANO:
          *data_ptr++ = Int96GetNanoSeconds(values[i]);
          break;
        case ::arrow::TimeUnit::MICRO:
          *data_ptr++ = Int96GetMicroSeconds(values[i]);
          break;
        case ::arrow::TimeUnit::MILLI:
          *data_ptr++ = Int96GetMilliSeconds(values[i]);
          break;
        case ::arrow::TimeUnit::SECOND:
          *data_ptr++ = Int96GetSeconds(values[i]);
          break;
      }
    }
  }
  if (field->nullable()) {
    *out =
        std::make_shared<TimestampArray>(field->type(), length, std::move(data),
                                         reader->ReleaseIsValid(), reader->null_count());
  } else {
    *out = std::make_shared<TimestampArray>(field->type(), length, std::move(data),
                                            /*null_bitmap=*/nullptr, /*null_count=*/0);
  }
  return Status::OK();
}

Status TransferDate64(RecordReader* reader, MemoryPool* pool,
                      const std::shared_ptr<Field>& field, Datum* out) {
  int64_t length = reader->values_written();
  auto values = reinterpret_cast<const int32_t*>(reader->values());

  ARROW_ASSIGN_OR_RAISE(auto data,
                        ::arrow::AllocateBuffer(length * sizeof(int64_t), pool));
  auto out_ptr = reinterpret_cast<int64_t*>(data->mutable_data());

  for (int64_t i = 0; i < length; i++) {
    *out_ptr++ = static_cast<int64_t>(values[i]) * kMillisecondsPerDay;
  }

  if (field->nullable()) {
    *out = std::make_shared<::arrow::Date64Array>(field->type(), length, std::move(data),
                                                  reader->ReleaseIsValid(),
                                                  reader->null_count());
  } else {
    *out =
        std::make_shared<::arrow::Date64Array>(field->type(), length, std::move(data),
                                               /*null_bitmap=*/nullptr, /*null_count=*/0);
  }
  return Status::OK();
}

// ----------------------------------------------------------------------
// Binary, direct to dictionary-encoded

Status TransferDictionary(RecordReader* reader, MemoryPool* pool,
                          const std::shared_ptr<DataType>& logical_value_type,
                          bool nullable, std::shared_ptr<ChunkedArray>* out) {
  auto dict_reader = dynamic_cast<DictionaryRecordReader*>(reader);
  DCHECK(dict_reader);
  *out = dict_reader->GetResult();
  if (!logical_value_type->Equals(*(*out)->type())) {
    ARROW_ASSIGN_OR_RAISE(*out, ViewOrCastChunkedArray(*out, pool, logical_value_type));
  }
  if (!nullable) {
    ::arrow::ArrayVector chunks = (*out)->chunks();
    ReconstructChunksWithoutNulls(&chunks);
    *out = std::make_shared<ChunkedArray>(std::move(chunks), logical_value_type);
  }
  return Status::OK();
}

Status TransferBinary(RecordReader* reader, MemoryPool* pool,
                      const std::shared_ptr<Field>& logical_type_field,
                      std::shared_ptr<ChunkedArray>* out) {
  if (reader->read_dictionary()) {
    return TransferDictionary(
        reader, pool, ::arrow::dictionary(::arrow::int32(), logical_type_field->type()),
        logical_type_field->nullable(), out);
  }
  ::arrow::compute::ExecContext ctx(pool);
  ::arrow::compute::CastOptions cast_options;
  cast_options.allow_invalid_utf8 = true;  // avoid spending time validating UTF8 data

  auto binary_reader = dynamic_cast<BinaryRecordReader*>(reader);
  DCHECK(binary_reader);
  auto chunks = binary_reader->GetBuilderChunks();
  for (auto& chunk : chunks) {
    if (!chunk->type()->Equals(*logical_type_field->type())) {
      // XXX: if a LargeBinary chunk is larger than 2GB, the MSBs of offsets
      // will be lost because they are first created as int32 and then cast to int64.
      ARROW_ASSIGN_OR_RAISE(
          chunk,
          ::arrow::compute::Cast(*chunk, logical_type_field->type(), cast_options, &ctx));
    }
  }
  if (!logical_type_field->nullable()) {
    ReconstructChunksWithoutNulls(&chunks);
  }
  *out = std::make_shared<ChunkedArray>(std::move(chunks), logical_type_field->type());
  return Status::OK();
}

// ----------------------------------------------------------------------
// INT32 / INT64 / BYTE_ARRAY / FIXED_LEN_BYTE_ARRAY -> Decimal128 || Decimal256

template <typename DecimalType>
Status RawBytesToDecimalBytes(const uint8_t* value, int32_t byte_width,
                              uint8_t* out_buf) {
  ARROW_ASSIGN_OR_RAISE(DecimalType t, DecimalType::FromBigEndian(value, byte_width));
  t.ToBytes(out_buf);
  return ::arrow::Status::OK();
}

template <typename DecimalArrayType>
struct DecimalTypeTrait;

template <>
struct DecimalTypeTrait<::arrow::Decimal128Array> {
  using value = ::arrow::Decimal128;
};

template <>
struct DecimalTypeTrait<::arrow::Decimal256Array> {
  using value = ::arrow::Decimal256;
};

template <typename DecimalArrayType, typename ParquetType>
struct DecimalConverter {
  static inline Status ConvertToDecimal(const Array& array,
                                        const std::shared_ptr<DataType>&,
                                        MemoryPool* pool, std::shared_ptr<Array>*) {
    return Status::NotImplemented("not implemented");
  }
};

template <typename DecimalArrayType>
struct DecimalConverter<DecimalArrayType, FLBAType> {
  static inline Status ConvertToDecimal(const Array& array,
                                        const std::shared_ptr<DataType>& type,
                                        MemoryPool* pool, std::shared_ptr<Array>* out) {
    const auto& fixed_size_binary_array =
        checked_cast<const ::arrow::FixedSizeBinaryArray&>(array);

    // The byte width of each decimal value
    const int32_t type_length =
        checked_cast<const ::arrow::DecimalType&>(*type).byte_width();

    // number of elements in the entire array
    const int64_t length = fixed_size_binary_array.length();

    // Get the byte width of the values in the FixedSizeBinaryArray. Most of the time
    // this will be different from the decimal array width because we write the minimum
    // number of bytes necessary to represent a given precision
    const int32_t byte_width =
        checked_cast<const ::arrow::FixedSizeBinaryType&>(*fixed_size_binary_array.type())
            .byte_width();
    // allocate memory for the decimal array
    ARROW_ASSIGN_OR_RAISE(auto data, ::arrow::AllocateBuffer(length * type_length, pool));

    // raw bytes that we can write to
    uint8_t* out_ptr = data->mutable_data();

    // convert each FixedSizeBinary value to valid decimal bytes
    const int64_t null_count = fixed_size_binary_array.null_count();

    using DecimalType = typename DecimalTypeTrait<DecimalArrayType>::value;
    if (null_count > 0) {
      for (int64_t i = 0; i < length; ++i, out_ptr += type_length) {
        if (!fixed_size_binary_array.IsNull(i)) {
          RETURN_NOT_OK(RawBytesToDecimalBytes<DecimalType>(
              fixed_size_binary_array.GetValue(i), byte_width, out_ptr));
        } else {
          std::memset(out_ptr, 0, type_length);
        }
      }
    } else {
      for (int64_t i = 0; i < length; ++i, out_ptr += type_length) {
        RETURN_NOT_OK(RawBytesToDecimalBytes<DecimalType>(
            fixed_size_binary_array.GetValue(i), byte_width, out_ptr));
      }
    }

    *out = std::make_shared<DecimalArrayType>(
        type, length, std::move(data), fixed_size_binary_array.null_bitmap(), null_count);

    return Status::OK();
  }
};

template <typename DecimalArrayType>
struct DecimalConverter<DecimalArrayType, ByteArrayType> {
  static inline Status ConvertToDecimal(const Array& array,
                                        const std::shared_ptr<DataType>& type,
                                        MemoryPool* pool, std::shared_ptr<Array>* out) {
    const auto& binary_array = checked_cast<const ::arrow::BinaryArray&>(array);
    const int64_t length = binary_array.length();

    const auto& decimal_type = checked_cast<const ::arrow::DecimalType&>(*type);
    const int64_t type_length = decimal_type.byte_width();

    ARROW_ASSIGN_OR_RAISE(auto data, ::arrow::AllocateBuffer(length * type_length, pool));

    // raw bytes that we can write to
    uint8_t* out_ptr = data->mutable_data();

    const int64_t null_count = binary_array.null_count();

    // convert each BinaryArray value to valid decimal bytes
    for (int64_t i = 0; i < length; i++, out_ptr += type_length) {
      int32_t record_len = 0;
      const uint8_t* record_loc = binary_array.GetValue(i, &record_len);

      if (record_len < 0 || record_len > type_length) {
        return Status::Invalid("Invalid BYTE_ARRAY length for ", type->ToString());
      }

      auto out_ptr_view = reinterpret_cast<uint64_t*>(out_ptr);
      out_ptr_view[0] = 0;
      out_ptr_view[1] = 0;

      // only convert rows that are not null if there are nulls, or
      // all rows, if there are not
      if ((null_count > 0 && !binary_array.IsNull(i)) || null_count <= 0) {
        using DecimalType = typename DecimalTypeTrait<DecimalArrayType>::value;
        RETURN_NOT_OK(
            RawBytesToDecimalBytes<DecimalType>(record_loc, record_len, out_ptr));
      }
    }
    *out = std::make_shared<DecimalArrayType>(type, length, std::move(data),
                                              binary_array.null_bitmap(), null_count);
    return Status::OK();
  }
};

/// \brief Convert an Int32 or Int64 array into a Decimal128Array
/// The parquet spec allows systems to write decimals in int32, int64 if the values are
/// small enough to fit in less 4 bytes or less than 8 bytes, respectively.
/// This function implements the conversion from int32 and int64 arrays to decimal arrays.
template <
    typename DecimalArrayType, typename ParquetIntegerType,
    typename = ::arrow::enable_if_t<std::is_same<ParquetIntegerType, Int32Type>::value ||
                                    std::is_same<ParquetIntegerType, Int64Type>::value>>
static Status DecimalIntegerTransfer(RecordReader* reader, MemoryPool* pool,
                                     const std::shared_ptr<Field>& field, Datum* out) {
  // Decimal128 and Decimal256 are only Arrow constructs.  Parquet does not
  // specifically distinguish between decimal byte widths.
  DCHECK(field->type()->id() == ::arrow::Type::DECIMAL128 ||
         field->type()->id() == ::arrow::Type::DECIMAL256);

  const int64_t length = reader->values_written();

  using ElementType = typename ParquetIntegerType::c_type;
  static_assert(std::is_same<ElementType, int32_t>::value ||
                    std::is_same<ElementType, int64_t>::value,
                "ElementType must be int32_t or int64_t");

  const auto values = reinterpret_cast<const ElementType*>(reader->values());

  const auto& decimal_type = checked_cast<const ::arrow::DecimalType&>(*field->type());
  const int64_t type_length = decimal_type.byte_width();

  ARROW_ASSIGN_OR_RAISE(auto data, ::arrow::AllocateBuffer(length * type_length, pool));
  uint8_t* out_ptr = data->mutable_data();

  using ::arrow::bit_util::FromLittleEndian;

  for (int64_t i = 0; i < length; ++i, out_ptr += type_length) {
    // sign/zero extend int32_t values, otherwise a no-op
    const auto value = static_cast<int64_t>(values[i]);

    if constexpr (std::is_same_v<DecimalArrayType, Decimal128Array>) {
      ::arrow::Decimal128 decimal(value);
      decimal.ToBytes(out_ptr);
    } else {
      ::arrow::Decimal256 decimal(value);
      decimal.ToBytes(out_ptr);
    }
  }

  if (reader->nullable_values() && field->nullable()) {
    std::shared_ptr<ResizableBuffer> is_valid = reader->ReleaseIsValid();
    *out = std::make_shared<DecimalArrayType>(field->type(), length, std::move(data),
                                              is_valid, reader->null_count());
  } else {
    *out = std::make_shared<DecimalArrayType>(field->type(), length, std::move(data));
  }
  return Status::OK();
}

/// \brief Convert an arrow::BinaryArray to an arrow::Decimal{128,256}Array
/// We do this by:
/// 1. Creating an arrow::BinaryArray from the RecordReader's builder
/// 2. Allocating a buffer for the arrow::Decimal{128,256}Array
/// 3. Converting the big-endian bytes in each BinaryArray entry to two integers
///    representing the high and low bits of each decimal value.
template <typename DecimalArrayType, typename ParquetType>
Status TransferDecimal(RecordReader* reader, MemoryPool* pool,
                       const std::shared_ptr<Field>& field, Datum* out) {
  auto binary_reader = dynamic_cast<BinaryRecordReader*>(reader);
  DCHECK(binary_reader);
  ::arrow::ArrayVector chunks = binary_reader->GetBuilderChunks();
  for (size_t i = 0; i < chunks.size(); ++i) {
    std::shared_ptr<Array> chunk_as_decimal;
    auto fn = &DecimalConverter<DecimalArrayType, ParquetType>::ConvertToDecimal;
    RETURN_NOT_OK(fn(*chunks[i], field->type(), pool, &chunk_as_decimal));
    // Replace the chunk, which will hopefully also free memory as we go
    chunks[i] = chunk_as_decimal;
  }
  if (!field->nullable()) {
    ReconstructChunksWithoutNulls(&chunks);
  }
  *out = std::make_shared<ChunkedArray>(chunks, field->type());
  return Status::OK();
}

Status TransferHalfFloat(RecordReader* reader, MemoryPool* pool,
                         const std::shared_ptr<Field>& field, Datum* out) {
  static const auto binary_type = ::arrow::fixed_size_binary(2);
  // Read as a FixedSizeBinaryArray - then, view as a HalfFloatArray
  std::shared_ptr<ChunkedArray> chunked_array;
  RETURN_NOT_OK(
      TransferBinary(reader, pool, field->WithType(binary_type), &chunked_array));
  ARROW_ASSIGN_OR_RAISE(*out, chunked_array->View(field->type()));
  return Status::OK();
}

}  // namespace

#define TRANSFER_INT32(ENUM, ArrowType)                                            \
  case ::arrow::Type::ENUM: {                                                      \
    Status s = TransferInt<ArrowType, Int32Type>(reader, std::move(metadata), ctx, \
                                                 value_field, &result);            \
    RETURN_NOT_OK(s);                                                              \
  } break;

#define TRANSFER_INT64(ENUM, ArrowType)                                            \
  case ::arrow::Type::ENUM: {                                                      \
    Status s = TransferInt<ArrowType, Int64Type>(reader, std::move(metadata), ctx, \
                                                 value_field, &result);            \
    RETURN_NOT_OK(s);                                                              \
  } break;

Status TransferColumnData(RecordReader* reader,
                          std::unique_ptr<::parquet::ColumnChunkMetaData> metadata,
                          const std::shared_ptr<Field>& value_field,
                          const ColumnDescriptor* descr, const ReaderContext* ctx,
                          std::shared_ptr<ChunkedArray>* out) {
  auto pool = ctx->pool;
  Datum result;
  std::shared_ptr<ChunkedArray> chunked_result;
  switch (value_field->type()->id()) {
    case ::arrow::Type::DICTIONARY: {
      RETURN_NOT_OK(TransferDictionary(reader, pool, value_field->type(),
                                       value_field->nullable(), &chunked_result));
      result = chunked_result;
    } break;
    case ::arrow::Type::NA: {
      result = std::make_shared<::arrow::NullArray>(reader->values_written());
      break;
    }
    case ::arrow::Type::INT32:
      result = TransferZeroCopy<::arrow::Int32Type, Int32Type>(
          reader, std::move(metadata), ctx, value_field);
      break;
    case ::arrow::Type::INT64:
      result = TransferZeroCopy<::arrow::Int64Type, Int64Type>(
          reader, std::move(metadata), ctx, value_field);
      break;
    case ::arrow::Type::FLOAT:
      result = TransferZeroCopy<::arrow::FloatType, FloatType>(
          reader, std::move(metadata), ctx, value_field);
      break;
    case ::arrow::Type::DOUBLE:
      result = TransferZeroCopy<::arrow::DoubleType, DoubleType>(
          reader, std::move(metadata), ctx, value_field);
      break;
    case ::arrow::Type::BOOL:
      RETURN_NOT_OK(TransferBool(reader, std::move(metadata), ctx,
                                 value_field->nullable(), &result));
      break;
      TRANSFER_INT32(UINT8, ::arrow::UInt8Type);
      TRANSFER_INT32(INT8, ::arrow::Int8Type);
      TRANSFER_INT32(UINT16, ::arrow::UInt16Type);
      TRANSFER_INT32(INT16, ::arrow::Int16Type);
      TRANSFER_INT32(UINT32, ::arrow::UInt32Type);
      TRANSFER_INT64(UINT64, ::arrow::UInt64Type);
      TRANSFER_INT32(DATE32, ::arrow::Date32Type);
      TRANSFER_INT32(TIME32, ::arrow::Time32Type);
      TRANSFER_INT64(TIME64, ::arrow::Time64Type);
      TRANSFER_INT64(DURATION, ::arrow::DurationType);
    case ::arrow::Type::DATE64:
      RETURN_NOT_OK(TransferDate64(reader, pool, value_field, &result));
      break;
    case ::arrow::Type::FIXED_SIZE_BINARY:
    case ::arrow::Type::BINARY:
    case ::arrow::Type::STRING:
    case ::arrow::Type::BINARY_VIEW:
    case ::arrow::Type::STRING_VIEW:
    case ::arrow::Type::LARGE_BINARY:
    case ::arrow::Type::LARGE_STRING: {
      RETURN_NOT_OK(TransferBinary(reader, pool, value_field, &chunked_result));
      result = chunked_result;
    } break;
    case ::arrow::Type::HALF_FLOAT: {
      const auto& type = *value_field->type();
      if (descr->physical_type() != ::parquet::Type::FIXED_LEN_BYTE_ARRAY) {
        return Status::Invalid("Physical type for ", type.ToString(),
                               " must be fixed length binary");
      }
      if (descr->type_length() != type.byte_width()) {
        return Status::Invalid("Fixed length binary type for ", type.ToString(),
                               " must have a byte width of ", type.byte_width());
      }
      RETURN_NOT_OK(TransferHalfFloat(reader, pool, value_field, &result));
    } break;
    case ::arrow::Type::DECIMAL128: {
      switch (descr->physical_type()) {
        case ::parquet::Type::INT32: {
          auto fn = DecimalIntegerTransfer<Decimal128Array, Int32Type>;
          RETURN_NOT_OK(fn(reader, pool, value_field, &result));
        } break;
        case ::parquet::Type::INT64: {
          auto fn = &DecimalIntegerTransfer<Decimal128Array, Int64Type>;
          RETURN_NOT_OK(fn(reader, pool, value_field, &result));
        } break;
        case ::parquet::Type::BYTE_ARRAY: {
          auto fn = &TransferDecimal<Decimal128Array, ByteArrayType>;
          RETURN_NOT_OK(fn(reader, pool, value_field, &result));
        } break;
        case ::parquet::Type::FIXED_LEN_BYTE_ARRAY: {
          auto fn = &TransferDecimal<Decimal128Array, FLBAType>;
          RETURN_NOT_OK(fn(reader, pool, value_field, &result));
        } break;
        default:
          return Status::Invalid(
              "Physical type for decimal128 must be int32, int64, byte array, or fixed "
              "length binary");
      }
    } break;
    case ::arrow::Type::DECIMAL256:
      switch (descr->physical_type()) {
        case ::parquet::Type::INT32: {
          auto fn = DecimalIntegerTransfer<Decimal256Array, Int32Type>;
          RETURN_NOT_OK(fn(reader, pool, value_field, &result));
        } break;
        case ::parquet::Type::INT64: {
          auto fn = &DecimalIntegerTransfer<Decimal256Array, Int64Type>;
          RETURN_NOT_OK(fn(reader, pool, value_field, &result));
        } break;
        case ::parquet::Type::BYTE_ARRAY: {
          auto fn = &TransferDecimal<Decimal256Array, ByteArrayType>;
          RETURN_NOT_OK(fn(reader, pool, value_field, &result));
        } break;
        case ::parquet::Type::FIXED_LEN_BYTE_ARRAY: {
          auto fn = &TransferDecimal<Decimal256Array, FLBAType>;
          RETURN_NOT_OK(fn(reader, pool, value_field, &result));
        } break;
        default:
          return Status::Invalid(
              "Physical type for decimal256 must be int32, int64, byte array, or fixed "
              "length binary");
      }
      break;

    case ::arrow::Type::TIMESTAMP: {
      const ::arrow::TimestampType& timestamp_type =
          checked_cast<::arrow::TimestampType&>(*value_field->type());
      if (descr->physical_type() == ::parquet::Type::INT96) {
        RETURN_NOT_OK(
            TransferInt96(reader, pool, value_field, &result, timestamp_type.unit()));
      } else {
        switch (timestamp_type.unit()) {
          case ::arrow::TimeUnit::MILLI:
          case ::arrow::TimeUnit::MICRO:
          case ::arrow::TimeUnit::NANO:
            result = TransferZeroCopy<::arrow::Int64Type, Int64Type>(
                reader, std::move(metadata), ctx, value_field);
            break;
          default:
            return Status::NotImplemented("TimeUnit not supported");
        }
      }
    } break;
    default:
      return Status::NotImplemented("No support for reading columns of type ",
                                    value_field->type()->ToString());
  }

  if (result.kind() == Datum::ARRAY) {
    *out = std::make_shared<ChunkedArray>(result.make_array());
  } else if (result.kind() == Datum::CHUNKED_ARRAY) {
    *out = result.chunked_array();
  } else {
    DCHECK(false) << "Should be impossible, result was " << result.ToString();
  }

  return Status::OK();
}

}  // namespace parquet::arrow
