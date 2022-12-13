/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clientSml.h"

#define OTD_JSON_SUB_FIELDS_NUM 2
#define OTD_JSON_FIELDS_NUM     4
#define JSON_METERS_NAME "__JM"


int32_t is_same_child_table_json(const void *a, const void *b){
  return (cJSON_Compare((const cJSON *)a, (const cJSON *)b, true)) ? 0 : 1;
}

static inline int32_t smlParseMetricFromJSON(SSmlHandle *info, cJSON *metric, SSmlLineInfo *elements) {
  elements->measureLen = strlen(metric->valuestring);
  if (IS_INVALID_TABLE_LEN(elements->measureLen)) {
    uError("OTD:0x%" PRIx64 " Metric lenght is 0 or large than 192", info->id);
    return TSDB_CODE_TSC_INVALID_TABLE_ID_LENGTH;
  }

  elements->measure = metric->valuestring;
  return TSDB_CODE_SUCCESS;
}

static int64_t smlParseTSFromJSONObj(SSmlHandle *info, cJSON *root, int32_t toPrecision) {
  int32_t size = cJSON_GetArraySize(root);
  if (unlikely(size != OTD_JSON_SUB_FIELDS_NUM)) {
    smlBuildInvalidDataMsg(&info->msgBuf, "invalidate json", NULL);
    return -1;
  }

  cJSON *value = cJSON_GetObjectItem(root, "value");
  if (unlikely(!cJSON_IsNumber(value))) {
    smlBuildInvalidDataMsg(&info->msgBuf, "invalidate json", NULL);
    return -1;
  }

  cJSON *type = cJSON_GetObjectItem(root, "type");
  if (unlikely(!cJSON_IsString(type))) {
    smlBuildInvalidDataMsg(&info->msgBuf, "invalidate json", NULL);
    return -1;
  }

  double timeDouble = value->valuedouble;
  if (unlikely(smlDoubleToInt64OverFlow(timeDouble))) {
    smlBuildInvalidDataMsg(&info->msgBuf, "timestamp is too large", NULL);
    return -1;
  }

  if (timeDouble == 0) {
    return taosGetTimestampNs()/smlFactorNS[toPrecision];
  }

  if (timeDouble < 0) {
    return timeDouble;
  }

  int64_t tsInt64 = timeDouble;
  size_t typeLen = strlen(type->valuestring);
  if (typeLen == 1 && (type->valuestring[0] == 's' || type->valuestring[0] == 'S')) {
    // seconds
    int8_t fromPrecision = TSDB_TIME_PRECISION_SECONDS;
    if(smlFactorS[toPrecision] < INT64_MAX / tsInt64){
      return tsInt64 * smlFactorS[toPrecision];
    }
    return -1;
  } else if (typeLen == 2 && (type->valuestring[1] == 's' || type->valuestring[1] == 'S')) {
    switch (type->valuestring[0]) {
      case 'm':
      case 'M':
        // milliseconds
        return convertTimePrecision(tsInt64, TSDB_TIME_PRECISION_MILLI, toPrecision);
        break;
      case 'u':
      case 'U':
        // microseconds
        return convertTimePrecision(tsInt64, TSDB_TIME_PRECISION_MICRO, toPrecision);
        break;
      case 'n':
      case 'N':
        return convertTimePrecision(tsInt64, TSDB_TIME_PRECISION_NANO, toPrecision);
        break;
      default:
        return -1;
    }
  } else {
    return -1;
  }
}

static inline uint8_t smlGetTimestampLen(int64_t num) {
  uint8_t len = 0;
  while ((num /= 10) != 0) {
    len++;
  }
  len++;
  return len;
}

