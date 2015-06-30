/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set_test_fixture.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/future.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::TaskExecutor;
using std::string;
using std::vector;
using unittest::assertGet;

static const stdx::chrono::seconds kFutureTimeout{5};

class RemoveShardTest : public CatalogManagerReplSetTestFixture {
public:
    void expectCount(const NamespaceString& expectedNs,
                     const BSONObj& expectedQuery,
                     const StatusWith<long long>& response) {
        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(configHost, request.target);
            string cmdName = request.cmdObj.firstElement().fieldName();
            ASSERT_EQUALS("count", cmdName);
            const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
            ASSERT_EQUALS(expectedNs.toString(), nss.toString());

            ASSERT_EQUALS(expectedQuery, request.cmdObj["query"].Obj());

            if (response.isOK()) {
                return BSON("ok" << 1 << "n" << response.getValue());
            }

            BSONObjBuilder responseBuilder;
            Command::appendCommandStatus(responseBuilder, response.getStatus());
            return responseBuilder.obj();
        });
    }

    void expectLogChange(const string& clientAddress,
                         const string& what,
                         const string& ns,
                         const BSONObj& detail) {
        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(configHost, request.target);
            ASSERT_EQUALS("config", request.dbname);
            BSONObj expectedCreateCmd = BSON("create" << ChangeLogType::ConfigNS << "capped" << true
                                                      << "size" << 1024 * 1024 * 10);
            ASSERT_EQUALS(expectedCreateCmd, request.cmdObj);

            return BSON("ok" << 1);
        });

        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(configHost, request.target);
            ASSERT_EQUALS("config", request.dbname);

            BatchedInsertRequest actualBatchedInsert;
            std::string errmsg;
            ASSERT_TRUE(actualBatchedInsert.parseBSON(request.dbname, request.cmdObj, &errmsg));
            ASSERT_EQUALS(ChangeLogType::ConfigNS, actualBatchedInsert.getNS().ns());
            auto inserts = actualBatchedInsert.getDocuments();
            ASSERT_EQUALS(1U, inserts.size());
            BSONObj insert = inserts.front();

            auto actualChangeLog = assertGet(ChangeLogType::fromBSON(insert));
            ASSERT_EQUALS(clientAddress, actualChangeLog.getClientAddr());
            ASSERT_EQUALS(what, actualChangeLog.getWhat());
            ASSERT_EQUALS(ns, actualChangeLog.getNS());
            ASSERT_EQUALS(detail, actualChangeLog.getDetails());

            BatchedCommandResponse response;
            response.setOk(true);

            return response.toBSON();
        });
    }

    void setUp() {
        CatalogManagerReplSetTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(configHost);
    }

protected:
    const HostAndPort configHost{"TestHost1"};
};

TEST_F(RemoveShardTest, RemoveShardAnotherShardDraining) {
    string shardName = "shardToRemove";

    auto future = launchAsync([&] {
        ASSERT_EQUALS(ErrorCodes::ConflictingOperationInProgress,
                      catalogManager()->removeShard(operationContext(), shardName));
    });

    expectCount(NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName << ShardType::draining(true)),
                1);

    future.timed_get(kFutureTimeout);
}

TEST_F(RemoveShardTest, RemoveShardCantRemoveLastShard) {
    string shardName = "shardToRemove";

    auto future = launchAsync([&] {
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      catalogManager()->removeShard(operationContext(), shardName));
    });

    // Report that there are no other draining operations ongoing
    expectCount(NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName << ShardType::draining(true)),
                0);

    // Now report that there are no other shard left
    expectCount(
        NamespaceString(ShardType::ConfigNS), BSON(ShardType::name() << NE << shardName), 0);

    future.timed_get(kFutureTimeout);
}

TEST_F(RemoveShardTest, RemoveShardStartDraining) {
    string shardName = "shardToRemove";
    const HostAndPort clientHost{"client1:12345"};
    getMessagingPort()->setRemote(clientHost);

    auto future = launchAsync([&] {
        auto result = assertGet(catalogManager()->removeShard(operationContext(), shardName));
        ASSERT_EQUALS(ShardDrainingStatus::STARTED, result);

    });

    // Report that there are no other draining operations ongoing
    expectCount(NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName << ShardType::draining(true)),
                0);

    // Report that there *are* other shards left
    expectCount(
        NamespaceString(ShardType::ConfigNS), BSON(ShardType::name() << NE << shardName), 1);

    // Report that the shard is not yet marked as draining
    expectCount(NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << shardName << ShardType::draining(true)),
                0);

    // Respond to request to update shard entry and mark it as draining.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BatchedUpdateRequest actualBatchedUpdate;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(ShardType::ConfigNS, actualBatchedUpdate.getNS().ns());
        auto updates = actualBatchedUpdate.getUpdates();
        ASSERT_EQUALS(1U, updates.size());
        auto update = updates.front();

        ASSERT_FALSE(update->getUpsert());
        ASSERT_FALSE(update->getMulti());
        ASSERT_EQUALS(BSON(ShardType::name() << shardName), update->getQuery());
        ASSERT_EQUALS(BSON("$set" << BSON(ShardType::draining(true))), update->getUpdateExpr());

        BatchedCommandResponse response;
        response.setOk(true);
        response.setNModified(1);

        return response.toBSON();
    });

    // Respond to request to reload information about existing shards
    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        auto query = assertGet(LiteParsedQuery::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(ShardType::ConfigNS, query->ns());
        ASSERT_EQ(BSONObj(), query->getFilter());
        ASSERT_EQ(BSONObj(), query->getSort());
        ASSERT_FALSE(query->getLimit().is_initialized());

        ShardType remainingShard;
        remainingShard.setHost("host1");
        remainingShard.setName("shard0");
        return vector<BSONObj>{remainingShard.toBSON()};
    });

    expectLogChange(clientHost.toString(), "removeShard.start", "", BSON("shard" << shardName));

    future.timed_get(kFutureTimeout);
}

