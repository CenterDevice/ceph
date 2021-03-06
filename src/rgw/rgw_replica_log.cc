// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 * Copyright 2013 Inktank
 */

#include "common/ceph_json.h"

#include "rgw_replica_log.h"
#include "cls/replica_log/cls_replica_log_client.h"
#include "cls/rgw/cls_rgw_client.h"
#include "rgw_rados.h"

#define dout_subsys ceph_subsys_rgw

void RGWReplicaBounds::dump(Formatter *f) const
{
  encode_json("marker", marker, f);
  encode_json("oldest_time", oldest_time, f);
  encode_json("markers", markers, f);
}

void RGWReplicaBounds::decode_json(JSONObj *obj) {
  JSONDecoder::decode_json("marker", marker, obj);
  JSONDecoder::decode_json("oldest_time", oldest_time, obj);
  JSONDecoder::decode_json("markers", markers, obj);
}

RGWReplicaLogger::RGWReplicaLogger(RGWRados *_store) :
    cct(_store->cct), store(_store) {}

int RGWReplicaLogger::open_ioctx(librados::IoCtx& ctx, const string& pool)
{
  int r = store->rados->ioctx_create(pool.c_str(), ctx);
  if (r == -ENOENT) {
    rgw_bucket p(pool.c_str());
    r = store->create_pool(p);
    if (r < 0)
      return r;

    // retry
    r = store->rados->ioctx_create(pool.c_str(), ctx);
  }
  if (r < 0) {
    lderr(cct) << "ERROR: could not open rados pool " << pool << dendl;
  }
  return r;
}

int RGWReplicaLogger::update_bound(const string& oid, const string& pool,
                                   const string& daemon_id,
                                   const string& marker, const utime_t& time,
                                   const list<RGWReplicaItemMarker> *entries)
{
  cls_replica_log_progress_marker progress;
  progress.entity_id = daemon_id;
  progress.position_marker = marker;
  progress.position_time = time;
  progress.items = *entries;

  librados::IoCtx ioctx;
  int r = open_ioctx(ioctx, pool);
  if (r < 0) {
    return r;
  }

  librados::ObjectWriteOperation opw;
  cls_replica_log_update_bound(opw, progress);
  return ioctx.operate(oid, &opw);
}

int RGWReplicaLogger::delete_bound(const string& oid, const string& pool,
                                   const string& daemon_id)
{
  librados::IoCtx ioctx;
  int r = open_ioctx(ioctx, pool);
  if (r < 0) {
    return r;
  }

  librados::ObjectWriteOperation opw;
  cls_replica_log_delete_bound(opw, daemon_id);
  return ioctx.operate(oid, &opw);
}

int RGWReplicaLogger::get_bounds(const string& oid, const string& pool,
                                 RGWReplicaBounds& bounds)
{
  librados::IoCtx ioctx;
  int r = open_ioctx(ioctx, pool);
  if (r < 0) {
    return r;
  }

  return cls_replica_log_get_bounds(ioctx, oid, bounds.marker, bounds.oldest_time, bounds.markers);
}

RGWReplicaObjectLogger::
RGWReplicaObjectLogger(RGWRados *_store,
                       const string& _pool,
                       const string& _prefix) : RGWReplicaLogger(_store),
                       pool(_pool), prefix(_prefix) {
  if (pool.empty())
    store->get_log_pool_name(pool);
}

int RGWReplicaObjectLogger::create_log_objects(int shards)
{
  librados::IoCtx ioctx;
  int r = open_ioctx(ioctx, pool);
  if (r < 0) {
    return r;
  }
  for (int i = 0; i < shards; ++i) {
    string oid;
    get_shard_oid(i, oid);
    r = ioctx.create(oid, false);
    if (r < 0)
      return r;
  }
  return r;
}

RGWReplicaBucketLogger::RGWReplicaBucketLogger(RGWRados *_store) :
  RGWReplicaLogger(_store)
{
  store->get_log_pool_name(pool);
  prefix = _store->ctx()->_conf->rgw_replica_log_obj_prefix;
  prefix.append(".");
}

string RGWReplicaBucketLogger::obj_name(const rgw_bucket& bucket, int shard_id)
{
  string s = prefix + bucket.name;

  if (shard_id >= 0) {
    char buf[16];
    snprintf(buf, sizeof(buf), ".%d", shard_id);
    s += buf;
  }
  return s;
}

int RGWReplicaBucketLogger::update_bound(const rgw_bucket& bucket, int shard_id, const string& daemon_id,
                   const string& marker, const utime_t& time,
                   const list<RGWReplicaItemMarker> *entries)
{
  if (shard_id >= 0 ||
      !BucketIndexShardsManager::is_shards_marker(marker)) {
    return RGWReplicaLogger::update_bound(obj_name(bucket, shard_id), pool,
                                          daemon_id, marker, time, entries);
  }

  BucketIndexShardsManager sm;
  int ret = sm.from_string(marker, shard_id);
  if (ret < 0) {
    ldout(cct, 0) << "ERROR: could not parse shards marker: " << marker << dendl;
    return ret;
  }

  map<int, string>& vals = sm.get();

  ret = 0;

  map<int, string>::iterator iter;
  for (iter = vals.begin(); iter != vals.end(); ++iter) {
    ldout(cct, 20) << "updating bound: bucket=" << bucket << " shard=" << iter->first << " marker=" << marker << dendl;
    int r = RGWReplicaLogger::update_bound(obj_name(bucket, iter->first), pool,
                                          daemon_id, iter->second, time, entries);
    if (r < 0) {
      ldout(cct, 0) << "failed to update bound: bucket=" << bucket << " shard=" << iter->first << " marker=" << marker << dendl;
      ret = r;
    }
  }

  return ret;
}