static int64_t smlParseTSFromJSON(SSmlHandle *info, cJSON *timestamp) {
  // Timestamp must be the first KV to parse
  int32_t toPrecision = info->currSTableMeta ? info->currSTableMeta->tableInfo.precision : TSDB_TIME_PRECISION_NANO;
  if (cJSON_IsNumber(timestamp)) {
    // timestamp value 0 indicates current system time
    double timeDouble = timestamp->valuedouble;
    if (unlikely(smlDoubleToInt64OverFlow(timeDouble))) {
      smlBuildInvalidDataMsg(&info->msgBuf, "timestamp is too large", NULL);
      return -1;
    }

    if (unlikely(timeDouble < 0)) {
      smlBuildInvalidDataMsg(&info->msgBuf,
                             "timestamp is negative", NULL);
      return timeDouble;
    }else if (unlikely(timeDouble == 0)) {
      return taosGetTimestampNs()/smlFactorNS[toPrecision];
    }

    uint8_t tsLen = smlGetTimestampLen((int64_t)timeDouble);

    int8_t fromPrecision = smlGetTsTypeByLen(tsLen);
    if (unlikely(fromPrecision == -1)) {
      smlBuildInvalidDataMsg(&info->msgBuf,
                             "timestamp precision can only be seconds(10 digits) or milli seconds(13 digits)", NULL);
      return -1;
    }
    int64_t tsInt64 = timeDouble;
    if(fromPrecision == TSDB_TIME_PRECISION_SECONDS){
      if(smlFactorS[toPrecision] < INT64_MAX / tsInt64){
        return tsInt64 * smlFactorS[toPrecision];
      }
      return -1;
    }else{
      return convertTimePrecision(timeDouble, fromPrecision, toPrecision);
    }
  } else if (cJSON_IsObject(timestamp)) {
    return smlParseTSFromJSONObj(info, timestamp, toPrecision);
  } else {
    smlBuildInvalidDataMsg(&info->msgBuf,
                           "invalidate json", NULL);
    return -1;
  }
}

static int32_t smlConvertJSONBool(SSmlKv *pVal, char *typeStr, cJSON *value) {
  if (strcasecmp(typeStr, "bool") != 0) {
    uError("OTD:invalid type(%s) for JSON Bool", typeStr);
    return TSDB_CODE_TSC_INVALID_JSON_TYPE;
  }
  pVal->type = TSDB_DATA_TYPE_BOOL;
  pVal->length = (int16_t)tDataTypes[pVal->type].bytes;
  pVal->i = value->valueint;

  return TSDB_CODE_SUCCESS;
}

