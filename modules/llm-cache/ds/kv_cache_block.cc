/** Copyright 2020-2023 Alibaba Group Holding Limited.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <memory>
#include <string>
#include <utility>

#include "client/client.h"
#include "common/memory/memcpy.h"
#include "common/util/logging.h"
#include "llm-cache/ds/kv_cache_block.h"

namespace vineyard {

// this function will be removed in the future
std::string KVCacheBlock::GetBitmapStr() {
  std::string result;
  const int bits = 8 * sizeof(uint64_t);
  for (int i = 0; i < this->bitmapSize; i++) {
    for (int j = bits - 1; j >= 0; --j) {
      result += (((this->bitmap[i]) >> j) & 1) ? '1' : '0';
    }
  }
  return result;
}

std::string KVCacheBlockBuilder::GetBitmapStr() {
  std::string result;
  const int bits = 8 * sizeof(uint64_t);
  for (int i = 0; i < this->bitmapSize; i++) {
    for (int j = bits - 1; j >= 0; --j) {
      result += (((this->bitmap[i]) >> j) & 1) ? '1' : '0';
    }
  }
  return result;
}

void KVCacheBlock::Construct(const ObjectMeta& meta) {
  Object::Construct(meta);

  std::string typeName = type_name<KVCacheBlock>();

  VINEYARD_ASSERT(meta.GetTypeName() == typeName,
                  "Expect typename '" + typeName + "', but got '" +
                      meta.GetTypeName() + "'");

  // TBD
  // 1. construct the keyStateTensorBuilder and valueStateTensorBuilder
  this->layer = this->meta_.GetKeyValue<int>("layer");
  for (int currentLayer = 0; currentLayer < this->layer; currentLayer++) {
    this->keyStateTensorList.push_back(
        std::dynamic_pointer_cast<KVTensor>(this->meta_.GetMember(
            "keyStateTensorBuilder_" + std::to_string(currentLayer))));
    this->valueStateTensorList.push_back(
        std::dynamic_pointer_cast<KVTensor>(this->meta_.GetMember(
            "valueStateTensorBuilder_" + std::to_string(currentLayer))));
  }
  // 2. construct the member field
  this->bitmapSize = this->meta_.GetKeyValue<int>("bitmap_size");
  VLOG(100) << "construct bitmap size:" << this->bitmapSize;
  this->bitmap = new uint64_t[this->bitmapSize];
  for (int i = 0; i < this->bitmapSize; i++) {
    this->bitmap[i] =
        this->meta_.GetKeyValue<uint64_t>("bitmap_" + std::to_string(i));
  }
  this->tensorNBytes = this->meta_.GetKeyValue<int>("tensorNBytes");
  this->blockSize = this->meta_.GetKeyValue<int>("block_size");
}

KVCacheBlock::~KVCacheBlock() { delete this->bitmap; }

KVCacheBlockBuilder::KVCacheBlockBuilder(Client& client, int tensorNBytes,
                                         int layer, int blockSize)
    : client(client) {
  this->blockSize = blockSize;
  this->bitmapSize = (blockSize + 63) / 64;
  this->bitmap = new uint64_t[this->bitmapSize];
  memset(this->bitmap, UINT8_MAX, this->bitmapSize * sizeof(uint64_t));
  std::vector<int64_t> shape = {(int64_t)(blockSize), tensorNBytes};
  for (int i = 0; i < layer; i++) {
    this->keyStateTensorBuilderList.push_back(
        std::make_shared<KVTensorBuilder>(client, shape));
    this->valueStateTensorBuilderList.push_back(
        std::make_shared<KVTensorBuilder>(client, shape));
  }
  this->tensorNBytes = tensorNBytes;
  this->layer = layer;
   LOG(INFO) << "create builder from block object, bitmap size:"
            << this->bitmapSize << " block size:" << blockSize
            << " tensorNBytes " << tensorNBytes;
}

KVCacheBlockBuilder::KVCacheBlockBuilder(
    Client& client, std::shared_ptr<KVCacheBlock> kvCacheBlock)
    : client(client) {
  this->bitmapSize = kvCacheBlock->bitmapSize;
  this->blockSize = kvCacheBlock->blockSize;
  VLOG(100) << "create builder from block object, bitmap size:"
            << this->bitmapSize << " block size:" << blockSize;
  LOG(INFO) << "create builder from block object, bitmap size:"
            << this->bitmapSize << " block size:" << blockSize
            << " kvCacheBlock " << kvCacheBlock.get()
            << " tensors " << kvCacheBlock->GetKeyTensor(0).get() 
            << " id " << kvCacheBlock->GetKeyTensor(0)->id()
            << " meta " << kvCacheBlock->GetKeyTensor(0)->meta().ToString();
  this->bitmap = new uint64_t[this->bitmapSize];
  for (int i = 0; i < this->bitmapSize; i++) {
    this->bitmap[i] = kvCacheBlock->bitmap[i];
  }
  this->tensorNBytes = kvCacheBlock->tensorNBytes;
  this->layer = kvCacheBlock->layer;
  std::vector<int64_t> shape = {(int64_t)(blockSize), this->tensorNBytes};
  for (int currentLayer = 0; currentLayer < this->layer; currentLayer++) {
    this->keyStateTensorBuilderList.push_back(
        std::make_shared<KVTensorBuilder>(client, shape));
    this->valueStateTensorBuilderList.push_back(
        std::make_shared<KVTensorBuilder>(client, shape));
  }

  for (int currentLayer = 0; currentLayer < this->layer; currentLayer++) {
    vineyard::memory::concurrent_memcpy(
        this->keyStateTensorBuilderList[currentLayer]->data(),
        kvCacheBlock->keyStateTensorList[currentLayer]->data(),
        (int64_t)(blockSize) * this->tensorNBytes);
    vineyard::memory::concurrent_memcpy(
        this->valueStateTensorBuilderList[currentLayer]->data(),
        kvCacheBlock->valueStateTensorList[currentLayer]->data(),
        (int64_t)(blockSize) * this->tensorNBytes);
  }
}

Status KVCacheBlockBuilder::Make(Client& client, TreeData* treeData,
                                 KVCacheBlockBuilder*& kvCacheBlockBuilder) {
  RETURN_ON_ASSERT(treeData != nullptr && treeData->isPtr == false);
  ObjectID blockObjectID = treeData->builderObjectID;

  std::shared_ptr<KVCacheBlock> blockObject;
  RETURN_ON_ERROR(client.FetchAndGetObject(blockObjectID, blockObject));
  kvCacheBlockBuilder = new KVCacheBlockBuilder(client, blockObject);
  LOG(INFO) << "Make:"
            << " treeData->builderObjectID " << treeData->builderObjectID
            << " blockObjectID " << blockObjectID
            << " kvCacheBlock " << blockObject.get()
            << " tensors " << blockObject->GetKeyTensor(0).get() 
            << " id " << blockObject->GetKeyTensor(0)->id()
            << " meta " << blockObject->GetKeyTensor(0)->meta().ToString();
  if (blockObjectID != blockObject->id()) {
    // If the object is migrated, we should delete the copied object.
    Status status = client.DelData(blockObject->id());
    if (!status.ok()) {
      LOG(ERROR) << "Delete object failed: " << status.ToString()
                 << " It may cause memory leak.";
    }
  }
  return Status::OK();
}

Status KVCacheBlockBuilder::Query(
    int index, std::vector<std::pair<LLMKV, LLMKV>>& kvState) {
  RETURN_ON_ASSERT((index >= 0 && index < this->blockSize),
                   "Index out of range: " + std::to_string(index));
  RETURN_ON_ASSERT(static_cast<int>(kvState.size()) == this->layer,
                   "The size of kvState is not equal to layer");
  for (int currentLayer = 0; currentLayer < this->layer; currentLayer++) {
    LLMKV& keyState = kvState[currentLayer].first;
    LLMKV& valueState = kvState[currentLayer].second;
    LOG(INFO) 
      << " Query: key state "
      << (keyState.data == nullptr? "null" : std::to_string(reinterpret_cast<uintptr_t>(keyState.data))) 
      << " value state "
      << (valueState.data == nullptr? "null" : std::to_string(reinterpret_cast<uintptr_t>(valueState.data)));

    // VINEYARD_ASSERT(keyState.data == nullptr && valueState.data == nullptr);
    keyState.data =
        keyStateTensorBuilderList[currentLayer]->data() + index * tensorNBytes;
    keyState.length = tensorNBytes;
    valueState.data = valueStateTensorBuilderList[currentLayer]->data() +
                      index * tensorNBytes;
    valueState.length = tensorNBytes;
  }
  return Status::OK();
}

int KVCacheBlockBuilder::FindEmptySlot() {
  for (int i = 0; i < this->bitmapSize; i++) {
    if (this->bitmap[i] != 0) {
      int index = ffsll(this->bitmap[i]) - 1;
      return index + i * 64;
    }
  }
  return -1;
}

bool KVCacheBlockBuilder::IsFull() {
  int left = this->blockSize;
  for (int i = 0; i < this->bitmapSize; i++) {
    if (this->bitmap[i] != 0 && ffsll(this->bitmap[i]) - 1 < left) {
      return false;
    }
    left -= sizeof(uint64_t) * 8;
  }
  return true;
}

Status KVCacheBlockBuilder::Update(
    const std::vector<std::pair<LLMKV, LLMKV>>& kvState, OffsetData* data) {
  int index = this->FindEmptySlot();
  RETURN_ON_ASSERT((index >= 0 && index < this->blockSize),
                   "Index out of range: " + std::to_string(index));
  RETURN_ON_ASSERT(kvState.size() == static_cast<size_t>(this->layer),
                   "The size of kvState is not equal to layer");

  for (int currentLayer = 0; currentLayer < this->layer; currentLayer++) {
    LLMKV keyState = kvState[currentLayer].first;
    LLMKV valueState = kvState[currentLayer].second;
    RETURN_ON_ASSERT((keyState.length == (size_t) this->tensorNBytes &&
                      valueState.length == (size_t) this->tensorNBytes));

    uint8_t* keyData = keyStateTensorBuilderList[currentLayer]->data();
    uint8_t* valueData = valueStateTensorBuilderList[currentLayer]->data();
    vineyard::memory::concurrent_memcpy(keyData + index * this->tensorNBytes,
                                        keyState.data, this->tensorNBytes);
    vineyard::memory::concurrent_memcpy(valueData + index * this->tensorNBytes,
                                        valueState.data, this->tensorNBytes);
  }
  data->offset = index;

  ACQUIRE_BIT_RESOURCE(this->bitmap[index / 64], index % 64);
  return Status::OK();
}

int16_t KVCacheBlockBuilder::Split(KVCacheBlockBuilder* child, int index) {
  // Child builder must be empty.
  int childIndex = child->FindEmptySlot();
  for (int currentLayer = 0; currentLayer < this->layer; currentLayer++) {
    std::shared_ptr<KVTensorBuilder> keyStateTensorBuilder =
        keyStateTensorBuilderList[currentLayer];
    std::shared_ptr<KVTensorBuilder> valueStateTensorBuilder =
        valueStateTensorBuilderList[currentLayer];
    std::shared_ptr<KVTensorBuilder> childKeyStateTensorBuilder =
        child->keyStateTensorBuilderList[currentLayer];
    std::shared_ptr<KVTensorBuilder> childValueStateTensorBuilder =
        child->valueStateTensorBuilderList[currentLayer];

    uint8_t* keyState =
        keyStateTensorBuilder->data() + index * this->tensorNBytes;
    uint8_t* valueState =
        valueStateTensorBuilder->data() + index * this->tensorNBytes;
    uint8_t* childKeyState =
        childKeyStateTensorBuilder->data() + childIndex * this->tensorNBytes;
    uint8_t* childValueState =
        childValueStateTensorBuilder->data() + childIndex * this->tensorNBytes;

    vineyard::memory::concurrent_memcpy(childKeyState, keyState,
                                        this->tensorNBytes);
    vineyard::memory::concurrent_memcpy(childValueState, valueState,
                                        this->tensorNBytes);
  }
  ACQUIRE_BIT_RESOURCE(child->bitmap[childIndex / 64], childIndex % 64);
  FREE_BIT_RESOURCE(this->bitmap[index / 64], index % 64);
  return childIndex;
}

Status KVCacheBlockBuilder::Build(Client& client) { return Status::OK(); }

std::shared_ptr<Object> KVCacheBlockBuilder::_Seal(Client& client) {
  VINEYARD_CHECK_OK(this->Build(client));

  std::shared_ptr<KVCacheBlock> kvCacheBlock = std::make_shared<KVCacheBlock>();

  // 1. seal keyStateTensorBuilder and valueStateTensorBuilder
  for (int currentLayer = 0; currentLayer < this->layer; currentLayer++) {
    kvCacheBlock->meta_.AddMember(
        "keyStateTensorBuilder_" + std::to_string(currentLayer),
        keyStateTensorBuilderList[currentLayer]->Seal(client));
    kvCacheBlock->meta_.AddMember(
        "valueStateTensorBuilder_" + std::to_string(currentLayer),
        valueStateTensorBuilderList[currentLayer]->Seal(client));
  }

  // 2. store the member field to meta
  kvCacheBlock->meta_.AddKeyValue("bitmap_size", this->bitmapSize);
  for (int i = 0; i < this->bitmapSize; i++) {
    kvCacheBlock->meta_.AddKeyValue("bitmap_" + std::to_string(i),
                                    this->bitmap[i]);
  }

  kvCacheBlock->meta_.AddKeyValue("block_size", this->blockSize);
  kvCacheBlock->meta_.AddKeyValue("tensorNBytes", this->tensorNBytes);
  kvCacheBlock->meta_.AddKeyValue("layer", this->layer);
  // 3. set the object type to meta
  kvCacheBlock->meta_.SetTypeName(type_name<KVCacheBlock>());

  VINEYARD_CHECK_OK(
      client.CreateMetaData(kvCacheBlock->meta_, kvCacheBlock->id_));
  this->set_sealed(true);
  return kvCacheBlock;
}

void KVCacheBlockBuilder::PrintKVCacheBlock() {
  LOG(INFO) << "builder:" << this;
  for (int i = 0; i < this->blockSize; i++) {
    LOG(INFO) << "index:" << i << " bitmap:" << this->GetBitmapStr();
  }

  for (int currentLayer = 0; currentLayer < this->layer; currentLayer++) {
    LOG(INFO) << "layer:" << currentLayer;
    for (int i = 0; i < this->blockSize; i++) {
      LOG(INFO) << "index:" << i;
      uint8_t* key_state_data = keyStateTensorBuilderList[currentLayer]->data();
      uint8_t* value_state_data =
          valueStateTensorBuilderList[currentLayer]->data();
      // print the first tensorNBytes bytes
      std::string keyState = "";
      std::string valueState = "";
      for (int j = 0; j < this->tensorNBytes; j++) {
        keyState += std::to_string(key_state_data[i * tensorNBytes + j]) + " ";
        valueState +=
            std::to_string(value_state_data[i * tensorNBytes + j]) + " ";
      }
      LOG(INFO) << "keyState:" << keyState;
      LOG(INFO) << "valueState:" << valueState;
    }
  }

  LOG(INFO) << "==========================";
}

KVCacheBlockBuilder::~KVCacheBlockBuilder() { delete this->bitmap; }

}  // namespace vineyard
