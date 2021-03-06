/**
 * Copyright 2020 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <iomanip>
#include "minddata/dataset/engine/cache/cache_client.h"
#include "minddata/dataset/engine/cache/cache_request.h"
#include "minddata/dataset/engine/cache/cache_service.h"
#include "minddata/dataset/engine/cache/cache_fbb.h"
#include "minddata/dataset/util/bit.h"

namespace mindspore {
namespace dataset {

// Constructor
CacheClient::CacheClient(session_id_type session_id, uint64_t cache_mem_sz, bool spill, std::string hostname,
                         int32_t port, int32_t num_workers, int32_t prefetch_size)
    : server_connection_id_(0),
      cache_mem_sz_(cache_mem_sz),
      spill_(spill),
      local_bypass_(false),
      hostname_(std::move(hostname)),
      port_(port),
      num_workers_(num_workers),
      prefetch_size_(prefetch_size) {
  cinfo_.set_session_id(session_id);
  comm_ = std::make_shared<CacheClientGreeter>(hostname_, port_, num_workers_);
}

// print method for display cache details
void CacheClient::Print(std::ostream &out) const {
  out << "  Session id: " << session_id() << "\n  Cache crc: " << cinfo_.crc()
      << "\n  Server cache id: " << server_connection_id_ << "\n  Cache mem size: " << getCacheMemSz()
      << "\n  Spilling: " << std::boolalpha << isSpill() << "\n  Hostname: " << getHostname()
      << "\n  Port: " << getPort() << "\n  Number of rpc workers: " << getNumWorkers()
      << "\n  Prefetch size: " << getPrefetchSize() << "\n  Local client support: " << std::boolalpha
      << SupportLocalClient();
}

Status CacheClient::WriteRow(const TensorRow &row, row_id_type *row_id_from_server) const {
  auto rq = std::make_shared<CacheRowRequest>(server_connection_id_, cookie(), SupportLocalClient());
  RETURN_IF_NOT_OK(rq->SerializeCacheRowRequest(this, row));
  RETURN_IF_NOT_OK(PushRequest(rq));
  RETURN_IF_NOT_OK(rq->Wait());
  if (row_id_from_server != nullptr) {
    *row_id_from_server = rq->GetRowIdAfterCache();
  }
  return Status::OK();
}

Status CacheClient::WriteBuffer(std::unique_ptr<DataBuffer> &&in) const {
  std::unique_ptr<DataBuffer> db_ptr = std::move(in);
  auto num_rows = db_ptr->NumRows();
  // We will send the requests async first on all rows and do a final wait.
  if (num_rows > 0) {
    auto arr = std::make_unique<std::shared_ptr<CacheRowRequest>[]>(num_rows);
    for (auto i = 0; i < num_rows; ++i) {
      TensorRow row;
      RETURN_IF_NOT_OK(db_ptr->PopRow(&row));
      arr[i] = std::make_shared<CacheRowRequest>(server_connection_id_, cookie(), SupportLocalClient());
      RETURN_IF_NOT_OK(arr[i]->SerializeCacheRowRequest(this, row));
      RETURN_IF_NOT_OK(PushRequest(arr[i]));
    }
    // Now we wait for them to come back
    for (auto i = 0; i < num_rows; ++i) {
      RETURN_IF_NOT_OK(arr[i]->Wait());
    }
  }
  return Status::OK();
}

Status CacheClient::GetRows(const std::vector<row_id_type> &row_id, TensorTable *out) const {
  RETURN_UNEXPECTED_IF_NULL(out);
  auto rq = std::make_shared<BatchFetchRequest>(server_connection_id_, row_id, SupportLocalClient());
  RETURN_IF_NOT_OK(PushRequest(rq));
  RETURN_IF_NOT_OK(rq->Wait());
  int64_t mem_addr;
  Status rc = rq->RestoreRows(out, comm_->SharedMemoryBaseAddr(), &mem_addr);
  // Free the memory by sending a request back to the server.
  if (mem_addr != -1) {
    auto mfree_req = std::make_shared<FreeSharedBlockRequest>(server_connection_id_, mem_addr);
    Status rc2 = PushRequest(mfree_req);
    // But we won't wait for the result for the sake of performance.
    if (rc.IsOk() && rc2.IsError()) {
      rc = rc2;
    }
  }
  return rc;
}

Status CacheClient::CreateCache(uint32_t tree_crc, bool generate_id) {
  UniqueLock lck(&mux_);
  // To create a cache, we identify ourself at the client by:
  // - the shared session id
  // - a crc for the tree nodes from the cache downward
  // Pack these 2 into a single 64 bit request id
  //
  // Consider this example:
  // tree1: tfreader --> map(decode) --> cache (session id = 1, crc = 123) --> batch
  // tree2: cifar10 --> map(rotate) --> cache (session id = 1, crc = 456) --> batch
  // These are different trees in a single session, but the user wants to share the cache.
  // This is not allowed because the data of these caches are different.
  //
  // Consider this example:
  // tree1: tfreader --> map(decode) --> cache (session id = 1, crc = 123) --> batch
  // tree2: tfreader --> map(decode) --> cache (session id = 1, crc = 123) --> map(rotate) --> batch
  // These are different trees in the same session, but the cached data is the same, so it is okay
  // to allow the sharing of this cache between these pipelines.

  // The CRC is computed by the tree prepare phase and passed to this function when creating the cache.
  // If we already have a server_connection_id_, then it means this same cache client has already been used
  // to create a cache and some other tree is trying to use the same cache.
  // That is allowed, however the crc better match!
  if (server_connection_id_) {
    if (cinfo_.crc() != tree_crc) {
      RETURN_STATUS_UNEXPECTED("Attempt to re-use a cache for a different tree!");
    }
    // Check the state of the server. For non-mappable case where there is a build phase and a fetch phase, we should
    // skip the build phase.
    lck.Unlock();  // GetStat will grab the mutex again. So unlock it to prevent deadlock.
    CacheServiceStat stat{};
    RETURN_IF_NOT_OK(GetStat(&stat));
    if (stat.cache_service_state == static_cast<uint8_t>(CacheService::State::kFetchPhase)) {
      return Status(StatusCode::kDuplicateKey, __LINE__, __FILE__, "Not an error and we should bypass the build phase");
    }
  } else {
    cinfo_.set_crc(tree_crc);  // It's really a new cache we're creating so save our crc in the client
    // Now execute the cache create request using this identifier and other configs
    CreateCacheRequest::CreateCacheFlag createFlag = CreateCacheRequest::CreateCacheFlag::kNone;
    if (spill_) {
      createFlag |= CreateCacheRequest::CreateCacheFlag::kSpillToDisk;
    }
    if (generate_id) {
      createFlag |= CreateCacheRequest::CreateCacheFlag::kGenerateRowId;
    }
    // Start the comm layer to receive reply
    RETURN_IF_NOT_OK(comm_->ServiceStart());
    // Initiate connection
    auto rq = std::make_shared<CreateCacheRequest>(cinfo_, cache_mem_sz_, createFlag);
    RETURN_IF_NOT_OK(PushRequest(rq));
    Status rc = rq->Wait();
    if (rc.IsOk() || rc.get_code() == StatusCode::kDuplicateKey) {
      std::string cookie;
      rq->ParseResult(&server_connection_id_, &cookie);
      if (rc.IsOk()) {
        // The 1st guy creating the cache will get a cookie back.
        // But this object may be shared among pipelines and we don't want
        // overwrite it.
        cookie_ = cookie;
      }
      // Attach to shared memory for local client
      RETURN_IF_NOT_OK(comm_->AttachToSharedMemory(port_, &local_bypass_));
    }
    // We are not resetting the Duplicate key return code. We are passing it back to the CacheOp. This will tell the
    // CacheOp to bypass the build phase.
    return rc;
  }
  return Status::OK();
}

Status CacheClient::PurgeCache() {
  UniqueLock lck(&mux_);
  auto rq = std::make_shared<PurgeCacheRequest>(server_connection_id_);
  RETURN_IF_NOT_OK(PushRequest(rq));
  RETURN_IF_NOT_OK(rq->Wait());
  return Status::OK();
}

Status CacheClient::DestroyCache() {
  UniqueLock lck(&mux_);
  auto rq = std::make_shared<DestroyCacheRequest>(server_connection_id_);
  RETURN_IF_NOT_OK(PushRequest(rq));
  RETURN_IF_NOT_OK(rq->Wait());
  return Status::OK();
}

Status CacheClient::GetStat(CacheServiceStat *stat) {
  SharedLock lck(&mux_);
  RETURN_UNEXPECTED_IF_NULL(stat);
  auto rq = std::make_shared<GetStatRequest>(server_connection_id_);
  RETURN_IF_NOT_OK(PushRequest(rq));
  RETURN_IF_NOT_OK(rq->Wait());
  rq->GetStat(stat);
  return Status::OK();
}

Status CacheClient::CacheSchema(const std::unordered_map<std::string, int32_t> &map) {
  SharedLock lck(&mux_);
  auto rq = std::make_shared<CacheSchemaRequest>(server_connection_id_);
  RETURN_IF_NOT_OK(rq->SerializeCacheSchemaRequest(map));
  RETURN_IF_NOT_OK(PushRequest(rq));
  RETURN_IF_NOT_OK(rq->Wait());
  return Status::OK();
}

Status CacheClient::FetchSchema(std::unordered_map<std::string, int32_t> *map) {
  SharedLock lck(&mux_);
  RETURN_UNEXPECTED_IF_NULL(map);
  auto rq = std::make_shared<FetchSchemaRequest>(server_connection_id_);
  RETURN_IF_NOT_OK(PushRequest(rq));
  RETURN_IF_NOT_OK(rq->Wait());
  *map = rq->GetColumnMap();
  return Status::OK();
}

Status CacheClient::BuildPhaseDone() const {
  SharedLock lck(&mux_);
  auto rq = std::make_shared<BuildPhaseDoneRequest>(server_connection_id_, cookie());
  RETURN_IF_NOT_OK(PushRequest(rq));
  RETURN_IF_NOT_OK(rq->Wait());
  return Status::OK();
}

Status CacheClient::PushRequest(std::shared_ptr<BaseRequest> rq) const { return comm_->HandleRequest(std::move(rq)); }
}  // namespace dataset
}  // namespace mindspore