static int32_t smlConvertJSONNumber(SSmlKv *pVal, char *typeStr, cJSON *value) {
  // tinyint
  if (strcasecmp(typeStr, "i8") == 0 || strcasecmp(typeStr, "tinyint") == 0) {
    if (!IS_VALID_TINYINT(value->valuedouble)) {
      uError("OTD:JSON value(%f) cannot fit in type(tinyint)", value->valuedouble);
      return TSDB_CODE_TSC_VALUE_OUT_OF_RANGE;
    }
    pVal->type = TSDB_DATA_TYPE_TINYINT;
    pVal->length = (int16_t)tDataTypes[pVal->type].bytes;
    pVal->i = value->valuedouble;
    return TSDB_CODE_SUCCESS;
  }
  // smallint
  if (strcasecmp(typeStr, "i16") == 0 || strcasecmp(typeStr, "smallint") == 0) {
    if (!IS_VALID_SMALLINT(value->valuedouble)) {
      uError("OTD:JSON value(%f) cannot fit in type(smallint)", value->valuedouble);
      return TSDB_CODE_TSC_VALUE_OUT_OF_RANGE;
    }
    pVal->type = TSDB_DATA_TYPE_SMALLINT;
    pVal->length = (int16_t)tDataTypes[pVal->type].bytes;
    pVal->i = value->valuedouble;
    return TSDB_CODE_SUCCESS;
  }
  // int
  if (strcasecmp(typeStr, "i32") == 0 || strcasecmp(typeStr, "int") == 0) {
    if (!IS_VALID_INT(value->valuedouble)) {
      uError("OTD:JSON value(%f) cannot fit in type(int)", value->valuedouble);
      return TSDB_CODE_TSC_VALUE_OUT_OF_RANGE;
    }
    pVal->type = TSDB_DATA_TYPE_INT;
    pVal->length = (int16_t)tDataTypes[pVal->type].bytes;
    pVal->i = value->valuedouble;
    return TSDB_CODE_SUCCESS;
  }
  // bigint
  if (strcasecmp(typeStr, "i64") == 0 || strcasecmp(typeStr, "bigint") == 0) {
    pVal->type = TSDB_DATA_TYPE_BIGINT;
    pVal->length = (int16_t)tDataTypes[pVal->type].bytes;
    if (value->valuedouble >= (double)INT64_MAX) {
      pVal->i = INT64_MAX;
    } else if (value->valuedouble <= (double)INT64_MIN) {
      pVal->i = INT64_MIN;
    } else {
      pVal->i = value->valuedouble;
    }
    return TSDB_CODE_SUCCESS;
  }
  // float
  if (strcasecmp(typeStr, "f32") == 0 || strcasecmp(typeStr, "float") == 0) {
    if (!IS_VALID_FLOAT(value->valuedouble)) {
      uError("OTD:JSON value(%f) cannot fit in type(float)", value->valuedouble);
      return TSDB_CODE_TSC_VALUE_OUT_OF_RANGE;
    }
    pVal->type = TSDB_DATA_TYPE_FLOAT;
    pVal->length = (int16_t)tDataTypes[pVal->type].bytes;
    pVal->f = value->valuedouble;
    return TSDB_CODE_SUCCESS;
  }
  // double
  if (strcasecmp(typeStr, "f64") == 0 || strcasecmp(typeStr, "double") == 0) {
    pVal->type = TSDB_DATA_TYPE_DOUBLE;
    pVal->length = (int16_t)tDataTypes[pVal->type].bytes;
    pVal->d = value->valuedouble;
    return TSDB_CODE_SUCCESS;
  }

  // if reach here means type is unsupported
  uError("OTD:invalid type(%s) for JSON Number", typeStr);
  return TSDB_CODE_TSC_INVALID_JSON_TYPE;
}

static int32_t smlConvertJSONString(SSmlKv *pVal, char *typeStr, cJSON *value) {
  if (strcasecmp(typeStr, "binary") == 0) {
    pVal->type = TSDB_DATA_TYPE_BINARY;
  } else if (strcasecmp(typeStr, "nchar") == 0) {
    pVal->type = TSDB_DATA_TYPE_NCHAR;
  } else {
    uError("OTD:invalid type(%s) for JSON String", typeStr);
    return TSDB_CODE_TSC_INVALID_JSON_TYPE;
  }
  pVal->length = (int16_t)strlen(value->valuestring);

  if (pVal->type == TSDB_DATA_TYPE_BINARY && pVal->length > TSDB_MAX_BINARY_LEN - VARSTR_HEADER_SIZE) {
    return TSDB_CODE_PAR_INVALID_VAR_COLUMN_LEN;
  }
  if (pVal->type == TSDB_DATA_TYPE_NCHAR &&
      pVal->length > (TSDB_MAX_NCHAR_LEN - VARSTR_HEADER_SIZE) / TSDB_NCHAR_SIZE) {
    return TSDB_CODE_PAR_INVALID_VAR_COLUMN_LEN;
  }

  pVal->value = value->valuestring;
  return TSDB_CODE_SUCCESS;
}

