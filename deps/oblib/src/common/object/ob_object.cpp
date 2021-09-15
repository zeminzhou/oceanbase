/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#include <string.h>
#include <algorithm>
#include <math.h>  // for fabs, fabsf
#define USING_LOG_PREFIX COMMON
#include "common/object/ob_object.h"
#include "lib/utility/serialization.h"
#include "lib/utility/utility.h"
#include "lib/checksum/ob_crc64.h"
#include "common/object/ob_obj_compare.h"
#include "common/ob_action_flag.h"
#include "lib/hash_func/murmur_hash.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/timezone/ob_time_convert.h"
#include "lib/number/ob_number_v2.h"
#include "lib/utility/ob_hang_fatal_error.h"
#include "lib/string/ob_sql_string.h"
#include "lib/worker.h"
#include "common/object/ob_obj_funcs.h"

using namespace oceanbase;
using namespace oceanbase::common;

void ObSortkeyExtraData::reset() 
{
  extra_buf_size = 0;
  str_offset = 0;
  sortkey_offset = 0;
}

int64_t ObLogicMacroBlockId::hash() const
{
  int64_t hash_val = 0;
  hash_val = common::murmurhash(&data_seq_, sizeof(data_seq_), hash_val);
  hash_val = common::murmurhash(&data_version_, sizeof(data_version_), hash_val);
  return hash_val;
}

bool ObLogicMacroBlockId::operator==(const ObLogicMacroBlockId& other) const
{
  return data_seq_ == other.data_seq_ && data_version_ == other.data_version_;
}

bool ObLogicMacroBlockId::operator!=(const ObLogicMacroBlockId& other) const
{
  return !(operator==(other));
}

OB_SERIALIZE_MEMBER(ObLogicMacroBlockId, data_seq_, data_version_);

bool ObLobIndex::operator==(const ObLobIndex& other) const
{
  return version_ == other.version_ && logic_macro_id_ == other.logic_macro_id_ && byte_size_ == other.byte_size_ &&
         char_size_ == other.char_size_;
}

bool ObLobIndex::operator!=(const ObLobIndex& other) const
{
  return !(operator==(other));
}

OB_SERIALIZE_MEMBER(ObLobIndex, version_, logic_macro_id_, byte_size_, char_size_);

void ObLobData::reset()
{
  version_ = LOB_DATA_VERSION;
  byte_size_ = 0;
  char_size_ = 0;
  idx_cnt_ = 0;
}

bool ObLobData::operator==(const ObLobData& other) const
{
  bool bret = version_ == other.version_ && byte_size_ == other.byte_size_ && char_size_ == other.char_size_ &&
              idx_cnt_ == other.idx_cnt_;
  for (int64_t i = 0; i < idx_cnt_ && bret; ++i) {
    bret = lob_idx_[i] == other.lob_idx_[i];
  }
  return bret;
}

bool ObLobData::operator!=(const ObLobData& other) const
{
  return !(operator==(other));
}

int64_t ObLobData::get_serialize_size() const
{
  int64_t serialize_size = 0;
  serialize_size += serialization::encoded_length_i32(version_);
  serialize_size += serialization::encoded_length_i32(idx_cnt_);
  serialize_size += serialization::encoded_length_i64(byte_size_);
  serialize_size += serialization::encoded_length_i64(char_size_);
  for (int64_t i = 0; i < idx_cnt_; ++i) {
    serialize_size += lob_idx_[i].get_serialize_size();
  }
  return serialize_size;
}

int ObLobData::serialize(char* buf, const int64_t buf_len, int64_t& pos) const
{
  int ret = OB_SUCCESS;
  const int64_t request_size = get_serialize_size();
  if (OB_UNLIKELY(NULL == buf || buf_len <= 0 || pos < 0 || pos + request_size > buf_len)) {
    ret = OB_BUF_NOT_ENOUGH;
    COMMON_LOG(WARN, "invalid arguments", K(ret), KP(buf), K(pos), K(request_size), K(buf_len));
  } else if (OB_FAIL(serialization::encode_i32(buf, buf_len, pos, version_))) {
    COMMON_LOG(WARN, "fail to encode version", K(ret), K(buf_len), K(pos));
  } else if (OB_FAIL(serialization::encode_i32(buf, buf_len, pos, idx_cnt_))) {
    COMMON_LOG(WARN, "fail to encode idx_cnt", K(ret));
  } else if (OB_FAIL(serialization::encode_i64(buf, buf_len, pos, byte_size_))) {
    COMMON_LOG(WARN, "fail to encode byte_size", K(ret), K(buf_len), K(pos));
  } else if (OB_FAIL(serialization::encode_i64(buf, buf_len, pos, char_size_))) {
    COMMON_LOG(WARN, "fail to encode char_size", K(ret), K(buf_len), K(pos));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < idx_cnt_; ++i) {
      if (OB_FAIL(lob_idx_[i].serialize(buf, buf_len, pos))) {
        COMMON_LOG(WARN, "fail to serialize lob index", K(ret), K(buf_len), K(pos));
      }
    }
  }
  return ret;
}

int ObLobData::deserialize(const char* buf, const int64_t buf_len, int64_t& pos)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(NULL == buf || buf_len <= 0 || pos > buf_len)) {
    ret = OB_BUF_NOT_ENOUGH;
    COMMON_LOG(WARN, "invalid arguments", K(ret), KP(buf), K(buf_len), K(pos));
  } else if (OB_FAIL(serialization::decode_i32(buf, buf_len, pos, reinterpret_cast<int32_t*>(&version_)))) {
    COMMON_LOG(WARN, "fail to decode version", K(ret));
  } else if (OB_FAIL(serialization::decode_i32(buf, buf_len, pos, reinterpret_cast<int32_t*>(&idx_cnt_)))) {
    COMMON_LOG(WARN, "fail to decode idx_cnt", K(ret));
  } else if (OB_FAIL(serialization::decode_i64(buf, buf_len, pos, reinterpret_cast<int64_t*>(&byte_size_)))) {
    COMMON_LOG(WARN, "fail to decode byte_size", K(ret));
  } else if (OB_FAIL(serialization::decode_i64(buf, buf_len, pos, reinterpret_cast<int64_t*>(&char_size_)))) {
    COMMON_LOG(WARN, "fail to decode char_size", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < idx_cnt_; ++i) {
      if (OB_FAIL(lob_idx_[i].deserialize(buf, buf_len, pos))) {
        COMMON_LOG(WARN, "fail to deseriaze lob index", K(ret));
      }
    }
  }
  return ret;
}

int ObLobLocator::init(const uint64_t table_id, const uint32_t column_id, const int64_t snapshot_version,
    const uint16_t flags, const ObString& rowid, const ObString& payload)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!is_valid_id(table_id) || !is_valid_id(column_id) || snapshot_version <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    COMMON_LOG(WARN,
        "Invalid argument to init ObLobLocator",
        K(table_id),
        K(column_id),
        K(snapshot_version),
        K(rowid),
        K(payload));
  } else {
    magic_code_ = MAGIC_CODE;
    version_ = LOB_LOCATOR_VERSION;
    snapshot_version_ = snapshot_version;
    table_id_ = table_id;
    column_id_ = column_id;
    option_ = 0;
    flags_ = flags;
    if (rowid.empty()) {
      // for old heap table withou rowid
      set_compat_mode();
      payload_offset_ = 0;
    } else {
      set_inline_mode();
      payload_offset_ = rowid.length();
      MEMCPY(data_, rowid.ptr(), rowid.length());
    }
    if (OB_NOT_NULL(payload.ptr())) {
      MEMCPY(data_ + payload_offset_, payload.ptr(), payload.length());
      payload_size_ = payload.length();
    } else {
      payload_size_ = 0;
    }
  }

  return ret;
}

int ObLobLocator::init(const ObString& payload)
{
  int ret = OB_SUCCESS;
  magic_code_ = MAGIC_CODE;
  version_ = LOB_LOCATOR_VERSION;
  snapshot_version_ = 0;
  table_id_ = 0;
  column_id_ = 0;
  option_ = 0;
  flags_ = LOB_DEFAULT_FLAGS;
  set_compat_mode();
  payload_offset_ = 0;
  if (OB_NOT_NULL(payload.ptr())) {
    MEMCPY(data_ + payload_offset_, payload.ptr(), payload.length());
    payload_size_ = payload.length();
  } else {
    payload_size_ = 0;
  }
  return ret;
}

int ObLobLocator::get_rowid(ObString& rowid) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_NOT_INIT;
    COMMON_LOG(WARN, "ObLobLocator is not init", K(ret), K(*this));
  } else if (!is_inline_mode()) {
    ret = OB_NOT_SUPPORTED;
    COMMON_LOG(WARN, "ObLobLocator with compat mode does not support rowid ", K(ret), K(*this));
  } else if (payload_offset_ <= 0) {
    ret = OB_ERR_UNEXPECTED;
    COMMON_LOG(WARN, "Unexpected payload offset to get rowid", K(ret), K(*this));
  } else {
    rowid = ObString(payload_offset_, data_);
  }
  return ret;
}

