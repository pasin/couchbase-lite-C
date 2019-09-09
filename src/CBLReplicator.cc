//
// CBLReplicator.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "CBLReplicator.h"
#include "CBLReplicatorConfig.hh"
#include "CBLDocument_Internal.hh"
#include "Internal.hh"
#include "c4.hh"
#include "c4Replicator.h"
#include "c4Private.h"
#include "fleece/Fleece.hh"
#include "fleece/Mutable.hh"
#include <mutex>

using namespace std;
using namespace fleece;
using namespace cbl_internal;


extern "C" {
    void C4RegisterBuiltInWebSocket();
}

static CBLReplicatorStatus external(const C4ReplicatorStatus &c4status) {
    return {
        CBLReplicatorActivityLevel(c4status.level),
        {
            c4status.progress.unitsCompleted / max(float(c4status.progress.unitsTotal), 1.0f),
            c4status.progress.documentCount
        },
        external(c4status.error)
    };
}


class CBLReplicator : public CBLRefCounted {
public:
    CBLReplicator(const CBLReplicatorConfiguration *conf _cbl_nonnull)
    :_conf(*conf)
    { }


    const ReplicatorConfiguration* configuration() const        {return &_conf;}
    bool validate(CBLError *err) const                          {return _conf.validate(err);}


    void start() {
        unique_lock<mutex> lock(_mutex);
        if (_c4repl)
            return;

        // One-time initialization of network transport:
        static once_flag once;
        call_once(once, std::bind(&C4RegisterBuiltInWebSocket));

        // Set up the LiteCore replicator parameters:
        C4ReplicatorParameters params = { };
        auto type = _conf.continuous ? kC4Continuous : kC4OneShot;
        if (_conf.replicatorType != kCBLReplicatorTypePull)
            params.push = type;
        if (_conf.replicatorType != kCBLReplicatorTypePush)
            params.pull = type;
        params.callbackContext = this;
        params.onStatusChanged = [](C4Replicator* c4repl, C4ReplicatorStatus status, void *ctx) {
            ((CBLReplicator*)ctx)->_statusChanged(c4repl, status);
        };

        if (_conf.pushFilter) {
            params.pushFilter = [](C4String docID,
                                   C4RevisionFlags flags,
                                   FLDict body,
                                   void* ctx)
            {
                return ((CBLReplicator*)ctx)->_filter(docID, flags, body, true);
            };
        }
        if (_conf.pullFilter) {
            params.validationFunc = [](C4String docID,
                                       C4RevisionFlags flags,
                                       FLDict body,
                                       void* ctx)
            {
                return ((CBLReplicator*)ctx)->_filter(docID, flags, body, false);
            };
        }

        alloc_slice properties;
        {
            Encoder enc;
            enc.beginDict();
            _conf.writeOptions(enc);
            if (_resetCheckpoint) {
                enc[slice(kC4ReplicatorResetCheckpoint)] = true;
                _resetCheckpoint = false;
            }
            enc.endDict();
            properties = enc.finish();
        }
        params.optionsDictFleece = properties;

        C4Database *otherLocalDB = nullptr;
        if (_conf.endpoint->otherLocalDB())
            otherLocalDB = internal(_conf.endpoint->otherLocalDB());

        // Create/start the LiteCore replicator:
        C4Error c4error;
        _c4repl = c4repl_new(internal(_conf.database),
                             _conf.endpoint->remoteAddress(),
                             _conf.endpoint->remoteDatabaseName(),
                             otherLocalDB,
                             params,
                             &c4error);
        if (!_c4repl) {
            C4ReplicatorStatus status = {kC4Stopped, {}, c4error};
            _status = status;

            lock.unlock();
            _callListener(status);
            return;
        }

        _status = c4repl_getStatus(_c4repl);
        _stopping = false;
        retain(this);
    }


    void stop() {
        lock_guard<mutex> lock(_mutex);
        _stop();
    }