TEST_F(RemoveShardTest, RemoveShardStillDrainingChunksRemaining) {
    string shardName = "shardToRemove";

    auto future = launchAsync([&] {
        auto result = assertGet(catalogManager()->removeShard(operationContext(), shardName));
        ASSERT_EQUALS(ShardDrainingStatus::ONGOING, result);

    });

    // Report that there are no other draining operations ongoing
    expectCount(NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName << ShardType::draining(true)),
                0);

    // Report that there *are* other shards left
    expectCount(
        NamespaceString(ShardType::ConfigNS), BSON(ShardType::name() << NE << shardName), 1);

    // Report that the shard is already marked as draining
    expectCount(NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << shardName << ShardType::draining(true)),
                1);

    // Report that there are still chunks to drain
    expectCount(NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::shard(shardName)), 10);

    // Report that there are no more databases to drain
    expectCount(NamespaceString(DatabaseType::ConfigNS), BSON(DatabaseType::primary(shardName)), 0);

    future.timed_get(kFutureTimeout);
}

TEST_F(RemoveShardTest, RemoveShardStillDrainingDatabasesRemaining) {
    string shardName = "shardToRemove";

    auto future = launchAsync([&] {
        auto result = assertGet(catalogManager()->removeShard(operationContext(), shardName));
        ASSERT_EQUALS(ShardDrainingStatus::ONGOING, result);

    });

    // Report that there are no other draining operations ongoing
    expectCount(NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName << ShardType::draining(true)),
                0);

    // Report that there *are* other shards left
    expectCount(
        NamespaceString(ShardType::ConfigNS), BSON(ShardType::name() << NE << shardName), 1);

    // Report that the shard is already marked as draining
    expectCount(NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << shardName << ShardType::draining(true)),
                1);

    // Report that there are no more chunks to drain
    expectCount(NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::shard(shardName)), 0);

    // Report that there are still more databases to drain
    expectCount(NamespaceString(DatabaseType::ConfigNS), BSON(DatabaseType::primary(shardName)), 5);

    future.timed_get(kFutureTimeout);
}

TEST_F(RemoveShardTest, RemoveShardCompletion) {
    string shardName = "shardToRemove";
    const HostAndPort clientHost{"client1:12345"};
    getMessagingPort()->setRemote(clientHost);

    auto future = launchAsync([&] {
        auto result = assertGet(catalogManager()->removeShard(operationContext(), shardName));
        ASSERT_EQUALS(ShardDrainingStatus::COMPLETED, result);

    });

    // Report that there are no other draining operations ongoing
    expectCount(NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName << ShardType::draining(true)),
                0);

    // Report that there *are* other shards left
    expectCount(
        NamespaceString(ShardType::ConfigNS), BSON(ShardType::name() << NE << shardName), 1);

    // Report that the shard is already marked as draining
    expectCount(NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << shardName << ShardType::draining(true)),
                1);

    // Report that there are no more chunks to drain
    expectCount(NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::shard(shardName)), 0);

    // Report that there are no more databases to drain
    expectCount(NamespaceString(DatabaseType::ConfigNS), BSON(DatabaseType::primary(shardName)), 0);

    // Respond to request to remove shard entry.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BatchedDeleteRequest actualBatchedDelete;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedDelete.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(ShardType::ConfigNS, actualBatchedDelete.getNS().ns());
        auto deletes = actualBatchedDelete.getDeletes();
        ASSERT_EQUALS(1U, deletes.size());
        auto deleteOp = deletes.front();

        ASSERT_EQUALS(0, deleteOp->getLimit());
        ASSERT_EQUALS(BSON(ShardType::name() << shardName), deleteOp->getQuery());

        BatchedCommandResponse response;
        response.setOk(true);
        response.setNModified(1);

        return response.toBSON();
    });

    // Respond to request to reload information about existing shards
    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        auto query = assertGet(LiteParsedQuery::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(ShardType::ConfigNS, query->ns());
        ASSERT_EQ(BSONObj(), query->getFilter());
        ASSERT_EQ(BSONObj(), query->getSort());
        ASSERT_FALSE(query->getLimit().is_initialized());

        ShardType remainingShard;
        remainingShard.setHost("host1");
        remainingShard.setName("shard0");
        return vector<BSONObj>{remainingShard.toBSON()};
    });

    expectLogChange(clientHost.toString(), "removeShard", "", BSON("shard" << shardName));

    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