int ObLobLocator::get_payload(ObString& payload) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_NOT_INIT;
    COMMON_LOG(WARN, "ObLobLocator is not init", K(ret), K(*this));
  } else if (payload_size_ > 0) {
    payload.assign_ptr(data_ + payload_offset_, payload_size_);
  } else {
    payload.reset();
  }
  return ret;
}

DEF_TO_STRING(ObLobLocator)
{
  int64_t pos = 0;
  J_OBJ_START();
  J_KV(K_(magic_code),
      K_(version),
      K_(snapshot_version),
      K_(table_id),
      K_(column_id),
      K_(flags),
      K_(option),
      K_(payload_offset),
      K_(payload_size));
  J_COMMA();
  if (buf_len > pos && is_valid()) {
    int64_t max_len = buf_len - pos;
    ObString payload(MIN(payload_size_, max_len), get_payload_ptr());
    J_KV("data", payload);
  } else {
    J_KV(K_(data));
  }
  J_OBJ_END();
  return pos;
}

#define PRINT_META()
//#define PRINT_META() BUF_PRINTO(obj.get_meta()); J_COLON();

const char* ObObj::MIN_OBJECT_VALUE_STR = "__OB__MIN__";
const char* ObObj::MAX_OBJECT_VALUE_STR = "__OB__MAX__";
const char* ObObj::NOP_VALUE_STR = "__OB__NOP__";

OB_SERIALIZE_MEMBER(ObDataType, meta_, accuracy_, is_zero_fill_);
OB_SERIALIZE_MEMBER(ObEnumSetInnerValue, numberic_value_, string_value_);

DEFINE_SERIALIZE(ObObjMeta)
{
  int ret = OB_SUCCESS;
  OB_UNIS_ENCODE(type_);
  OB_UNIS_ENCODE(cs_level_);
  OB_UNIS_ENCODE(cs_type_);
  OB_UNIS_ENCODE(scale_);
  return ret;
}

DEFINE_DESERIALIZE(ObObjMeta)
{
  int ret = OB_SUCCESS;
  OB_UNIS_DECODE(type_);
  OB_UNIS_DECODE(cs_level_);
  OB_UNIS_DECODE(cs_type_);
  OB_UNIS_DECODE(scale_);
  return ret;
}

DEFINE_GET_SERIALIZE_SIZE(ObObjMeta)
{
  int64_t len = 0;
  OB_UNIS_ADD_LEN(type_);
  OB_UNIS_ADD_LEN(cs_level_);
  OB_UNIS_ADD_LEN(cs_type_);
  OB_UNIS_ADD_LEN(scale_);
  return len;
}

////////////////////////////////////////////////////////////////

bool ObObj::is_zero() const
{
  bool ret = is_numeric_type();
  if (ret) {
    switch (meta_.get_type()) {
      case ObTinyIntType:
        // fall through
      case ObSmallIntType:
        // fall through
      case ObMediumIntType:
        // fall through
      case ObInt32Type:
        // fall through
      case ObIntType:
        ret = (0 == v_.int64_);
        break;
      case ObUTinyIntType:
        // fall through
      case ObUSmallIntType:
        // fall through
      case ObUMediumIntType:
        // fall through
      case ObUInt32Type:
        // fall through
      case ObUInt64Type:
        ret = (0 == v_.uint64_);
        break;
      // Please do not bother yourself too much to take +0 and -0 into consideration
      // According to the IEEE754 standard, +0 equals to -0
      // https://en.wikipedia.org/wiki/Signed_zero
      case ObFloatType:
        ret = (0 == v_.float_);
        break;
      case ObDoubleType:
        ret = (0 == v_.double_);
        break;
      case ObUFloatType:
        ret = (0 == v_.float_);
        break;
      case ObUDoubleType:
        ret = (0 == v_.double_);
        break;
      case ObNumberType:
        // fall through
      case ObUNumberType:
      case ObNumberFloatType: {
        ret = is_zero_number();
        break;
      }
      case ObBitType: {
        ret = (0 == v_.uint64_);
        break;
      }
      default:
        BACKTRACE(ERROR, true, "unexpected numeric type=%u", meta_.get_type());
        right_to_die_or_duty_to_live();
    }
  }
  return ret;
}