    void resetCheckpoint() {
        lock_guard<mutex> lock(_mutex);
        if (!_c4repl)
            _resetCheckpoint = true;
    }


    CBLReplicatorStatus status() {
        lock_guard<mutex> lock(_mutex);
        return external(_status);
    }


    void setListener(CBLReplicatorChangeListener listener, void *context) {
        lock_guard<mutex> lock(_mutex);
        _listener = listener;
        _listenerContext = context;
    }

private:

    void _stop() {
        if (!_c4repl || _stopping)
            return;

        _stopping = true;
        c4repl_stop(_c4repl);
    }


    void _statusChanged(C4Replicator* c4repl, const C4ReplicatorStatus &status) {
        C4Log("StatusChanged: level=%d, err=%d", status.level, status.error.code);
        {
            lock_guard<mutex> lock(_mutex);
            if (c4repl != _c4repl)
                return;
            _status = status;
        }

        _callListener(status);

        if (status.level == kC4Stopped) {
            lock_guard<mutex> lock(_mutex);
            _c4repl = nullptr;
            _stopping = false;
            release(this);
        }
    }


    void _callListener(C4ReplicatorStatus status) {
        if (_listener) {
            auto cblStatus = external(status);
            _listener(_listenerContext, this, &cblStatus);
        } else if (status.error.code) {
            char buf[256];
            C4Warn("No listener to receive error from CBLReplicator %p: %s",
                   this, c4error_getDescriptionC(status.error, buf, sizeof(buf)));
        }
    }


    bool _filter(slice docID, C4RevisionFlags flags, Dict body, bool pushing) {
        Retained<CBLDocument> doc = new CBLDocument(_conf.database, string(docID), flags, body);
        CBLReplicationFilter filter = pushing ? _conf.pushFilter : _conf.pullFilter;
        return filter(_conf.filterContext, doc, (flags & kRevDeleted) != 0);
    }


    ReplicatorConfiguration const _conf;
    Retained<CBLDatabase> const _otherLocalDB;
    std::mutex _mutex;
    c4::ref<C4Replicator> _c4repl;
    C4ReplicatorStatus _status {kC4Stopped};
    CBLReplicatorChangeListener _listener {nullptr};
    void* _listenerContext {nullptr};
    bool _resetCheckpoint {false};
    bool _stopping {false};
};


#pragma mark - C API:


CBLEndpoint* CBLEndpoint_NewWithURL(const char *url _cbl_nonnull) CBLAPI {
    return new CBLURLEndpoint(url);
}

void CBLEndpoint_Free(CBLEndpoint *endpoint) CBLAPI {
    delete endpoint;
}

CBLAuthenticator* CBLAuth_NewBasic(const char *username, const char *password) CBLAPI {
    return new BasicAuthenticator(username, password);
}

CBLAuthenticator* CBLAuth_NewSession(const char *sessionID, const char *cookieName) CBLAPI {
    return new SessionAuthenticator(sessionID, cookieName);
}

void CBLAuth_Free(CBLAuthenticator *auth) CBLAPI {
    delete auth;
}

CBLReplicator* CBLReplicator_New(const CBLReplicatorConfiguration* conf, CBLError *outError) CBLAPI {
    return validated(new CBLReplicator(conf), outError);
}

const CBLReplicatorConfiguration* CBLReplicator_Config(CBLReplicator* repl) CBLAPI {
    return repl->configuration();
}

CBLReplicatorStatus CBLReplicator_Status(CBLReplicator* repl) CBLAPI {
    return repl->status();
}

void CBLReplicator_Start(CBLReplicator* repl) CBLAPI            {repl->start();}
void CBLReplicator_Stop(CBLReplicator* repl) CBLAPI             {repl->stop();}
void CBLReplicator_ResetCheckpoint(CBLReplicator* repl) CBLAPI  {repl->resetCheckpoint();}