static int32_t smlParseValueFromJSONObj(cJSON *root, SSmlKv *kv) {
  int32_t ret = TSDB_CODE_SUCCESS;
  int32_t size = cJSON_GetArraySize(root);

  if (size != OTD_JSON_SUB_FIELDS_NUM) {
    return TSDB_CODE_TSC_INVALID_JSON;
  }

  cJSON *value = cJSON_GetObjectItem(root, "value");
  if (value == NULL) {
    return TSDB_CODE_TSC_INVALID_JSON;
  }

  cJSON *type = cJSON_GetObjectItem(root, "type");
  if (!cJSON_IsString(type)) {
    return TSDB_CODE_TSC_INVALID_JSON;
  }

  switch (value->type) {
    case cJSON_True:
    case cJSON_False: {
      ret = smlConvertJSONBool(kv, type->valuestring, value);
      if (ret != TSDB_CODE_SUCCESS) {
        return ret;
      }
      break;
    }
    case cJSON_Number: {
      ret = smlConvertJSONNumber(kv, type->valuestring, value);
      if (ret != TSDB_CODE_SUCCESS) {
        return ret;
      }
      break;
    }
    case cJSON_String: {
      ret = smlConvertJSONString(kv, type->valuestring, value);
      if (ret != TSDB_CODE_SUCCESS) {
        return ret;
      }
      break;
    }
    default:
      return TSDB_CODE_TSC_INVALID_JSON_TYPE;
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t smlParseValueFromJSON(cJSON *root, SSmlKv *kv) {
  switch (root->type) {
    case cJSON_True:
    case cJSON_False: {
      kv->type = TSDB_DATA_TYPE_BOOL;
      kv->length = (int16_t)tDataTypes[kv->type].bytes;
      kv->i = root->valueint;
      break;
    }
    case cJSON_Number: {
      kv->type = TSDB_DATA_TYPE_DOUBLE;
      kv->length = (int16_t)tDataTypes[kv->type].bytes;
      kv->d = root->valuedouble;
      break;
    }
    case cJSON_String: {
      /* set default JSON type to binary/nchar according to
       * user configured parameter tsDefaultJSONStrType
       */

      char *tsDefaultJSONStrType = "nchar";  // todo
      smlConvertJSONString(kv, tsDefaultJSONStrType, root);
      break;
    }
    case cJSON_Object: {
      int32_t ret = smlParseValueFromJSONObj(root, kv);
      if (ret != TSDB_CODE_SUCCESS) {
        uError("OTD:Failed to parse value from JSON Obj");
        return ret;
      }
      break;
    }
    default:
      return TSDB_CODE_TSC_INVALID_JSON;
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t smlParseTagsFromJSON(SSmlHandle *info, cJSON *tags, SSmlLineInfo *elements) {
  int32_t ret = TSDB_CODE_SUCCESS;

  // add measure to tags to identify one child table
//  cJSON *cMeasure = cJSON_AddStringToObject(tags, JSON_METERS_NAME, elements->measure);
//  if(unlikely(cMeasure == NULL)){
//    return TSDB_CODE_TSC_INVALID_JSON;
//  }
  elements->tags = (char*)tags;
  if(is_same_child_table_json(elements->tags, info->preLine.tags) == 0){
//    cJSON_DeleteItemFromObjectCaseSensitive(tags, JSON_METERS_NAME);
    return TSDB_CODE_SUCCESS;
  }
//  cJSON_DeleteItemFromObjectCaseSensitive(tags, JSON_METERS_NAME);

  bool isSameMeasure = IS_SAME_SUPER_TABLE;

  int     cnt = 0;
  SArray *preLineKV = info->preLineTagKV;
  bool    isSuperKVInit = true;
  SArray *superKV = NULL;
  if(info->dataFormat){
    if(unlikely(!isSameMeasure)){
      SSmlSTableMeta *sMeta = (SSmlSTableMeta *)nodeListGet(info->superTables, elements->measure, elements->measureLen, NULL);

      if(unlikely(sMeta == NULL)){
        sMeta = smlBuildSTableMeta(info->dataFormat);
        STableMeta * pTableMeta = smlGetMeta(info, elements->measure, elements->measureLen);
        sMeta->tableMeta = pTableMeta;
        if(pTableMeta == NULL){
          info->dataFormat = false;
          info->reRun      = true;
          return TSDB_CODE_SUCCESS;
        }
        nodeListSet(&info->superTables, elements->measure, elements->measureLen, sMeta, NULL);
      }
      info->currSTableMeta = sMeta->tableMeta;
      superKV = sMeta->tags;

      if(unlikely(taosArrayGetSize(superKV) == 0)){
        isSuperKVInit = false;
      }
      taosArraySetSize(preLineKV, 0);
    }
  }else{
    taosArraySetSize(preLineKV, 0);
  }

  int32_t tagNum = cJSON_GetArraySize(tags);
  for (int32_t i = 0; i < tagNum; ++i) {
    cJSON *tag = cJSON_GetArrayItem(tags, i);
    if (unlikely(tag == NULL)) {
      return TSDB_CODE_TSC_INVALID_JSON;
    }
//    if(unlikely(tag == cMeasure)) continue;
    size_t keyLen = strlen(tag->string);
    if (unlikely(IS_INVALID_COL_LEN(keyLen))) {
      uError("OTD:Tag key length is 0 or too large than 64");
      return TSDB_CODE_TSC_INVALID_COLUMN_LENGTH;
    }

    // add kv to SSmlKv
    SSmlKv kv ={.key = tag->string, .keyLen = keyLen};
    // value
    ret = smlParseValueFromJSON(tag, &kv);
    if (unlikely(ret != TSDB_CODE_SUCCESS)) {
      return ret;
    }

    if(info->dataFormat){
      if(unlikely(cnt + 1 > info->currSTableMeta->tableInfo.numOfTags)){
        info->dataFormat = false;
        info->reRun      = true;
        return TSDB_CODE_SUCCESS;
      }

      if(isSameMeasure){
        if(unlikely(cnt >= taosArrayGetSize(preLineKV))) {
          info->dataFormat = false;
          info->reRun      = true;
          return TSDB_CODE_SUCCESS;
        }
        SSmlKv *preKV = (SSmlKv *)taosArrayGet(preLineKV, cnt);
        if(unlikely(kv.length > preKV->length)){
          preKV->length = kv.length;
          SSmlSTableMeta *tableMeta = (SSmlSTableMeta *)nodeListGet(info->superTables, elements->measure, elements->measureLen, NULL);
          ASSERT(tableMeta != NULL);

          SSmlKv *oldKV = (SSmlKv *)taosArrayGet(tableMeta->tags, cnt);
          oldKV->length = kv.length;
          info->needModifySchema = true;
        }
        if(unlikely(!IS_SAME_KEY)){
          info->dataFormat = false;
          info->reRun      = true;
          return TSDB_CODE_SUCCESS;
        }
      }else{
        if(isSuperKVInit){
          if(unlikely(cnt >= taosArrayGetSize(superKV))) {
            info->dataFormat = false;
            info->reRun      = true;
            return TSDB_CODE_SUCCESS;
          }
          SSmlKv *preKV = (SSmlKv *)taosArrayGet(superKV, cnt);
          if(unlikely(kv.length > preKV->length)) {
            preKV->length = kv.length;
          }else{
            kv.length = preKV->length;
          }
          info->needModifySchema = true;

          if(unlikely(!IS_SAME_KEY)){
            info->dataFormat = false;
            info->reRun      = true;
            return TSDB_CODE_SUCCESS;
          }
        }else{
          taosArrayPush(superKV, &kv);
        }
        taosArrayPush(preLineKV, &kv);
      }
    }else{
      taosArrayPush(preLineKV, &kv);
    }
    cnt++;
  }

  SSmlTableInfo *tinfo = (SSmlTableInfo *)nodeListGet(info->childTables, elements->tags, POINTER_BYTES, is_same_child_table_json);
  if (unlikely(tinfo == NULL)) {
    tinfo = smlBuildTableInfo(1, elements->measure, elements->measureLen);
    if (unlikely(!tinfo)) {
      return TSDB_CODE_OUT_OF_MEMORY;
    }
    tinfo->tags = taosArrayDup(preLineKV, NULL);

    smlSetCTableName(tinfo);
    if (info->dataFormat) {
      info->currSTableMeta->uid = tinfo->uid;
      tinfo->tableDataCtx = smlInitTableDataCtx(info->pQuery, info->currSTableMeta);
      if (tinfo->tableDataCtx == NULL) {
        smlBuildInvalidDataMsg(&info->msgBuf, "smlInitTableDataCtx error", NULL);
        return TSDB_CODE_SML_INVALID_DATA;
      }
    }

    nodeListSet(&info->childTables, tags, POINTER_BYTES, tinfo, is_same_child_table_json);
  }
  if (info->dataFormat) info->currTableDataCtx = tinfo->tableDataCtx;

  return ret;
}

const char *jsonName[OTD_JSON_FIELDS_NUM] = {"metric", "timestamp", "value", "tags"};
static int32_t smlGetJsonElements(cJSON *root, cJSON ***marks){
  cJSON *child = root->child;

  for (int i = 0; i < OTD_JSON_FIELDS_NUM; ++i) {
    while(child != NULL)
    {
      if(strcasecmp(child->string, jsonName[i]) == 0){
        *marks[i] = child;
        break;
      }
      child = child->next;
    }
    if(*marks[i] == NULL){
      uError("smlGetJsonElements error, not find mark:%s", jsonName[i]);
      return -1;
    }
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t smlParseJSONString(SSmlHandle *info, cJSON *root, SSmlLineInfo *elements) {
  int32_t ret = TSDB_CODE_SUCCESS;

  cJSON *metricJson = NULL;
  cJSON *tsJson = NULL;
  cJSON *valueJson = NULL;
  cJSON *tagsJson = NULL;

  cJSON **marks[OTD_JSON_FIELDS_NUM] = {&metricJson, &tsJson, &valueJson, &tagsJson};
  ret = smlGetJsonElements(root, marks);
  if (unlikely(ret != TSDB_CODE_SUCCESS)) {
    return ret;
  }

  // Parse metric
  ret = smlParseMetricFromJSON(info, metricJson, elements);
  if (unlikely(ret != TSDB_CODE_SUCCESS)) {
    uError("OTD:0x%" PRIx64 " Unable to parse metric from JSON payload", info->id);
    return ret;
  }

  // Parse metric value
  SSmlKv kv = {.key = VALUE, .keyLen = VALUE_LEN};
  ret = smlParseValueFromJSON(valueJson, &kv);
  if (unlikely(ret)) {
    uError("OTD:0x%" PRIx64 " Unable to parse metric value from JSON payload", info->id);
    return ret;
  }

  // Parse tags
  ret = smlParseTagsFromJSON(info, tagsJson, elements);
  if (unlikely(ret)) {
    uError("OTD:0x%" PRIx64 " Unable to parse tags from JSON payload", info->id);
    return ret;
  }

  if(unlikely(info->reRun)){
    return TSDB_CODE_SUCCESS;
  }

  // Parse timestamp
  // notice!!! put ts back to tag to ensure get meta->precision
  int64_t ts = smlParseTSFromJSON(info, tsJson);
  if (unlikely(ts < 0)) {
    uError("OTD:0x%" PRIx64 " Unable to parse timestamp from JSON payload", info->id);
    return TSDB_CODE_INVALID_TIMESTAMP;
  }
  SSmlKv kvTs = { .key = TS, .keyLen = TS_LEN, .type = TSDB_DATA_TYPE_TIMESTAMP, .i = ts, .length = (size_t)tDataTypes[TSDB_DATA_TYPE_TIMESTAMP].bytes};

  if(info->dataFormat){
    ret = smlBuildCol(info->currTableDataCtx, info->currSTableMeta->schema, &kvTs, 0);
    if(ret == TSDB_CODE_SUCCESS){
      ret = smlBuildCol(info->currTableDataCtx, info->currSTableMeta->schema, &kv, 1);
    }
    if(ret == TSDB_CODE_SUCCESS){
      ret = smlBuildRow(info->currTableDataCtx);
    }
    if (unlikely(ret != TSDB_CODE_SUCCESS)) {
      smlBuildInvalidDataMsg(&info->msgBuf, "smlBuildCol error", NULL);
      return ret;
    }
  }else{
    if(elements->colArray == NULL){
      elements->colArray = taosArrayInit(16, sizeof(SSmlKv));
    }
    taosArrayPush(elements->colArray, &kvTs);
    taosArrayPush(elements->colArray, &kv);
  }
  info->preLine = *elements;

  return TSDB_CODE_SUCCESS;
}

int32_t smlParseJSON(SSmlHandle *info, char *payload) {
  int32_t payloadNum = 0;
  int32_t ret = TSDB_CODE_SUCCESS;

  if (unlikely(payload == NULL)) {
    uError("SML:0x%" PRIx64 " empty JSON Payload", info->id);
    return TSDB_CODE_TSC_INVALID_JSON;
  }

  info->root = cJSON_Parse(payload);
  if (unlikely(info->root == NULL)) {
    uError("SML:0x%" PRIx64 " parse json failed:%s", info->id, payload);
    return TSDB_CODE_TSC_INVALID_JSON;
  }

  // multiple data points must be sent in JSON array
  if (cJSON_IsArray(info->root)) {
    payloadNum = cJSON_GetArraySize(info->root);
  } else if (cJSON_IsObject(info->root)) {
    payloadNum = 1;
  } else {
    uError("SML:0x%" PRIx64 " Invalid JSON Payload", info->id);
    return TSDB_CODE_TSC_INVALID_JSON;
  }

  cJSON *head = (payloadNum == 1 && cJSON_IsObject(info->root)) ? info->root : info->root->child;

  int cnt = 0;
  cJSON *dataPoint = head;
  while (dataPoint) {
    if(info->dataFormat) {
      SSmlLineInfo element = {0};
      ret = smlParseJSONString(info, dataPoint, &element);
    }else{
      ret = smlParseJSONString(info, dataPoint, info->lines + cnt);
    }
    if (unlikely(ret != TSDB_CODE_SUCCESS)) {
      uError("SML:0x%" PRIx64 " Invalid JSON Payload", info->id);
      return ret;
    }

    if(unlikely(info->reRun)){
      cnt = 0;
      dataPoint = head;
      info->lineNum = payloadNum;
      ret = smlClearForRerun(info);
      if(ret != TSDB_CODE_SUCCESS){
        return ret;
      }
      continue;
    }
    cnt++;
    dataPoint = dataPoint->next;
  }

  return TSDB_CODE_SUCCESS;
}

int32_t smlParseJSONOld(SSmlHandle *info, char *payload) {
  int32_t payloadNum = 0;
  int32_t ret = TSDB_CODE_SUCCESS;

  if (unlikely(payload == NULL)) {
    uError("SML:0x%" PRIx64 " empty JSON Payload", info->id);
    return TSDB_CODE_TSC_INVALID_JSON;
  }

  info->root = cJSON_Parse(payload);
  if (unlikely(info->root == NULL)) {
    uError("SML:0x%" PRIx64 " parse json failed:%s", info->id, payload);
    return TSDB_CODE_TSC_INVALID_JSON;
  }

  // multiple data points must be sent in JSON array
  if (cJSON_IsArray(info->root)) {
    payloadNum = cJSON_GetArraySize(info->root);
  } else if (cJSON_IsObject(info->root)) {
    payloadNum = 1;
  } else {
    uError("SML:0x%" PRIx64 " Invalid JSON Payload", info->id);
    return TSDB_CODE_TSC_INVALID_JSON;
  }

  cJSON *head = (payloadNum == 1 && cJSON_IsObject(info->root)) ? info->root : info->root->child;

  int cnt = 0;
  cJSON *dataPoint = head;
  while (dataPoint) {
    if(info->dataFormat) {
      SSmlLineInfo element = {0};
      ret = smlParseJSONString(info, dataPoint, &element);
    }else{
      ret = smlParseJSONString(info, dataPoint, info->lines + cnt);
    }
    if (unlikely(ret != TSDB_CODE_SUCCESS)) {
      uError("SML:0x%" PRIx64 " Invalid JSON Payload", info->id);
      return ret;
    }

    if(unlikely(info->reRun)){
      cnt = 0;
      dataPoint = head;
      info->lineNum = payloadNum;
      ret = smlClearForRerun(info);
      if(ret != TSDB_CODE_SUCCESS){
        return ret;
      }
      continue;
    }
    cnt++;
    dataPoint = dataPoint->next;
  }

  return TSDB_CODE_SUCCESS;
}