int ObObj::build_not_strict_default_value()
{
  int ret = OB_SUCCESS;
  const ObObjType& data_type = meta_.get_type();
  switch (data_type) {
    case ObTinyIntType:
      set_tinyint(0);
      break;
    case ObSmallIntType:
      set_smallint(0);
      break;
    case ObMediumIntType:
      set_mediumint(0);
      break;
    case ObInt32Type:
      set_int32(0);
      break;
    case ObIntType:
      set_int(0);
      break;
    case ObUTinyIntType:
      set_utinyint(0);
      break;
    case ObUSmallIntType:
      set_usmallint(0);
      break;
    case ObUMediumIntType:
      set_umediumint(0);
      break;
    case ObUInt32Type:
      set_uint32(0);
      break;
    case ObUInt64Type:
      set_uint64(0);
      break;
    case ObFloatType:
      set_float(0);
      break;
    case ObDoubleType:
      set_double(0);
      break;
    case ObUFloatType:
      set_ufloat(0);
      break;
    case ObUDoubleType:
      set_udouble(0);
      break;
    case ObNumberType: {
      number::ObNumber zero;
      zero.set_zero();
      set_number(zero);
      break;
    }
    case ObUNumberType: {
      number::ObNumber zero;
      zero.set_zero();
      set_unumber(zero);
      break;
    }
    case ObDateTimeType:
      set_datetime(ObTimeConverter::ZERO_DATETIME);
      break;
    case ObTimestampType:
      set_timestamp(ObTimeConverter::ZERO_DATETIME);
      break;
    case ObDateType:
      set_date(ObTimeConverter::ZERO_DATE);
      break;
    case ObTimeType:
      set_time(0);
      break;
    case ObYearType:
      set_year(0);
      break;
    case ObVarcharType: {
      ObString null_str;
      set_varchar(null_str);
    } break;
    case ObCharType: {
      ObString null_str;
      set_char(null_str);
    } break;
    case ObTinyTextType:
    case ObTextType:
    case ObMediumTextType:
    case ObLongTextType: {
      ObString null_str;
      set_string(data_type, null_str);
      meta_.set_lob_inrow();
    } break;
    case ObBitType:
      set_bit(0);
      break;
    case ObEnumType:
      set_enum(1);
      break;
    case ObSetType:
      set_set(0);
      break;
    case ObTimestampTZType:
    case ObTimestampLTZType:
    case ObTimestampNanoType: {
      set_otimestamp_null(data_type);
      break;
    }
    case ObRawType: {
      ObString null_str;
      set_raw(null_str);
      break;
    }
    case ObIntervalYMType: {
      const ObIntervalYMValue empty_value;
      set_interval_ym(empty_value);
      break;
    }
    case ObIntervalDSType: {
      const ObIntervalDSValue empty_value;
      set_interval_ds(empty_value);
      break;
    }
    case ObNumberFloatType: {
      number::ObNumber zero;
      zero.set_zero();
      set_number_float(zero);
      break;
    }
    case ObURowIDType: {
      ObURowIDData urowid_data;
      set_urowid(urowid_data);
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
      _OB_LOG(WARN, "unexpected data type=%u", data_type);
  }
  return ret;
}

int ObObj::make_sort_key(char* to, int16_t& offset, int32_t& size,
                           ObSortkeyExtraData* extra_param)
{
  int ret = OB_SUCCESS;
  int64_t buf = 0;
  int32_t copied = 0;
  char* buf_ptr = reinterpret_cast<char*>(&buf);
  char* ptr = reinterpret_cast<char*>(&v_);
  if (offset == 0) {
    to[0] = 1;
    offset = 1;
    copied = 1;
  }
  const ObObjType& data_type = meta_.get_type();
  switch (data_type) {
    case ObNullType: {
      to[0] = 0;
      offset = 0;
      size = 1;
      break;
    }
    case ObTinyIntType: {
      buf_ptr[0] = (char)(ptr[0] ^ 128);
      size = std::min(size - copied, 1 - offset + 1);
      memcpy(to + copied, buf_ptr + offset - 1, size);
      offset += size;
      size += copied;
      if (offset == 2) {
        offset = 0;
      }
      break;
    }
    case ObSmallIntType: {
      buf_ptr[0] = (char)(ptr[1] ^ 128);
      buf_ptr[1] = ptr[0];
      size = std::min(size - copied, 2 - offset + 1);
      memcpy(to + copied, buf_ptr + offset - 1, size);
      offset += size;
      size += copied;
      if (offset == 3) {
        offset = 0;
      }
      break;
    }
    case ObMediumIntType:
    case ObInt32Type: {
      buf_ptr[0] = (char)(ptr[3] ^ 128);
      buf_ptr[1] = ptr[2];
      buf_ptr[2] = ptr[1];
      buf_ptr[3] = ptr[0];
      size = std::min(size - copied, 4 - offset + 1);
      memcpy(to + copied, buf_ptr + offset - 1, size);
      offset += size;
      size += copied;
      if (offset == 5) {
        offset = 0;
      }
      break;
    }
    case ObIntType: {
      buf_ptr[0] = (char)(ptr[7] ^ 128);
      buf_ptr[1] = ptr[6];
      buf_ptr[2] = ptr[5];
      buf_ptr[3] = ptr[4];
      buf_ptr[4] = ptr[3];
      buf_ptr[5] = ptr[2];
      buf_ptr[6] = ptr[1];
      buf_ptr[7] = ptr[0];
      size = std::min(size - copied, 8 - offset + 1);
      memcpy(to + copied, buf_ptr + offset - 1, size);
      offset += size;
      size += copied;
      if (offset == 9) {
        offset = 0;
      }
      break;
    }
    case ObUTinyIntType: {
      buf_ptr[0] = ptr[0];
      size = std::min(size - copied, 1 - offset + 1);
      memcpy(to + copied, buf_ptr + offset - 1, size);
      offset += size;
      size += copied;
      if (offset == 2) {
        offset = 0;
      }
      break;
    }
    case ObUSmallIntType: {
      buf_ptr[0] = ptr[1];
      buf_ptr[1] = ptr[0];
      size = std::min(size - copied, 2 - offset + 1);
      memcpy(to + copied, buf_ptr + offset - 1, size);
      offset += size;
      size += copied;
      if (offset == 3) {
        offset = 0;
      }
      break;
    }
    case ObUMediumIntType:
    case ObUInt32Type: {
      buf_ptr[0] = ptr[3];
      buf_ptr[1] = ptr[2];
      buf_ptr[2] = ptr[1];
      buf_ptr[3] = ptr[0];
      size = std::min(size - copied, 4 - offset + 1);
      memcpy(to + copied, buf_ptr + offset - 1, size);
      offset += size;
      size += copied;
      if (offset == 5) {
        offset = 0;
      }
      break;
    }
    case ObUInt64Type: {
      buf_ptr[0] = ptr[7];
      buf_ptr[1] = ptr[6];
      buf_ptr[2] = ptr[5];
      buf_ptr[3] = ptr[4];
      buf_ptr[4] = ptr[3];
      buf_ptr[5] = ptr[2];
      buf_ptr[6] = ptr[1];
      buf_ptr[7] = ptr[0];
      size = std::min(size - copied, 8 - offset + 1);
      memcpy(to + copied, buf_ptr + offset - 1, size);
      offset += size;
      size += copied;
      if (offset == 9) {
        offset = 0;
      }
      break;
    }
    case ObUFloatType:
    case ObFloatType: {
      if (v_.float_ == 0.0f) v_.float_ = 0.0;
      int32_t tmp;
      memcpy(&tmp, &(v_.float_), sizeof(v_.float_));
      tmp = (tmp ^ (tmp >> 31)) | ((~tmp) & 0x80000000);
      char* ptr1 = reinterpret_cast<char*>(&tmp);
      buf_ptr[0] = ptr1[3];
      buf_ptr[1] = ptr1[2];
      buf_ptr[2] = ptr1[1];
      buf_ptr[3] = ptr1[0];
      size = std::min(size - copied, 4 - offset + 1);
      memcpy(to + copied, buf_ptr + offset - 1, size);
      offset += size;
      size += copied;
      if (offset == 5) {
        offset = 0;
      }
      break;
    }
    case ObUDoubleType:
    case ObDoubleType: {
      if (v_.double_ == 0.0) v_.double_ = 0.0;
      int64_t tmp;
      memcpy(&tmp, &(v_.double_), sizeof(v_.double_));
      tmp = (tmp ^ (tmp >> 63)) | ((~tmp) & 0x8000000000000000ULL);
      char* ptr1 = reinterpret_cast<char*>(&tmp);
      buf_ptr[0] = ptr1[7];
      buf_ptr[1] = ptr1[6];
      buf_ptr[2] = ptr1[5];
      buf_ptr[3] = ptr1[4];
      buf_ptr[4] = ptr1[3];
      buf_ptr[5] = ptr1[2];
      buf_ptr[6] = ptr1[1];
      buf_ptr[7] = ptr1[0];
      size = std::min(size - copied, 8 - offset + 1);
      memcpy(to + copied, buf_ptr + offset - 1, size);
      offset += size;
      size += copied;
      if (offset == 9) {
        offset = 0;
      }
      break;
    }
    case ObBitType:
    case ObEnumType:
    case ObSetType:
    case ObTimeType:
    case ObDateTimeType:
    case ObTimestampType: {
      buf_ptr[0] = ptr[7];
      buf_ptr[1] = ptr[6];
      buf_ptr[2] = ptr[5];
      buf_ptr[3] = ptr[4];
      buf_ptr[4] = ptr[3];
      buf_ptr[5] = ptr[2];
      buf_ptr[6] = ptr[1];
      buf_ptr[7] = ptr[0];
      size = std::min(size - copied, 8 - offset + 1);
      memcpy(to + copied, buf_ptr + offset - 1, size);
      offset += size;
      size += copied;
      if (offset == 9) {
        offset = 0;
      }
      break;
    }
    case ObDateType: {
      buf_ptr[0] = ptr[3];
      buf_ptr[1] = ptr[2];
      buf_ptr[2] = ptr[1];
      buf_ptr[3] = ptr[0];
      size = std::min(size - copied, 4 - offset + 1);
      memcpy(to + copied, buf_ptr + offset - 1, size);
      offset += size;
      size += copied;
      if (offset == 5) {
        offset = 0;
      }
      break;
    }
    case ObYearType: {
      buf_ptr[0] = ptr[0];
      size = std::min(size - copied, 1 - offset + 1);
      memcpy(to + copied, buf_ptr + offset - 1, size);
      offset += size;
      size += copied;
      if (offset == 2) {
        offset = 0;
      }
      break;
    }
    case ObVarcharType:
    case ObCharType: {
      if ((get_meta().get_collation_type() == CS_TYPE_BINARY)) {
        // 对于变长的二进制数据，我们将数据分成 8 个字节的组，
        // 我们在其上附加一个额外的字节，该字节表示上一节中的有效字节数。
        // 额外字节的范围可以从 1 到 9，如是 9 表示后续还有数据。
        //
        // offset 表示当前一个字节数组的索引，字节数组如下所示：
        // ｜null-flag ｜ v_.string_[0] | v_.string_[1] | v_.string_[2] | 
        // v_.string_[3] | v_.string_[4] | v_.string_[5] | v_.string_[6] |
        // v_.string_[7] | 9 | v_.string[8] | ...
        // 该数组由一个null-flag和binary类型的memcomparable格式组成
        //
        int32_t ids = 0; // to字节数组索引
        int32_t ids1 = offset - 1; // memcomparable字符数组索引
        int32_t ids2; // 原数据索引
        int32_t t = val_len_ % 8; // 最后一组数据的大小
        int32_t mem_len = (val_len_ / 8) * 9 + (t ? 9 : 0); // memcomparable数组长度
        // 需要拷贝数据的长度
        // size - copied 表示 to数组剩余的大小; mem_len - offset + 1 表示memparable数组剩余大小
        size = std::min(mem_len - offset + 1, size - copied);
        while (ids < size) {
          // 如果ids1 + 1是9的倍数，则该字节用来表示"上一节中的有效字节数"
          if ((ids1 + 1) % 9 == 0) {
            if (ids1 == mem_len - 1) {
              to[ids + copied] = (t == 0) ? 8 : t;
            } else {
              to[ids + copied] = 9;
            }
          } else {
            ids2 = (ids1 / 9) * 8 + (ids1 % 9);
            if (ids2 >= val_len_) {
              to[ids + copied] = 0;
            } else {
              to[ids + copied] = v_.string_[ids2];
            }
          }
          ids1 += 1;
          ids += 1;
        }
        // 修改偏移
        offset += size;
        // 计算一共拷贝了多少数据
        size += copied;
        if (offset > mem_len) {
          // 如果这个ob_object的数据读完了，重置offset
          offset = 0;
        }
      } else {
        // 对于非二进制的数据，首先将数据通过查表的方式转换为可以比较的二进制数据，
        // 然后参考上面二进制数据转memcomparable格式的方法，
        // 将查表得到的可以比较的二进制数据转换为memcomparable格式。
        // 这里我们有三种类型的数据：
        //      1. 原始数据
        //      2. 查表得到的可以按字节比较的数据(用sortkey数组表示)
        //      3. null-flag + ob_object的memcomparable格式的数据(用memcmp数据表示)

        // 如果是第一次读取改ob_object，重置extra_param
        if (offset == 1) {
          extra_param->reset();
        }
        // 对于mysql的varchar类型，其'xxx' and 'xxx '是相等的。首先要移除尾部' '
        int32_t val_len = val_len_;
        if (data_type == ObVarcharType) {
          while (val_len > 0 && v_.string_[val_len - 1] == ' ') {
            val_len--;
          }
        }
        // sortkey_offset 表示sortkey数组已读的偏移
        // str_offset 表示已读原数据已读的偏移
        int32_t sortkey_offset = extra_param->sortkey_offset;
        int32_t str_offset = extra_param->str_offset;
        // 如果上一次读取的数据没有用完，先从上次剩下的数据填充to数组
        if (extra_param->extra_buf_size != 0) {
          int32_t ids = 0;
          while (copied < size && extra_param->extra_buf_size > 0) {
            to[copied] = extra_param->extra_buf[ids];
            copied++;
            ids++;
            extra_param->extra_buf_size--;
          }
          offset += copied;
        }
        if (copied < size) {
          if (str_offset < val_len) {
            int32_t need = size - copied;
            int32_t t1 = (offset - 1) % 9;
            int32_t t2 = ((offset - 1) + need) % 9;
            // 用offset（memcmp数组的偏移）和 需要的数据的长度计算出：
            // sortkey_start, sortkey_end
            // 表示需要sortkey数组的sortkey_start到sortkey_end这段数据
            int32_t sortkey_start = (offset - 1) / 9 * 8 + t1;
            int32_t sortkey_end = ((offset - 1) + need) / 9 * 8 + t2;
            // 需要的sortkey数组的长度
            int64_t sortkey_len = sortkey_end - sortkey_start;
            // 原始数据还剩下的数据的长度（原始数据未读数据的长度）
            int64_t str_len = val_len - str_offset;
            // 转换是否成功标记
            bool is_valid_collation = false;
            // 存放转结果的buf
            char buf[16];
            // 将原始数据转换为可以按字节比较的数据，转换得到的结果存放在buf里面
            ObCharset::sortkey_v2(get_collation_type(), 
                    get_string_ptr() + str_offset, 
                    str_len, 
                    buf, 
                    sortkey_len, 
                    is_valid_collation, 
                    16);
            if (is_valid_collation) {
              // 成功转换
              // 更新偏移，将数据转换为memcomparable格式
              sortkey_offset += sortkey_len;
              str_offset += str_len;
              int32_t ids = 0;
              int32_t ids1 = 0;
              while (copied < size) {
                if (offset % 9 == 0) {
                  to[copied] = 9;
                } else if (ids < sortkey_len) {
                  to[copied] = buf[ids];
                  ids++;
                } else {
                  break;
                }
                offset++;
                copied++;
              }
              // 处理剩下的数据
              // 比如一个字符占3个字节，转换出来4个字节的用来比较的数据，但是我们只需要2个字节。
              // 在这种情况下，我们需要将剩下的两个字节存起来
              extra_param->extra_buf_size = sortkey_len - ids;
              while (ids < sortkey_len) {
                extra_param->extra_buf[ids1] = buf[ids];
                ids++;
                ids1++;
              }
            } else {
              // 转换失败，直接用原始数据进行比较
              while (copied < size && str_offset < val_len) {
                if (offset % 9 == 0) {
                  to[copied] = 9;
                } else {
                  to[copied] = v_.string_[str_offset];
                  str_offset++;
                }
                copied++;
                offset++;
              }
            }
          }
          extra_param->str_offset = str_offset;
          extra_param->sortkey_offset = sortkey_offset;
        }
        // 将原数据全部转换为memcomparable格式以后，剩下的部分填0，
        // 最后一个字节用来表示"上一节中的有效字节数"
        while (str_offset == val_len && copied < size) {
          if (offset % 9 == 0) {
            int32_t t = sortkey_offset % 8;
            to[copied] = t == 0 ? 8 : t;
            copied++;
            offset = 0;
            break;
          } else {
            to[copied] = 0;
            copied++;
            offset++;
          }
        }
        size = copied;
      }
      break;
    }
    case ObNumberType: 
    case ObUNumberType: {
      // 对于ObNumberType，我们先比较其符号位和指数位(一共占一个字节)，
      // 如果符号位和指数位不相等，则能直接区分大小。
      // 如果符号为和指数位相等，则需要逐个比较其uint32数组里面的数值大小。
      //
      // 所以，其memcomparable如下：
      // | null-flag | sign+exp ｜ uint32[0][3] | uint32[0][2] | uint32[0][1] |
      // uint32[0][0] | uint32[1][3] | uint32[1][2] | 9 | uint32[1][1] | ...
      // 其中，uint32[i][j] 表示uint32数组的第i个数的第j的字节
      //
      // 注意，ObNumberType也是变长的二进制数据，我们需要将数据分成 8 个字节的组，
      // 其上附加一个额外的字节，该字节表示上一节中的有效字节数。
      // 额外字节的范围可以从 1 到 9，如是 9 表示后续还有数据。
      int32_t len = nmb_desc_.len_;
      // len_ 表示的是uint32数组的长度，需要将len * 4 + 1，表示原字节数据组的长度
      len = len << 2;
      int32_t t = (len + 1) % 8;
      int32_t ids = 0;
      int32_t ids1 = offset - 1;
      int32_t ids2;
      char* num = reinterpret_cast<char*>(v_.nmb_digits_);
      int32_t mem_len = (len + 1) / 8 * 9 + (t ? 9 : 0);
      size = std::min(mem_len - offset + 1, size - copied);
      while (ids < size) {
        if ((ids1 + 1) % 9 == 0) {
          if (ids1 == mem_len - 1) {
            to[ids + copied] = (t == 0) ? 8 : t;
          } else {
            to[ids + copied] = 9;
          }
        } else {
          if (ids1 == 0) {
            to[ids + copied] = nmb_desc_.se_;
          } else {
            ids2 = ids1 - (ids1 / 9) - 1;
            if (ids2 >= len) {
              to[ids + copied] = 0;
            } else {
              int32_t n1 = ids2 / 4;
              int32_t t1 = ids2 % 4;
              to[ids + copied] = num[n1 * 4 + (3 - t1)];
              // 如果是负数，数值越大，原值越小。
              // 所以我们需要按位取反。
              if (nmb_desc_.sign_ == number::ObNumber::NEGATIVE) {
                  to[ids + copied] = ~to[ids + copied];
              }
            }
          }
        }
        ids += 1;
        ids1 += 1;
      }
      offset += size;
      size += copied;
      if (offset > mem_len) {
        offset = 0;
      }
      break;
    }
    default:
      offset = 0;
      ret = OB_INVALID_ARGUMENT;
      _OB_LOG(WARN, "unexpected data type=%u", data_type);
  }
  return ret;
}

int ObObj::deep_copy(const ObObj& src, char* buf, const int64_t size, int64_t& pos)
{
  int ret = OB_SUCCESS;
  if (ob_is_string_type(src.get_type())) {
    ObString src_str = src.get_string();
    if (OB_UNLIKELY(size < (pos + src_str.length()))) {
      ret = OB_BUF_NOT_ENOUGH;
    } else {
      MEMCPY(buf + pos, src_str.ptr(), src_str.length());
      *this = src;
      this->set_string(src.get_type(), buf + pos, src_str.length());
      pos += src_str.length();
    }
  } else if (ob_is_raw(src.get_type())) {
    const ObString& src_str = src.get_string();
    if (OB_UNLIKELY(size < (pos + src_str.length()))) {
      ret = OB_BUF_NOT_ENOUGH;
    } else {
      MEMCPY(buf + pos, src_str.ptr(), src_str.length());
      *this = src;
      this->set_raw(buf + pos, src_str.length());
      pos += src_str.length();
    }
  } else if (ob_is_number_tc(src.get_type())) {
    const int64_t number_size = src.get_number_byte_length();
    if (OB_UNLIKELY(size < (int64_t)(pos + number_size))) {
      ret = OB_BUF_NOT_ENOUGH;
    } else {
      MEMCPY(buf + pos, src.get_number_digits(), number_size);
      *this = src;
      this->set_number(src.get_type(), src.get_number_desc(), (uint32_t*)(buf + pos));
      pos += number_size;
    }
  } else if (ob_is_rowid_tc(src.get_type())) {
    if (OB_UNLIKELY(size < (int64_t)(pos + src.get_string_len()))) {
      ret = OB_BUF_NOT_ENOUGH;
    } else {
      MEMCPY(buf + pos, src.get_string_ptr(), src.get_string_len());
      *this = src;
      this->set_urowid(buf + pos, src.get_string_len());
      pos += src.get_string_len();
    }
  } else if (ob_is_lob_locator(src.get_type())) {
    if (OB_UNLIKELY(size < (pos + src.get_val_len()))) {
      ret = OB_BUF_NOT_ENOUGH;
    } else {
      // copy all the value
      MEMCPY(buf + pos, src.get_string_ptr(), src.get_val_len());
      *this = src;
      ObLobLocator* res = reinterpret_cast<ObLobLocator*>((buf + pos));
      this->set_lob_locator(*res);
      pos += src.get_val_len();
    }
  } else {
    *this = src;
  }
  return ret;
}

void* ObObj::get_deep_copy_obj_ptr()
{
  void * ptr = NULL;
  if (ob_is_string_type(this->get_type())) {
    // val_len_ == 0 is empty string, and it may point to unexpected address
    // Therefore, reset it to NULL
    if (val_len_ != 0) {
      ptr = (void *)v_.string_;
    }
  } else if (ob_is_raw(this->get_type())) {
    ptr = (void *)v_.string_;
  } else if (ob_is_number_tc(this->get_type())) {
    ptr = (void *)v_.nmb_digits_;
  } else if (ob_is_rowid_tc(this->get_type())) {
    ptr = (void *)v_.string_;
  } else if (ob_is_lob_locator(this->get_type())) {
    ptr = (void *)&v_.lob_locator_;
  } else {
    // do nothing
  }
  return ptr;
}

bool ObObj::can_compare(const ObObj &other) const
{
  obj_cmp_func cmp_func = NULL;
  return (is_min_value() || is_max_value() || other.is_min_value() || other.is_max_value() ||
          ObObjCmpFuncs::can_cmp_without_cast(get_meta(), other.get_meta(), CO_CMP, cmp_func));
}

int ObObj::check_collation_free_and_compare(const ObObj& other, int& cmp) const
{
  int ret = OB_SUCCESS;
  cmp = 0;
  if (CS_TYPE_COLLATION_FREE != get_collation_type() && CS_TYPE_COLLATION_FREE != other.get_collation_type()) {
    ret = compare(other, CS_TYPE_INVALID, cmp);
  } else if (is_null() || other.is_null() || is_min_value() || is_max_value() || other.is_min_value() ||
             other.is_max_value()) {
    ret = ObObjCmpFuncs::compare(*this, other, CS_TYPE_INVALID, cmp);
  } else if (OB_UNLIKELY(get_collation_type() != other.get_collation_type()) ||
             CS_TYPE_COLLATION_FREE != get_collation_type() || get_type() != other.get_type() || !is_character_type()) {
    LOG_ERROR("unexpected error, invalid argument", K(*this), K(other));
    ret = OB_ERR_UNEXPECTED;
  } else {
    const int32_t lhs_len = get_val_len();
    const int32_t rhs_len = other.get_val_len();
    const int32_t cmp_len = std::min(lhs_len, rhs_len);
    const bool is_oracle = lib::is_oracle_mode();
    bool need_skip_tail_space = false;
    cmp = memcmp(get_string_ptr(), other.get_string_ptr(), cmp_len);
    // if two strings only have different trailing spaces:
    // 1. in oracle varchar mode, the strings are considered to be different,
    // 2. in oracle char mode, the strings are considered to be same,
    // 3. in mysql mode, the strings are considered to be different.
    if (is_oracle) {
      if (0 == cmp) {
        if (!is_varying_len_char_type()) {
          need_skip_tail_space = true;
        } else if (lhs_len != cmp_len || rhs_len != cmp_len) {
          cmp = lhs_len > cmp_len ? 1 : -1;
        }
      }
    } else if (0 == cmp && (lhs_len != cmp_len || rhs_len != cmp_len)) {
      need_skip_tail_space = true;
    }
    if (need_skip_tail_space) {
      bool has_non_space = false;
      const int32_t left_len = (lhs_len > cmp_len) ? lhs_len - cmp_len : rhs_len - cmp_len;
      const char* ptr = (lhs_len > cmp_len) ? get_string_ptr() : other.get_string_ptr();
      const unsigned char* uptr = reinterpret_cast<const unsigned char*>(ptr);
      int32_t i = 0;
      uptr += cmp_len;
      for (; i < left_len; ++i) {
        if (*(uptr + i) != ' ') {
          has_non_space = true;
          break;
        }
      }
      if (has_non_space) {
        // special behavior of mysql: a\1 < a, but ab > a
        if (*(uptr + i) < ' ') {
          cmp = lhs_len > cmp_len ? -1 : 1;
        } else {
          cmp = lhs_len > cmp_len ? 1 : -1;
        }
      }
    }
  }
  return ret;
}

/*
 * ATTENTION:
 *
 * that_obj MUST have same type with this obj (*this)
 */

int ObObj::compare(const ObObj& other, int& cmp) const
{
  return ObObjCmpFuncs::compare(*this, other, CS_TYPE_INVALID, cmp);
}

// TODO : remove this function
int ObObj::compare(const ObObj& other) const
{
  return ObObjCmpFuncs::compare_nullsafe(*this, other, CS_TYPE_INVALID);
}

int ObObj::compare(const ObObj& other, ObCollationType cs_type, int& cmp) const
{
  return ObObjCmpFuncs::compare(*this, other, cs_type, cmp);
}

// TODO : remove this function
int ObObj::compare(const ObObj& other, ObCollationType cs_type /*COLLATION_TYPE_MAX*/) const
{
  return ObObjCmpFuncs::compare_nullsafe(*this, other, cs_type);
}

int ObObj::compare(const ObObj& other, ObCompareCtx& cmp_ctx, int& cmp) const
{
  return ObObjCmpFuncs::compare(*this, other, cmp_ctx, cmp);
}

// TODO : remove this function
int ObObj::compare(const ObObj& other, ObCompareCtx& cmp_ctx) const
{
  return ObObjCmpFuncs::compare_nullsafe(*this, other, cmp_ctx);
}

int ObObj::compare(const ObObj& other, ObCollationType cs_type, const ObCmpNullPos null_pos) const
{
  ObCompareCtx cmp_ctx(ObMaxType, cs_type, true, INVALID_TZ_OFF, null_pos);
  return ObObjCmpFuncs::compare_nullsafe(*this, other, cmp_ctx);
}

int ObObj::equal(const ObObj& other, bool& is_equal) const
{
  return ObObjCmpFuncs::compare_oper(*this, other, CS_TYPE_INVALID, CO_EQ, is_equal);
}
/*
 * ATTENTION:
 *
 * that_obj MUST have same type with this obj (*this)
 */
bool ObObj::is_equal(const ObObj& other) const
{
  return ObObjCmpFuncs::compare_oper_nullsafe(*this, other, CS_TYPE_INVALID, CO_EQ);
}

int ObObj::equal(const ObObj& other, ObCollationType cs_type, bool& is_equal) const
{
  return ObObjCmpFuncs::compare_oper(*this, other, cs_type, CO_EQ, is_equal);
}
/*
 * ATTENTION:
 *
 * that_obj MUST have same type with this obj (*this)
 */
bool ObObj::is_equal(const ObObj& other, ObCollationType cs_type) const
{
  return ObObjCmpFuncs::compare_oper_nullsafe(*this, other, cs_type, CO_EQ);
}

/*
 * ATTENTION:
 *
 * that_obj MUST have same type with this obj (*this)
 */
bool ObObj::operator<(const ObObj& other) const
{
  return ObObjCmpFuncs::compare_oper_nullsafe(*this, other, CS_TYPE_INVALID, CO_LT);
}

/*
 * ATTENTION:
 *
 * that_obj MUST have same type with this obj (*this)
 */
bool ObObj::operator>(const ObObj& other) const
{
  return ObObjCmpFuncs::compare_oper_nullsafe(*this, other, CS_TYPE_INVALID, CO_GT);
}

/*
 * ATTENTION:
 *
 * that_obj MUST have same type with this obj (*this)
 */
bool ObObj::operator>=(const ObObj& other) const
{
  return ObObjCmpFuncs::compare_oper_nullsafe(*this, other, CS_TYPE_INVALID, CO_GE);
}

/*
 * ATTENTION:
 *
 * that_obj MUST have same type with this obj (*this)
 */
bool ObObj::operator<=(const ObObj& other) const
{
  return ObObjCmpFuncs::compare_oper_nullsafe(*this, other, CS_TYPE_INVALID, CO_LE);
}

/*
 * ATTENTION:
 *
 * that_obj MUST have same type with this obj (*this)
 */
bool ObObj::operator==(const ObObj& other) const
{
  return ObObjCmpFuncs::compare_oper_nullsafe(*this, other, CS_TYPE_INVALID, CO_EQ);
}

/*
 * ATTENTION:
 *
 * that_obj MUST have same type with this obj (*this)
 */
bool ObObj::operator!=(const ObObj& other) const
{
  return ObObjCmpFuncs::compare_oper_nullsafe(*this, other, CS_TYPE_INVALID, CO_NE);
}

int ObObj::apply(const ObObj& mutation)
{
  int ret = OB_SUCCESS;
  int org_type = get_type();
  int mut_type = mutation.get_type();
  if (OB_UNLIKELY(
          ObMaxType <= mut_type ||
          (ObExtendType != org_type && ObNullType != org_type && ObExtendType != mut_type && ObNullType != mut_type &&
              org_type != mut_type && !(ObLongTextType == org_type && ObLobType == mut_type)))) {
    _OB_LOG(WARN, "type not coincident or invalid type[this->type:%d,mutation.type:%d]", org_type, mut_type);
    ret = OB_INVALID_ARGUMENT;
  } else {
    switch (mut_type) {
      case ObNullType:
        set_null();
        break;
      case ObExtendType: {
        int64_t org_ext = get_ext();
        switch (mutation.get_ext()) {
          case ObActionFlag::OP_DEL_ROW:
          case ObActionFlag::OP_DEL_TABLE:
            /// used for join, if right row was deleted, set the cell to null
            set_null();
            break;
          case ObActionFlag::OP_ROW_DOES_NOT_EXIST:
            /// do nothing
            break;
          case ObActionFlag::OP_NOP:
            if (org_ext == ObActionFlag::OP_ROW_DOES_NOT_EXIST || org_ext == ObActionFlag::OP_DEL_ROW) {
              set_null();
            }
            break;
          default:
            ret = OB_INVALID_ARGUMENT;
            _OB_LOG(ERROR, "unsupported ext value [value:%ld]", mutation.get_ext());
            break;
        }  // end switch
        break;
      }
      default:
        *this = mutation;
        break;
    }  // end switch
  }
  return ret;
}

////////////////////////////////////////////////////////////////
#define DEF_FUNC_ENTRY(OBJTYPE)                                                                            \
  {                                                                                                        \
    obj_print_sql<OBJTYPE>, obj_print_str<OBJTYPE>, obj_print_plain_str<OBJTYPE>, obj_print_json<OBJTYPE>, \
        obj_crc64<OBJTYPE>, obj_crc64_v2<OBJTYPE>, obj_batch_checksum<OBJTYPE>, obj_murmurhash<OBJTYPE>,   \
        ObjHashCalculator<OBJTYPE, ObDefaultHash, ObObj>::calc_hash_value, obj_val_serialize<OBJTYPE>,     \
        obj_val_deserialize<OBJTYPE>, obj_val_get_serialize_size<OBJTYPE>,                                 \
        ObjHashCalculator<OBJTYPE, ObWyHash, ObObj>::calc_hash_value, obj_crc64_v3<OBJTYPE>,               \
        ObjHashCalculator<OBJTYPE, ObXxHash, ObObj>::calc_hash_value,                                      \
        ObjHashCalculator<OBJTYPE, ObMurmurHash, ObObj>::calc_hash_value,                                  \
  }

ObObjTypeFuncs OBJ_FUNCS[ObMaxType] = {
    DEF_FUNC_ENTRY(ObNullType),           // 0
    DEF_FUNC_ENTRY(ObTinyIntType),        // 1
    DEF_FUNC_ENTRY(ObSmallIntType),       // 2
    DEF_FUNC_ENTRY(ObMediumIntType),      // 3
    DEF_FUNC_ENTRY(ObInt32Type),          // 4
    DEF_FUNC_ENTRY(ObIntType),            // 5
    DEF_FUNC_ENTRY(ObUTinyIntType),       // 6
    DEF_FUNC_ENTRY(ObUSmallIntType),      // 7
    DEF_FUNC_ENTRY(ObUMediumIntType),     // 8
    DEF_FUNC_ENTRY(ObUInt32Type),         // 9
    DEF_FUNC_ENTRY(ObUInt64Type),         // 10
    DEF_FUNC_ENTRY(ObFloatType),          // 11
    DEF_FUNC_ENTRY(ObDoubleType),         // 12
    DEF_FUNC_ENTRY(ObUFloatType),         // 13
    DEF_FUNC_ENTRY(ObUDoubleType),        // 14
    DEF_FUNC_ENTRY(ObNumberType),         // 15
    DEF_FUNC_ENTRY(ObUNumberType),        // 16: unumber is the same as number
    DEF_FUNC_ENTRY(ObDateTimeType),       // 17
    DEF_FUNC_ENTRY(ObTimestampType),      // 18
    DEF_FUNC_ENTRY(ObDateType),           // 19
    DEF_FUNC_ENTRY(ObTimeType),           // 20
    DEF_FUNC_ENTRY(ObYearType),           // 21
    DEF_FUNC_ENTRY(ObVarcharType),        // 22, varchar
    DEF_FUNC_ENTRY(ObCharType),           // 23, char
    DEF_FUNC_ENTRY(ObHexStringType),      // 24, hex_string
    DEF_FUNC_ENTRY(ObExtendType),         // 25, ext
    DEF_FUNC_ENTRY(ObUnknownType),        // 26, unknown
    DEF_FUNC_ENTRY(ObTinyTextType),       // 27, tiny_text
    DEF_FUNC_ENTRY(ObTextType),           // 28, text
    DEF_FUNC_ENTRY(ObMediumTextType),     // 29, medium_text
    DEF_FUNC_ENTRY(ObLongTextType),       // 30, longtext
    DEF_FUNC_ENTRY(ObBitType),            // 31, bit
    DEF_FUNC_ENTRY(ObEnumType),           // 32, enum
    DEF_FUNC_ENTRY(ObSetType),            // 33, set
    DEF_FUNC_ENTRY(ObEnumInnerType),      // 34, enum
    DEF_FUNC_ENTRY(ObSetInnerType),       // 35, set
    DEF_FUNC_ENTRY(ObTimestampTZType),    // 36, timestamp with time zone
    DEF_FUNC_ENTRY(ObTimestampLTZType),   // 37, timestamp with local time zone
    DEF_FUNC_ENTRY(ObTimestampNanoType),  // 38, timestamp (9)
    DEF_FUNC_ENTRY(ObRawType),            // 39, timestamp (9)
    DEF_FUNC_ENTRY(ObIntervalYMType),     // 40, interval year to month
    DEF_FUNC_ENTRY(ObIntervalDSType),     // 41, interval day to second
    DEF_FUNC_ENTRY(ObNumberFloatType),    // 42, number float
    DEF_FUNC_ENTRY(ObNVarchar2Type),      // 43, nvarchar2
    DEF_FUNC_ENTRY(ObNCharType),          // 44, nchar
    DEF_FUNC_ENTRY(ObURowIDType),         // 45, urowid
    DEF_FUNC_ENTRY(ObLobType),            // 46, lob
};

ob_obj_hash ObObjUtil::get_murmurhash_v3(ObObjType type)
{
  return OBJ_FUNCS[type].murmurhash_v3;
}

ob_obj_hash ObObjUtil::get_murmurhash_v2(ObObjType type)
{
  return OBJ_FUNCS[type].murmurhash_v2;
}

ob_obj_hash ObObjUtil::get_wyhash(ObObjType type)
{
  return OBJ_FUNCS[type].wyhash;
}

ob_obj_crc64_v3 ObObjUtil::get_crc64_v3(ObObjType type)
{
  return ::OBJ_FUNCS[type].crc64_v3;
}

ob_obj_hash ObObjUtil::get_xxhash64(ObObjType type)
{
  return ::OBJ_FUNCS[type].xxhash64;
}

////////////////////////////////////////////////////////////////
int ObObj::print_sql_literal(char* buffer, int64_t length, int64_t& pos, const ObObjPrintParams& params) const
{
  return OBJ_FUNCS[meta_.get_type()].print_sql(*this, buffer, length, pos, params);
}

// used for show create table default value
// for example:
// `a` int(11) NOT NULL DEFAULT '0'  (with '')
// always with ''
int ObObj::print_varchar_literal(char* buffer, int64_t length, int64_t& pos, const ObObjPrintParams& params) const
{
  return OBJ_FUNCS[meta_.get_type()].print_str(*this, buffer, length, pos, params);
}

int ObObj::print_plain_str_literal(char* buffer, int64_t length, int64_t& pos, const ObObjPrintParams& params) const
{
  return OBJ_FUNCS[meta_.get_type()].print_plain_str(*this, buffer, length, pos, params);
}

void ObObj::print_str_with_repeat(char* buf, int64_t buf_len, int64_t& pos) const
{
  const unsigned char* uptr = reinterpret_cast<const unsigned char*>(v_.string_);
  int32_t real_len = val_len_;
  int32_t repeats = 0;
  int8_t cnt_space = 0;  // There is no space for whole multibyte character, then add trailing spaces.
  if (NULL != uptr && real_len > 0) {
    while (' ' == uptr[real_len - 1]) {
      --real_len;
      ++cnt_space;
    }
    // for utf-8 character set, pad BFBFEF as the tailing characters in a loop
    while (real_len - 2 > 0 && 0xBF == uptr[real_len - 1] && 0xBF == uptr[real_len - 2] && 0xEF == uptr[real_len - 3]) {
      real_len -= 3;
      ++repeats;
    }
  }
  if (0 == repeats) {
    real_len = val_len_;
  }
  BUF_PRINTO(ObString(0, real_len, v_.string_));
  if (repeats > 0) {
    BUF_PRINTF(" \'<%X%X%X><repeat %d times>\' ", uptr[real_len], uptr[real_len + 1], uptr[real_len + 2], repeats);
    // There is no space for whole multibyte character, then add trailing spaces.
    if (1 == cnt_space) {
      BUF_PRINTO(" ");
    } else if (2 == cnt_space) {
      BUF_PRINTO("  ");
    }
  }
}

int ObObj::print_smart(char* buf, int64_t buf_len, int64_t& pos) const
{
  int ret = OB_SUCCESS;
  if (get_type() < ObMaxType && get_type() >= ObNullType) {
    ObObjPrintParams params;
    bool can_print = true;
    if (OB_ISNULL(buf) || OB_UNLIKELY(buf_len <= 0)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (!(meta_.is_string_or_lob_locator_type() && ObHexStringType != meta_.get_type())) {
      ret = OBJ_FUNCS[meta_.get_type()].print_json(*this, buf, buf_len, pos, params);
    } else if (OB_FAIL(is_printable(get_string_ptr(), get_string_len(), can_print))) {
    } else if (can_print) {
      ret = OBJ_FUNCS[meta_.get_type()].print_json(*this, buf, buf_len, pos, params);
    } else {
      J_OBJ_START();
      PRINT_META();
      BUF_PRINTO(ob_obj_type_str(get_type()));
      J_COLON();
      if (OB_FAIL(obj_print_sql<ObHexStringType>(*this, buf, buf_len, pos, params))) {
      } else {
        J_COMMA();
        J_KV(N_COLLATION, ObCharset::collation_name(get_collation_type()));
        J_OBJ_END();
      }
    }
  }
  return ret;
}

int ObObj::print_format(char* buf, int64_t buf_len, int64_t& pos) const
{
  int ret = OB_SUCCESS;
  if (get_type() < ObMaxType && get_type() >= ObNullType) {
    ObObjPrintParams params;
    bool can_print = true;
    if (OB_ISNULL(buf) || OB_UNLIKELY(buf_len <= 0)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (!(meta_.is_string_type() && ObHexStringType != meta_.get_type())) {
      ret = OBJ_FUNCS[meta_.get_type()].print_sql(*this, buf, buf_len, pos, params);
    } else if (OB_FAIL(is_printable(get_string_ptr(), get_string_len(), can_print))) {
    } else if (can_print) {
      ret = OBJ_FUNCS[meta_.get_type()].print_sql(*this, buf, buf_len, pos, params);
    } else {
      ret = obj_print_sql<ObHexStringType>(*this, buf, buf_len, pos, params);
    }
  }
  return ret;
}

void ObObj::print_range_value(char* buf, int64_t buf_len, int64_t& pos) const
{
  if (is_string_type()) {
    J_OBJ_START();
    BUF_PRINTO(ob_obj_type_str(this->get_type()));
    J_COLON();
    // for Unicode character set
    print_str_with_repeat(buf, buf_len, pos);
    J_COMMA();
    J_KV(N_COLLATION, ObCharset::collation_name(this->get_collation_type()));
    J_OBJ_END();
  } else {
    (void)databuff_print_obj(buf, buf_len, pos, *this);
  }
}

int64_t ObObj::to_string(char* buf, const int64_t buf_len, const ObObjPrintParams& params) const
{
  int64_t pos = 0;
  if (get_type() < ObMaxType && get_type() >= ObNullType) {
    (void)OBJ_FUNCS[meta_.get_type()].print_json(*this, buf, buf_len, pos, params);
  }
  return pos;
}

bool ObObj::check_collation_integrity() const
{
  bool is_ok = true;
#ifndef NDEBUG
  if (ObNullType == get_type()) {
    // ignore null
    // is_ok = (CS_TYPE_BINARY == get_collation_type() && CS_LEVEL_IGNORABLE == get_collation_level());
  } else if (ob_is_numeric_type(get_type()) || ob_is_temporal_type(get_type())) {
    is_ok = (CS_TYPE_BINARY == get_collation_type() && CS_LEVEL_NUMERIC == get_collation_level());
  } else {
    // ignore: varchar, char, binary, varbinary, unknown, ext
  }
  if (!is_ok) {
    if (REACH_TIME_INTERVAL(10 * 1000 * 1000)) {
      BACKTRACE(WARN, true, "unexpected collation type: %s", to_cstring(get_meta()));
    }
  }
#endif
  return is_ok;
}

uint64_t ObObj::hash_v1(uint64_t seed) const
{
  check_collation_integrity();
  return OBJ_FUNCS[meta_.get_type()].murmurhash(*this, seed);
}

uint64_t ObObj::hash(uint64_t seed) const
{
  check_collation_integrity();
  return OBJ_FUNCS[meta_.get_type()].murmurhash_v2(*this, seed);
}

uint64_t ObObj::hash_murmur(uint64_t seed) const
{
  check_collation_integrity();
  return OBJ_FUNCS[meta_.get_type()].murmurhash_v3(*this, seed);
}

uint64_t ObObj::hash_wy(uint64_t seed) const
{
  check_collation_integrity();
  return OBJ_FUNCS[meta_.get_type()].wyhash(*this, seed);
}

uint64_t ObObj::hash_xx(uint64_t seed) const
{
  check_collation_integrity();
  return OBJ_FUNCS[meta_.get_type()].xxhash64(*this, seed);
}

int64_t ObObj::checksum(const int64_t current) const
{
  check_collation_integrity();
  return OBJ_FUNCS[meta_.get_type()].crc64(*this, current);
}

int64_t ObObj::checksum_v2(const int64_t current) const
{
  check_collation_integrity();
  return OBJ_FUNCS[meta_.get_type()].crc64_v2(*this, current);
}

void ObObj::checksum(ObBatchChecksum& bc) const
{
  check_collation_integrity();
  OBJ_FUNCS[meta_.get_type()].batch_checksum(*this, bc);
}

void ObObj::dump(const int32_t log_level /*= OB_LOG_LEVEL_DEBUG*/) const
{
  _OB_NUM_LEVEL_LOG(log_level, "%s", S(*this));
}

int ObObj::print_varchar_literal(const ObIArray<ObString>& type_infos, char* buffer, int64_t length, int64_t& pos) const
{
  int ret = OB_SUCCESS;
  ObSqlString str_val;
  if (OB_UNLIKELY(!meta_.is_enum_or_set())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected obj type", KPC(this), K(ret));
  } else if (is_enum()) {
    if (OB_FAIL(get_enum_str_val(str_val, type_infos))) {
      LOG_WARN("fail to get enum str val", K(str_val), K(type_infos), K(ret));
    }
  } else {
    if (OB_FAIL(get_set_str_val(str_val, type_infos))) {
      LOG_WARN("fail to get set str val", K(str_val), K(type_infos), K(ret));
    }
  }
  if (OB_SUCC(ret) &&
      databuff_printf(buffer, length, pos, "'%.*s'", static_cast<int32_t>(str_val.length()), str_val.ptr())) {
    LOG_WARN("fail to print string", K(buffer), K(length), K(pos), K(str_val), K(ret));
  }
  return ret;
}

int ObObj::print_plain_str_literal(
    const ObIArray<ObString>& type_infos, char* buffer, int64_t length, int64_t& pos) const
{
  int ret = OB_SUCCESS;
  ObSqlString str_val;
  if (OB_UNLIKELY(!meta_.is_enum_or_set())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected obj type", KPC(this), K(ret));
  } else if (is_enum()) {
    if (OB_FAIL(get_enum_str_val(str_val, type_infos))) {
      LOG_WARN("fail to get enum str val", K(str_val), K(type_infos), K(ret));
    }
  } else {
    if (OB_FAIL(get_set_str_val(str_val, type_infos))) {
      LOG_WARN("fail to get set str val", K(str_val), K(type_infos), K(ret));
    }
  }
  if (OB_SUCC(ret) &&
      databuff_printf(buffer, length, pos, "%.*s", static_cast<int32_t>(str_val.length()), str_val.ptr())) {
    LOG_WARN("fail to print string", K(buffer), K(length), K(pos), K(str_val), K(ret));
  }
  return ret;
}

int ObObj::get_enum_str_val(ObSqlString& str_val, const ObIArray<ObString>& type_infos) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!meta_.is_enum())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected type", KPC(this), K(ret));
  } else {
    uint64_t val = get_enum();
    if (OB_UNLIKELY(val > type_infos.count())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected obj value", K(type_infos), KPC(this), K(ret));
    } else if (0 == val) {
      if (OB_FAIL(str_val.append(ObString("")))) {
        LOG_WARN("fail to append string", K(str_val), K(ret));
      }
    } else {
      const ObString& type_info = type_infos.at(val - 1);  // enum value start from 1
      if (OB_FAIL(str_val.append(type_info))) {
        LOG_WARN("fail to append string", K(str_val), K(type_info), K(ret));
      }
    }
  }
  return ret;
}

int ObObj::get_set_str_val(ObSqlString& str_val, const ObIArray<ObString>& type_infos) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!meta_.is_set())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected type", KPC(this), K(ret));
  } else {
    uint64_t val = get_set();
    int64_t type_info_cnt = type_infos.count();
    if (OB_UNLIKELY(type_info_cnt > 64 || type_info_cnt <= 0)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected type infos", K(type_infos), K(ret));
    } else if (OB_UNLIKELY(type_info_cnt < 64 && (val > ((1ULL << type_info_cnt) - 1)))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected obj value", K(val), K(type_infos), K(ret));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < type_info_cnt; ++i) {
      if (val & (1ULL << i)) {
        if (OB_FAIL(str_val.append(type_infos.at(i)))) {
          LOG_WARN("fail to append string", K(str_val), K(type_infos.at(i)), K(ret));
        } else if (OB_FAIL(str_val.append(","))) {
          LOG_WARN("fail to print string", K(str_val), K(ret));
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (val != 0 && OB_FAIL(str_val.set_length(str_val.length() - 1))) {  // remove last comma
      LOG_WARN("fail to str length", K(str_val), K(ret));
    }
  }
  return ret;
}

int ObObj::get_char_length(const ObAccuracy accuracy, int32_t& char_len, bool is_oracle_mode) const
{
  int ret = OB_SUCCESS;

  if (!is_fixed_len_char_type()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("type must be char", K(get_type()));
  } else {
    if (is_oracle_byte_length(is_oracle_mode, accuracy.get_length_semantics())) {
      // get byte length
      char_len = static_cast<int32_t>(get_val_len());
    } else {
      // get char length
      char_len = static_cast<int32_t>(ObCharset::strlen_char(get_collation_type(), get_string_ptr(), get_val_len()));
    }
  }

  return ret;
}

int ObObj::convert_string_value_charset(ObCharsetType charset_type, ObIAllocator& allocator)
{
  int ret = OB_SUCCESS;
  ObString str;
  get_string(str);
  if (ObCharset::is_valid_charset(charset_type) && CHARSET_BINARY != charset_type) {
    ObCollationType collation_type = ObCharset::get_default_collation(charset_type);
    const ObCharsetInfo* from_charset_info = ObCharset::get_charset(get_collation_type());
    const ObCharsetInfo* to_charset_info = ObCharset::get_charset(collation_type);
    if (OB_ISNULL(from_charset_info) || OB_ISNULL(to_charset_info)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("charsetinfo is null", K(ret), K(get_collation_type()), K(collation_type));
    } else if (CS_TYPE_INVALID == get_collation_type() || CS_TYPE_INVALID == collation_type) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid collation", K(get_collation_type()), K(collation_type), K(ret));
    } else if (CS_TYPE_BINARY != get_collation_type() && CS_TYPE_BINARY != collation_type &&
               strcmp(from_charset_info->csname, to_charset_info->csname) != 0) {
      char* buf = NULL;
      int32_t buf_len = str.length() * 4;
      uint32_t result_len = 0;
      if (0 == buf_len) {
        // do noting
      } else if (OB_UNLIKELY(NULL == (buf = static_cast<char*>(allocator.alloc(buf_len))))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("alloc memory failed", K(ret), K(buf_len));
      } else {
        ret = ObCharset::charset_convert(
            get_collation_type(), str.ptr(), str.length(), collation_type, buf, buf_len, result_len);
        if (OB_SUCCESS != ret) {
          int32_t str_offset = 0;
          int64_t buf_offset = 0;
          ObString question_mark = ObCharsetUtils::get_const_str(collation_type, '?');
          while (str_offset < str.length() && buf_offset + question_mark.length() <= buf_len) {
            int64_t offset =
                ObCharset::charpos(get_collation_type(), str.ptr() + str_offset, str.length() - str_offset, 1);
            ret = ObCharset::charset_convert(get_collation_type(),
                str.ptr() + str_offset,
                offset,
                collation_type,
                buf + buf_offset,
                buf_len - buf_offset,
                result_len);
            str_offset += offset;
            if (OB_SUCCESS == ret) {
              buf_offset += result_len;
            } else {
              MEMCPY(buf + buf_offset, question_mark.ptr(), question_mark.length());
              buf_offset += question_mark.length();
            }
          }
          if (str_offset < str.length()) {
            ret = OB_SIZE_OVERFLOW;
            LOG_WARN("size overflow", K(ret), K(str), KPHEX(str.ptr(), str.length()));
          } else {
            result_len = buf_offset;
            ret = OB_SUCCESS;
            LOG_WARN("charset convert failed", K(ret), K(get_collation_type()), K(collation_type));
          }
        }
        if (OB_SUCC(ret)) {
          set_string(get_type(), buf, static_cast<int32_t>(result_len));
          set_collation_type(collation_type);
        }
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////
DEFINE_SERIALIZE(ObObj)
{
  int ret = OB_SUCCESS;
  OB_UNIS_ENCODE(meta_);
  if (OB_SUCC(ret)) {
    if (meta_.is_invalid()) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ret = OBJ_FUNCS[meta_.get_type()].serialize(*this, buf, buf_len, pos);
    }
  }
  return ret;
}

DEFINE_DESERIALIZE(ObObj)
{
  int ret = OB_SUCCESS;
  OB_UNIS_DECODE(meta_);
  if (OB_SUCC(ret)) {
    if (meta_.is_invalid()) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ret = OBJ_FUNCS[meta_.get_type()].deserialize(*this, buf, data_len, pos);
    }
  }
  return ret;
}

DEFINE_GET_SERIALIZE_SIZE(ObObj)
{
  int64_t len = 0;
  OB_UNIS_ADD_LEN(meta_);
  len += OBJ_FUNCS[meta_.get_type()].get_serialize_size(*this);
  return len;
}

OB_SERIALIZE_MEMBER_INHERIT(ObObjParam, ObObj, accuracy_, res_flags_);

OB_SERIALIZE_MEMBER(ParamFlag, flag_);

void ObObjParam::reset()
{
  accuracy_.reset();
  res_flags_ = 0;
  flag_.reset();
}

void ParamFlag::reset()
{
  need_to_check_type_ = true;
  need_to_check_bool_value_ = false;
  expected_bool_value_ = false;
  need_to_check_extend_type_ = true;
  is_ref_cursor_type_ = false;
}

DEF_TO_STRING(ObHexEscapeSqlStr)
{
  int64_t buf_pos = 0;
  if (buf != NULL && buf_len > 0 && !str_.empty()) {
    const char* end = str_.ptr() + str_.length();
    if (lib::is_oracle_mode()) {
      for (const char* cur = str_.ptr(); cur < end && buf_pos < buf_len; ++cur) {
        if ('\'' == *cur) {
          buf[buf_pos++] = '\'';
          if (buf_pos < buf_len) {
            buf[buf_pos++] = *cur;
          }
        } else {
          buf[buf_pos++] = *cur;
        }
      }
    } else {
      for (const char* cur = str_.ptr(); cur < end && buf_pos < buf_len; ++cur) {
        switch (*cur) {
          case '\\': {
            buf[buf_pos++] = '\\';
            if (buf_pos < buf_len) {
              buf[buf_pos++] = '\\';
            }
            break;
          }
          case '\0': {
            buf[buf_pos++] = '\\';
            if (buf_pos < buf_len) {
              buf[buf_pos++] = '0';
            }
            break;
          }
          case '\'':
          case '\"': {
            buf[buf_pos++] = '\\';
            if (buf_pos < buf_len) {
              buf[buf_pos++] = *cur;
            }
            break;
          }
          case '\n': {
            buf[buf_pos++] = '\\';
            if (buf_pos < buf_len) {
              buf[buf_pos++] = 'n';
            }
            break;
          }
          case '\r': {
            buf[buf_pos++] = '\\';
            if (buf_pos < buf_len) {
              buf[buf_pos++] = 'r';
            }
            break;
          }
          case '\t': {
            buf[buf_pos++] = '\\';
            if (buf_pos < buf_len) {
              buf[buf_pos++] = 't';
            }
            break;
          }
          default: {
            buf[buf_pos++] = *cur;
            break;
          }
        }
      }
    }
  }
  return buf_pos;
}

int64_t ObHexEscapeSqlStr::get_extra_length() const
{
  int64_t ret_length = 0;
  if (!str_.empty()) {
    const char* end = str_.ptr() + str_.length();
    if (lib::is_oracle_mode()) {
      for (const char* cur = str_.ptr(); cur < end; ++cur) {
        if ('\'' == *cur) {
          ++ret_length;
        }
      }
    } else {
      for (const char* cur = str_.ptr(); cur < end; ++cur) {
        switch (*cur) {
          case '\\':
          case '\0':
          case '\'':
          case '\"':
          case '\n':
          case '\r':
          case '\t': {
            ++ret_length;
            break;
          }
          default: {
            // do nothing
          }
        }
      }
    }
  }
  return ret_length;
}
